#include <mutex>
//---------------------------------------------------------------------------
// Copyright (c) 2020-2022 Michael G. Brehm
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//---------------------------------------------------------------------------

#include "hdstream.h"
#include <sys/stat.h>
#include <cstdio>
#include "id3v2tag.h"
#include "exception_control/string_exception.h"
#include "utils/align.h"
#include "utils/value_size_defines.h"
#include <kodi/General.h>
#include <algorithm>
#include <chrono>
#include <memory.h>

#include <string>
#include <sys/types.h>
#include <cctype>
#include <set>
#include <map>
// Uncomment to test ID3 tag support
#define KODI_HAS_ID3

#pragma warning(push, 4)

// hdstream::MAX_PACKET_QUEUE
//
// Maximum number of queued demux packets
size_t const hdstream::MAX_PACKET_QUEUE = 800; // ~2sec analog / ~10sec digital

// hdstream::SAMPLE_RATE
//
// Fixed device sample rate required for HD Radio
uint32_t const hdstream::SAMPLE_RATE = 1488375;

// hdstream::STREAM_ID_AUDIO
//
// Stream identifier for the audio output stream
int const hdstream::STREAM_ID_AUDIO = 1;

// hdstream::STREAM_ID_ID3TAG
//
// Stream identifier for the ID3v2 tag output stream
int const hdstream::STREAM_ID_ID3TAG = 2;


// PVR_RTLRADIO_HD_NOWPLAYING_EPG_IMPL
namespace pvr_rtlradio_hd_nowplaying
{
  static std::mutex g_lock;
  static std::string g_title;
  static std::string g_artist;
  static std::string g_album;
  static std::string g_signal;
  static int g_channel_uid = 0;

  void set_active_channel(int channelUid)
  {
    std::lock_guard<std::mutex> lock(g_lock);

    g_channel_uid = channelUid;
    g_title.clear();
    g_artist.clear();
    g_album.clear();
    g_signal.clear();
  }

  bool get(std::string& title,
           std::string& artist,
           std::string& album,
           std::string& signal)
  {
    std::lock_guard<std::mutex> lock(g_lock);

    if (g_title.empty() && g_artist.empty() && g_album.empty() && g_signal.empty())
      return false;

    title = g_title;
    artist = g_artist;
    album = g_album;
    signal = g_signal;

    return true;
  }

  bool get_for_channel(int channelUid,
                       std::string& title,
                       std::string& artist,
                       std::string& album,
                       std::string& signal)
  {
    std::lock_guard<std::mutex> lock(g_lock);

    if (g_channel_uid != channelUid)
      return false;

    if (g_title.empty() && g_artist.empty() && g_album.empty() && g_signal.empty())
      return false;

    title = g_title;
    artist = g_artist;
    album = g_album;
    signal = g_signal;

    return true;
  }

  void set(std::string const& title,
           std::string const& artist,
           std::string const& album,
           std::string const& signal)
  {
    std::lock_guard<std::mutex> lock(g_lock);

    g_title = title;
    g_artist = artist;
    g_album = album;
    g_signal = signal;
  }
}


namespace pvr_rtlradio_hd_station_identity
{
  static std::mutex g_lock;
  static std::string g_name;
  static std::string g_slogan;
  static std::map<uint16_t, unsigned int> g_station_logo_port_programs;

  static std::string one_line(std::string value)
  {
    for (char& c : value)
    {
      if (c == '\r' || c == '\n' || c == '\0')
        c = ' ';
    }

    return value;
  }

  static std::string slug(std::string value)
  {
    std::string out;
    bool last_dash = true;

    for (unsigned char ch : value)
    {
      if ((ch >= 'A') && (ch <= 'Z'))
        ch = static_cast<unsigned char>(ch - 'A' + 'a');

      if (((ch >= 'a') && (ch <= 'z')) ||
          ((ch >= '0') && (ch <= '9')))
      {
        out.push_back(static_cast<char>(ch));
        last_dash = false;
      }
      else if (!last_dash)
      {
        out.push_back('-');
        last_dash = true;
      }
    }

    while (!out.empty() && out.back() == '-')
      out.pop_back();

    return out;
  }

  static std::string image_ext(uint32_t mime,
                               uint8_t const* data,
                               size_t size)
  {
    if (mime == NRSC5_MIME_PNG)
      return "png";

    if (mime == NRSC5_MIME_JPEG)
      return "jpg";

    if (data && size >= 8 &&
        data[0] == 0x89 &&
        data[1] == 'P' &&
        data[2] == 'N' &&
        data[3] == 'G' &&
        data[4] == '\r' &&
        data[5] == '\n' &&
        data[6] == 0x1a &&
        data[7] == '\n')
    {
      return "png";
    }

    if (data && size >= 2 &&
        data[0] == 0xff &&
        data[1] == 0xd8)
    {
      return "jpg";
    }

    return std::string();
  }

  void clear()
  {
    std::lock_guard<std::mutex> lock(g_lock);

    g_name.clear();
    g_slogan.clear();
    g_station_logo_port_programs.clear();
  }

  void set_sis(std::string const& name,
               std::string const& slogan)
  {
    std::lock_guard<std::mutex> lock(g_lock);

    g_name = one_line(name);
    g_slogan = one_line(slogan);
  }

  std::string program_key(std::string const& station_key,
                          unsigned int program)
  {
    if (station_key.empty())
      return std::string();

    return station_key + "---" + std::to_string(program);
  }

  void add_station_logo_port(uint16_t port,
                             unsigned int program)
  {
    std::lock_guard<std::mutex> lock(g_lock);

    g_station_logo_port_programs[port] = program;
  }

