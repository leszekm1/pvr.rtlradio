// SPDX-License-Identifier: MIT
#ifndef PVR_RTLRADIO_HD_AUDIO_BUFFER_H
#define PVR_RTLRADIO_HD_AUDIO_BUFFER_H

#include <cassert>
#include <chrono>
#include <cstddef>

// Used under hdstream's queue lock. HD audio arrives in roughly 1.5-second
// bursts. Keep a reserve at startup/recovery so Kodi's live queue controller
// does not repeatedly slow music by 5% between otherwise healthy bursts.
class hd_audio_buffer
{
public:
  using clock = std::chrono::steady_clock;
  static constexpr std::size_t minimum_frames() { return 44100 * 5 / 2; }

  void queued(std::size_t frames, clock::time_point now = clock::now())
  {
    if (frames == 0)
      return;
    if (m_seen_audio && now - m_last_audio >= std::chrono::seconds(2))
      m_filling = true;
    m_queued_frames += frames;
    m_last_audio = now;
    m_seen_audio = true;
  }

  bool ready()
  {
    if (m_filling && m_queued_frames < minimum_frames())
      return false;
    m_filling = false;
    return true;
  }

  void consumed(std::size_t frames)
  {
    assert(frames <= m_queued_frames);
    m_queued_frames -= frames;
  }

  void clear()
  {
    m_queued_frames = 0;
    m_filling = true;
    m_seen_audio = false;
    m_last_audio = clock::time_point{};
  }

  std::size_t frames() const { return m_queued_frames; }

private:
  std::size_t m_queued_frames{0};
  bool m_filling{true};
  bool m_seen_audio{false};
  clock::time_point m_last_audio{};
};

#endif
