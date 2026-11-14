# SPDX-License-Identifier: MIT
"""Run the production HD gain-probe lambda against controlled tuner input."""
import pathlib
import subprocess
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
source = repo / 'src/addon.cpp'

def extract(text):
    begin = text.index('    struct gain_scan_result\n')
    end = text.index('    try\n', begin)
    declaration = text[begin:end]
    begin = text.index('      scan_one_gain =', end)
    end = text.index('      uint32_t frequency_index = 0;', begin)
    return declaration, text[begin:end]

fixture = r'''
#include "rtldevice.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
using namespace std::chrono;
class string_exception : public std::runtime_error {
  template<class... T> static std::string message(T const&... values) {
    std::ostringstream out; (out << ... << values); return out.str();
  }
public:
  template<class... T> string_exception(T const&... values):std::runtime_error(message(values...)){}
};
struct muxscanner {
  struct multiplex {
    bool sync=false; std::vector<int> subchannels;
    bool ber_valid=false, mer_valid=false;
    float cber=1, mer_lower=0, mer_upper=0;
  };
  virtual ~muxscanner()=default;
  virtual void inputsamples(uint8_t const*,size_t)=0;
};
bool report_sync=true;
struct hdmuxscanner : muxscanner {
  std::function<void(multiplex const&)> callback;
  hdmuxscanner(std::function<void(multiplex const&)> cb):callback(cb){}
  static std::unique_ptr<muxscanner> create(uint32_t,uint32_t,std::function<void(multiplex const&)> cb) {
    return std::make_unique<hdmuxscanner>(cb);
  }
  void inputsamples(uint8_t const*,size_t) override {
    multiplex data{}; data.sync=report_sync;
    if(report_sync) data.subchannels.push_back(1);
    callback(data);
  }
};
enum class input_mode { healthy, silent, stalled, ended, failed };
struct fake_device : rtldevice {
  input_mode mode=input_mode::healthy;
  mutable std::atomic<bool> cancel{false};
  mutable unsigned begins=0;
  void begin_stream() const override { cancel=false; ++begins; }
  void cancel_async() const override { cancel=true; }
  char const* get_device_name() const override { return "controlled tuner"; }
  void get_valid_gains(std::vector<int>& gains) const override { gains={27,87}; }
  size_t read(uint8_t*,size_t) const override { return 0; }
  void read_async(asynccallback const& callback,uint32_t length) const override {
    if(mode==input_mode::failed) throw std::runtime_error("USB transfer failed");
    if(mode==input_mode::ended) return;
    uint8_t data[32768]{}; unsigned count=0;
    while(!cancel.load()) {
      if(mode==input_mode::healthy || (mode==input_mode::stalled && count==0)) callback(data,length);
      ++count; std::this_thread::sleep_for(10ms);
    }
  }
  void set_automatic_gain_control(bool) const override {}
  uint32_t set_center_frequency(uint32_t value) const override { return value; }
  int set_frequency_correction(int value) const override { return value; }
  int set_gain(int value) const override { return value; }
  void set_direct_sampling(int) const override {}
  uint32_t set_sample_rate(uint32_t value) const override { return value; }
  void set_test_mode(bool) const override {}
};
int factory_calls=0;
std::unique_ptr<rtldevice> create_device(int) { ++factory_calls; return std::make_unique<fake_device>(); }
template<class... T> void log_info(T const&...) {}
int channel_updates=0, group_updates=0;
void TriggerChannelUpdate() { ++channel_updates; }
void TriggerChannelGroupsUpdate() { ++group_updates; }
DECLARATION
int main(int argc,char**) {
  std::unique_ptr<rtldevice> device=std::make_unique<fake_device>();
  auto& fake=static_cast<fake_device&>(*device);
  constexpr uint32_t sample_rate=1488375;
  char const* frequency_unit=" MHz";
  int muxes_found=5, settings=0;
  bool hd_am_scan=false, canceled=false, cancel_test=false;
  auto subchannel_quiet_time=4s;
  auto update_progress=[&](int,std::string const&,std::string const&,std::string const&) {return cancel_test;};
  std::function<gain_scan_result(uint32_t,int,seconds,seconds,bool,char const*,int)> scan_one_gain;
  METHOD
  auto probe=[&](seconds duration=1s) {return scan_one_gain(92900000,87,duration,0s,true,"92.9",25);};
  auto first=probe(); auto second=probe();
  assert(first.sync && first.bytes>0 && second.bytes>0);
  assert(factory_calls==0 && fake.begins==2); // same tuner across gain probes
  report_sync=false;
  auto empty=probe(); assert(!empty.sync && empty.bytes>0); // RF silence is valid input
  auto expect_error=[&](input_mode mode,std::string const& reason, bool quick=false) {
    fake.mode=mode; auto before=steady_clock::now(); bool caught=false;
    int updates=channel_updates;
    try { probe(4s); }
    catch(std::exception const& ex) {
      std::string message=ex.what();
      caught=message.find(reason)!=std::string::npos && message.find("92.9 MHz")!=std::string::npos &&
             message.find("5 station(s) already found are kept")!=std::string::npos;
    }
    assert(caught && channel_updates==updates+1 && group_updates==channel_updates);
    assert(steady_clock::now()-before < (quick ? 1s : 3800ms));
  };
  expect_error(input_mode::silent,"stopped delivering samples");
  expect_error(input_mode::stalled,"stopped delivering samples");
  expect_error(input_mode::failed,"USB transfer failed",true);
  expect_error(input_mode::ended,"stopped delivering samples",true);
  fake.mode=input_mode::silent; cancel_test=true;
  int updates=channel_updates;
  probe(4s); assert(canceled && channel_updates==updates);
  std::cout << "PASS: shared tuner, live RF silence, no input, mid-probe stall, reader failure/exit, partial results and cancellation\n";
}
'''

declaration, method = extract(source.read_text())
with tempfile.TemporaryDirectory(prefix='hd-scan-test-') as directory:
    cpp, binary = pathlib.Path(directory)/'test.cpp', pathlib.Path(directory)/'test'
    def build(body):
        cpp.write_text(fixture.replace('DECLARATION', declaration).replace('METHOD', body))
        subprocess.run(['g++','-std=c++17','-O2','-pthread','-I',str(repo/'src'),str(cpp),'-o',str(binary)],check=True)
    build(method)
    subprocess.run([str(binary)],check=True,timeout=20)
    # Restore just the old treatment of missing input. It must fail the
    # no-input regression, while still using the same controlled device.
    regression = method.replace('if (!canceled && (reader_exception || input_failed))', 'if (false)')
    build(regression)
    failed = subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
    assert failed.returncode != 0, 'Ignoring tuner input failure must fail the regression'
    print('Negative control fails when missing tuner input is silently accepted.')