  bool station_logo_program_for_port(uint16_t port,
                                     unsigned int& program)
  {
    std::lock_guard<std::mutex> lock(g_lock);

    auto found = g_station_logo_port_programs.find(port);
    if (found == g_station_logo_port_programs.end())
      return false;

    program = found->second;
    return true;
  }

  void get(std::string& name,
           std::string& slogan,
           std::string& key)
  {
    std::lock_guard<std::mutex> lock(g_lock);

    name = g_name;
    slogan = g_slogan;
    key = slug(g_name);
  }

  bool is_station_logo_lot(uint16_t port,
                           uint32_t mime)
  {
    std::lock_guard<std::mutex> lock(g_lock);

    return mime == NRSC5_MIME_STATION_LOGO ||
           g_station_logo_port_programs.find(port) != g_station_logo_port_programs.end();
  }

  bool cache_station_logo_lot(uint16_t port,
                              unsigned int lot,
                              uint32_t mime,
                              uint8_t const* data,
                              size_t size)
  {
    if (!data || size == 0)
      return false;

    if (!is_station_logo_lot(port, mime))
      return false;

    std::string station_name;
    std::string station_slogan;
    std::string station_key;
    get(station_name, station_slogan, station_key);

    if (station_key.empty())
    {
      kodi::Log(ADDON_LOG_DEBUG,
                "HDRADIO_STATION logo lot=%u port=%u mime=%u skipped because station_key is empty",
                lot,
                port,
                mime);
      return false;
    }

    std::string ext = image_ext(mime, data, size);

    if (ext.empty())
    {
      kodi::Log(ADDON_LOG_WARNING,
                "HDRADIO_STATION logo lot=%u port=%u mime=%u skipped because image type is unknown size=%zu station='%s'",
                lot,
                port,
                mime,
                size,
                station_name.c_str());
      return false;
    }

    char const* data_dir =
        "/storage/.kodi/userdata/addon_data/pvr.rtlradio";
    char const* stationcache_dir =
        "/storage/.kodi/userdata/addon_data/pvr.rtlradio/stationcache";

    ::mkdir(data_dir, 0755);
    ::mkdir(stationcache_dir, 0755);

    unsigned int logo_program = 0;
    bool const has_logo_program =
        station_logo_program_for_port(port, logo_program);

    std::string logo_key =
        has_logo_program ? program_key(station_key, logo_program) : station_key;

    std::string path = stationcache_dir;
    path += "/";
    path += logo_key;
    path += ".";
    path += ext;

    FILE* imgfp = std::fopen(path.c_str(), "wb");
    if (!imgfp)
      return false;

    bool const wrote =
        std::fwrite(data, 1, size, imgfp) == size;

    std::fclose(imgfp);

    kodi::Log(ADDON_LOG_DEBUG,
              "HDRADIO_STATION cache_logo lot=%u port=%u mime=%u image_ok=%d station_key='%s' logo_key='%s' program=%u has_program=%d station_name='%s' image='%s'",
              lot,
              port,
              mime,
              wrote ? 1 : 0,
              station_key.c_str(),
              logo_key.c_str(),
              logo_program,
              has_logo_program ? 1 : 0,
              station_name.c_str(),
              path.c_str());

    return wrote;
  }
}


namespace
{
struct hd_id3_text_t
{
  std::string title;
  std::string artist;
  std::string album;
  std::string genre;
};

uint32_t hd_id3_syncsafe_size(const uint8_t* p)
{
  return ((p[0] & 0x7F) << 21) | ((p[1] & 0x7F) << 14) | ((p[2] & 0x7F) << 7) |
         (p[3] & 0x7F);
}

uint32_t hd_id3_be32(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool hd_id3_valid_frame_id(const uint8_t* p)
{
  for (int i = 0; i < 4; ++i)
  {
    if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9')))
      return false;
  }

  return true;
}

std::string hd_id3_trim(std::string value)
{
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                            value.front() == '\r' || value.front() == '\n' ||
                            value.front() == '\0'))
    value.erase(value.begin());

  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r' || value.back() == '\n' ||
                            value.back() == '\0'))
    value.pop_back();

  return value;
}

std::string hd_id3_truncate(std::string value, size_t maxlen)
{
  if (value.size() <= maxlen)
    return value;

  if (maxlen <= 3)
    return value.substr(0, maxlen);

  return value.substr(0, maxlen - 3) + "...";
}

std::string hd_id3_decode_text_frame(const uint8_t* data, size_t size)
{
  if (!data || size == 0)
    return {};

  uint8_t encoding = data[0];
  data++;
  size--;

  std::string out;

  // ISO-8859-1 or UTF-8. Preserve normal printable bytes.
  if (encoding == 0 || encoding == 3)
  {
    for (size_t i = 0; i < size; ++i)
    {
      uint8_t c = data[i];
      if (c == 0)
        break;

      if (c >= 32 || c >= 128)
        out.push_back(static_cast<char>(c));
      else
        out.push_back(' ');
    }
  }

  // Basic UTF-16 / UTF-16BE handling for ASCII-range characters.
  else if (encoding == 1 || encoding == 2)
  {
    bool little = false;
    size_t pos = 0;

    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
      little = true;
      pos = 2;
    }
    else if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF)
    {
      little = false;
      pos = 2;
    }
    else
    {
      little = false;
    }

    for (; pos + 1 < size; pos += 2)
    {
      uint16_t ch = little ? (data[pos] | (data[pos + 1] << 8))
                           : ((data[pos] << 8) | data[pos + 1]);

      if (ch == 0)
        break;

      if (ch >= 32 && ch <= 126)
        out.push_back(static_cast<char>(ch));
      else
        out.push_back(' ');
    }
  }

  return hd_id3_trim(out);
}

