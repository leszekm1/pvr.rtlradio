"""Exercise the production HD prebuffer policy with a deterministic clock."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / 'src'
cpp = r'''
#include "hd_audio_buffer.h"
#include <cassert>
#include <cstdio>
using namespace std::chrono;
int main() {
  hd_audio_buffer buffer;
  const auto start = hd_audio_buffer::clock::time_point{};
  constexpr std::size_t burst = 65536;
  assert(!buffer.ready());
  buffer.queued(0, start);
  assert(!buffer.ready()); // Metadata cannot start audio playback.
  buffer.queued(burst, start);
  assert(!buffer.ready()); // One burst would drain before the next arrives.
  buffer.queued(burst, start + milliseconds(1486));
  assert(buffer.ready());
  assert(buffer.frames() == burst * 2);
  buffer.consumed(burst * 2 - 2048);
  assert(buffer.ready()); // Do not rebuffer on every normal low-water mark.
  buffer.queued(burst, start + milliseconds(2972));
  assert(buffer.ready());
  buffer.consumed(burst + 2048);
  assert(buffer.frames() == 0);
  buffer.queued(burst, start + milliseconds(6000)); // Actual signal gap.
  assert(!buffer.ready());
  buffer.queued(burst, start + milliseconds(7486));
  assert(buffer.ready());
  buffer.clear(); // A queue overflow/stream reset discards its frame count.
  assert(buffer.frames() == 0 && !buffer.ready());
  buffer.queued(hd_audio_buffer::minimum_frames() - 1, start);
  assert(!buffer.ready());
  buffer.queued(1, start + milliseconds(1));
  assert(buffer.ready());
  buffer.consumed(hd_audio_buffer::minimum_frames());
  assert(buffer.frames() == 0);
  puts("PASS: startup reserve, metadata exclusion, frame conservation, uninterrupted normal bursts, gap recovery and queue reset");
}
'''
with tempfile.TemporaryDirectory(prefix='hd-audio-buffer-test-') as temporary:
    root = Path(temporary)
    (root / 'test.cpp').write_text(cpp)
    subprocess.run(['g++', '-std=c++17', '-O2', '-I', str(source),
                    str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
