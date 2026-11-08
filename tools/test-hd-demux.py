# SPDX-License-Identifier: MIT
"""Exercise the production demux method with a controlled packet producer."""
import pathlib
import subprocess
import tempfile

source = pathlib.Path(__file__).resolve().parents[1] / 'src/hdstream.cpp'
text = source.read_text()
start = text.index('DEMUX_PACKET* hdstream::demuxread(')
end = text.index('//---------------------------------------------------------------------------', start)
method = text[start:end]
fixture = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <cstdio>
using namespace std::chrono;
struct DEMUX_PACKET {
  int iStreamId=0, iSize=0;
  double duration=0, dts=0, pts=0;
  unsigned char pData[16]{};
};
class hdstream {
public:
  struct demux_packet_t {
    int streamid=1, size=2;
    double duration=20, dts=12345, pts=12345;
    std::unique_ptr<unsigned char[]> data{new unsigned char[2]{7,9}};
  };
  std::mutex m_queuelock;
  std::condition_variable m_cv;
  std::queue<std::unique_ptr<demux_packet_t>> m_queue;
  std::atomic<bool> m_stopped{false};
  std::exception_ptr m_worker_exception;
  DEMUX_PACKET* demuxread(std::function<DEMUX_PACKET*(int)> const& allocator);
  void push() {
    { std::lock_guard<std::mutex> lock(m_queuelock);
      m_queue.emplace(std::make_unique<demux_packet_t>()); }
    m_cv.notify_all();
  }
};
METHOD
int main() {
  auto allocator=[](int size) { auto p=new DEMUX_PACKET; p->iSize=size; return p; };
  hdstream empty;
  auto before=steady_clock::now();
  std::unique_ptr<DEMUX_PACKET> packet(empty.demuxread(allocator));
  double idle=duration<double,std::milli>(steady_clock::now()-before).count();
  if (!packet || packet->iSize != 0 || idle>200) {
    printf("FAIL: idle demux blocked playback control for %.1f ms\n",idle); return 1;
  }
  hdstream delayed;
  std::thread producer([&] { std::this_thread::sleep_for(500ms); delayed.push(); });
  int controls=0;
  double longest=0;
  do {
    before=steady_clock::now();
    packet.reset(delayed.demuxread(allocator));
    longest=std::max(longest,duration<double,std::milli>(steady_clock::now()-before).count());
    ++controls;
  } while (packet && packet->iSize==0);
  producer.join();
  if (controls<4 || longest>200 || !packet || packet->iSize!=2 ||
      packet->iStreamId!=1 || packet->dts!=12345 || packet->pts!=12345 ||
      packet->duration!=20 || packet->pData[0]!=7 || packet->pData[1]!=9) {
    printf("FAIL: burst gap responsiveness or packet delivery\n"); return 1;
  }
  hdstream stopped;
  std::thread stopper([&] { std::this_thread::sleep_for(20ms);
    stopped.m_stopped=true; stopped.m_cv.notify_all(); });
  before=steady_clock::now();
  packet.reset(stopped.demuxread(allocator));
  stopper.join();
  if (!packet || packet->iSize || steady_clock::now()-before>200ms) return 1;
  stopped.m_worker_exception=std::make_exception_ptr(std::runtime_error("worker error"));
  bool rethrown=false;
  try { packet.reset(stopped.demuxread(allocator)); }
  catch (const std::runtime_error&) { rethrown=true; }
  if (!rethrown) return 1;
  printf("PASS: control loop serviced %d times in 500ms burst gap; max wait %.1fms; packet data, stop and worker exception retained\n",controls,longest);
}
'''
with tempfile.TemporaryDirectory(prefix='hd-demux-test-') as temporary:
    path = pathlib.Path(temporary)
    cpp, exe = path/'test.cpp', path/'test'
    cpp.write_text(fixture.replace('METHOD', method))
    subprocess.run(['g++', '-std=c++17', '-O2', '-pthread', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    original = method.replace('std::chrono::milliseconds(50)', 'std::chrono::milliseconds(1500)')
    cpp.write_text(fixture.replace('METHOD', original))
    subprocess.run(['g++', '-std=c++17', '-O2', '-pthread', str(cpp), '-o', str(exe)], check=True)
    assert subprocess.run([str(exe)]).returncode != 0, 'Original wait should fail the responsiveness test'
    print('Original HD wait fails the responsiveness test; patched production method passes.')