bool hd_id3_parse_text(const uint8_t* data, size_t size, hd_id3_text_t& text)
{
  if (!data || size < 10 || memcmp(data, "ID3", 3) != 0)
    return false;

  uint8_t major = data[3];
  size_t tag_end = 10 + hd_id3_syncsafe_size(data + 6);
  tag_end = std::min(tag_end, size);

  size_t pos = 10;
  while (pos + 10 <= tag_end)
  {
    if (data[pos] == 0)
      break;

    if (!hd_id3_valid_frame_id(data + pos))
      break;

    std::string frame(reinterpret_cast<const char*>(data + pos), 4);
    uint32_t frame_size = (major == 4) ? hd_id3_syncsafe_size(data + pos + 4)
                                       : hd_id3_be32(data + pos + 4);

    pos += 10;

    if (frame_size == 0 || pos + frame_size > tag_end)
      break;

    std::string value = hd_id3_decode_text_frame(data + pos, frame_size);

    if (frame == "TIT2")
      text.title = value;
    else if (frame == "TPE1")
      text.artist = value;
    else if (frame == "TALB")
      text.album = value;
    else if (frame == "TCON")
      text.genre = value;

    pos += frame_size;
  }

  return !text.title.empty() || !text.artist.empty() || !text.album.empty() || !text.genre.empty();
}
}

//---------------------------------------------------------------------------
// hdstream Constructor (private)
//
// Arguments:
//
//	device			- RTL-SDR device instance
//	tunerprops		- Tuner device properties
//	channelprops	- Channel properties
//	hdprops			- HD Radio digital signal processor properties
//	subchannel		- Multiplex subchannel number

hdstream::hdstream(std::unique_ptr<rtldevice> device,
                   struct tunerprops const& tunerprops,
                   struct channelprops const& channelprops,
                   struct hdprops const& hdprops,
                   uint32_t subchannel)
  : m_device(std::move(device)),
    m_subchannel((subchannel > 0) ? subchannel : 1),
    m_muxname([](uint32_t frequency_hz) {
      // TEMP_SET_MUXNAME_FROM_FREQUENCY
      char text[32] = {};
      snprintf(text, sizeof(text), "%.1f MHz", frequency_hz / 1000000.0);
      return std::string(text);
    }(channelprops.frequency)),
    m_pcmgain(powf(10.0f, hdprops.outputgain / 10.0f))
{
  // Initialize the RTL-SDR device instance
  m_device->set_frequency_correction(tunerprops.freqcorrection + channelprops.freqcorrection);
  m_device->set_sample_rate(SAMPLE_RATE);
  m_device->set_center_frequency(channelprops.frequency);

  // Adjust the device gain as specified by the channel properties
  m_device->set_automatic_gain_control(channelprops.autogain);
  if (channelprops.autogain == false)
    m_device->set_gain(channelprops.manualgain);

  // Initialize the HD Radio demodulator
  nrsc5_open_pipe(&m_nrsc5);
  nrsc5_set_mode(m_nrsc5, NRSC5_MODE_FM);
  nrsc5_set_callback(m_nrsc5, nrsc5_callback, this);

  // Create a worker thread on which to perform demodulation
  scalar_condition<bool> started{false};
  m_worker = std::thread(&hdstream::worker, this, std::ref(started));
  started.wait_until_equals(true);
}

//---------------------------------------------------------------------------
// hdstream Destructor

hdstream::~hdstream()
{
  close();
}

//---------------------------------------------------------------------------
// hdstream::canseek
//
// Gets a flag indicating if the stream allows seek operations
//
// Arguments:
//
//	NONE

bool hdstream::canseek(void) const
{
  return false;
}

//---------------------------------------------------------------------------
// hdstream::close
//
// Closes the stream
//
// Arguments:
//
//	NONE

void hdstream::close(void)
{
  m_stop = true; // Signal worker thread to stop
  if (m_device)
    m_device->cancel_async(); // Cancel any async read operations
  if (m_worker.joinable())
    m_worker.join(); // Wait for thread

  nrsc5_close(m_nrsc5); // Close NRSC5
  m_nrsc5 = nullptr; // Reset NRSC5 API handle

  m_device.reset(); // Release RTL-SDR device
}

//---------------------------------------------------------------------------
// hdstream::create (static)
//
// Factory method, creates a new hdstream instance
//
// Arguments:
//
//	device			- RTL-SDR device instance
//	tunerprops		- Tunder device properties
//	channelprops	- Channel properties
//	hdprops			- HD Radio digital signal processor properties
//	subchannel		- Multiplex subchannel number

std::unique_ptr<hdstream> hdstream::create(std::unique_ptr<rtldevice> device,
                                           struct tunerprops const& tunerprops,
                                           struct channelprops const& channelprops,
                                           struct hdprops const& hdprops,
                                           uint32_t subchannel)
{
  return std::unique_ptr<hdstream>(
      new hdstream(std::move(device), tunerprops, channelprops, hdprops, subchannel));
}

//---------------------------------------------------------------------------
// hdstream::demuxabort
//
// Aborts the demultiplexer
//
// Arguments:
//
//	NONE

void hdstream::demuxabort(void)
{
}

//---------------------------------------------------------------------------
// hdstream::demuxflush
//
// Flushes the demultiplexer
//
// Arguments:
//
//	NONE

void hdstream::demuxflush(void)
{
}

//---------------------------------------------------------------------------
// hdstream::demuxread
//
// Reads the next packet from the demultiplexer
//
// Arguments:
//
//	allocator		- DemuxPacket allocation function

DEMUX_PACKET* hdstream::demuxread(std::function<DEMUX_PACKET*(int)> const& allocator)
{
  // Wait up to 1500ms for there to be a packet available for processing, don't use
  // an unconditional wait here; unlike analog radio there may not be data until
  // the digitial signal has been synchronized
  std::unique_lock<std::mutex> lock(m_queuelock);
  if (!m_cv.wait_for(lock, std::chrono::milliseconds(1500),
                     [&]() -> bool { return ((m_queue.size() > 0) || m_stopped.load() == true); }))
    return allocator(0);

  // If the worker thread was stopped, check for and re-throw any exception that occurred,
  // otherwise assume it was stopped normally and return an empty demultiplexer packet
  if (m_stopped.load() == true)
  {

    if (m_worker_exception)
      std::rethrow_exception(m_worker_exception);
    else
      return allocator(0);
  }

  // Pop off the topmost object from the queue<> and release the lock
  std::unique_ptr<demux_packet_t> packet(std::move(m_queue.front()));
  m_queue.pop();
  lock.unlock();

  // The packet queue should never have a null packet in it
  assert(packet);
  if (!packet)
    return allocator(0);

  // Allocate and initialize the DEMUX_PACKET
  DEMUX_PACKET* demuxpacket = allocator(packet->size);
  if (demuxpacket != nullptr)
  {

    demuxpacket->iStreamId = packet->streamid;
    demuxpacket->iSize = packet->size;
    demuxpacket->duration = packet->duration;
    demuxpacket->dts = packet->dts;
    demuxpacket->pts = packet->pts;
    if (packet->size > 0)
      memcpy(demuxpacket->pData, packet->data.get(), packet->size);
  }

  return demuxpacket;
}

//---------------------------------------------------------------------------
// hdstream::demuxreset
//
// Resets the demultiplexer
//
// Arguments:
//
//	NONE

void hdstream::demuxreset(void)
{
}

//---------------------------------------------------------------------------
// hdstream::devicename
//
// Gets the device name associated with the stream
//
// Arguments:
//
//	NONE

std::string hdstream::devicename(void) const
{
  return std::string(m_device->get_device_name());
}

//---------------------------------------------------------------------------
// hdstream::enumproperties
//
// Enumerates the stream properties
//
// Arguments:
//
//	callback		- Callback to invoke for each stream

void hdstream::enumproperties(std::function<void(struct streamprops const& props)> const& callback)
{
  // AUDIO STREAM
  //
  streamprops audio = {};
  audio.codec = "pcm_s16le";
  audio.pid = STREAM_ID_AUDIO;
  audio.channels = 2;
  audio.samplerate = 44100;
  audio.bitspersample = 16;
  callback(audio);

#ifdef KODI_HAS_ID3
  // LOW_CPU_SIDECAR_ONLY_V9
  //
  // Do not expose a Kodi ID3 demux stream. The sidecar service handles
  // title/artist/artwork/station-logo UI updates, and avoiding stream 2
  // prevents Kodi from starting the Audio ID3 tag processor thread.
#endif
}

//---------------------------------------------------------------------------
// hdstream::length
//
// Gets the length of the stream; or -1 if stream is real-time
//
// Arguments:
//
//	NONE

long long hdstream::length(void) const
{
  return -1;
}

//---------------------------------------------------------------------------
// hdstream::muxname
//
// Gets the mux name associated with the stream
//
// Arguments:
//
//	NONE

std::string hdstream::muxname(void) const
{
  return m_muxname;
}

//---------------------------------------------------------------------------
// hdstream::nrsc5_callback (private, static)
//
// NRSC5 library event callback function
//
// Arguments:
//
//	event	- NRSC5 event being raised
//	arg		- Implementation-specific context pointer

void hdstream::nrsc5_callback(nrsc5_event_t const* event, void* arg)
{
  assert(arg != nullptr);
  reinterpret_cast<hdstream*>(arg)->nrsc5_callback(event);
}

//---------------------------------------------------------------------------
// hdstream::nrsc5_callback (private)
//
// NRSC5 library event callback function
//
// Arguments:
//
//	event	- NRSC5 event being raised

void hdstream::nrsc5_callback(nrsc5_event_t const* event)
{
  // TEMP_ID3_BER_MER_INJECTION
  //
  // Keep the latest live HD Radio BER/MER so the next ID3 metadata packet
  // can carry signal-quality text into Kodi's music/info UI.
  static bool pvr_rtlradio_live_ber_valid = false;
  static bool pvr_rtlradio_live_mer_valid = false;
  static float pvr_rtlradio_live_cber = 0.0f;
  static float pvr_rtlradio_live_mer_lower = 0.0f;
  static float pvr_rtlradio_live_mer_upper = 0.0f;

  if (event->event == NRSC5_EVENT_BER)
  {
    pvr_rtlradio_live_ber_valid = true;
    pvr_rtlradio_live_cber = event->ber.cber;
  }

  if (event->event == NRSC5_EVENT_MER)
  {
    pvr_rtlradio_live_mer_valid = true;
    pvr_rtlradio_live_mer_lower = event->mer.lower;
    pvr_rtlradio_live_mer_upper = event->mer.upper;
  }
  bool queued = false; // Flag if an item was queued

  std::unique_lock<std::mutex> lock(m_queuelock);
  // NRSC5_EVENT_AUDIO
  //
  // A digital stream audio packet has been generated
  if (event->event == NRSC5_EVENT_AUDIO)
  {

    // Filter out anything other than program zero for now
    if (event->audio.program == (m_subchannel - 1))
    {

      // Allocate an initialize a heap buffer and copy the audio data into it
      size_t audiosize = event->audio.count * sizeof(int16_t);
      std::unique_ptr<uint8_t[]> audiodata(new uint8_t[audiosize]);

      // Apply the specified PCM output gain while copying the audio data into the packet buffer
      int16_t* pcmdata = reinterpret_cast<int16_t*>(audiodata.get());
      for (size_t index = 0; index < event->audio.count; index++)
        pcmdata[index] = static_cast<int16_t>(event->audio.data[index] * m_pcmgain);

      // Generate and queue the audio packet
      std::unique_ptr<demux_packet_t> packet = std::make_unique<demux_packet_t>();
      packet->streamid = STREAM_ID_AUDIO;
      packet->size = static_cast<int>(audiosize);
      packet->duration = (event->audio.count / 2.0 / 44100.0) * STREAM_TIME_BASE;
      packet->dts = packet->pts = m_dts;
      packet->data = std::move(audiodata);

      m_dts += packet->duration;

      m_queue.emplace(std::move(packet));
      queued = true;
    }
  }

  // NRSC5_EVENT_BER
  //
  // Reporting the current bit error rate
  else if (event->event == NRSC5_EVENT_BER)
    m_ber.store(event->ber.cber);

  // NRSC5_EVENT_MER
  //
  // Reporting the current modulatation error ratio
  else if (event->event == NRSC5_EVENT_MER)
  {

    // Store the higher of the two values instead of the mean, some HD radio stations
    // are allowed to transmit one sideband at a higher power than the other
    m_mer.store(std::max(event->mer.lower, event->mer.upper));
  }


  // NRSC5_EVENT_SIS
  //
  // Legacy nrsc5 API station identity. event->sis.name is the stable station
  // name, e.g. KQMV-FM. Use this for station-logo cache keys.
  else if (event->event == NRSC5_EVENT_SIS)
  {
    char const* name = event->sis.name ? event->sis.name : "";
    char const* slogan = event->sis.slogan ? event->sis.slogan : "";

    pvr_rtlradio_hd_station_identity::set_sis(name, slogan);

    kodi::Log(ADDON_LOG_DEBUG,
              "HDRADIO_STATION sis name='%s' slogan='%s' key='%s'",
              name,
              slogan,
              pvr_rtlradio_hd_station_identity::slug(name).c_str());
  }

  // NRSC5_EVENT_SIG
  //
  // SIG may identify which data ports carry station-logo LOTs. Track those
  // ports so a LOT with JPEG/PNG bytes can still be cached as station logo.
  else if (event->event == NRSC5_EVENT_SIG)
  {
    for (nrsc5_sig_service_t* service = event->sig.services;
         service;
         service = service->next)
    {
      for (nrsc5_sig_component_t* component = service->components;
           component;
           component = component->next)
      {
        if (component->type == NRSC5_SIG_COMPONENT_DATA &&
            component->data.mime == NRSC5_MIME_STATION_LOGO)
        {
          unsigned int const logo_program =
              (service->number > 0) ? static_cast<unsigned int>(service->number - 1) : 0;

          pvr_rtlradio_hd_station_identity::add_station_logo_port(
              component->data.port,
              logo_program);

          kodi::Log(ADDON_LOG_DEBUG,
                    "HDRADIO_STATION logo_port=%u service='%s' service_number=%u program=%u",
                    component->data.port,
                    service->name ? service->name : "",
                    static_cast<unsigned int>(service->number),
                    logo_program);
        }
      }
    }
  }

#ifdef KODI_HAS_ID3
  // NRSC5_EVENT_ID3
  //
  // LOW_CPU_SIDECAR_ONLY_V9
  //
  // Use nrsc5's decoded ID3 fields only to update nowplaying.txt and the
  // artist/title artwork cache. Do not build synthetic ID3/APIC packets and
  // do not queue an ID3 demux packet for Kodi.
  else if (event->event == NRSC5_EVENT_ID3)
  {
    if (event->id3.program == (m_subchannel - 1))
    {
      std::string freqtext = muxname();

      char signaltext[200] = {};

      if (pvr_rtlradio_live_ber_valid && pvr_rtlradio_live_mer_valid)
      {
        float mer_avg =
            (pvr_rtlradio_live_mer_lower + pvr_rtlradio_live_mer_upper) / 2.0f;

        if (!freqtext.empty())
        {
          snprintf(signaltext,
                   sizeof(signaltext),
                   "%s | BER %.6f | MER %.2f dB",
                   freqtext.c_str(),
                   pvr_rtlradio_live_cber,
                   mer_avg);
        }
        else
        {
          snprintf(signaltext,
                   sizeof(signaltext),
                   "BER %.6f | MER %.2f dB",
                   pvr_rtlradio_live_cber,
                   mer_avg);
        }
      }
      else if (pvr_rtlradio_live_ber_valid)
      {
        if (!freqtext.empty())
        {
          snprintf(signaltext,
                   sizeof(signaltext),
                   "%s | BER %.6f",
                   freqtext.c_str(),
                   pvr_rtlradio_live_cber);
        }
        else
        {
          snprintf(signaltext,
                   sizeof(signaltext),
                   "BER %.6f",
                   pvr_rtlradio_live_cber);
        }
      }
      else if (pvr_rtlradio_live_mer_valid)
      {
        float mer_avg =
            (pvr_rtlradio_live_mer_lower + pvr_rtlradio_live_mer_upper) / 2.0f;

        if (!freqtext.empty())
        {
          snprintf(signaltext,
                   sizeof(signaltext),
                   "%s | MER %.2f dB",
                   freqtext.c_str(),
                   mer_avg);
        }
        else
        {
          snprintf(signaltext,
                   sizeof(signaltext),
                   "MER %.2f dB",
                   mer_avg);
        }
      }
      else if (!freqtext.empty())
      {
        snprintf(signaltext, sizeof(signaltext), "%s", freqtext.c_str());
      }

      auto clean_text = [](std::string const& input, size_t maxlen)
      {
        std::string out;
        bool last_space = false;

        for (unsigned char ch : input)
        {
          char c = 0;

          if ((ch >= 32) && (ch <= 126))
            c = static_cast<char>(ch);
          else if ((ch == '\t') || (ch == '\r') || (ch == '\n'))
            c = ' ';
          else
            c = '?';

          if (c == ' ')
          {
            if (!out.empty() && !last_space)
            {
              out.push_back(' ');
              last_space = true;
            }
          }
          else
          {
            out.push_back(c);
            last_space = false;
          }

          if (out.size() >= maxlen)
            break;
        }

        while (!out.empty() && out.back() == ' ')
          out.pop_back();

        return out;
      };

      auto one_line = [](std::string value)
      {
        for (char& c : value)
        {
          if (c == '\r' || c == '\n' || c == '\0')
            c = ' ';
        }
        return value;
      };

      auto slug = [](std::string value)
      {
        std::string out;
        bool last_dash = true;

        for (unsigned char ch : value)
        {
          if ((ch >= 'A') && (ch <= 'Z'))
            ch = static_cast<unsigned char>(ch - 'A' + 'a');

          if (((ch >= 'a') && (ch <= 'z')) ||
              ((ch >= '0') && (ch <= '9')))
          {
            out.push_back(static_cast<char>(ch));
            last_dash = false;
          }
          else if (!last_dash)
          {
            out.push_back('-');
            last_dash = true;
          }
        }

        while (!out.empty() && out.back() == '-')
          out.pop_back();

        return out;
      };

      std::string title =
          event->id3.title ? clean_text(std::string(event->id3.title), 100) : std::string();
      std::string artist =
          event->id3.artist ? clean_text(std::string(event->id3.artist), 100) : std::string();
      std::string album =
          event->id3.album ? clean_text(std::string(event->id3.album), 100) : std::string();

      if (title.empty())
        title = freqtext.empty() ? "HD Radio" : freqtext;

      if (signaltext[0] != '\0')
      {
        if (!album.empty())
        {
          album += " | ";
          album += signaltext;
        }
        else
        {
          album = signaltext;
        }
      }
      else if (album.empty())
      {
        album = "HD Radio";
      }

      std::string const safe_title = one_line(title);
      std::string const safe_artist = one_line(artist);
      std::string const safe_album = one_line(album);

      std::string const title_key = slug(safe_title);
      std::string const artist_key = slug(safe_artist);

      std::string cache_key;
      if (!title_key.empty() && !artist_key.empty())
      {
        cache_key = artist_key;
        cache_key += "---";
        cache_key += title_key;
      }

      // Cache exact song artwork by artist/title when the referenced LOT is
      // already available. If the LOT arrives later, a later ID3 refresh will
      // fill the cache.
      if (!cache_key.empty() && event->id3.xhdr.lot > 0)
      {
        auto selected = m_lots.find(event->id3.xhdr.lot);

        if (selected != m_lots.end() &&
            selected->second.data &&
            selected->second.size > 0 &&
            ((selected->second.mime == NRSC5_MIME_JPEG) ||
             (selected->second.mime == NRSC5_MIME_PNG)))
        {
          char const* data_dir =
              "/storage/.kodi/userdata/addon_data/pvr.rtlradio";
          char const* artcache_dir =
              "/storage/.kodi/userdata/addon_data/pvr.rtlradio/artcache";

          ::mkdir(data_dir, 0755);
          ::mkdir(artcache_dir, 0755);

          char const* ext =
              (selected->second.mime == NRSC5_MIME_PNG) ? "png" : "jpg";

          std::string image_path = std::string(artcache_dir);
          image_path += "/";
          image_path += cache_key;
          image_path += ".";
          image_path += ext;

          bool wrote_image = false;

          struct stat st = {};
          if (::stat(image_path.c_str(), &st) == 0 &&
              st.st_size == static_cast<off_t>(selected->second.size))
          {
            wrote_image = true;
          }
          else
          {
            FILE* imgfp = std::fopen(image_path.c_str(), "wb");
            if (imgfp)
            {
              wrote_image =
                  (std::fwrite(selected->second.data.get(),
                               1,
                               selected->second.size,
                               imgfp) == selected->second.size);
              std::fclose(imgfp);
            }
          }

          kodi::Log(ADDON_LOG_DEBUG,
                    "HDRADIO_SIDECAR cache_art lot=%d image_ok=%d cache_key='%s' image='%s' title='%s' artist='%s'",
                    selected->first,
                    wrote_image ? 1 : 0,
                    cache_key.c_str(),
                    image_path.c_str(),
                    safe_title.c_str(),
                    safe_artist.c_str());
        }
      }

      std::string station_name;
      std::string station_slogan;
      std::string station_key;
      pvr_rtlradio_hd_station_identity::get(station_name,
                                            station_slogan,
                                            station_key);

      std::string const station_base_key = station_key;
      std::string const station_program_key =
          pvr_rtlradio_hd_station_identity::program_key(
              station_base_key,
              static_cast<unsigned int>(event->id3.program));

      int const event_lot = event->id3.xhdr.lot;
      int const state_lot = (event_lot > 0) ? event_lot : -1;

      char const* data_dir =
          "/storage/.kodi/userdata/addon_data/pvr.rtlradio";
      char const* state_path =
          "/storage/.kodi/userdata/addon_data/pvr.rtlradio/nowplaying.txt";
      char const* state_tmp =
          "/storage/.kodi/userdata/addon_data/pvr.rtlradio/nowplaying.txt.tmp";

      ::mkdir(data_dir, 0755);

      FILE* statefp = std::fopen(state_tmp, "wb");
      if (statefp)
      {
        std::fprintf(statefp, "version=1\n");
        std::fprintf(statefp, "lot=%d\n", state_lot);
        std::fprintf(statefp, "blocked_lot=-1\n");
        std::fprintf(statefp, "image=\n");
        std::fprintf(statefp, "artcache_key=%s\n", cache_key.c_str());
        std::fprintf(statefp, "station_key=%s\n", station_program_key.c_str());
        std::fprintf(statefp, "station_base_key=%s\n", station_base_key.c_str());
        std::fprintf(statefp, "station_program=%u\n", static_cast<unsigned int>(event->id3.program));
        std::fprintf(statefp, "station_name=%s\n", station_name.c_str());
        std::fprintf(statefp, "station_slogan=%s\n", station_slogan.c_str());
        std::fprintf(statefp, "title=%s\n", safe_title.c_str());
        std::fprintf(statefp, "artist=%s\n", safe_artist.c_str());
        std::fprintf(statefp, "album=%s\n", safe_album.c_str());
        std::fclose(statefp);
        std::rename(state_tmp, state_path);
      }

      kodi::Log(ADDON_LOG_DEBUG,
                "HDRADIO_SIDECAR low_cpu text lot=%d event_lot=%d cache_key='%s' station_key='%s' station_base_key='%s' station_program=%u station_name='%s' title='%s' artist='%s'",
                state_lot,
                event_lot,
                cache_key.c_str(),
                station_program_key.c_str(),
                station_base_key.c_str(),
                static_cast<unsigned int>(event->id3.program),
                station_name.c_str(),
                safe_title.c_str(),
                safe_artist.c_str());
    }
  }

  // NRSC5_EVENT_LOT
  //
  // Reporting LOT item data
  else if (event->event == NRSC5_EVENT_LOT)
  {
    // Cache station logo LOTs separately from song artwork. Do this before
    // existing song-art m_lots handling. The normal song art path remains
    // artist/title keyed in artcache/.
    pvr_rtlradio_hd_station_identity::cache_station_logo_lot(
        event->lot.port,
        event->lot.lot,
        event->lot.mime,
        event->lot.data,
        event->lot.size);

    // SAFE_GUARDED_HD_LOT_ARTWORK
    //
    // Re-enable LOT artwork, but only cache bounded JPEG/PNG image payloads.
    // The raw station image is never forwarded directly; it is only used later
    // to build an APIC frame inside our synthetic safe ID3 tag.
    size_t const max_lot_image_size = 1024 * 1024;
    size_t const max_cached_lot_items = 16;

    if (((event->lot.mime == NRSC5_MIME_JPEG) ||
         (event->lot.mime == NRSC5_MIME_PNG)) &&
        event->lot.data &&
        event->lot.size > 0 &&
        event->lot.size <= max_lot_image_size)
    {
      try
      {
        lot_item_t item = {};
        item.mime = event->lot.mime;
        item.size = event->lot.size;
        item.data = std::unique_ptr<uint8_t[]>(new uint8_t[event->lot.size]);

        memcpy(item.data.get(), event->lot.data, event->lot.size);

        if (m_lots.size() >= max_cached_lot_items)
        {
          // HDRADIO_ART WORKING FIX:
          // Do not clear cached LOT artwork during metadata handling.
          // Keep the LOT insertion below unconditional; otherwise artwork cache stays empty.
          // TODO: add safe bounded pruning later without removing active artwork.
        }

        m_lots[event->lot.lot] = std::move(item);

        // BOUNDED_LOT_CACHE_PRUNING
        //
        // Keep the guarded artwork cache bounded.  Since LOT numbers generally
        // increase over time, prune the lowest/oldest LOT first, but never prune
        // the LOT that was just inserted.
        while (m_lots.size() > max_cached_lot_items)
        {
          auto prune = m_lots.begin();

          if (prune != m_lots.end() && prune->first == event->lot.lot)
          {
            ++prune;
            if (prune == m_lots.end())
              break;
          }

          int const pruned_lot = prune->first;
          size_t const pruned_size = prune->second.size;

          m_lots.erase(prune);

          kodi::Log(ADDON_LOG_DEBUG,
                    "HDRADIO_ART LOT pruned lot=%d size=%zu cached=%zu",
                    pruned_lot,
                    pruned_size,
                    m_lots.size());
        }

        // TEMP_ARTWORK_DEBUG_LOGS
        kodi::Log(ADDON_LOG_DEBUG,
                  "HDRADIO_ART LOT cached lot=%d mime=%d size=%zu cached=%zu",
                  event->lot.lot,
                  event->lot.mime,
                  event->lot.size,
                  m_lots.size());
      }
      catch (...)
      {
        // HDRADIO_ART TEST:
        // Do not erase this LOT from the guarded artwork cache yet.
        // We are testing whether persistent cached APIC fixes Kodi artwork display.
        // m_lots.erase(event->lot.lot);
      }
    }
  }
#endif

  if (queued)
  {

    // If the queue size has exceeded the maximum, the packets aren't
    // being processed quickly enough by the demux read function
    if (m_queue.size() > MAX_PACKET_QUEUE)
    {

      m_queue = demux_queue_t(); // Replace the queue<>

      // Push a DEMUX_SPECIALID_STREAMCHANGE packet into the new queue
      std::unique_ptr<demux_packet_t> packet = std::make_unique<demux_packet_t>();
      packet->streamid = DEMUX_SPECIALID_STREAMCHANGE;
      m_queue.emplace(std::move(packet));

      // Reset the decode time stamp
      m_dts = STREAM_TIME_BASE;
    }

    m_cv.notify_all(); // Notify queue was updated
  }
}

//---------------------------------------------------------------------------
// hdstream::position
//
// Gets the current position of the stream
//
// Arguments:
//
//	NONE

long long hdstream::position(void) const
{
  return -1;
}

//---------------------------------------------------------------------------
// hdstream::read
//
// Reads data from the live stream
//
// Arguments:
//
//	buffer		- Buffer to receive the live stream data
//	count		- Size of the destination buffer in bytes

size_t hdstream::read(uint8_t* /*buffer*/, size_t /*count*/)
{
  return 0;
}

//---------------------------------------------------------------------------
// hdstream::realtime
//
// Gets a flag indicating if the stream is real-time
//
// Arguments:
//
//	NONE

bool hdstream::realtime(void) const
{
  return true;
}

//---------------------------------------------------------------------------
// hdstream::seek
//
// Sets the stream pointer to a specific position
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

long long hdstream::seek(long long /*position*/, int /*whence*/)
{
  return -1;
}

//---------------------------------------------------------------------------
// hdstream::servicename
//
// Gets the service name associated with the stream
//
// Arguments:
//
//	NONE

std::string hdstream::servicename(void) const
{
  return std::string("Hybrid Digital (HD) Radio");
}

//---------------------------------------------------------------------------
// hdstream::signalquality
//
// Gets the signal quality as percentages
//
// Arguments:
//
//	NONE

void hdstream::signalquality(int& quality, int& snr) const
{
  // For signal quality, use the NRSC5 Bit Error Rate (BER). A BER of zero
  // implies ideal signal quality. I have no idea what the BER tolerance
  // is for decoding HD Radio, but from observation a BER with a value
  // higher than 0.1 is effectively undecodable so let's call that zero
  float ber = m_ber.load();
  ber = std::min(std::max(ber, 0.0f), 0.1f) * 100.0f;
  quality = static_cast<int>(((ber - 100.0f) * 100.0f) / -100.0f);

  // For signal-to-noise ratio, use the NRSC5 Modulation Error Ratio (MER).
  // A MER of 14 is apparently the ideal for HD Radio, so for now use
  // a linear scale from (0...13) to define the SNR percentage
  float mer = m_mer.load();
  mer = std::max(std::min(13.0f, mer), 0.0f);
  snr = static_cast<int>((mer * 100.0f) / 13.0f);
}

//---------------------------------------------------------------------------
// hdstream::worker (private)
//
// Worker thread procedure used to transfer data from the device
//
// Arguments:
//
//	started		- Condition variable to set when thread has started

void hdstream::worker(scalar_condition<bool>& started)
{
  // HDRADIO_SIDECAR_CLEAR_ON_WORKER_START
  //
  // Clear old sidecar artwork at the start of a stream/tune so station changes
  // do not keep showing artwork from the previous station.
  {
    pvr_rtlradio_hd_station_identity::clear();

    char const* data_dir =
        "/storage/.kodi/userdata/addon_data/pvr.rtlradio";
    char const* state_path =
        "/storage/.kodi/userdata/addon_data/pvr.rtlradio/nowplaying.txt";
    char const* state_tmp =
        "/storage/.kodi/userdata/addon_data/pvr.rtlradio/nowplaying.txt.tmp";

    ::mkdir(data_dir, 0755);

    FILE* statefp = std::fopen(state_tmp, "wb");
    if (statefp)
    {
      std::fprintf(statefp, "version=1\n");
      std::fprintf(statefp, "lot=-1\n");
      std::fprintf(statefp, "blocked_lot=-1\n");
      std::fprintf(statefp, "image=\n");
      std::fprintf(statefp, "artcache_key=\n");
      std::fprintf(statefp, "station_key=\n");
      std::fprintf(statefp, "station_base_key=\n");
      std::fprintf(statefp, "station_program=\n");
      std::fprintf(statefp, "station_name=\n");
      std::fprintf(statefp, "station_slogan=\n");
      std::fprintf(statefp, "title=\n");
      std::fprintf(statefp, "artist=\n");
      std::fprintf(statefp, "album=\n");
      std::fclose(statefp);
      std::rename(state_tmp, state_path);
    }

    kodi::Log(ADDON_LOG_DEBUG, "HDRADIO_SIDECAR cleared on stream start");
  }


  assert(m_device);
  assert(m_nrsc5);

  // read_callback_func (local)
  //
  // Asynchronous read callback function for the RTL-SDR device
  auto read_callback_func = [&](uint8_t const* buffer, size_t count) -> void
  {
    // Pipe the samples into NRSC5, it will invoke the necessary callback(s)
    nrsc5_pipe_samples_cu8(m_nrsc5, const_cast<uint8_t*>(buffer), static_cast<unsigned int>(count));
  };

  // Begin streaming from the device and inform the caller that the thread is running
  m_device->begin_stream();
  started = true;

  // Continuously read data from the device until cancel_async() has been called
  // 256 KiB = larger low-power buffering block
  try
  {
    m_device->read_async(read_callback_func, 256 KiB);
  }
  catch (...)
  {
    m_worker_exception = std::current_exception();
  }

  m_stopped.store(true); // Thread is stopped
  m_cv.notify_all(); // Unblock any waiters
}

//---------------------------------------------------------------------------

#pragma warning(pop)
