#include <mutex>
#include <limits>
#include <functional>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <chrono>
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

#include "addon.h"
#include "dabstream.h"
#include "dbtypes.h"
#include "filedevice.h"
#include "fmstream.h"
#include "hdstream.h"
#include "hdmuxscanner.h"
#include "tcpdevice.h"
#ifdef USB_DEVICE_SUPPORT
#include "usbdevice.h"
#endif
#include "wxstream.h"
#include "exception_control/sqlite_exception.h"
#include "exception_control/string_exception.h"
#include "gui/channeladd.h"
#include "gui/channelsettings.h"
#include "signalmeter.h"
#include "utils/value_size_defines.h"

#include <assert.h>
#include <kodi/Filesystem.h>
#include <kodi/General.h>
#include <kodi/gui/dialogs/FileBrowser.h>
#include <kodi/gui/dialogs/OK.h>
#include <kodi/gui/dialogs/Progress.h>
#include <kodi/gui/dialogs/ExtendedProgress.h>
#include <kodi/gui/dialogs/Select.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <utility>
#include <vector>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <memory>

#ifdef WIN32
#include <windows.h>
#ifdef TARGET_WINDOWS_STORE
#include <ws2tcpip.h>
#endif

#ifdef CreateDirectory
#undef CreateDirectory
#endif // CreateDirectory

#elif defined(__ANDROID__)
#include <android/log.h>
#endif
#pragma warning(push, 4)

// Addon Entry Points
//
ADDONCREATOR(addon)

//---------------------------------------------------------------------------
// addon Instance Constructor
//
// Arguments:
//
//	NONE

addon::addon() : m_settings{}
{
}

//---------------------------------------------------------------------------
// addon Destructor

addon::~addon()
{
  // There is no corresponding "Destroy" method in CAddonBase, only the class
  // destructor will be invoked; to keep the implementation pieces near each
  // other, perform the tear-down in a helper function
  Destroy();
}

//---------------------------------------------------------------------------
// addon::channeladd_dab (private)
//
// Performs the channel add operation for DAB
//
// Arguments:
//
//	settings		- Current addon settings
//	channelprops	- Channel properties to be populated on success

bool addon::channeladd_dab(struct settings const& settings, struct channelprops& channelprops) const
{
  std::vector<std::string> channelnames; // Channel names
  std::vector<std::string> channellabels; // Channel labels
  std::vector<uint32_t> channelfrequencies; // Channel frequencies
  std::vector<struct subchannelprops> subchannelprops; // Existing subchannels


  // Pull a database handle out of the connection pool
  connectionpool::handle dbhandle(m_connpool);

  // Enumerate the named channels available for the specified modulation (DAB)
  enumerate_namedchannels(dbhandle, modulation::dab,
                          [&](struct namedchannel const& item) -> void
                          {
                            if ((item.frequency > 0) && (item.name != nullptr))
                            {

                              // Append the frequency of the channel in megahertz (xxx.xxx format) to the channel label
                              char label[256]{};
                              unsigned int mhz = item.frequency / 1000000;
                              unsigned int khz = (item.frequency % 1000000) / 1000;
                              snprintf(label, std::extent<decltype(label)>::value, "%s (%u.%u MHz)",
                                       item.name, mhz, khz);

                              channelnames.emplace_back(item.name);
                              channellabels.emplace_back(label);
                              channelfrequencies.emplace_back(item.frequency);
                            }
                          });

  assert((channelnames.size() == channellabels.size()) &&
         (channelnames.size() == channelfrequencies.size()));
  if (channelnames.size() == 0)
    throw string_exception("No DAB ensembles were enumerated from the database");

  // The user has to select what DAB ensemble will be added from the hard-coded options in the database
  int selected =
      kodi::gui::dialogs::Select::Show(kodi::addon::GetLocalizedString(30418), channellabels);
  if (selected < 0)
    return false;

  // Initialize enough properties for the settings dialog to work
  channelprops.frequency = channelfrequencies[selected];
  channelprops.modulation = modulation::dab;
  channelprops.name =
      kodi::addon::GetLocalizedString(30322).append(" ").append(channelnames[selected]);

  // If the channel already exists in the database, get the previously set properties and subchannels
  bool exists = channel_exists(dbhandle, channelprops);
  if (exists)
    get_channel_properties(dbhandle, channelprops.frequency, channelprops.modulation, channelprops,
                           subchannelprops);

  // Set up the tuner device properties
  struct tunerprops tunerprops = {};
  tunerprops.freqcorrection = settings.device_frequency_correction;

  // Create and initialize a channel settings dialog instance to allow the user to fine-tune the channel
  std::unique_ptr<channelsettings> settingsdialog =
      channelsettings::create(create_device(settings), tunerprops, channelprops, true);
  settingsdialog->DoModal();

  if (settingsdialog->get_dialog_result())
  {

    std::vector<struct subchannelprops> subchannels; // Subchannel information

    // Retrieve the updated channel and subchannel properties from the dialog box
    settingsdialog->get_channel_properties(channelprops);
    settingsdialog->get_subchannel_properties(subchannels);

    // Prompt the user to select what subchannels they want to add for this channel, assuming all of them
    if (subchannels.size() > 0)
    {

      std::vector<kodi::gui::dialogs::SSelectionEntry> entries;
      for (auto const& subchannel : subchannels)
      {

        // GCC 4.9 doesn't allow a push_back() on SSelectionEntry with a braced initializer list, do this the verbose way
        kodi::gui::dialogs::SSelectionEntry entry = {};
        entry.id = std::to_string(subchannel.number);
        entry.name = std::string(entry.id).append(" ").append(subchannel.name);

        // Pre-select the subchannel if there were no prior subchannels defined or if it matches a previously defined subchannel
        entry.selected = ((subchannelprops.size() == 0) ||
                          (std::find_if(subchannelprops.begin(), subchannelprops.end(),
                                        [&](auto const& val) -> bool {
                                          return val.number == subchannel.number;
                                        }) != subchannelprops.end()));

        entries.emplace_back(std::move(entry));
      }

      // TODO: use the existing dialog box for now; this needs a custom replacement. It seems to always toggle
      // the first item to selected == false and has no means to determine a 'cancel'
      if (!kodi::gui::dialogs::Select::ShowMultiSelect(kodi::addon::GetLocalizedString(30320),
                                                       entries))
        return false;
      entries[0].selected = true;

      // Remove any subchannels that were de-selected by the user from the vector<> of subchannels
      for (auto const& entry : entries)
      {

        if (entry.selected == false)
        {

          auto found = std::find_if(subchannels.begin(), subchannels.end(),
                                    [&](auto const& val) -> bool
                                    { return std::to_string(val.number) == entry.id; });
          if (found != subchannels.end())
            subchannels.erase(found);
        }
      }
    }

    // Add or update the channel/subchannels in the database
    if (!exists)
      add_channel(dbhandle, channelprops, subchannels);
    else
      update_channel(dbhandle, channelprops, subchannels);

    return true;
  }

  return false;
}

//---------------------------------------------------------------------------
// addon::channeladd_am (private)
//
// Performs the channel add operation for analog AM Radio

bool addon::channeladd_am(struct settings const& settings, struct channelprops& channelprops) const
{
  std::vector<std::string> labels;
  for (uint32_t frequency = amradio::FIRST_FREQUENCY;
       frequency <= amradio::LAST_FREQUENCY;
       frequency += amradio::STEP_FREQUENCY)
    labels.emplace_back(std::to_string(frequency / 1000) + " kHz");

  int const selected = kodi::gui::dialogs::Select::Show("Select AM Radio frequency", labels);
  if (selected < 0)
    return false;

  channelprops = {};
  channelprops.frequency = amradio::FIRST_FREQUENCY +
                           static_cast<uint32_t>(selected) * amradio::STEP_FREQUENCY;
  channelprops.modulation = modulation::am;
  channelprops.name = "New AM channel";
  channelprops.autogain = false;

  connectionpool::handle dbhandle(m_connpool);
  bool const exists = channel_exists(dbhandle, channelprops);
  if (exists)
    get_channel_properties(dbhandle, channelprops.frequency, channelprops.modulation, channelprops);

  struct tunerprops tunerprops = {};
  tunerprops.freqcorrection = settings.device_frequency_correction;

  std::unique_ptr<channelsettings> settingsdialog =
      channelsettings::create(create_device(settings), tunerprops, channelprops, true);
  settingsdialog->DoModal();
  if (!settingsdialog->get_dialog_result())
    return true;

  settingsdialog->get_channel_properties(channelprops);
  if (!exists)
    add_channel(dbhandle, channelprops);
  else
    update_channel(dbhandle, channelprops);

  return true;
}

//---------------------------------------------------------------------------
// addon::channeladd_fm (private)
//
// Performs the channel add operation for FM Radio
//
// Arguments:
//
//	settings		- Current addon settings
//	channelprops	- Channel properties to be populated on success

bool addon::channeladd_fm(struct settings const& settings, struct channelprops& channelprops) const
{
  // Create and initialize the frequency input dialog box
  std::unique_ptr<channeladd> adddialog = channeladd::create(modulation::fm);
  adddialog->DoModal();

  // If the dialog was successful add the channel to the database
  if (adddialog->get_dialog_result())
  {

    // Retrieve the new channel properties from the dialog box
    adddialog->get_channel_properties(channelprops);
    assert(channelprops.modulation == modulation::fm);

    // Pull a database handle out of the connection pool
    connectionpool::handle dbhandle(m_connpool);

    // If the channel already exists in the database, get the previously set properties
    bool exists = channel_exists(dbhandle, channelprops);
    if (exists)
      get_channel_properties(dbhandle, channelprops.frequency, channelprops.modulation,
                             channelprops);

    // Set up the tuner device properties
    struct tunerprops tunerprops = {};
    tunerprops.freqcorrection = settings.device_frequency_correction;

    // Create and initialize the dialog box against a new signal meter instance
    std::unique_ptr<channelsettings> settingsdialog =
        channelsettings::create(create_device(settings), tunerprops, channelprops, true);
    settingsdialog->DoModal();

    if (settingsdialog->get_dialog_result())
    {

      // Retrieve the updated channel properties from the dialog box
      settingsdialog->get_channel_properties(channelprops);

      // Add or update the channel in the database
      if (!exists)
        add_channel(dbhandle, channelprops);
      else
        update_channel(dbhandle, channelprops);
    }

    return true;
  }

  return false;
}

//---------------------------------------------------------------------------
// addon::channeladd_hd (private)
//
// Performs the channel add operation for HD Radio
//
// Arguments:
//
//	settings		- Current addon settings
//	channelprops	- Channel properties to be populated on success

bool addon::channeladd_hd(struct settings const& settings, struct channelprops& channelprops) const
{
  std::vector<struct subchannelprops> subchannelprops; // Existing subchannels

  bool am_band = false;
  if (is_region_northamerica(settings))
  {
    std::vector<std::string> bands{"AM", "FM"};
    int const selected_band = kodi::gui::dialogs::Select::Show("Select HD Radio band", bands);
    if (selected_band < 0)
      return false;
    am_band = (selected_band == 0);
  }

  if (am_band)
  {
    std::vector<std::string> labels;
    std::vector<uint32_t> frequencies;
    for (uint32_t frequency = hdradio::AM_FIRST_FREQUENCY; frequency <= hdradio::AM_LAST_FREQUENCY;
         frequency += hdradio::AM_STEP_FREQUENCY)
    {
      labels.emplace_back(std::to_string(frequency / 1000) + " kHz");
      frequencies.emplace_back(frequency);
    }

    int const selected = kodi::gui::dialogs::Select::Show("Select AM HD Radio frequency", labels);
    if (selected < 0)
      return false;

    channelprops.frequency = frequencies[selected];
    channelprops.modulation = modulation::hd;
    channelprops.name = kodi::addon::GetLocalizedString(19204, "New channel");
    channelprops.autogain = false;
  }
  else
  {
    // Use the existing numeric dialog for FM HD Radio.
    std::unique_ptr<channeladd> adddialog = channeladd::create(modulation::hd);
    adddialog->DoModal();
    if (!adddialog->get_dialog_result())
      return false;
    adddialog->get_channel_properties(channelprops);
  }

  {
    assert(channelprops.modulation == modulation::hd);

    // For HD Radio, change "New channel" to "New multiplex"
    channelprops.name = kodi::addon::GetLocalizedString(30321);

    // Pull a database handle out of the connection pool
    connectionpool::handle dbhandle(m_connpool);

    // If the channel already exists in the database, get the previously set properties
    bool exists = channel_exists(dbhandle, channelprops);
    if (exists)
      get_channel_properties(dbhandle, channelprops.frequency, channelprops.modulation,
                             channelprops, subchannelprops);

    // Set up the tuner device properties
    struct tunerprops tunerprops = {};
    tunerprops.freqcorrection = settings.device_frequency_correction;

    // Create and initialize a channel settings dialog instance to allow the user to fine-tune the channel
    std::unique_ptr<channelsettings> settingsdialog =
        channelsettings::create(create_device(settings), tunerprops, channelprops, true);
    settingsdialog->DoModal();

    if (settingsdialog->get_dialog_result())
    {

      std::vector<struct subchannelprops> subchannels; // Subchannel information

      // Retrieve the updated channel and subchannel properties from the dialog box
      settingsdialog->get_channel_properties(channelprops);
      settingsdialog->get_subchannel_properties(subchannels);

      // Prompt the user to select what subchannels they want to add for this channel, assuming all of them.
      // For HD Radio, if there is only one audio stream subchannel, bypass the selection process
      if (subchannels.size() > 1)
      {

        std::vector<kodi::gui::dialogs::SSelectionEntry> entries;
        for (auto const& subchannel : subchannels)
        {

          // GCC 4.9 doesn't allow a push_back() on SSelectionEntry with a braced initializer list, do this the verbose way
          kodi::gui::dialogs::SSelectionEntry entry = {};
          entry.id = std::to_string(subchannel.number);
          entry.name = subchannel.name;

          // Pre-select the subchannel if there were no prior subchannels defined or if it matches a previously defined subchannel
          entry.selected = ((subchannelprops.size() == 0) ||
                            (std::find_if(subchannelprops.begin(), subchannelprops.end(),
                                          [&](auto const& val) -> bool {
                                            return val.number == subchannel.number;
                                          }) != subchannelprops.end()));

          entries.emplace_back(std::move(entry));
        }

        // TODO: use the existing dialog box for now; this needs a custom replacement. It seems to always toggle
        // the first item to selected == false and has no means to determine a 'cancel'
        if (!kodi::gui::dialogs::Select::ShowMultiSelect(kodi::addon::GetLocalizedString(30319),
                                                         entries))
          return false;
        entries[0].selected = true;

        // Remove any subchannels that were de-selected by the user from the vector<> of subchannels
        for (auto const& entry : entries)
        {

          if (entry.selected == false)
          {

            auto found = std::find_if(subchannels.begin(), subchannels.end(),
                                      [&](auto const& val) -> bool
                                      { return std::to_string(val.number) == entry.id; });
            if (found != subchannels.end())
              subchannels.erase(found);
          }
        }
      }

      // Add or update the channel/subchannels in the database
      if (!exists)
        add_channel(dbhandle, channelprops, subchannels);
      else
        update_channel(dbhandle, channelprops, subchannels);

      return true;
    }
  }

  return false;
}

//---------------------------------------------------------------------------
// addon::channeladd_wx (private)
//
// Performs the channel add operation for Weather Radio
//
// Arguments:
//
//	settings		- Current addon settings
//	channelprops	- Channel properties to be populated on success

bool addon::channeladd_wx(struct settings const& settings, struct channelprops& channelprops) const
{
  std::vector<std::string> channelnames; // Channel names
  std::vector<std::string> channellabels; // Channel labels
  std::vector<uint32_t> channelfrequencies; // Channel frequencies

  // Pull a database handle out of the connection pool
  connectionpool::handle dbhandle(m_connpool);

  // Enumerate the named channels available for the specified modulation (WX)
  enumerate_namedchannels(dbhandle, modulation::wx,
                          [&](struct namedchannel const& item) -> void
                          {
                            if ((item.frequency > 0) && (item.name != nullptr))
                            {

                              // Append the frequency of the channel in megahertz (xxx.xxx format) to the channel name
                              char label[256]{};
                              unsigned int mhz = item.frequency / 1000000;
                              unsigned int khz = (item.frequency % 1000000) / 1000;
                              snprintf(label, std::extent<decltype(label)>::value, "%s (%u.%u MHz)",
                                       item.name, mhz, khz);

                              channelnames.emplace_back(item.name);
                              channellabels.emplace_back(label);
                              channelfrequencies.emplace_back(item.frequency);
                            }
                          });

  assert((channelnames.size() == channellabels.size()) &&
         (channelnames.size() == channelfrequencies.size()));
  if (channelnames.size() == 0)
    throw string_exception("No Weather Radio channels were enumerated from the database");

  // The user has to select what Weather Radio channel will be added from the hard-coded options in the database
  int selected =
      kodi::gui::dialogs::Select::Show(kodi::addon::GetLocalizedString(30428), channellabels);
  if (selected < 0)
    return false;

  // Initialize enough properties for the settings dialog to work
  channelprops.frequency = channelfrequencies[selected];
  channelprops.modulation = modulation::wx;
  channelprops.name = channelnames[selected];

  // If the channel already exists in the database, get the previously set properties
  bool exists = channel_exists(dbhandle, channelprops);
  if (exists)
    get_channel_properties(dbhandle, channelprops.frequency, channelprops.modulation, channelprops);

  // Set up the tuner device properties
  struct tunerprops tunerprops = {};
  tunerprops.freqcorrection = settings.device_frequency_correction;

  // Create and initialize a channel settings dialog instance to allow the user to fine-tune the channel
  std::unique_ptr<channelsettings> settingsdialog =
      channelsettings::create(create_device(settings), tunerprops, channelprops, true);
  settingsdialog->DoModal();

  if (settingsdialog->get_dialog_result() == true)
  {

    // Retrieve the updated channel properties from the dialog box
    settingsdialog->get_channel_properties(channelprops);

    // Add or update the channel in the database
    if (!exists)
      add_channel(dbhandle, channelprops);
    else
      update_channel(dbhandle, channelprops);

    return true;
  }

  return false;
}

//---------------------------------------------------------------------------
// addon::copy_settings (private, inline)
//
// Atomically creates a copy of the member addon_settings structure
//
// Arguments:
//
//	NONE

inline struct settings addon::copy_settings(void) const
{
  std::unique_lock<std::recursive_mutex> settings_lock(m_settings_lock);
  return m_settings;
}

//---------------------------------------------------------------------------
// addon::create_device (private)
//
// Creates the RTL-SDR device instance
//
// Arguments:
//
//	settings		- Current addon settings structure

std::unique_ptr<rtldevice> addon::create_device(struct settings const& settings) const
{
  // Pull a database handle out of the connection pool
  connectionpool::handle dbhandle(m_connpool);

  // File device
  if (has_rawfiles(dbhandle))
  {

    std::vector<std::string> names; // File names
    std::vector<std::pair<std::string, uint32_t>> files; // File paths and sample rates

    // Enumerate the available raw files registered in the database
    enumerate_rawfiles(dbhandle,
                       [&](struct rawfile const& item) -> void
                       {
                         if ((item.path != nullptr) && (item.name != nullptr) &&
                             (item.samplerate > 0))
                         {

                           names.emplace_back(std::string(item.name));
                           files.emplace_back(std::string(item.path), item.samplerate);
                         }
                       });

    // Prompt to select from among the available files, or cancel the operation
    int selected =
        kodi::gui::dialogs::Select::Show(kodi::addon::GetLocalizedString(30412), names, -1, 0);
    if (selected >= 0)
    {

      auto const& item = files[selected];
      return filedevice::create(item.first.c_str(), item.second);
    }
  }

#ifdef USB_DEVICE_SUPPORT
  // USB device
  if (settings.device_connection == device_connection::usb)
    return usbdevice::create(settings.device_connection_usb_index);
#endif

  // Network device
  if (settings.device_connection == device_connection::rtltcp)
    return tcpdevice::create(settings.device_connection_tcp_host.c_str(),
                             static_cast<uint16_t>(settings.device_connection_tcp_port));

  // Unknown device type
  throw string_exception("invalid device_connection type specified");
}

//---------------------------------------------------------------------------
// addon::downsample_quality_to_string (private, static)
//
// Converts a downsample_quality enumeration value into a string
//
// Arguments:
//
//	quality			- Downsample quality to convert into a string

std::string addon::downsample_quality_to_string(enum downsample_quality quality)
{
  switch (quality)
  {

    case downsample_quality::fast:
      return kodi::addon::GetLocalizedString(30216);
    case downsample_quality::standard:
      return kodi::addon::GetLocalizedString(30217);
    case downsample_quality::maximum:
      return kodi::addon::GetLocalizedString(30218);
  }

  return "Unknown";
}

//---------------------------------------------------------------------------
// addon::device_connection_to_string (private, static)
//
// Converts a device_connection enumeration value into a string
//
// Arguments:
//
//	connection		- Connection type to convert into a string

std::string addon::device_connection_to_string(enum device_connection connection)
{
  switch (connection)
  {

    case device_connection::usb:
      return kodi::addon::GetLocalizedString(30200);
    case device_connection::rtltcp:
      return kodi::addon::GetLocalizedString(30201);
  }

  return "Unknown";
}

//---------------------------------------------------------------------------
// addon::is_region_northamerica (private)
//
// Determines if the currently set region is North America
//
// Arguments:
//
//	settings		- Reference to the current addon settings

bool addon::is_region_northamerica(struct settings const& settings) const
{
  // If a region code has not been set, try to determine if the RTL-SDR device is
  // being operated in North America based on the ISO language code
  if (settings.region_regioncode == regioncode::notset)
  {

    std::string language = kodi::GetLanguage(LANG_FMT_ISO_639_1, true);

    // Only North American countries use the RBDS standard
    if (language.find("-us") != std::string::npos)
      return true;
    else if (language.find("-ca") != std::string::npos)
      return true;
    else if (language.find("-mx") != std::string::npos)
      return true;
    else
      return false;
  }

  return (settings.region_regioncode == regioncode::northamerica);
}

//---------------------------------------------------------------------------
// addon::handle_generalexception (private)
//
// Handler for thrown generic exceptions
//
// Arguments:
//
//	function		- Name of the function where the exception was thrown

void addon::handle_generalexception(char const* function)
{
  log_error(function, " failed due to an exception");
}

//---------------------------------------------------------------------------
// addon::handle_generalexception (private)
//
// Handler for thrown generic exceptions
//
// Arguments:
//
//	function		- Name of the function where the exception was thrown
//	result			- Result code to return

template<typename _result>
_result addon::handle_generalexception(char const* function, _result result)
{
  handle_generalexception(function);
  return result;
}

//---------------------------------------------------------------------------
// addon::handle_stdexception (private)
//
// Handler for thrown std::exceptions
//
// Arguments:
//
//	function		- Name of the function where the exception was thrown
//	exception		- std::exception that was thrown

void addon::handle_stdexception(char const* function, std::exception const& ex)
{
  log_error(function, " failed due to an exception: ", ex.what());
}

//---------------------------------------------------------------------------
// addon::handle_stdexception (private)
//
// Handler for thrown std::exceptions
//
// Arguments:
//
//	function		- Name of the function where the exception was thrown
//	exception		- std::exception that was thrown
//	result			- Result code to return

template<typename _result>
_result addon::handle_stdexception(char const* function, std::exception const& ex, _result result)
{
  handle_stdexception(function, ex);
  return result;
}

//---------------------------------------------------------------------------
// addon::log_debug (private)
//
// Variadic method of writing a LOG_DEBUG entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_debug(_args&&... args) const
{
  log_message(ADDON_LOG::ADDON_LOG_DEBUG, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_error (private)
//
// Variadic method of writing a LOG_ERROR entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_error(_args&&... args) const
{
  log_message(ADDON_LOG::ADDON_LOG_ERROR, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_info (private)
//
// Variadic method of writing a LOG_INFO entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_info(_args&&... args) const
{
  log_message(ADDON_LOG::ADDON_LOG_INFO, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_message (private)
//
// Variadic method of writing a log entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_message(ADDON_LOG level, _args&&... args) const
{
  std::ostringstream stream;
  int unpack[] = {0, (static_cast<void>(stream << args), 0)...};
  (void)unpack;

  kodi::Log(level, stream.str().c_str());

  // Write ADDON_LOG_ERROR level messages to an appropriate secondary log mechanism
  if (level == ADDON_LOG::ADDON_LOG_ERROR)
  {

#if defined(_WINDOWS) || defined(WINAPI_FAMILY)
    std::string message = "ERROR: " + stream.str() + "\r\n";
#elif __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, VERSION_PRODUCTNAME_ANSI, "ERROR: %s\n",
                        stream.str().c_str());
#else
    fprintf(stderr, "ERROR: %s\r\n", stream.str().c_str());
#endif
  }
}

//---------------------------------------------------------------------------
// addon::log_warning (private)
//
// Variadic method of writing a LOG_WARNING entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_warning(_args&&... args) const
{
  log_message(ADDON_LOG::ADDON_LOG_WARNING, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::menuhook_clearchannels (private)
//
// Menu hook to delete all channels from the database
//
// Arguments:
//
//	NONE

void addon::menuhook_clearchannels(void)
{
  log_info(__func__, ": clearing channel data");

  try
  {

    // Clear the channel data from the database and inform the user if successful
    clear_channels(connectionpool::handle(m_connpool));
    kodi::gui::dialogs::OK::ShowAndGetInput(kodi::addon::GetLocalizedString(30402),
                                            "Channel data successfully cleared");

    TriggerChannelGroupsUpdate(); // Trigger a channel group update in Kodi
  }

  catch (std::exception& ex)
  {

    // Log the error, inform the user that the operation failed, and re-throw the exception with this function name
    handle_stdexception(__func__, ex);
    kodi::gui::dialogs::OK::ShowAndGetInput(kodi::addon::GetLocalizedString(30402),
                                            "An error occurred clearing the channel data:", "",
                                            ex.what());
    throw string_exception(__func__, ": ", ex.what());
  }

  catch (...)
  {
    handle_generalexception(__func__);
  }
}

//---------------------------------------------------------------------------
// addon::menuhook_exportchannels (private)
//
// Menu hook to export the channel information from the database
//
// Arguments:
//
//	NONE

void addon::menuhook_exportchannels(void)
{
  std::string folderpath; // Export folder path

  // Prompt the user to locate the folder where the .json file will be exported ...
  if (kodi::gui::dialogs::FileBrowser::ShowAndGetDirectory(
          "local|network|removable", kodi::addon::GetLocalizedString(30403), folderpath, true))
  {

    try
    {

      rapidjson::Document document; // Resultant JSON document

      // Generate the output file name based on the selected path
      std::string filepath(folderpath);
      filepath.append("radiochannels.json");
      log_info(__func__, ": exporting channel data to file ", filepath.c_str());

      // Export the channels from the database into a JSON string
      std::string json = export_channels(connectionpool::handle(m_connpool));

      // Parse the JSON data so that it can be pretty printed for the user
      document.Parse(json.c_str());
      if (document.HasParseError())
        throw string_exception("JSON parse error during export - ",
                               rapidjson::GetParseError_En(document.GetParseError()));

      // Pretty print the JSON data
      rapidjson::StringBuffer sb;
      rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
      document.Accept(writer);

      // Attempt to create the output file in the selected directory
      kodi::vfs::CFile jsonfile;
      if (!jsonfile.OpenFileForWrite(filepath, true))
        throw string_exception("unable to open file ", filepath.c_str(), " for write access");

      // Write the pretty printed JSON data into the output file
      ssize_t written = jsonfile.Write(sb.GetString(), sb.GetSize());
      jsonfile.Close();

      // If the file wasn't written properly, throw an exception
      if (written != static_cast<ssize_t>(sb.GetSize()))
        throw string_exception("short write occurred generating file ", filepath.c_str());

      // Inform the user that the operation was successful
      kodi::gui::dialogs::OK::ShowAndGetInput(kodi::addon::GetLocalizedString(30401),
                                              "Channels successfully exported to:", "",
                                              filepath.c_str());
    }

    catch (std::exception& ex)
    {

      // Log the error, inform the user that the operation failed, and re-throw the exception with this function name
      handle_stdexception(__func__, ex);
      kodi::gui::dialogs::OK::ShowAndGetInput(kodi::addon::GetLocalizedString(30401),
                                              "An error occurred exporting the channel data:", "",
                                              ex.what());
      throw string_exception(__func__, ": ", ex.what());
    }

    catch (...)
    {
      handle_generalexception(__func__);
    }
  }
}

//---------------------------------------------------------------------------
// addon::menuhook_importchannels (private)
//
// Menu hook to import channel information into the database
//
// Arguments:
//
//	NONE

void addon::menuhook_importchannels(void)
{
  std::string filepath; // Import file path
  std::string json; // Imported JSON data

  // Prompt the user to locate the .json file to be imported ...
  if (kodi::gui::dialogs::FileBrowser::ShowAndGetFile(
          "local|network|removable", "*.json", kodi::addon::GetLocalizedString(30404), filepath))
  {

    try
    {

      log_info(__func__, ": importing channel data from file ", filepath.c_str());

      // Ensure the file exists before trying to open it
      if (!kodi::vfs::FileExists(filepath, false))
        throw string_exception("input file ", filepath.c_str(), " does not exist");

      // Attempt to open the specified input file
      kodi::vfs::CFile jsonfile;
      if (!jsonfile.OpenFile(filepath))
        throw string_exception("unable to open file ", filepath.c_str(), " for read access");

      // Read in the input file in 1KiB chunks; it shouldn't be that big
      std::unique_ptr<char[]> buffer(new char[1 KiB]);
      ssize_t read = jsonfile.Read(&buffer[0], 1 KiB);
      while (read > 0)
      {

        json.append(&buffer[0], read);
        read = jsonfile.Read(&buffer[0], 1 KiB);
      }

      // Close the input file
      jsonfile.Close();

      // Only try to import channels from the file if something was actually in there ...
      if (json.length())
        import_channels(connectionpool::handle(m_connpool), json.c_str());

      // Inform the user that the operation was successful
      kodi::gui::dialogs::OK::ShowAndGetInput(kodi::addon::GetLocalizedString(30400),
                                              "Channels successfully imported from:", "",
                                              filepath.c_str());

      TriggerChannelGroupsUpdate(); // Trigger a channel group update in Kodi
    }

    catch (std::exception& ex)
    {

      // Log the error, inform the user that the operation failed, and re-throw the exception with this function name
      handle_stdexception(__func__, ex);
      kodi::gui::dialogs::OK::ShowAndGetInput(kodi::addon::GetLocalizedString(30400),
                                              "An error occurred importing the channel data:", "",
                                              ex.what());
      throw string_exception(__func__, ": ", ex.what());
    }

    catch (...)
    {
      handle_generalexception(__func__);
    }
  }
}

//---------------------------------------------------------------------------
// addon::regioncode_to_string (private, static)
//
// Converts a regioncode enumeration value into a string
//
// Arguments:
//
//	code		- Region code value to be converted into a string

std::string addon::regioncode_to_string(enum regioncode code)
{
  switch (code)
  {

    case regioncode::notset:
      return kodi::addon::GetLocalizedString(30219);
    case regioncode::world:
      return kodi::addon::GetLocalizedString(30220);
    case regioncode::northamerica:
      return kodi::addon::GetLocalizedString(30221);
    case regioncode::europe:
      return kodi::addon::GetLocalizedString(30222);
  }

  return "Unknown";
}

//---------------------------------------------------------------------------
// addon::update_regioncode (private)
//
// Updates the addon region code
//
// Arguments:
//
//	code		- The updated region code

void addon::update_regioncode(enum regioncode code) const
{
  std::string region = regioncode_to_string(code);

  // NORTH AMERICA
  //
  if (code == regioncode::northamerica)
  {

    kodi::addon::SetSettingBoolean("fmradio_enable", true);
    log_info(__func__, ": setting fmradio_enable systemically changed to true for region ", region);

    kodi::addon::SetSettingBoolean("hdradio_enable", true);
    log_info(__func__, ": setting hdradio_enable systemically changed to true for region ", region);

    kodi::addon::SetSettingBoolean("dabradio_enable", false);
    log_info(__func__, ": setting dabradio_enable systemically changed to false for region ",
             region);

    kodi::addon::SetSettingBoolean("wxradio_enable", true);
    log_info(__func__, ": setting wxradio_enable systemically changed to true for region ", region);
  }

  // EUROPE/AUSTRALIA
  //
  else if (code == regioncode::europe)
  {

    kodi::addon::SetSettingBoolean("fmradio_enable", true);
    log_info(__func__, ": setting fmradio_enable systemically changed to true for region ", region);

    kodi::addon::SetSettingBoolean("hdradio_enable", false);
    log_info(__func__, ": setting hdradio_enable systemically changed to false for region ",
             region);

    kodi::addon::SetSettingBoolean("dabradio_enable", true);
    log_info(__func__, ": setting dabradio_enable systemically changed to true for region ",
             region);

    kodi::addon::SetSettingBoolean("wxradio_enable", false);
    log_info(__func__, ": setting wxradio_enable systemically changed to false for region ",
             region);
  }

  // WORLD
  //
  else
  {

    kodi::addon::SetSettingBoolean("fmradio_enable", true);
    log_info(__func__, ": setting fmradio_enable systemically changed to true for region ", region);

    kodi::addon::SetSettingBoolean("hdradio_enable", false);
    log_info(__func__, ": setting hdradio_enable systemically changed to false for region ",
             region);

    kodi::addon::SetSettingBoolean("dabradio_enable", false);
    log_info(__func__, ": setting dabradio_enable systemically changed to false for region ",
             region);

    kodi::addon::SetSettingBoolean("wxradio_enable", false);
    log_info(__func__, ": setting wxradio_enable systemically changed to false for region ",
             region);
  }
}

//---------------------------------------------------------------------------
// CADDONBASE IMPLEMENTATION
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// addon::Create (CAddonBase)
//
// Initializes the addon instance
//
// Arguments:
//
//	NONE

ADDON_STATUS addon::Create(void)
{
  try
  {

#ifdef _WINDOWS
    // On Windows, initialize winsock in case broadcast discovery is used; WSAStartup is
    // reference-counted so if it has already been called this won't hurt anything
    WSADATA wsaData;
    int wsaresult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaresult != 0)
      throw string_exception(__func__, ": WSAStartup failed with error code ", wsaresult);
#endif

    // Initialize SQLite
    int result = sqlite3_initialize();
    if (result != SQLITE_OK)
      throw sqlite_exception(result, "sqlite3_initialize() failed");

    // Throw a banner out to the Kodi log indicating that the add-on is being loaded
    log_info(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loading");

    try
    {

      // The user data path doesn't always exist when an addon has been installed
      if (!kodi::vfs::DirectoryExists(UserPath()))
      {

        log_info(__func__, ": user data directory ", UserPath().c_str(), " does not exist");
        if (!kodi::vfs::CreateDirectory(UserPath()))
          throw string_exception(__func__, ": unable to create addon user data directory");
        log_info(__func__, ": user data directory ", UserPath().c_str(), " created");
      }

      // Load the device settings
#ifdef USB_DEVICE_SUPPORT
      m_settings.device_connection =
          kodi::addon::GetSettingEnum("device_connection", device_connection::usb);
      m_settings.device_connection_usb_index =
          kodi::addon::GetSettingInt("device_connection_usb_index", 0);
#else
      m_settings.device_connection = device_connection::rtltcp;
      m_settings.device_connection_usb_index = 0;
#endif
      m_settings.device_connection_tcp_host =
          kodi::addon::GetSettingString("device_connection_tcp_host");
      m_settings.device_connection_tcp_port =
          kodi::addon::GetSettingInt("device_connection_tcp_port", 1234);
      m_settings.device_frequency_correction =
          kodi::addon::GetSettingInt("device_frequency_correction", 0);

      // Load the region settings
      m_settings.region_regioncode =
          kodi::addon::GetSettingEnum("region_regioncode", regioncode::notset);

      // Load the FM Radio settings
      m_settings.fmradio_enable_rds = kodi::addon::GetSettingBoolean("fmradio_enable_rds", true);
      m_settings.fmradio_prepend_channel_numbers =
          kodi::addon::GetSettingBoolean("fmradio_prepend_channel_numbers", false);
      m_settings.fmradio_sample_rate =
          kodi::addon::GetSettingInt("fmradio_sample_rate", (1600 KHz));
      m_settings.fmradio_downsample_quality =
          kodi::addon::GetSettingEnum("fmradio_downsample_quality", downsample_quality::standard);
      m_settings.fmradio_output_samplerate =
          kodi::addon::GetSettingInt("fmradio_output_samplerate", 48000);
      m_settings.fmradio_output_gain = kodi::addon::GetSettingFloat("fmradio_output_gain", -3.0f);

      // Load the HD Radio settings
      m_settings.hdradio_enable = kodi::addon::GetSettingBoolean("hdradio_enable", false);
      m_settings.hdradio_prepend_channel_numbers =
          kodi::addon::GetSettingBoolean("hdradio_prepend_channel_numbers", false);
      m_settings.hdradio_output_gain = kodi::addon::GetSettingFloat("hdradio_output_gain", -3.0f);

      // Load the DAB settings
      m_settings.dabradio_enable = kodi::addon::GetSettingBoolean("dabradio_enable", false);
      m_settings.dabradio_output_gain = kodi::addon::GetSettingFloat("dabradio_output_gain", -3.0f);
      m_settings.dabradio_coarse_corrector = kodi::addon::GetSettingBoolean("dabradio_coarse_corrector", true);
      m_settings.dabradio_coarse_corrector_type = kodi::addon::GetSettingInt("dabradio_coarse_corrector_type", 1);

      // Load the Weather Radio settings
      m_settings.wxradio_enable = kodi::addon::GetSettingBoolean("wxradio_enable", false);
      m_settings.wxradio_sample_rate =
          kodi::addon::GetSettingInt("wxradio_sample_rate", (1600 KHz));
      m_settings.wxradio_output_samplerate =
          kodi::addon::GetSettingInt("wxradio_output_samplerate", 48000);
      m_settings.wxradio_output_gain = kodi::addon::GetSettingFloat("wxradio_output_gain", -3.0f);

      // Log the setting values
      log_info(__func__,
               ": m_settings.dabradio_enable                   = ", m_settings.dabradio_enable);
      log_info(__func__, ": m_settings.dabradio_output_gain              = ",
               m_settings.dabradio_output_gain);
      log_info(__func__, ": m_settings.dabradio_coarse_corrector         = ",
               m_settings.dabradio_coarse_corrector);
      log_info(__func__, ": m_settings.dabradio_coarse_corrector_type    = ",
               m_settings.dabradio_coarse_corrector_type);
      log_info(__func__, ": m_settings.device_connection                 = ",
               device_connection_to_string(m_settings.device_connection));
      log_info(__func__, ": m_settings.device_connection_tcp_host        = ",
               m_settings.device_connection_tcp_host);
      log_info(__func__, ": m_settings.device_connection_tcp_port        = ",
               m_settings.device_connection_tcp_port);
      log_info(__func__, ": m_settings.device_connection_usb_index       = ",
               m_settings.device_connection_usb_index);
      log_info(__func__, ": m_settings.device_frequency_correction       = ",
               m_settings.device_frequency_correction);
      log_info(__func__, ": m_settings.fmradio_downsample_quality        = ",
               downsample_quality_to_string(m_settings.fmradio_downsample_quality));
      log_info(__func__,
               ": m_settings.fmradio_enable_rds                = ", m_settings.fmradio_enable_rds);
      log_info(__func__, ": m_settings.fmradio_prepend_channel_numbers   = ",
               m_settings.fmradio_prepend_channel_numbers);
      log_info(__func__,
               ": m_settings.fmradio_output_gain               = ", m_settings.fmradio_output_gain);
      log_info(__func__, ": m_settings.fmradio_output_samplerate         = ",
               m_settings.fmradio_output_samplerate);
      log_info(__func__,
               ": m_settings.fmradio_sample_rate               = ", m_settings.fmradio_sample_rate);
      log_info(__func__,
               ": m_settings.hdradio_enable                    = ", m_settings.hdradio_enable);
      log_info(__func__,
               ": m_settings.hdradio_output_gain               = ", m_settings.hdradio_output_gain);
      log_info(__func__, ": m_settings.hdradio_prepend_channel_numbers   = ",
               m_settings.hdradio_prepend_channel_numbers);
      log_info(__func__, ": m_settings.region_regioncode                 = ",
               regioncode_to_string(m_settings.region_regioncode));
      log_info(__func__,
               ": m_settings.wxradio_enable                    = ", m_settings.wxradio_enable);
      log_info(__func__,
               ": m_settings.wxradio_output_gain               = ", m_settings.wxradio_output_gain);
      log_info(__func__, ": m_settings.wxradio_output_samplerate         = ",
               m_settings.wxradio_output_samplerate);
      log_info(__func__,
               ": m_settings.wxradio_sample_rate               = ", m_settings.wxradio_sample_rate);

      // Register the PVR_MENUHOOK_SETTING category menu hooks
      AddMenuHook(
          kodi::addon::PVRMenuhook(MENUHOOK_SETTING_IMPORTCHANNELS, 30400, PVR_MENUHOOK_SETTING));
      AddMenuHook(
          kodi::addon::PVRMenuhook(MENUHOOK_SETTING_EXPORTCHANNELS, 30401, PVR_MENUHOOK_SETTING));
      AddMenuHook(
          kodi::addon::PVRMenuhook(MENUHOOK_SETTING_CLEARCHANNELS, 30402, PVR_MENUHOOK_SETTING));

      // Generate the local file system and URL-based file names for the channels database
      std::string databasefile = UserPath() + "/channels.db";
      std::string databasefileuri = "file:///" + databasefile;

      // Create the global database connection pool instance
      try
      {
        m_connpool = std::make_shared<connectionpool>(
            databasefileuri.c_str(), DATABASE_CONNECTIONPOOL_SIZE,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);
      }
      catch (sqlite_exception const& dbex)
      {

        log_error(__func__, ": unable to create/open the channels database ", databasefile, " - ",
                  dbex.what());
        throw;
      }

      // If the user has not specified a region code, attempt to get them to do it during startup
      if (m_settings.region_regioncode == regioncode::notset)
      {

        std::vector<enum regioncode> regioncodes; // Region codes
        std::vector<std::string> regionlabels; // Region labels

        // NORTH AMERICA (FM / HD / WX)
        //
        regioncodes.emplace_back(regioncode::northamerica);
        regionlabels.emplace_back(kodi::addon::GetLocalizedString(30317));

        // EUROPE/AUSTRALIA (FM / DAB)
        //
        regioncodes.emplace_back(regioncode::europe);
        regionlabels.emplace_back(kodi::addon::GetLocalizedString(30318));

        // WORLD (FM)
        //
        regioncodes.emplace_back(regioncode::world);
        regionlabels.emplace_back(kodi::addon::GetLocalizedString(30316));

        assert(regioncodes.size() == regionlabels.size());

        // Prompt the user; if they cancel the operation the region will remain as "not set"
        // and default to "world" for things like FM Radio RDS vs RBDS
        result =
            kodi::gui::dialogs::Select::Show(kodi::addon::GetLocalizedString(30315), regionlabels);
        if (result >= 0)
        {

          kodi::addon::SetSettingEnum<enum regioncode>("region_regioncode", regioncodes[result]);
          update_regioncode(regioncodes[result]);
        }
      }
    }

    catch (std::exception& ex)
    {
      handle_stdexception(__func__, ex);
      throw;
    }
    catch (...)
    {
      handle_generalexception(__func__);
      throw;
    }
  }

  // Anything that escapes above can't be logged at this point, just return ADDON_STATUS_PERMANENT_FAILURE
  catch (...)
  {
    return ADDON_STATUS::ADDON_STATUS_PERMANENT_FAILURE;
  }

  // Throw a simple banner out to the Kodi log indicating that the add-on has been loaded
  log_info(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loaded");

  return ADDON_STATUS::ADDON_STATUS_OK;
}

//---------------------------------------------------------------------------
// addon::Destroy (private)
//
// Uninitializes/unloads the addon instance
//
// Arguments:
//
//	NONE

void addon::Destroy(void) noexcept
{
  try
  {

    // Throw a message out to the Kodi log indicating that the add-on is being unloaded
    log_info(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " unloading");

    m_pvrstream.reset(); // Destroy any active stream instance

    // Check for more than just the global connection pool reference during shutdown
    long poolrefs = m_connpool.use_count();
    if (poolrefs != 1)
      log_warning(__func__, ": m_connpool.use_count = ", m_connpool.use_count());
    m_connpool.reset();

    sqlite3_shutdown(); // Clean up SQLite

#ifdef _WINDOWS
    WSACleanup(); // Release winsock reference
#endif

    // Send a notice out to the Kodi log as late as possible and destroy the addon callbacks
    log_info(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " unloaded");
  }

  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex);
  }
  catch (...)
  {
    return handle_generalexception(__func__);
  }
}

//---------------------------------------------------------------------------
// addon::SetSetting (CAddonBase)
//
// Notifies the addon that a setting has been changed
//
// Arguments:
//

ADDON_STATUS addon::SetSetting(std::string const& settingName,
                               kodi::addon::CSettingValue const& settingValue)
{
  // Changing settings may be recursive operation, use recursive_lock
  std::unique_lock<std::recursive_mutex> settings_lock(m_settings_lock);

  // For comparison purposes
  struct settings previous = m_settings;

#ifdef USB_DEVICE_SUPPORT

  // device_connection
  //
  if (settingName == "device_connection")
  {

    enum device_connection value = settingValue.GetEnum<enum device_connection>();
    if (value != m_settings.device_connection)
    {

      m_settings.device_connection = value;
      log_info(__func__, ": setting device_connection changed to ",
               device_connection_to_string(value).c_str());
    }
  }

  // device_connection_usb_index
  //
  else if (settingName == "device_connection_usb_index")
  {

    int nvalue = settingValue.GetInt();
    if (nvalue != static_cast<int>(m_settings.device_connection_usb_index))
    {

      m_settings.device_connection_usb_index = nvalue;
      log_info(__func__, ": setting device_connection_usb_index changed to ",
               m_settings.device_connection_usb_index);
    }
  }

  else

#endif

  // device_connection_tcp_host
  //
  if (settingName == "device_connection_tcp_host")
  {

    std::string strvalue = settingValue.GetString();
    if (strvalue != m_settings.device_connection_tcp_host)
    {

      m_settings.device_connection_tcp_host = strvalue;
      log_info(__func__, ": setting device_connection_tcp_host changed to ", strvalue.c_str());
    }
  }

  // device_connection_tcp_port
  //
  else if (settingName == "device_connection_tcp_port")
  {

    int nvalue = settingValue.GetInt();
    if (nvalue != m_settings.device_connection_tcp_port)
    {

      m_settings.device_connection_tcp_port = nvalue;
      log_info(__func__, ": setting device_connection_tcp_port changed to ",
               m_settings.device_connection_tcp_port);
    }
  }

  // device_frequency_correction
  //
  else if (settingName == "device_frequency_correction")
  {

    int nvalue = settingValue.GetInt();
    if (nvalue != m_settings.device_frequency_correction)
    {

      m_settings.device_frequency_correction = nvalue;
      log_info(__func__, ": setting device_frequency_correction changed to ",
               m_settings.device_frequency_correction, "PPM");
    }
  }

  // fmradio_enable_rds
  //
  else if (settingName == "fmradio_enable_rds")
  {

    bool bvalue = settingValue.GetBoolean();
    if (bvalue != m_settings.fmradio_enable_rds)
    {

      m_settings.fmradio_enable_rds = bvalue;
      log_info(__func__, ": setting fmradio_enable_rds changed to ", bvalue);
    }
  }

  // fmradio_prepend_channel_numbers
  //
  else if (settingName == "fmradio_prepend_channel_numbers")
  {

    bool bvalue = settingValue.GetBoolean();
    if (bvalue != m_settings.fmradio_prepend_channel_numbers)
    {

      m_settings.fmradio_prepend_channel_numbers = bvalue;
      log_info(__func__, ": setting fmradio_prepend_channel_numbers changed to ", bvalue);

      // Trigger an update to refresh the channel names
      TriggerChannelUpdate();
    }
  }

  // fmradio_sample_rate
  //
  else if (settingName == "fmradio_sample_rate")
  {

    int nvalue = settingValue.GetInt();
    if (nvalue != m_settings.fmradio_sample_rate)
    {

      m_settings.fmradio_sample_rate = nvalue;
      log_info(__func__, ": setting fmradio_sample_rate changed to ",
               m_settings.fmradio_sample_rate, "Hz");
    }
  }

  // fmradio_downsample_quality
  //
  else if (settingName == "fmradio_downsample_quality")
  {

    enum downsample_quality value = settingValue.GetEnum<enum downsample_quality>();
    if (value != m_settings.fmradio_downsample_quality)
    {

      m_settings.fmradio_downsample_quality = value;
      log_info(__func__, ": setting fmradio_downsample_quality changed to ",
               downsample_quality_to_string(value).c_str());
    }
  }

  // fmradio_output_samplerate
  //
  else if (settingName == "fmradio_output_samplerate")
  {

    int nvalue = settingValue.GetInt();
    if (nvalue != m_settings.fmradio_output_samplerate)
    {

      m_settings.fmradio_output_samplerate = nvalue;
      log_info(__func__, ": setting fmradio_output_samplerate changed to ", nvalue, "Hz");
    }
  }

  // fmradio_output_gain
  //
  else if (settingName == "fmradio_output_gain")
  {

    float fvalue = settingValue.GetFloat();
    if (fvalue != m_settings.fmradio_output_gain)
    {

      m_settings.fmradio_output_gain = fvalue;
      log_info(__func__, ": setting fmradio_output_gain changed to ", fvalue, "dB");
    }
  }

  // hdradio_enable
  //
  else if (settingName == "hdradio_enable")
  {

    bool bvalue = settingValue.GetBoolean();
    if (bvalue != m_settings.hdradio_enable)
    {

      m_settings.hdradio_enable = bvalue;
      log_info(__func__, ": setting hdradio_enable changed to ", bvalue);

      // Trigger an update to refresh the channel groups
      TriggerChannelGroupsUpdate();
    }
  }

  // hdradio_prepend_channel_numbers
  //
  else if (settingName == "hdradio_prepend_channel_numbers")
  {

    bool bvalue = settingValue.GetBoolean();
    if (bvalue != m_settings.hdradio_prepend_channel_numbers)
    {

      m_settings.hdradio_prepend_channel_numbers = bvalue;
      log_info(__func__, ": setting hdradio_prepend_channel_numbers changed to ", bvalue);

      // Trigger an update to refresh the channel names
      TriggerChannelUpdate();
    }
  }

  // hdradio_output_gain
  //
  else if (settingName == "hdradio_output_gain")
  {

    float fvalue = settingValue.GetFloat();
    if (fvalue != m_settings.hdradio_output_gain)
    {

      m_settings.hdradio_output_gain = fvalue;
      log_info(__func__, ": setting hdradio_output_gain changed to ", fvalue, "dB");
    }
  }

  // dabradio_enable
  //
  else if (settingName == "dabradio_enable")
  {

    bool bvalue = settingValue.GetBoolean();
    if (bvalue != m_settings.dabradio_enable)
    {

      m_settings.dabradio_enable = bvalue;
      log_info(__func__, ": setting dabradio_enable changed to ", bvalue);

      // Trigger an update to refresh the channel groups
      TriggerChannelGroupsUpdate();
    }
  }

  // dabradio_output_gain
  //
  else if (settingName == "dabradio_output_gain")
  {

    float fvalue = settingValue.GetFloat();
    if (fvalue != m_settings.dabradio_output_gain)
    {

      m_settings.dabradio_output_gain = fvalue;
      log_info(__func__, ": setting dabradio_output_gain changed to ", fvalue, "dB");
    }
  }

  // dabradio_coarse_corrector
  //
  else if (settingName == "dabradio_coarse_corrector")
  {

    bool bvalue = settingValue.GetBoolean();
    if (bvalue != m_settings.dabradio_coarse_corrector)
    {

      m_settings.dabradio_coarse_corrector = bvalue;
      log_info(__func__, ": setting dabradio_coarse_corrector changed to ", bvalue);
    }
  }

  // dabradio_coarse_corrector_type
  //
  else if (settingName == "dabradio_coarse_corrector_type")
  {

    int nvalue = settingValue.GetInt();
    if (nvalue != m_settings.dabradio_coarse_corrector_type)
    {

      m_settings.dabradio_coarse_corrector_type = nvalue;
      log_info(__func__, ": setting dabradio_coarse_corrector_type changed to ", nvalue);
    }
  }

  // region_regioncode
  //
  if (settingName == "region_regioncode")
  {

    enum regioncode value = settingValue.GetEnum<enum regioncode>();
    if (value != m_settings.region_regioncode)
    {

      m_settings.region_regioncode = value;
      log_info(__func__, ": setting region_regioncode changed to ",
               regioncode_to_string(value).c_str());

      // Update the region code (warning: recursive)
      update_regioncode(m_settings.region_regioncode);
    }
  }

  // wxradio_enable
  //
  else if (settingName == "wxradio_enable")
  {

    bool bvalue = settingValue.GetBoolean();
    if (bvalue != m_settings.wxradio_enable)
    {

      m_settings.wxradio_enable = bvalue;
      log_info(__func__, ": setting wxradio_enable changed to ", bvalue);

      // Trigger an update to refresh the channel groups
      TriggerChannelGroupsUpdate();
    }
  }

  // wxradio_sample_rate
  //
  else if (settingName == "wxradio_sample_rate")
  {

    int nvalue = settingValue.GetInt();
    if (nvalue != m_settings.wxradio_sample_rate)
    {

      m_settings.wxradio_sample_rate = nvalue;
      log_info(__func__, ": setting wxradio_sample_rate changed to ",
               m_settings.wxradio_sample_rate, "Hz");
    }
  }

  // wxradio_output_samplerate
  //
  else if (settingName == "wxradio_output_samplerate")
  {

    int nvalue = settingValue.GetInt();
    if (nvalue != m_settings.wxradio_output_samplerate)
    {

      m_settings.wxradio_output_samplerate = nvalue;
      log_info(__func__, ": setting wxradio_output_samplerate changed to ", nvalue, "Hz");
    }
  }

  // wxradio_output_gain
  //
  else if (settingName == "wxradio_output_gain")
  {

    float fvalue = settingValue.GetFloat();
    if (fvalue != m_settings.wxradio_output_gain)
    {

      m_settings.wxradio_output_gain = fvalue;
      log_info(__func__, ": setting wxradio_output_gain changed to ", fvalue, "dB");
    }
  }

  return ADDON_STATUS::ADDON_STATUS_OK;
}

//---------------------------------------------------------------------------
// CINSTANCEPVRCLIENT IMPLEMENTATION
//---------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// addon::CallSettingsMenuHook (CInstancePVRClient)
//
// Call one of the settings related menu hooks
//
// Arguments:
//
//	menuhook	- The hook being invoked

PVR_ERROR addon::CallSettingsMenuHook(kodi::addon::PVRMenuhook const& menuhook)
{
  try
  {

    // Invoke the proper helper function to handle the settings menu hook implementation
    if (menuhook.GetHookId() == MENUHOOK_SETTING_IMPORTCHANNELS)
      menuhook_importchannels();
    else if (menuhook.GetHookId() == MENUHOOK_SETTING_EXPORTCHANNELS)
      menuhook_exportchannels();
    else if (menuhook.GetHookId() == MENUHOOK_SETTING_CLEARCHANNELS)
      menuhook_clearchannels();
  }

  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED);
  }
  catch (...)
  {
    return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::CanSeekStream (CInstancePVRClient)
//
// Check if the backend supports seeking for the currently playing stream
//
// Arguments:
//
//	NONE

bool addon::CanSeekStream(void)
{
  try
  {
    return (m_pvrstream) ? m_pvrstream->canseek() : false;
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, false);
  }
  catch (...)
  {
    return handle_generalexception(__func__, false);
  }
}

//-----------------------------------------------------------------------------
// addon::CloseLiveStream (CInstancePVRClient)
//
// Close an open live stream
//
// Arguments:
//
//	NONE

void addon::CloseLiveStream(void)
{
  // Prevent race condition with GetSignalStatus()
  std::unique_lock<std::mutex> lock(m_pvrstream_lock);

  try
  {
    m_pvrstream.reset();
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex);
  }
  catch (...)
  {
    return handle_generalexception(__func__);
  }
}

//-----------------------------------------------------------------------------
// addon::DeleteChannel (CInstancePVRClient)
//
// Deletes a channel from the backend
//
// Arguments:
//
//	channel		- Channel to be deleted

PVR_ERROR addon::DeleteChannel(kodi::addon::PVRChannel const& channel)
{
  channelid channelid(channel.GetUniqueId()); // Convert UniqueID back into a channelid

  try
  {

    connectionpool::handle dbhandle(m_connpool);

    uint32_t const frequency = channelid.frequency();
    enum modulation const modulationtype = channelid.modulation();
    uint32_t const subchannel = channelid.subchannel();

    // For HD Radio and DAB, if the subchannel number is set only delete the subchannel
    if ((modulationtype == modulation::hd || modulationtype == modulation::dab) && (subchannel > 0))
      delete_subchannel(dbhandle, channelid.frequency(), channelid.modulation(),
                        channelid.subchannel());

    else
      delete_channel(dbhandle, frequency, modulationtype);
  }

  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED);
  }
  catch (...)
  {
    return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::DemuxAbort (CInstancePVRClient)
//
// Abort the demultiplexer thread in the add-on
//
// Arguments:
//
//	NONE

void addon::DemuxAbort(void)
{
  try
  {
    if (m_pvrstream)
      m_pvrstream->demuxabort();
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex);
  }
  catch (...)
  {
    return handle_generalexception(__func__);
  }
}

//-----------------------------------------------------------------------------
// addon::DemuxFlush (CInstancePVRClient)
//
// Flush all data that's currently in the demultiplexer buffer
//
// Arguments:
//
//	NONE

void addon::DemuxFlush(void)
{
  try
  {
    if (m_pvrstream)
      m_pvrstream->demuxflush();
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex);
  }
  catch (...)
  {
    return handle_generalexception(__func__);
  }
}

//-----------------------------------------------------------------------------
// addon::DemuxRead (CInstancePVRClient)
//
// Read the next packet from the demultiplexer
//
// Arguments:
//
//	NONE

DEMUX_PACKET* addon::DemuxRead(void)
{
  // Prevent race condition with GetSignalStatus()
  std::unique_lock<std::mutex> lock(m_pvrstream_lock);

  if (!m_pvrstream)
    return nullptr;

  try
  {

    // Use an inline lambda to provide the stream an std::function to use to invoke AllocateDemuxPacket()
    DEMUX_PACKET* packet = m_pvrstream->demuxread([&](int size) -> DEMUX_PACKET*
                                                  { return AllocateDemuxPacket(size); });

    // Log a warning if a stream change packet was detected; this means the application isn't keeping up with the device
    if ((packet != nullptr) && (packet->iStreamId == DEMUX_SPECIALID_STREAMCHANGE))
      log_warning(__func__,
                  ": stream buffer has been flushed; device sample rate may need to be reduced");

    return packet;
  }

  catch (std::exception& ex)
  {

    // Log the exception and alert the user of the failure with an error notification
    log_error(__func__, ": read operation failed with exception: ", ex.what());
    kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, "Unable to read from stream: %s",
                                     ex.what());

    m_pvrstream.reset(); // Close the stream
    return nullptr; // Return a null demultiplexer packet
  }

  catch (...)
  {
    return handle_generalexception(__func__, nullptr);
  }
}

//-----------------------------------------------------------------------------
// addon::DemuxReset (CInstancePVRClient)
//
// Reset the demultiplexer in the add-on
//
// Arguments:
//
//	NONE

void addon::DemuxReset(void)
{
  try
  {
    if (m_pvrstream)
      m_pvrstream->demuxreset();
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex);
  }
  catch (...)
  {
    return handle_generalexception(__func__);
  }
}

//-----------------------------------------------------------------------------
// addon::GetBackendName (CInstancePVRClient)
//
// Get the name reported by the backend that will be displayed in the UI
//
// Arguments:
//
//	name		- Backend name string to be initialized

PVR_ERROR addon::GetBackendName(std::string& name)
{
  name.assign(VERSION_PRODUCTNAME_ANSI);

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetBackendVersion (CInstancePVRClient)
//
// Get the version string reported by the backend that will be displayed in the UI
//
// Arguments:
//
//	version		- Backend version string to be initialized

PVR_ERROR addon::GetBackendVersion(std::string& version)
{
  version.assign(STR(PVRRTLRADIO_VERSION));

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetCapabilities (CInstancePVRClient)
//
// Get the list of features that this add-on provides
//
// Arguments:
//
//	capabilities	- PVR capability attributes class

PVR_ERROR addon::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  capabilities.SetSupportsRadio(true);
  capabilities.SetSupportsChannelGroups(true);
  capabilities.SetSupportsChannelSettings(true);
    capabilities.SetSupportsChannelScan(true);
capabilities.SetHandlesInputStream(true);
  capabilities.SetHandlesDemuxing(true);
  capabilities.SetSupportsEPG(true);

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannelGroupsAmount (CInstancePVRClient)
//
// Get the total amount of channel groups on the backend
//
// Arguments:
//
//	amount		- Set to the number of available channel groups

PVR_ERROR addon::GetChannelGroupsAmount(int& amount)
{
  // Create a copy of the current addon settings structure
  struct settings settings = copy_settings();

  amount = 2; // Analog AM and FM Radio are always enabled
  amount += settings.hdradio_enable; // HD Radio
  amount += settings.dabradio_enable; // DAB
  amount += settings.wxradio_enable; // Weather Radio

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannelGroupMembers (CInstancePVRClient)
//
// Request the list of all group members of a group from the backend
//
// Arguments:
//
//	group		- Channel group for which to get the group members
//	results		- Channel group members result set to be loaded

PVR_ERROR addon::GetChannelGroupMembers(kodi::addon::PVRChannelGroup const& group,
                                        kodi::addon::PVRChannelGroupMembersResultSet& results)
{
  // Only interested in radio channel groups
  if (!group.GetIsRadio())
    return PVR_ERROR::PVR_ERROR_NO_ERROR;

  // Create a copy of the current addon settings structure
  struct settings settings = copy_settings();

  // Select the proper enumerator for the channel group
  std::function<void(sqlite3*, enumerate_channels_callback)> enumerator = nullptr;

  if (group.GetGroupName() == kodi::addon::GetLocalizedString(30408))
    enumerator = std::bind(enumerate_fmradio_channels, std::placeholders::_1,
                           settings.fmradio_prepend_channel_numbers, std::placeholders::_2);

  else if (group.GetGroupName() == kodi::addon::GetLocalizedString(30419))
    enumerator = std::bind(enumerate_amradio_channels, std::placeholders::_1,
                           settings.fmradio_prepend_channel_numbers, std::placeholders::_2);

  else if (settings.hdradio_enable &&
           (group.GetGroupName() == kodi::addon::GetLocalizedString(30409)))
    enumerator = std::bind(enumerate_hdradio_channels, std::placeholders::_1,
                           settings.hdradio_prepend_channel_numbers, std::placeholders::_2);

  else if (settings.dabradio_enable &&
           (group.GetGroupName() == kodi::addon::GetLocalizedString(30411)))
    enumerator = enumerate_dabradio_channels;

  else if (settings.wxradio_enable &&
           (group.GetGroupName() == kodi::addon::GetLocalizedString(30410)))
    enumerator = enumerate_wxradio_channels;

  // If no enumerator was selected, there isn't any work to do here
  if (enumerator == nullptr)
    return PVR_ERROR::PVR_ERROR_NO_ERROR;

  try
  {

    // Enumerate all of the channels in the specified group
    enumerator(connectionpool::handle(m_connpool),
               [&](struct channel const& channel) -> void
               {
                 // Create and initialize a PVRChannelGroupMember instance for the enumerated channel
                 kodi::addon::PVRChannelGroupMember member;
                 member.SetGroupName(group.GetGroupName());
                 member.SetChannelUniqueId(channel.id);
                 member.SetChannelNumber(channel.channel);
                 member.SetSubChannelNumber(channel.subchannel);

                 results.Add(member);
               });
  }

  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED);
  }
  catch (...)
  {
    return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannelGroups (CInstancePVRClient)
//
// Request the list of all channel groups from the backend
//
// Arguments:
//
//	radio		- True to get radio groups, false to get TV channel groups
//	results		- Channel groups result set to be loaded

PVR_ERROR addon::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
  // The PVR only supports radio channel groups
  if (!radio)
    return PVR_ERROR::PVR_ERROR_NO_ERROR;

  // Create a copy of the current addon settings structure
  struct settings settings = copy_settings();

  kodi::addon::PVRChannelGroup fmradio; // FM Radio
  fmradio.SetGroupName(kodi::addon::GetLocalizedString(30408));
  fmradio.SetIsRadio(true);
  results.Add(fmradio);

  kodi::addon::PVRChannelGroup amradio; // Analog AM Radio
  amradio.SetGroupName(kodi::addon::GetLocalizedString(30419));
  amradio.SetIsRadio(true);
  results.Add(amradio);

  if (settings.hdradio_enable)
  {
    kodi::addon::PVRChannelGroup hdradio; // HD Radio
    hdradio.SetGroupName(kodi::addon::GetLocalizedString(30409));
    hdradio.SetIsRadio(true);
    results.Add(hdradio);
  }

  if (settings.dabradio_enable)
  {
    kodi::addon::PVRChannelGroup dabradio; // DAB
    dabradio.SetGroupName(kodi::addon::GetLocalizedString(30411));
    dabradio.SetIsRadio(true);
    results.Add(dabradio);
  }

  if (settings.wxradio_enable)
  {
    kodi::addon::PVRChannelGroup wxradio; // Weather Radio
    wxradio.SetGroupName(kodi::addon::GetLocalizedString(30410));
    wxradio.SetIsRadio(true);
    results.Add(wxradio);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannels (CInstancePVRClient)
//
// Request the list of all channels from the backend
//
// Arguments:
//
//	radio		- True to get radio channels, false to get TV channels
//	results		- Channels result set to be loaded

PVR_ERROR addon::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
  // The PVR only supports radio channels
  if (!radio)
    return PVR_ERROR::PVR_ERROR_NO_ERROR;

  // Create a copy of the current addon settings structure
  struct settings settings = copy_settings();

  try
  {

    auto callback = [&](struct channel const& item) -> void
    {
      kodi::addon::PVRChannel channel;

      channel.SetUniqueId(item.id);
      channel.SetIsRadio(true);
      channel.SetChannelNumber(item.channel);
      channel.SetSubChannelNumber(item.subchannel);
      if (item.name != nullptr)
        channel.SetChannelName(item.name);
      if (item.logourl != nullptr)
        channel.SetIconPath(item.logourl);

      results.Add(channel);
    };

    connectionpool::handle dbhandle(m_connpool);
    enumerate_fmradio_channels(dbhandle, settings.fmradio_prepend_channel_numbers, callback);
    enumerate_amradio_channels(dbhandle, settings.fmradio_prepend_channel_numbers, callback);
    if (settings.hdradio_enable)
      enumerate_hdradio_channels(dbhandle, settings.hdradio_prepend_channel_numbers, callback);
    if (settings.dabradio_enable)
      enumerate_dabradio_channels(dbhandle, callback);
    if (settings.wxradio_enable)
      enumerate_wxradio_channels(dbhandle, callback);
  }

  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED);
  }
  catch (...)
  {
    return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannelsAmount (CInstancePVRClient)
//
// Gets the total amount of channels on the backend
//
// Arguments:
//
//	amount		- Set to the number of available channels

PVR_ERROR addon::GetChannelsAmount(int& amount)
{
  try
  {
    amount = get_channel_count(connectionpool::handle(m_connpool));
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED);
  }
  catch (...)
  {
    return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannelStreamProperties (CInstancePVRClient)
//
// Get the stream properties for a channel from the backend
//
// Arguments:
//
//	channel		- channel to get the stream properties for
//	properties	- properties required to play the stream

PVR_ERROR addon::GetChannelStreamProperties(kodi::addon::PVRChannel const& /*channel*/,
                                            std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");
  properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM_PLAYER, "audiodefaultplayer");

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetEPGForChannel (CInstancePVRClient)
//
// Request the EPG for a channel from the backend
//
// Arguments:
//
//	channelUid		- Channel identifier
//	start			- Start of the requested time frame
//	end				- End of the requested time frame
//	results			- EPG tag result set

PVR_ERROR addon::GetEPGForChannel(int /*channelUid*/,
                                  time_t /*start*/,
                                  time_t /*end*/,
                                  kodi::addon::PVREPGTagsResultSet& /*results*/)
{
  // This PVR doesn't support EPG, but if it doesn't claim that it does
  // the radio and TV channels get all mixed up ...
  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetSignalStatus (CInstancePVRClient)
//
// Get the signal status of the stream that's currently open
//
// Arguments:
//
//	channelUid		- Channel identifier
//	signalStatus	- Structure to set with the signal status information

PVR_ERROR addon::GetSignalStatus(int /*channelUid*/, kodi::addon::PVRSignalStatus& signalStatus)
{
  // Prevent race condition with functions that modify m_pvrstream
  std::unique_lock<std::mutex> lock(m_pvrstream_lock);

  // Kodi may call this function before the stream is open, avoid the error log
  if (!m_pvrstream)
    return PVR_ERROR::PVR_ERROR_NO_ERROR;

  try
  {

    int quality = 0; // Quality as a percentage
    int snr = 0; // SNR as a percentage

    // Retrieve the quality metrics from the stream instance
    m_pvrstream->signalquality(quality, snr);

    signalStatus.SetAdapterName(m_pvrstream->devicename());
    signalStatus.SetAdapterStatus("Active");
    signalStatus.SetServiceName(m_pvrstream->servicename());
    signalStatus.SetProviderName("RTL-SDR");
    signalStatus.SetMuxName(m_pvrstream->muxname());

    signalStatus.SetSignal(quality * 655); // Range: 0-65535
    signalStatus.SetSNR(snr * 655); // Range: 0-65535
  }

  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED);
  }
  catch (...)
  {
    return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetStreamProperties (CInstancePVRClient)
//
// Get the stream properties of the stream that's currently being read
//
// Arguments:
//
//	properties		- Stream properties to be set

PVR_ERROR addon::GetStreamProperties(std::vector<kodi::addon::PVRStreamProperties>& properties)
{
  if (!m_pvrstream)
    return PVR_ERROR::PVR_ERROR_FAILED;

  // Enumerate the stream properties as specified by the PVR stream instance
  m_pvrstream->enumproperties(
      [&](struct streamprops const& props) -> void
      {
        kodi::addon::PVRCodec codec = GetCodecByName(props.codec);
        if (codec.GetCodecType() != PVR_CODEC_TYPE::PVR_CODEC_TYPE_UNKNOWN)
        {

          kodi::addon::PVRStreamProperties streamprops;

          streamprops.SetPID(props.pid);
          streamprops.SetCodecType(codec.GetCodecType());
          streamprops.SetCodecId(codec.GetCodecId());
          streamprops.SetChannels(props.channels);
          streamprops.SetSampleRate(props.samplerate);
          streamprops.SetBitsPerSample(props.bitspersample);
          streamprops.SetBitRate(props.samplerate * props.channels * props.bitspersample);

          properties.emplace_back(std::move(streamprops));
        }
      });

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetConnectionString (CInstancePVRClient)
//
// Gets the connection string reported by the backend
//
// Arguments:
//
//	connection	- Set to the connection string to report

PVR_ERROR addon::GetConnectionString(std::string& connection)
{
  // Create a copy of the current addon settings structure
  struct settings settings = copy_settings();

  // This property is fairly useless; just return the device connection type
  if (settings.device_connection == device_connection::usb)
    connection = "usb";
  else if (settings.device_connection == device_connection::rtltcp)
    connection = "network";
  else
    connection = "unknown";

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::IsRealTimeStream (CInstancePVRClient)
//
// Check for real-time streaming
//
// Arguments:
//
//	NONE

bool addon::IsRealTimeStream(void)
{
  try
  {
    return (m_pvrstream) ? m_pvrstream->realtime() : false;
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, false);
  }
  catch (...)
  {
    return handle_generalexception(__func__, false);
  }
}

//-----------------------------------------------------------------------------
// addon::LengthLiveStream (CInstancePVRClient)
//
// Obtain the length of a live stream
//
// Arguments:
//
//	NONE

int64_t addon::LengthLiveStream(void)
{
  try
  {
    return (m_pvrstream) ? m_pvrstream->length() : -1;
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, -1);
  }
  catch (...)
  {
    return handle_generalexception(__func__, -1);
  }
}

//-----------------------------------------------------------------------------
// addon::OpenDialogChannelAdd (CInstancePVRClient)
//
// Show the dialog to add a channel on the backend
//
// Arguments:
//
//	channel		- The channel to add

PVR_ERROR addon::OpenDialogChannelAdd(kodi::addon::PVRChannel const& /*channel*/)
{
  // Create a copy of the current addon settings structure
  struct settings settings = copy_settings();

  // The user has the option to disable support for each type of channel
  std::vector<std::string> channeltypes;
  std::vector<enum modulation> modulationtypes;

  channeltypes.emplace_back(kodi::addon::GetLocalizedString(30414));
  modulationtypes.emplace_back(modulation::fm);

  channeltypes.emplace_back(kodi::addon::GetLocalizedString(30420));
  modulationtypes.emplace_back(modulation::am);

  if (settings.hdradio_enable)
  {

    channeltypes.emplace_back(kodi::addon::GetLocalizedString(30415));
    modulationtypes.emplace_back(modulation::hd);
  }

  if (settings.dabradio_enable)
  {

    channeltypes.emplace_back(kodi::addon::GetLocalizedString(30416));
    modulationtypes.emplace_back(modulation::dab);
  }

  if (settings.wxradio_enable)
  {

    channeltypes.emplace_back(kodi::addon::GetLocalizedString(30417));
    modulationtypes.emplace_back(modulation::wx);
  }

  assert(channeltypes.size() == modulationtypes.size());

  // If the user has no channel types enabled, just return without error
  if ((channeltypes.size() == 0) || (modulationtypes.size() == 0))
    return PVR_ERROR::PVR_ERROR_NO_ERROR;

  // If more than one modulation type is available, prompt the user to select one
  enum modulation modulationtype = modulationtypes[0];
  if (modulationtypes.size() > 1)
  {

    int selected =
        kodi::gui::dialogs::Select::Show(kodi::addon::GetLocalizedString(30413), channeltypes);
    if (selected < 0)
      return PVR_ERROR::PVR_ERROR_NO_ERROR;

    modulationtype = modulationtypes[selected];
  }

  // TODO: see if there is a better way we can work with this
  if (m_pvrstream)
  {

    // TODO: This message is terrible
    kodi::gui::dialogs::OK::ShowAndGetInput(
        kodi::addon::GetLocalizedString(30405),
        "Modifying PVR Radio channel settings requires "
        "exclusive access to the connected RTL-SDR tuner device.",
        "", "Active playback of PVR Radio streams must be stopped before continuing.");

    return PVR_ERROR::PVR_ERROR_NO_ERROR;
  }

  try
  {

    struct channelprops channelprops = {}; // New channel properties
    bool result = false; // Result from channel add helper

    if (modulationtype == modulation::fm)
      result = channeladd_fm(settings, channelprops);
    else if (modulationtype == modulation::am)
      result = channeladd_am(settings, channelprops);
    else if (modulationtype == modulation::hd)
      result = channeladd_hd(settings, channelprops);
    else if (modulationtype == modulation::dab)
      result = channeladd_dab(settings, channelprops);
    else if (modulationtype == modulation::wx)
      result = channeladd_wx(settings, channelprops);

    if (result == false)
      return PVR_ERROR::PVR_ERROR_NO_ERROR;
  }

  catch (std::exception& ex)
  {

    // Log the error and inform the user that the operation failed, do not return an error code
    handle_stdexception(__func__, ex);
    kodi::gui::dialogs::OK::ShowAndGetInput(kodi::addon::GetLocalizedString(30407),
                                            "An error occurred displaying the "
                                            "add channel dialog:",
                                            "", ex.what());
  }

  catch (...)
  {
    return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::OpenDialogChannelScan (CInstancePVRClient)
//
// Show the channel scan dialog
//
// Arguments:
//
//	NONE

PVR_ERROR addon::OpenDialogChannelScan(void)
{
  static std::atomic_bool scan_running{false};

  bool expected = false;
  if (!scan_running.compare_exchange_strong(expected, true))
  {
    kodi::gui::dialogs::OK::ShowAndGetInput(
        "Radio channel scan",
        "A radio channel scan is already running.");
    return PVR_ERROR::PVR_ERROR_NO_ERROR;
  }

  std::thread([this]() -> void
  {
    struct scan_guard
    {
      std::atomic_bool& running;
      ~scan_guard() { running.store(false); }
    } guard{scan_running};

    std::unique_lock lock(m_pvrstream_lock);

    if (m_pvrstream)
    {
      kodi::gui::dialogs::OK::ShowAndGetInput(
          kodi::addon::GetLocalizedString(30405),
          "Automatic radio channel scan requires exclusive access to the RTL-SDR tuner.",
          "",
          "Stop active playback before starting the scan.");
      return;
    }

    struct settings settings = copy_settings();

    enum scan_mode_type
    {
      scan_mode_hd_fm_full,
      scan_mode_hd_fm_single,
      scan_mode_hd_am_full,
      scan_mode_hd_am_single,
      scan_mode_am_full,
      scan_mode_am_single,
      scan_mode_fm_rds,
      scan_mode_fm_rds_single,
      scan_mode_wx
    };

    std::vector<std::string> scan_types;
    std::vector<scan_mode_type> scan_modes;

    if (settings.hdradio_enable)
    {
      scan_types.emplace_back("FM HD Radio full scan");
      scan_modes.emplace_back(scan_mode_hd_fm_full);

      scan_types.emplace_back("FM HD Radio single-frequency scan");
      scan_modes.emplace_back(scan_mode_hd_fm_single);

      if (is_region_northamerica(settings))
      {
        scan_types.emplace_back("AM HD Radio full scan");
        scan_modes.emplace_back(scan_mode_hd_am_full);

        scan_types.emplace_back("AM HD Radio single-frequency scan");
        scan_modes.emplace_back(scan_mode_hd_am_single);
      }
    }

    if (settings.fmradio_enable_rds)
    {
      scan_types.emplace_back("FM Radio RDS scan");
      scan_modes.emplace_back(scan_mode_fm_rds);

      scan_types.emplace_back("FM Radio RDS single-frequency scan");
      scan_modes.emplace_back(scan_mode_fm_rds_single);
    }

    if (is_region_northamerica(settings))
    {
      scan_types.emplace_back("AM Radio full scan");
      scan_modes.emplace_back(scan_mode_am_full);

      scan_types.emplace_back("AM Radio single-frequency scan");
      scan_modes.emplace_back(scan_mode_am_single);
    }

    if (settings.wxradio_enable)
    {
      scan_types.emplace_back("Weather Radio scan");
      scan_modes.emplace_back(scan_mode_wx);
    }

    if (scan_modes.empty())
    {
      kodi::gui::dialogs::OK::ShowAndGetInput(
          "Radio channel scan",
          "No scannable radio types are enabled in the add-on settings.");
      return;
    }

    scan_mode_type scan_mode = scan_modes[0];

    if (scan_modes.size() > 1)
    {
      int selected = kodi::gui::dialogs::Select::Show("Select scan type", scan_types);
      if (selected < 0)
        return;

      scan_mode = scan_modes[selected];
    }

    enum modulation scan_modulation = modulation::hd;
    if (scan_mode == scan_mode_wx)
      scan_modulation = modulation::wx;
    else if ((scan_mode == scan_mode_am_full) || (scan_mode == scan_mode_am_single))
      scan_modulation = modulation::am;

    bool single_frequency_hd_scan = false;
    uint32_t selected_single_hd_frequency = 0;
    bool const hd_am_scan = (scan_mode == scan_mode_hd_am_full) ||
                            (scan_mode == scan_mode_hd_am_single);

    bool single_frequency_fm_rds_scan = false;
    uint32_t selected_single_fm_rds_frequency = 0;
    bool const single_frequency_am_scan = (scan_mode == scan_mode_am_single);
    uint32_t selected_single_am_frequency = 0;

    if (single_frequency_am_scan)
    {
      std::vector<std::string> labels;
      for (uint32_t frequency = amradio::FIRST_FREQUENCY;
           frequency <= amradio::LAST_FREQUENCY;
           frequency += amradio::STEP_FREQUENCY)
        labels.emplace_back(std::to_string(frequency / 1000) + " kHz");

      int const selected =
          kodi::gui::dialogs::Select::Show("Select AM Radio frequency", labels);
      if (selected < 0)
        return;

      selected_single_am_frequency = amradio::FIRST_FREQUENCY +
          static_cast<uint32_t>(selected) * amradio::STEP_FREQUENCY;
    }

    if ((scan_mode == scan_mode_hd_fm_single) ||
        (scan_mode == scan_mode_hd_am_single))
    {
      std::vector<std::string> hd_frequency_labels;
      std::vector<uint32_t> hd_frequencies;

      uint32_t const first_frequency = hd_am_scan ? hdradio::AM_FIRST_FREQUENCY
                                                  : hdradio::FM_FIRST_FREQUENCY;
      uint32_t const last_frequency = hd_am_scan ? hdradio::AM_LAST_FREQUENCY
                                                 : hdradio::FM_LAST_FREQUENCY;
      uint32_t const step_frequency = hd_am_scan ? hdradio::AM_STEP_FREQUENCY
                                                 : hdradio::FM_STEP_FREQUENCY;

      for (uint32_t frequency = first_frequency;
           frequency <= last_frequency;
           frequency += step_frequency)
      {
        char label[64] = {};
        if (hd_am_scan)
          snprintf(label, std::extent<decltype(label)>::value, "%u kHz", frequency / 1000);
        else
          snprintf(label, std::extent<decltype(label)>::value, "%u.%u MHz",
                   frequency / 1000000,
                   (frequency % 1000000) / 100000);

        hd_frequency_labels.emplace_back(label);
        hd_frequencies.emplace_back(frequency);
      }

      int selected_frequency =
          kodi::gui::dialogs::Select::Show(
              hd_am_scan ? "Select AM HD Radio frequency" : "Select FM HD Radio frequency",
              hd_frequency_labels);
      if (selected_frequency < 0)
        return;

      single_frequency_hd_scan = true;
      selected_single_hd_frequency = hd_frequencies[selected_frequency];
    }

    if (scan_mode == scan_mode_fm_rds_single)
    {
      std::vector<std::string> fm_frequency_labels;
      std::vector<uint32_t> fm_frequencies;

      bool const is_north_america = is_region_northamerica(settings);
      uint32_t const first_frequency = is_north_america ? 87900000 : 87500000;
      uint32_t const last_frequency = 107900000;
      uint32_t const step_frequency = is_north_america ? 200000 : 100000;

      for (uint32_t frequency = first_frequency;
           frequency <= last_frequency;
           frequency += step_frequency)
      {
        char label[64] = {};
        snprintf(label, std::extent<decltype(label)>::value, "%u.%u MHz",
                 frequency / 1000000,
                 (frequency % 1000000) / 100000);

        fm_frequency_labels.emplace_back(label);
        fm_frequencies.emplace_back(frequency);
      }

      int selected_frequency =
          kodi::gui::dialogs::Select::Show("Select FM Radio RDS frequency",
                                           fm_frequency_labels);
      if (selected_frequency < 0)
        return;

      single_frequency_fm_rds_scan = true;
      selected_single_fm_rds_frequency = fm_frequencies[selected_frequency];
    }

    struct screensaver_guard
    {
      bool inhibited = false;

      screensaver_guard()
      {
#ifdef TARGET_LINUX
        inhibited =
            (std::system(
                 "kodi-send --action='InhibitScreensaver(true)' >/dev/null 2>&1") == 0);
        kodi::Log(ADDON_LOG_INFO,
                  inhibited
                      ? "Screen saver inhibited for channel scan"
                      : "Unable to inhibit screen saver for channel scan");
#endif
      }

      ~screensaver_guard()
      {
#ifdef TARGET_LINUX
        if (inhibited)
        {
          // Kodi ignores built-in actions while a modal dialog's closing
          // animation is active. Allow that animation to finish first.
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          std::system(
              "kodi-send --action='InhibitScreensaver(false)' >/dev/null 2>&1");
          kodi::Log(ADDON_LOG_INFO, "Screen saver restored after channel scan");
        }
#endif
      }
    } screensaver;

    // FM_RDS_SCAN_BRANCH_BEGIN
    if ((scan_mode == scan_mode_fm_rds) || (scan_mode == scan_mode_fm_rds_single))
    {
      if (!settings.fmradio_enable_rds)
      {
        kodi::gui::dialogs::OK::ShowAndGetInput(
            "FM Radio RDS scan",
            "FM Radio RDS/RBDS decoding is disabled in the add-on settings.");
        return;
      }

      struct fm_rds_scan_result
      {
        bool found = false;
        bool perfect_levels = false;
        uint64_t bytes = 0;
        int gain = 0;
        int quality = 0;
        int snr = 0;
        int stereo_lock = 0;
        std::string name;
        std::string callsign;
        std::string ps;
        std::string radiotext;
        int score = -1000000000;
      };

      auto const is_north_america = is_region_northamerica(settings);

      uint32_t const band_first_frequency = is_north_america ? 87900000 : 87500000;
      uint32_t const band_last_frequency = 107900000;
      uint32_t const step_frequency = is_north_america ? 200000 : 100000;

      uint32_t const first_frequency =
          single_frequency_fm_rds_scan ? selected_single_fm_rds_frequency : band_first_frequency;
      uint32_t const last_frequency =
          single_frequency_fm_rds_scan ? selected_single_fm_rds_frequency : band_last_frequency;
      uint32_t const total_frequencies =
          single_frequency_fm_rds_scan
              ? 1
              : (((band_last_frequency - band_first_frequency) / step_frequency) + 1);

      uint32_t const fm_sample_rate = static_cast<uint32_t>(settings.fmradio_sample_rate);
      if ((fm_sample_rate < 900001) || (fm_sample_rate > 3200000))
        throw string_exception(
            "FM Radio RDS scan requires FM sample rate between 900001Hz and 3200000Hz");

      auto const scan_time = std::chrono::seconds(6);
      auto const fine_scan_time = std::chrono::seconds(3);
      auto const minimum_signal_time = std::chrono::milliseconds(1500);
      auto const minimum_name_time = std::chrono::milliseconds(2500);
      int const minimum_unnamed_quality = 45;
      int const minimum_unnamed_stereo_lock = 60;

      int channels_found = 0;
      bool canceled = false;

      auto format_frequency = [](uint32_t frequency) -> std::string
      {
        char buffer[32] = {};
        snprintf(buffer, sizeof(buffer), "%.1f MHz",
                 static_cast<double>(frequency) / 1000000.0);
        return std::string(buffer);
      };

      auto format_fm_channel_name = [](uint32_t frequency) -> std::string
      {
        char buffer[32] = {};
        snprintf(buffer, sizeof(buffer), "%.1f-FM",
                 static_cast<double>(frequency) / 1000000.0);
        return std::string(buffer);
      };

      kodi::gui::dialogs::CProgress progress;
      progress.SetHeading("FM Radio RDS scan");
      progress.SetCanCancel(true);
      progress.ShowProgressBar(true);
      progress.SetPercentage(0);
      progress.SetLine(0, "Preparing RTL-SDR tuner.");
      progress.SetLine(1, is_north_america ? "RBDS mode" : "RDS mode");
      progress.SetLine(2, "Press Cancel to stop");
      progress.Open();

      auto update_progress =
          [&](int percent,
              std::string const& line0,
              std::string const& line1,
              std::string const& line2) -> bool
      {
        if (percent < 0)
          percent = 0;
        if (percent > 100)
          percent = 100;

        progress.SetPercentage(percent);
        progress.SetLine(0, line0);
        progress.SetLine(1, line1);
        progress.SetLine(2, line2);

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return progress.IsCanceled();
      };

      std::vector<int> valid_gains;
      {
        std::unique_ptr<rtldevice> gain_device = create_device(settings);
        gain_device->get_valid_gains(valid_gains);
      }

      if (valid_gains.empty())
      {
        // Fallback R820T/R820T2-style values, in tenths of dB.
        valid_gains = {
            0, 9, 14, 27, 37, 77, 87, 125, 144, 157,
            166, 197, 207, 229, 254, 280, 297, 328,
            338, 364, 372, 386, 402, 421, 434, 439,
            445, 480, 496};
      }

      std::sort(valid_gains.begin(), valid_gains.end());
      valid_gains.erase(std::unique(valid_gains.begin(), valid_gains.end()), valid_gains.end());

      auto nearest_valid_gain = [&](int target) -> int
      {
        int best = valid_gains.front();
        int best_distance = std::abs(best - target);

        for (int gain : valid_gains)
        {
          int distance = std::abs(gain - target);
          if (distance < best_distance)
          {
            best = gain;
            best_distance = distance;
          }
        }

        return best;
      };

      std::vector<int> coarse_gains;

      // Preserve the broad FM coarse coverage, then match the HD scan by
      // fine tuning around every coarse gain that detects a usable station.
      for (int desired : {27, 87, 125, 197, 280, 328, 386, 439})
      {
        int gain = nearest_valid_gain(desired);
        if (std::find(coarse_gains.begin(), coarse_gains.end(), gain) == coarse_gains.end())
          coarse_gains.emplace_back(gain);
      }

      auto score_fm_rds_scan_result =
          [](fm_rds_scan_result const& result) -> int
      {
        if (!result.found || result.name.empty())
          return -1000000000 + (result.quality * 10) + (result.snr * 10);

        int score = 0;

        // Prefer stable station identity first.
        if (!result.callsign.empty())
          score += 100000;
        else if (!result.ps.empty())
          score += 80000;
        else
          score += 50000;

        // RadioText is useful confirmation, but not as stable as PS/call sign.
        if (!result.radiotext.empty())
          score += 10000;

        score += result.quality * 100;
        score += result.snr * 100;
        score += result.stereo_lock * 100;

        // Small tie-breaker: prefer the lower gain when quality/SNR are equal.
        score -= result.gain / 10;

        // No bytes means something went wrong.
        if (result.bytes == 0)
          score -= 10000;

        return score;
      };

      auto scan_one_fm =
          [&](uint32_t frequency,
              int gain,
              int percent,
              std::chrono::milliseconds scan_duration) -> fm_rds_scan_result
      {
        fm_rds_scan_result result = {};
        result.gain = gain;

        std::unique_ptr<rtldevice> device = create_device(settings);
        device->set_frequency_correction(settings.device_frequency_correction);
        uint32_t const actual_sample_rate = device->set_sample_rate(fm_sample_rate);
        uint32_t const actual_frequency =
            device->set_center_frequency(frequency + (actual_sample_rate / 4));
        device->set_automatic_gain_control(false);
        device->set_gain(gain);

        tDemodInfo demodinfo = {};
        demodinfo.HiCutmax = 100000;
        demodinfo.HiCut = 100000;
        demodinfo.LowCut = -100000;
        demodinfo.SquelchValue = -160;
        demodinfo.WfmDownsampleQuality =
            static_cast<enum DownsampleQuality>(settings.fmradio_downsample_quality);

        CDemodulator demodulator;
        demodulator.SetUSFmVersion(is_north_america);
        demodulator.SetInputSampleRate(static_cast<TYPEREAL>(actual_sample_rate));
        demodulator.SetDemod(DEMOD_WFM, demodinfo);
        demodulator.SetDemodFreq(static_cast<TYPEREAL>(actual_frequency - frequency));

        rdsdecoder decoder(is_north_america);

        int const input_samples = demodulator.GetInputBufferLimit();
        size_t const readsize = static_cast<size_t>(input_samples) * 2;

        std::atomic_bool found_name{false};
        std::atomic<int> quality{0};
        std::atomic<int> snr{0};
        std::atomic_bool perfect_levels{false};
        std::atomic<uint64_t> quality_sum{0};
        std::atomic<uint64_t> snr_sum{0};
        std::atomic<uint64_t> stereo_lock_samples{0};
        std::atomic<uint64_t> level_samples{0};
        std::atomic<uint64_t> bytes{0};
        std::exception_ptr reader_exception = nullptr;
        std::mutex result_lock;

        device->begin_stream();

        std::thread reader_thread([&]() -> void
        {
          try
          {
            device->read_async(
                [&](uint8_t const* buffer, size_t count) -> void
                {
                  bytes.fetch_add(static_cast<uint64_t>(count));

                  if (count != readsize)
                    return;

                  std::unique_ptr<TYPECPX[]> samples(new TYPECPX[input_samples]);
                  for (int index = 0; index < input_samples; index++)
                  {
                    samples[index] = {

#ifdef FMDSP_USE_DOUBLE_PRECISION
                        (static_cast<TYPEREAL>(buffer[(index * 2)]) - 127.5) *
                            256.9960784313725,
                        (static_cast<TYPEREAL>(buffer[(index * 2) + 1]) - 127.5) *
                            256.9960784313725,
#else
                        (static_cast<TYPEREAL>(buffer[(index * 2)]) - 127.5f) *
                            256.9960784313725f,
                        (static_cast<TYPEREAL>(buffer[(index * 2) + 1]) - 127.5f) *
                            256.9960784313725f,
#endif
                    };
                  }

                  demodulator.ProcessData(input_samples, samples.get(), samples.get());

                  tRDS_GROUPS rdsgroup = {};
                  while (demodulator.GetNextRdsGroupData(&rdsgroup))
                    decoder.decode_rdsgroup(rdsgroup);

                  TYPEREAL demodquality = 0;
                  TYPEREAL demodsnr = 0;
                  demodulator.GetSignalLevels(demodquality, demodsnr);

                  int const q =
                      std::max(0, std::min(100,
                          static_cast<int>(100.0 * (demodquality / 0.80))));
                  int const s =
                      std::max(0, std::min(100,
                          static_cast<int>(100.0 * (demodsnr / 0.60))));
                  int pilot_lock = 0;
                  demodulator.GetStereoLock(&pilot_lock);

                  quality.store(q);
                  snr.store(s);
                  if (q == 100 && s == 100)
                    perfect_levels.store(true);
                  quality_sum.fetch_add(static_cast<uint64_t>(q));
                  snr_sum.fetch_add(static_cast<uint64_t>(s));
                  if (pilot_lock != 0)
                    stereo_lock_samples.fetch_add(1);
                  level_samples.fetch_add(1);

                  if (decoder.has_rbds_callsign())
                  {
                    std::lock_guard<std::mutex> guard(result_lock);
                    result.callsign = decoder.get_rbds_callsign();
                    result.name = result.callsign;
                    result.found = true;
                    found_name.store(true);
                  }
                  else if (decoder.has_programservice())
                  {
                    std::lock_guard<std::mutex> guard(result_lock);
                    result.ps = decoder.get_programservice();
                    result.name = result.ps;
                    result.found = true;
                    found_name.store(true);
                  }

                  if (decoder.has_radiotext())
                  {
                    std::lock_guard<std::mutex> guard(result_lock);
                    result.radiotext = decoder.get_radiotext();
                  }
                },
                static_cast<uint32_t>(readsize));
          }
          catch (...)
          {
            reader_exception = std::current_exception();
          }
        });

        auto const started = std::chrono::steady_clock::now();
        auto const deadline = started + scan_duration;

        while (std::chrono::steady_clock::now() < deadline)
        {
          auto const now = std::chrono::steady_clock::now();
          auto const elapsed = now - started;

          int const q = quality.load();
          int const s = snr.load();

          if (found_name.load() && elapsed >= minimum_name_time)
            break;

          // Fast reject: weak/no usable WFM carrier after the demod has had time to settle.
          if ((elapsed >= minimum_signal_time) &&
              (q < 8) &&
              (s < 8) &&
              (stereo_lock_samples.load() == 0))
            break;

          if (update_progress(percent,
                              "Scanning " + format_frequency(frequency),
                              "gain=" + std::to_string(gain) +
                                  " quality=" + std::to_string(q) +
                                  " snr=" + std::to_string(s),
                              "Found " + std::to_string(channels_found) +
                                  " FM RDS station(s)"))
          {
            canceled = true;
            break;
          }

          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        device->cancel_async();
        if (reader_thread.joinable())
          reader_thread.join();

        if (reader_exception)
          std::rethrow_exception(reader_exception);

        {
          std::lock_guard<std::mutex> guard(result_lock);
          uint64_t const samples = level_samples.load();
          result.quality =
              samples > 0 ? static_cast<int>(quality_sum.load() / samples) : quality.load();
          result.snr =
              samples > 0 ? static_cast<int>(snr_sum.load() / samples) : snr.load();
          result.stereo_lock =
              samples > 0
                  ? static_cast<int>((stereo_lock_samples.load() * 100) / samples)
                  : 0;
          result.perfect_levels = perfect_levels.load();
          result.bytes = bytes.load();

          if (!result.found &&
              result.bytes > 0 &&
              (result.quality >= minimum_unnamed_quality ||
               result.stereo_lock >= minimum_unnamed_stereo_lock))
          {
            result.found = true;
            result.name = format_fm_channel_name(frequency);
          }

          result.score = score_fm_rds_scan_result(result);
        }

        return result;
      };

      connectionpool::handle dbhandle(m_connpool);

      for (uint32_t frequency_index = 0, frequency = first_frequency;
           frequency <= last_frequency;
           frequency += step_frequency, frequency_index++)
      {
        if (canceled)
          break;

        int const percent =
            static_cast<int>((static_cast<uint64_t>(frequency_index) * 100) /
                             total_frequencies);

        fm_rds_scan_result best = {};
        std::vector<fm_rds_scan_result> coarse_locks;
        bool perfect_gain_found = false;

        auto consider_result =
            [&](fm_rds_scan_result const& candidate) -> void
        {
          if (candidate.found && !candidate.name.empty() &&
              (!best.found || (candidate.score > best.score)))
          {
            best = candidate;
          }
        };

        for (int gain : coarse_gains)
        {
          fm_rds_scan_result current = scan_one_fm(frequency, gain, percent, scan_time);

          if (current.found && !current.name.empty())
          {
            coarse_locks.emplace_back(current);
            log_info(__func__,
                     ": coarse FM candidate at ",
                     format_frequency(frequency),
                     " gain=",
                     current.gain,
                     " quality=",
                     current.quality,
                     " snr=",
                     current.snr,
                     " stereo_lock=",
                     current.stereo_lock,
                     " of 100 ",
                     (!current.callsign.empty() || !current.ps.empty())
                         ? " identity=RDS"
                         : " identity=frequency");
          }

          consider_result(current);

          if (current.found && !current.name.empty() && current.perfect_levels)
          {
            best = current;
            perfect_gain_found = true;
            log_info(__func__,
                     ": accepting FM gain=",
                     current.gain,
                     " at ",
                     format_frequency(frequency),
                     " after quality/SNR reached 100/100");
            break;
          }

          if (canceled)
            break;
        }

        if (canceled)
          break;

        if (!coarse_locks.empty() && !perfect_gain_found)
        {
          int const fine_gain_window = 200;
          std::vector<int> fine_gains;

          for (fm_rds_scan_result const& coarse_lock : coarse_locks)
          {
            for (int gain : valid_gains)
            {
              if (std::abs(gain - coarse_lock.gain) <= fine_gain_window)
                fine_gains.emplace_back(gain);
            }
          }

          std::sort(fine_gains.begin(), fine_gains.end());
          fine_gains.erase(std::unique(fine_gains.begin(), fine_gains.end()), fine_gains.end());

          log_info(__func__,
                   ": fine tuning ",
                   format_frequency(frequency),
                   " using ",
                   fine_gains.size(),
                   " candidate gain(s) within +/-",
                   fine_gain_window,
                   " of coarse lock(s)");

          for (int gain : fine_gains)
          {
            fm_rds_scan_result current =
                scan_one_fm(frequency, gain, percent, fine_scan_time);
            consider_result(current);

            if (current.found && !current.name.empty() && current.perfect_levels)
            {
              best = current;
              perfect_gain_found = true;
              log_info(__func__,
                       ": accepting FM fine gain=",
                       current.gain,
                       " at ",
                       format_frequency(frequency),
                       " after quality/SNR reached 100/100");
              break;
            }

            if (canceled)
              break;
          }
        }

        if (canceled)
          break;

        if (!best.found || best.name.empty())
          continue;

        struct channelprops channelprops = {};
        channelprops.frequency = frequency;
        channelprops.modulation = modulation::fm;
        channelprops.name = best.name;
        channelprops.autogain = false;
        channelprops.manualgain = best.gain;
        channelprops.freqcorrection = 0;

        bool exists = channel_exists(dbhandle, channelprops);
        if (exists)
        {
          get_channel_properties(dbhandle,
                                 channelprops.frequency,
                                 channelprops.modulation,
                                 channelprops);

          // Do not replace an existing RDS or user-supplied identity with a
          // frequency fallback when this scan did not decode an RDS name.
          if (!best.callsign.empty() || !best.ps.empty())
            channelprops.name = best.name;
          channelprops.autogain = false;
          channelprops.manualgain = best.gain;
        }

        if (!exists)
          add_channel(dbhandle, channelprops);
        else
          update_channel(dbhandle, channelprops);

        channels_found++;

        log_info(__func__,
                 ": FM RDS scan found ",
                 best.name,
                 " at ",
                 format_frequency(frequency),
                 " gain=",
                 best.gain,
                 " quality=",
                 best.quality,
                 " snr=",
                 best.snr,
                 " stereo_lock=",
                 best.stereo_lock,
                 " of 100 ",
                 " bytes=",
                 best.bytes,
                 (!best.callsign.empty() || !best.ps.empty())
                     ? " identity=RDS"
                     : " identity=frequency");
      }

      progress.SetPercentage(canceled ? 95 : 100);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));

      TriggerChannelUpdate();
      TriggerChannelGroupsUpdate();

      if (canceled)
      {
        kodi::gui::dialogs::OK::ShowAndGetInput(
            "FM Radio RDS scan canceled",
            std::to_string(channels_found) + " FM RDS station(s) found before cancel.");
      }
      else
      {
        kodi::gui::dialogs::OK::ShowAndGetInput(
            "FM Radio RDS scan complete",
            std::to_string(channels_found) + " FM RDS station(s) added or updated.");
      }

      return;
    }
    // FM_RDS_SCAN_BRANCH_END

    if (scan_modulation == modulation::am)
    {
      struct am_scan_measurement
      {
        int gain = 0;
        float power = -999.0f;
        float snr = -999.0f;
        int reports = 0;
        bool overload = false;
        bool detected = false;
      };

      std::vector<int> valid_gains;
      {
        std::unique_ptr<rtldevice> gain_device = create_device(settings);
        gain_device->get_valid_gains(valid_gains);
      }
      if (valid_gains.empty())
        valid_gains = {0, 27, 77, 125, 197, 280, 328, 386, 439, 496};

      auto nearest_gain = [&](int desired) -> int
      {
        return *std::min_element(valid_gains.begin(), valid_gains.end(),
                                 [&](int left, int right)
        {
          return std::abs(left - desired) < std::abs(right - desired);
        });
      };

      std::vector<int> coarse_gains;
      for (int desired : {27, 197, 328, 439})
      {
        int const gain = nearest_gain(desired);
        if (std::find(coarse_gains.begin(), coarse_gains.end(), gain) == coarse_gains.end())
          coarse_gains.emplace_back(gain);
      }

      auto measure_am = [&](uint32_t frequency, int gain) -> am_scan_measurement
      {
        am_scan_measurement measurement;
        measurement.gain = gain;

        struct signalprops signalprops = {};
        signalprops.filter = false;
        signalprops.samplerate = 1600000;
        signalprops.bandwidth = 20000;
        // Put the carrier away from the RTL-SDR center/DC spike.
        signalprops.offset = 20000;
        signalprops.lowcut = -5000;
        signalprops.highcut = 5000;

        struct signalplotprops plotprops = {};
        plotprops.height = 200;
        plotprops.width = 512;
        plotprops.mindb = -72.0f;
        plotprops.maxdb = 4.0f;

        std::mutex status_mutex;
        std::unique_ptr<signalmeter> meter = signalmeter::create(
            signalprops, plotprops, 100,
            [&](struct signalmeter::signal_status const& status) -> void
        {
          std::lock_guard<std::mutex> status_lock(status_mutex);
          measurement.reports++;
          measurement.overload = measurement.overload || status.overload;
          if (!std::isnan(status.power) && status.power > measurement.power)
            measurement.power = status.power;
          if (!std::isnan(status.snr) && status.snr > measurement.snr)
            measurement.snr = status.snr;
        });

        std::unique_ptr<rtldevice> device = create_device(settings);
        device->set_direct_sampling(2);
        device->set_center_frequency(frequency + signalprops.offset);
        device->set_frequency_correction(settings.device_frequency_correction);
        device->set_sample_rate(signalprops.samplerate);
        device->set_automatic_gain_control(false);
        device->set_gain(gain);
        device->begin_stream();

        size_t const buffer_size = 32 KiB;
        std::unique_ptr<uint8_t[]> buffer(new uint8_t[buffer_size]);
        auto const deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(700);
        while (std::chrono::steady_clock::now() < deadline)
        {
          size_t const count = device->read(buffer.get(), buffer_size);
          if (count > 0)
            meter->inputsamples(buffer.get(), count);
        }

        measurement.detected = measurement.reports >= 2 &&
                               measurement.snr >= 6.0f &&
                               measurement.power > -65.0f &&
                               !measurement.overload;
        return measurement;
      };

      uint32_t const first_frequency = single_frequency_am_scan
          ? selected_single_am_frequency : amradio::FIRST_FREQUENCY;
      uint32_t const last_frequency = single_frequency_am_scan
          ? selected_single_am_frequency : amradio::LAST_FREQUENCY;
      size_t const frequency_count =
          ((last_frequency - first_frequency) / amradio::STEP_FREQUENCY) + 1;

      kodi::gui::dialogs::CProgress progress;
      progress.SetHeading(single_frequency_am_scan
                              ? "AM Radio single-frequency scan" : "AM Radio scan");
      progress.SetCanCancel(true);
      progress.ShowProgressBar(true);
      progress.SetLine(1, "Q-branch direct sampling; four-gain search");
      progress.SetLine(2, "Press Cancel to stop");
      progress.Open();

      int channels_found = 0;
      bool canceled = false;
      size_t frequency_index = 0;
      for (uint32_t frequency = first_frequency;
           frequency <= last_frequency;
           frequency += amradio::STEP_FREQUENCY, ++frequency_index)
      {
        progress.SetPercentage(static_cast<int>((frequency_index * 100) /
                                                std::max<size_t>(frequency_count, 1)));
        progress.SetLine(0, "Scanning " + std::to_string(frequency / 1000) + " kHz AM");
        if (progress.IsCanceled())
        {
          canceled = true;
          break;
        }

        am_scan_measurement best;
        for (int gain : coarse_gains)
        {
          progress.SetLine(1, "Coarse gain " + std::to_string(gain / 10) + "." +
                                      std::to_string(std::abs(gain % 10)) + " dB");
          am_scan_measurement const current = measure_am(frequency, gain);
          kodi::Log(ADDON_LOG_DEBUG,
                    "AM_SCAN freq=%u gain=%d reports=%d power=%.1f snr=%.1f overload=%d detected=%d",
                    frequency, gain, current.reports, current.power, current.snr,
                    current.overload ? 1 : 0, current.detected ? 1 : 0);
          if ((best.reports == 0) || (!current.overload && best.overload) ||
              ((current.overload == best.overload) && current.snr > best.snr))
            best = current;
        }

        if (best.snr >= 4.0f && best.power > -68.0f)
        {
          auto best_iterator = std::find(valid_gains.begin(), valid_gains.end(), best.gain);
          if (best_iterator != valid_gains.end())
          {
            size_t const best_index = static_cast<size_t>(best_iterator - valid_gains.begin());
            size_t const begin = (best_index > 2) ? best_index - 2 : 0;
            size_t const end = std::min(valid_gains.size(), best_index + 3);
            for (size_t index = begin; index < end; ++index)
            {
              int const gain = valid_gains[index];
              if (std::find(coarse_gains.begin(), coarse_gains.end(), gain) != coarse_gains.end())
                continue;
              progress.SetLine(1, "Fine gain search");
              am_scan_measurement const current = measure_am(frequency, gain);
              if ((!current.overload && best.overload) ||
                  ((current.overload == best.overload) && current.snr > best.snr))
                best = current;
            }
          }
        }

        if (best.detected)
        {
          connectionpool::handle dbhandle(m_connpool);
          struct channelprops channelprops = {};
          channelprops.frequency = frequency;
          channelprops.modulation = modulation::am;
          channelprops.name = std::to_string(frequency / 1000) + " AM";
          channelprops.autogain = false;
          channelprops.manualgain = best.gain;
          channelprops.freqcorrection = 0;

          bool const exists = channel_exists(dbhandle, channelprops);
          if (exists)
          {
            struct channelprops existing = {};
            get_channel_properties(dbhandle, frequency, modulation::am, existing);
            channelprops.name = existing.name;
            channelprops.logourl = existing.logourl;
            channelprops.freqcorrection = existing.freqcorrection;
          }
          if (exists)
            update_channel(dbhandle, channelprops);
          else
            add_channel(dbhandle, channelprops);
          channels_found++;
        }
      }

      progress.SetPercentage(canceled ? 95 : 100);
      TriggerChannelUpdate();
      TriggerChannelGroupsUpdate();
      kodi::gui::dialogs::OK::ShowAndGetInput(
          canceled ? "AM Radio scan canceled" : "AM Radio scan complete",
          std::to_string(channels_found) + " AM station(s) added or updated.");
      return;
    }

    if (scan_modulation == modulation::wx)
    {
      struct wx_named_channel
      {
        uint32_t frequency = 0;
        std::string name;
      };

      struct wx_scan_measurement
      {
        int gain = 0;
        float best_power = -999.0f;
        float best_snr = -999.0f;
        bool overload = false;
        int reports = 0;
        bool detected = false;
      };

      // WX_SCAN_PROGRESS_DETAILS_HELPER
      auto wx_gain_text = [](int gain) -> std::string
      {
        char buffer[32] = {};
        snprintf(buffer, sizeof(buffer), "%.1f dB",
                 static_cast<double>(gain) / 10.0);
        return std::string(buffer);
      };

      auto wx_measurement_text =
          [&](wx_scan_measurement const& measurement) -> std::string
      {
        char buffer[192] = {};
        snprintf(buffer, sizeof(buffer),
                 "gain=%s power=%.1f snr=%.1f reports=%d%s%s",
                 wx_gain_text(measurement.gain).c_str(),
                 measurement.best_power,
                 measurement.best_snr,
                 measurement.reports,
                 measurement.detected ? " detected" : " no carrier",
                 measurement.overload ? " overload" : "");
        return std::string(buffer);
      };


      std::vector<wx_named_channel> wx_channels;

      {
        connectionpool::handle dbhandle(m_connpool);

        enumerate_namedchannels(dbhandle, modulation::wx,
                                [&](struct namedchannel const& item) -> void
        {
          if ((item.frequency > 0) && (item.name != nullptr))
          {
            wx_named_channel channel;
            channel.frequency = item.frequency;
            channel.name = item.name;
            wx_channels.emplace_back(std::move(channel));
          }
        });
      }

      if (wx_channels.empty())
      {
        kodi::gui::dialogs::OK::ShowAndGetInput(
            "Weather Radio scan",
            "No Weather Radio channels were enumerated from the database.");
        return;
      }

      std::vector<int> valid_gains;
      {
        std::unique_ptr<rtldevice> gain_device = create_device(settings);
        gain_device->get_valid_gains(valid_gains);
      }

      if (valid_gains.empty())
      {
        valid_gains = {
            0, 9, 14, 27, 37, 77, 87, 125, 144, 157,
            166, 197, 207, 229, 254, 280, 297, 328,
            338, 364, 372, 386, 402, 421, 434, 439,
            445, 480, 496};
      }

      auto nearest_valid_gain = [&](int desired) -> int
      {
        int best = valid_gains.front();
        int best_delta = std::abs(best - desired);

        for (int gain : valid_gains)
        {
          int const delta = std::abs(gain - desired);
          if (delta < best_delta)
          {
            best = gain;
            best_delta = delta;
          }
        }

        return best;
      };

      std::vector<int> scan_gains;
      for (int desired : {27, 328, 197, 87})
      {
        int gain = nearest_valid_gain(desired);
        if (std::find(scan_gains.begin(), scan_gains.end(), gain) == scan_gains.end())
          scan_gains.emplace_back(gain);
      }

      auto measure_wx =
          [&](uint32_t frequency, int gain) -> wx_scan_measurement
      {
        wx_scan_measurement measurement;
        measurement.gain = gain;

        struct signalprops signalprops = {};
        signalprops.filter = false;
        signalprops.samplerate = 1600000;
        signalprops.bandwidth = 200000;
        signalprops.lowcut = -8000;
        signalprops.highcut = 8000;
        signalprops.offset = signalprops.samplerate / 4;

        struct signalplotprops plotprops = {};
        plotprops.height = 200;
        plotprops.width = 512;
        plotprops.mindb = -72.0f;
        plotprops.maxdb = 4.0f;

        std::mutex status_mutex;

        std::unique_ptr<signalmeter> meter =
            signalmeter::create(signalprops, plotprops, 200,
                                [&](struct signalmeter::signal_status const& status) -> void
        {
          std::lock_guard<std::mutex> status_lock(status_mutex);

          measurement.reports += 1;
          measurement.overload = measurement.overload || status.overload;

          if (!std::isnan(status.power) && status.power > measurement.best_power)
            measurement.best_power = status.power;

          if (!std::isnan(status.snr) && status.snr > measurement.best_snr)
            measurement.best_snr = status.snr;
        });

        std::unique_ptr<rtldevice> device = create_device(settings);
        device->set_center_frequency(frequency + signalprops.offset);
        device->set_frequency_correction(settings.device_frequency_correction);
        device->set_sample_rate(signalprops.samplerate);
        device->set_automatic_gain_control(false);
        device->set_gain(gain);
        device->begin_stream();

        size_t const buffer_size = 32 KiB;
        std::unique_ptr<uint8_t[]> buffer(new uint8_t[buffer_size]);

        auto const end_time =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2200);

        while (std::chrono::steady_clock::now() < end_time)
        {
          size_t const count = device->read(buffer.get(), buffer_size);
          if (count > 0)
            meter->inputsamples(buffer.get(), count);
        }

        {
          std::lock_guard<std::mutex> status_lock(status_mutex);

          // NOAA Weather Radio is narrowband FM. A real carrier should stand
          // clearly above the nearby noise bins. Keep this intentionally modest
          // so weak but listenable WX stations are not missed.
          measurement.detected =
              measurement.reports >= 2 &&
              measurement.best_snr >= 5.0f &&
              measurement.best_power > -65.0f;
        }

        return measurement;
      };

      int channels_added = 0;
      bool canceled = false;

      kodi::gui::dialogs::CProgress progress;
      progress.SetHeading("Weather Radio scan");
      progress.SetCanCancel(true);
      progress.ShowProgressBar(true);
      progress.SetPercentage(0);
      progress.SetLine(0, "Preparing RTL-SDR tuner...");
      progress.SetLine(1, "Scanning WX1-WX7");
      progress.SetLine(2, "Press Cancel to stop");
      progress.Open();

      for (size_t index = 0; index < wx_channels.size(); ++index)
      {
        wx_named_channel const& wx = wx_channels[index];

        char freq_text[128]{};
        std::snprintf(freq_text,
                      std::extent<decltype(freq_text)>::value,
                      "%s %u.%03u MHz",
                      wx.name.c_str(),
                      wx.frequency / 1000000,
                      (wx.frequency % 1000000) / 1000);

        int const percent =
            static_cast<int>((index * 100) / std::max<size_t>(wx_channels.size(), 1));

        progress.SetPercentage(percent);
        progress.SetLine(0, freq_text);
        progress.SetLine(1, "Measuring signal...");
        progress.SetLine(2, "Press Cancel to stop");

        if (progress.IsCanceled())
        {
          canceled = true;
          break;
        }

        wx_scan_measurement best;

        for (int gain : scan_gains)
        {
          // WX_SCAN_PROGRESS_DETAILS_PER_GAIN
          progress.SetLine(1, "gain=" + wx_gain_text(gain) + " measuring");
          progress.SetLine(2, "Waiting for Weather Radio signal reports");
          wx_scan_measurement measurement = measure_wx(wx.frequency, gain);
          progress.SetLine(1, wx_measurement_text(measurement));
          progress.SetLine(2, measurement.detected ? "Detected candidate" : "No usable carrier");

          kodi::Log(ADDON_LOG_DEBUG,
                    "WX_SCAN freq=%u name='%s' gain=%d reports=%d power=%.1f snr=%.1f overload=%d detected=%d",
                    wx.frequency,
                    wx.name.c_str(),
                    gain,
                    measurement.reports,
                    measurement.best_power,
                    measurement.best_snr,
                    measurement.overload ? 1 : 0,
                    measurement.detected ? 1 : 0);

          if (measurement.best_snr > best.best_snr)
            best = measurement;

          if (measurement.detected)
            break;

          if (progress.IsCanceled())
          {
            canceled = true;
            break;
          }
        }

        if (canceled)
          break;

        if (best.detected)
        {
          connectionpool::handle dbhandle(m_connpool);

          struct channelprops channelprops = {};
          channelprops.frequency = wx.frequency;
          channelprops.modulation = modulation::wx;
          channelprops.name = wx.name;
          channelprops.autogain = false;
          channelprops.manualgain = best.gain;
          channelprops.freqcorrection = 0;

          bool const exists = channel_exists(dbhandle, channelprops);
          if (exists)
          {
            get_channel_properties(dbhandle,
                                   channelprops.frequency,
                                   channelprops.modulation,
                                   channelprops);
            channelprops.name = wx.name;
            channelprops.autogain = false;
            channelprops.manualgain = best.gain;
            update_channel(dbhandle, channelprops);
          }
          else
          {
            add_channel(dbhandle, channelprops);
          }

          channels_added += 1;

          kodi::Log(ADDON_LOG_INFO,
                    "WX_SCAN added freq=%u name='%s' gain=%d snr=%.1f power=%.1f",
                    wx.frequency,
                    wx.name.c_str(),
                    best.gain,
                    best.best_snr,
                    best.best_power);
        }
      }

      progress.SetPercentage(100);

      if (canceled)
      {
        kodi::gui::dialogs::OK::ShowAndGetInput(
            "Weather Radio scan",
            "Scan canceled.",
            "",
            std::to_string(channels_added) + " Weather Radio channel(s) added or updated.");
      }
      else
      {
        kodi::gui::dialogs::OK::ShowAndGetInput(
            "Weather Radio scan",
            "Scan complete.",
            "",
            std::to_string(channels_added) + " Weather Radio channel(s) added or updated.");
      }

      return;
    }

    if (!settings.hdradio_enable)
    {
      kodi::gui::dialogs::OK::ShowAndGetInput(
          "HD Radio scan",
          "HD Radio support is disabled in the add-on settings.");
      return;
    }

    struct gain_scan_result
    {
      int gain = 0;
      bool sync = false;
      bool has_subchannels = false;
      uint64_t bytes = 0;
      muxscanner::multiplex muxdata = {};
    };

    try
    {
      uint32_t const sample_rate = 1488375;
      uint32_t const band_first_frequency =
          hd_am_scan ? hdradio::AM_FIRST_FREQUENCY : hdradio::FM_FIRST_FREQUENCY;
      uint32_t const band_last_frequency =
          hd_am_scan ? hdradio::AM_LAST_FREQUENCY : hdradio::FM_LAST_FREQUENCY;
      uint32_t const first_frequency =
          single_frequency_hd_scan ? selected_single_hd_frequency : band_first_frequency;
      uint32_t const last_frequency =
          single_frequency_hd_scan ? selected_single_hd_frequency : band_last_frequency;
      uint32_t const step_frequency =
          hd_am_scan ? hdradio::AM_STEP_FREQUENCY : hdradio::FM_STEP_FREQUENCY;
      uint32_t const total_frequencies = ((last_frequency - first_frequency) / step_frequency) + 1;
      char const* const frequency_unit = hd_am_scan ? " kHz" : " MHz";

      // Coarse gains based on your measured Seattle gain JSON:
      // high ~= 197, mid ~= 87, low ~= 27, in tenths of dB.
      std::vector<int> coarse_gain_targets = {328, 197, 87, 27};

      auto const coarse_scan_time = std::chrono::seconds(8);
      auto const coarse_minimum_time = std::chrono::seconds(3);

      auto const fine_scan_time = std::chrono::seconds(6);
      auto const fine_minimum_time = std::chrono::seconds(4);

      auto const final_scan_time = std::chrono::seconds(18);
      auto const final_minimum_time = std::chrono::seconds(5);
      auto const subchannel_quiet_time = std::chrono::seconds(4);

      int muxes_found = 0;
      int subchannels_found = 0;
      bool canceled = false;

      kodi::gui::dialogs::CProgress progress;
      progress.SetHeading(
          hd_am_scan
              ? (single_frequency_hd_scan ? "AM HD Radio single-frequency scan"
                                          : "AM HD Radio scan")
              : (single_frequency_hd_scan ? "FM HD Radio single-frequency scan"
                                          : "FM HD Radio scan"));
      progress.SetCanCancel(true);
      progress.ShowProgressBar(true);
      progress.SetPercentage(0);
      progress.SetLine(0, "Preparing RTL-SDR tuner...");
      progress.SetLine(1, hd_am_scan ? "Q-branch direct sampling; manual gain scan"
                                    : "Manual gain scan");
      progress.SetLine(2, "Press Cancel to stop");
      progress.Open();

      auto update_progress =
          [&](int percent,
              std::string const& line0,
              std::string const& line1,
              std::string const& line2) -> bool
      {
        if (percent < 0)
          percent = 0;
        if (percent > 100)
          percent = 100;

        progress.SetPercentage(percent);
        progress.SetLine(0, line0);
        progress.SetLine(1, line1);
        progress.SetLine(2, line2);

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return progress.IsCanceled();
      };

      std::vector<int> valid_gains;

      {
        std::unique_ptr<rtldevice> gain_device = create_device(settings);
        gain_device->get_valid_gains(valid_gains);
      }

      if (valid_gains.empty())
      {
        // Fallback R820T/R820T2-style values, in tenths of dB.
        valid_gains = {
            0, 9, 14, 27, 37, 77, 87, 125, 144, 157,
            166, 197, 207, 229, 254, 280, 297, 328,
            338, 364, 372, 386, 402, 421, 434, 439,
            445, 480, 496};
      }

      std::sort(valid_gains.begin(), valid_gains.end());
      valid_gains.erase(std::unique(valid_gains.begin(), valid_gains.end()), valid_gains.end());

      auto nearest_valid_gain = [&](int target) -> int
      {
        auto best = valid_gains.front();
        auto best_distance = std::abs(best - target);

        for (auto gain : valid_gains)
        {
          auto distance = std::abs(gain - target);
          if (distance < best_distance)
          {
            best = gain;
            best_distance = distance;
          }
        }

        return best;
      };

      std::vector<int> coarse_gains;

      for (auto target : coarse_gain_targets)
        coarse_gains.push_back(nearest_valid_gain(target));

      std::sort(coarse_gains.begin(), coarse_gains.end(), std::greater<int>());
      coarse_gains.erase(std::unique(coarse_gains.begin(), coarse_gains.end()), coarse_gains.end());

      auto quality_score = [](gain_scan_result const& result) -> float
        {
          // TEMP_MER_TIEBREAK_SCORE_MUXDATA_ONLY
          //
          // Lower score is better. Use muxdata because gain_scan_result itself
          // does not have direct ber/mer fields.
          float const ber = result.muxdata.ber_valid ? result.muxdata.cber : 1.0f;
          float const mer = result.muxdata.mer_valid
                                ? (result.muxdata.mer_lower + result.muxdata.mer_upper) / 2.0f
                                : -100.0f;

          // Zero/tiny BER bucket: prefer higher MER.
          if (ber <= 0.0000005f)
            return -std::max(0.0f, mer) * 0.000000001f;

          // Non-zero BER: BER dominates; MER only nudges close ties.
          return ber - (std::max(0.0f, mer) * 0.000000001f);
        };

      auto has_zero_ber = [](gain_scan_result const& result) -> bool
        {
          return result.muxdata.ber_valid && result.muxdata.cber <= 0.0000005f;
        };

      connectionpool::handle dbhandle(m_connpool);

      std::function<gain_scan_result(uint32_t,
                                     int,
                                     std::chrono::seconds,
                                     std::chrono::seconds,
                                     bool,
                                     char const*,
                                     int)> scan_one_gain;

      scan_one_gain =
          [&](uint32_t frequency,
              int gain,
              std::chrono::seconds scan_time,
              std::chrono::seconds minimum_time,
              bool stop_after_sync_only,
              char const* freq_label,
              int percent) -> gain_scan_result
      {
        gain_scan_result result = {};
        result.gain = gain;

        muxscanner::multiplex muxdata = {};
        std::mutex muxlock;

        std::unique_ptr<muxscanner> scanner =
            hdmuxscanner::create(
                sample_rate,
                frequency,
                [&](muxscanner::multiplex const& data) -> void
                {
                  std::lock_guard<std::mutex> guard(muxlock);
                  muxdata = data;
                });

        std::unique_ptr<rtldevice> device = create_device(settings);

        device->set_direct_sampling(hd_am_scan ? 2 : 0);
        device->set_frequency_correction(0);
        device->set_sample_rate(sample_rate);
        device->set_center_frequency(frequency);
        device->set_automatic_gain_control(false);
        device->set_gain(gain);
        device->begin_stream();

        std::atomic<uint64_t> sample_bytes{0};
        std::exception_ptr reader_exception = nullptr;

        std::thread reader_thread([&]() -> void
        {
          try
          {
            device->read_async(
                [&](uint8_t const* samples, size_t count) -> void
                {
                  sample_bytes.fetch_add(static_cast<uint64_t>(count));
                  scanner->inputsamples(samples, count);
                },
                32768);
          }
          catch (...)
          {
            reader_exception = std::current_exception();
          }
        });

        auto const started = std::chrono::steady_clock::now();
        auto const deadline = started + scan_time;
        auto next_ui_update = started + std::chrono::milliseconds(250);

        size_t last_subchannel_count = 0;
        auto last_subchannel_change = started;

        while (std::chrono::steady_clock::now() < deadline)
        {
          auto const now = std::chrono::steady_clock::now();

          bool current_sync = false;
          size_t current_subchannel_count = 0;
          float current_cber = 1.0f;
          bool current_ber_valid = false;

          {
            std::lock_guard<std::mutex> guard(muxlock);
            current_sync = muxdata.sync;
            current_subchannel_count = muxdata.subchannels.size();
            current_ber_valid = muxdata.ber_valid;
            current_cber = muxdata.cber;
          }

          if (current_subchannel_count > last_subchannel_count)
          {
            last_subchannel_count = current_subchannel_count;
            last_subchannel_change = now;

            log_info(__func__,
                     ": ",
                     freq_label,
                     frequency_unit,
                     " gain=",
                     gain,
                     " now has ",
                     current_subchannel_count,
                     " HD subchannel candidate(s)");
          }

          if (now >= next_ui_update)
          {
            std::string quality =
                current_ber_valid
                    ? (std::string("BER ") + std::to_string(current_cber))
                    : std::string("waiting for BER");

            if (update_progress(
                    percent,
                    std::string("Scanning ") + freq_label + frequency_unit + " HD",
                    std::string("gain ") + std::to_string(gain) + " / " + quality,
                    std::to_string(current_subchannel_count) +
                        " candidate HD channel(s), read " +
                        std::to_string(sample_bytes.load() / 1024) +
                        " KiB"))
            {
              canceled = true;
              break;
            }

            next_ui_update = now + std::chrono::milliseconds(250);
          }

          if (current_sync &&
              stop_after_sync_only &&
              (now - started) >= minimum_time)
          {
            break;
          }

          if (current_sync &&
              !stop_after_sync_only &&
              current_subchannel_count > 0 &&
              (now - started) >= minimum_time &&
              (now - last_subchannel_change) >= subchannel_quiet_time)
          {
            break;
          }

          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        device->cancel_async();

        if (reader_thread.joinable())
          reader_thread.join();

        if (reader_exception)
        {
          try
          {
            std::rethrow_exception(reader_exception);
          }
          catch (std::exception& ex)
          {
            log_info(__func__,
                     ": async reader stopped at ",
                     freq_label,
                     frequency_unit,
                     " gain=",
                     gain,
                     ": ",
                     ex.what());
          }
          catch (...)
          {
            log_info(__func__,
                     ": async reader stopped at ",
                     freq_label,
                     frequency_unit,
                     " gain=",
                     gain);
          }
        }

        device.reset();

        {
          std::lock_guard<std::mutex> guard(muxlock);
          result.muxdata = muxdata;
        }

        result.sync = result.muxdata.sync;
        result.has_subchannels = !result.muxdata.subchannels.empty();
        result.bytes = sample_bytes.load();

        log_info(__func__,
                 ": gain test ",
                 freq_label,
                 frequency_unit,
                 " gain=",
                 gain,
                 " sync=",
                 result.sync ? "yes" : "no",
                 " subchannels=",
                 result.muxdata.subchannels.size(),
                 " ber=",
                 result.muxdata.ber_valid ? std::to_string(result.muxdata.cber) : "n/a",
                 " mer=",
                 result.muxdata.mer_valid
                     ? std::to_string((result.muxdata.mer_lower + result.muxdata.mer_upper) / 2.0f)
                     : "n/a",
                 " bytes=",
                 result.bytes);

        return result;
      };

      uint32_t frequency_index = 0;

      for (uint32_t frequency = first_frequency;
           frequency <= last_frequency;
           frequency += step_frequency, ++frequency_index)
      {
        char freq_label[64] = {};
        if (hd_am_scan)
          snprintf(freq_label, sizeof(freq_label), "%u", frequency / 1000);
        else
          snprintf(freq_label,
                   sizeof(freq_label),
                   "%u.%u",
                   frequency / 1000000,
                   (frequency % 1000000) / 100000);

        int percent = static_cast<int>((frequency_index * 100) / total_frequencies);

        if (update_progress(
                percent,
                std::string("Scanning ") + freq_label + frequency_unit + " HD",
                std::to_string(muxes_found) +
                    " station(s), " +
                    std::to_string(subchannels_found) +
                    " HD channel(s) found",
                hd_am_scan ? "Using Q-branch direct sampling"
                           : "Trying high/mid/low manual gain"))
        {
          canceled = true;
          break;
        }

        log_info(__func__,
                 ": scanning ",
                 freq_label,
                 frequency_unit,
                 hd_am_scan ? " HD with direct sampling" : " HD with manual gain search");

        std::vector<gain_scan_result> coarse_locks;
        gain_scan_result locked = {};
        bool found_lock = false;

        for (auto coarse_gain : coarse_gains)
        {
          auto coarse =
              scan_one_gain(frequency,
                            coarse_gain,
                            coarse_scan_time,
                            coarse_minimum_time,
                            true,
                            freq_label,
                            percent);

          if (canceled)
            break;

          if (coarse.sync)
          {
            coarse_locks.push_back(coarse);

            float coarse_score = quality_score(coarse);

            log_info(__func__,
                     ": coarse lock at ",
                     freq_label,
                     frequency_unit,
                     " gain=",
                     coarse_gain,
                     " score=",
                     coarse_score,
                     " ber=",
                     coarse.muxdata.ber_valid ? std::to_string(coarse.muxdata.cber) : "n/a",
                     " subchannels=",
                     coarse.muxdata.subchannels.size());

            if (!found_lock)
            {
              locked = coarse;
              found_lock = true;
            }
            else
            {
              float locked_score = quality_score(locked);

              if (coarse_score < locked_score ||
                  (coarse_score == locked_score &&
                   coarse.muxdata.subchannels.size() > locked.muxdata.subchannels.size()))
              {
                locked = coarse;
              }
            }

            if (has_zero_ber(coarse))
            {
              locked = coarse;
              log_info(__func__,
                       ": accepting HD coarse gain=",
                       coarse.gain,
                       " at ",
                       freq_label,
                       frequency_unit,
                       " after BER reached 0");
              break;
            }
          }
        }

        if (canceled)
          break;

        if (!found_lock)
        {
          log_info(__func__,
                   ": no HD lock at ",
                   freq_label,
                   frequency_unit,
                   hd_am_scan ? " using direct sampling" : " using coarse gains high/mid/low");
          continue;
        }

        // Fine tune around every coarse gain that locked.
        // Use a wider window because the best gain is often at the edge
        // of the original +/-50 search range.
        int const fine_gain_window = 200;

        std::vector<int> fine_gains;

        auto add_fine_window = [&](int center_gain) -> void
        {
          for (auto gain : valid_gains)
          {
            if (std::abs(gain - center_gain) <= fine_gain_window)
              fine_gains.push_back(gain);
          }
        };

        if (!has_zero_ber(locked))
        {
          for (auto const& coarse_lock : coarse_locks)
            add_fine_window(coarse_lock.gain);

          add_fine_window(locked.gain);

          std::sort(fine_gains.begin(), fine_gains.end());
          fine_gains.erase(std::unique(fine_gains.begin(), fine_gains.end()), fine_gains.end());

          log_info(__func__,
                   ": fine tuning ",
                   freq_label,
                   frequency_unit,
                   " using ",
                   fine_gains.size(),
                   " candidate gain(s) within +/-",
                   fine_gain_window,
                   " of coarse lock(s)");
        }
        else
        {
          log_info(__func__,
                   ": skipping fine gain search at ",
                   freq_label,
                   frequency_unit,
                   " because BER reached 0");
        }

        gain_scan_result best = locked;
        float best_score = quality_score(best);

        auto gain_is_better = [&](gain_scan_result const& candidate,
                                  gain_scan_result const& current,
                                  float candidate_score,
                                  float current_score) -> bool
        {
          // TEMP_EXPLICIT_MER_TIEBREAK_MUXDATA_ONLY
          //
          // Prefer more decoded HD subchannels. If subchannel count is the same,
          // BER remains primary. If BER/score is effectively tied, prefer
          // higher MER. Use muxdata only.
          size_t const candidate_subchannels = candidate.muxdata.subchannels.size();
          size_t const current_subchannels = current.muxdata.subchannels.size();

          if (candidate_subchannels != current_subchannels)
            return candidate_subchannels > current_subchannels;

          float const candidate_ber =
              candidate.muxdata.ber_valid ? candidate.muxdata.cber : 1.0f;
          float const current_ber =
              current.muxdata.ber_valid ? current.muxdata.cber : 1.0f;

          float const candidate_mer =
              candidate.muxdata.mer_valid
                  ? (candidate.muxdata.mer_lower + candidate.muxdata.mer_upper) / 2.0f
                  : -100.0f;
          float const current_mer =
              current.muxdata.mer_valid
                  ? (current.muxdata.mer_lower + current.muxdata.mer_upper) / 2.0f
                  : -100.0f;

          if (candidate_ber <= 0.0000005f && current_ber <= 0.0000005f)
            return candidate_mer > current_mer;

          if (std::abs(candidate_score - current_score) <= 0.0000005f ||
              std::abs(candidate_ber - current_ber) <= 0.0000005f)
            return candidate_mer > current_mer;

          return candidate_ber < current_ber;
        };

        for (auto fine_gain : fine_gains)
        {
          auto fine =
              scan_one_gain(frequency,
                            fine_gain,
                            fine_scan_time,
                            fine_minimum_time,
                            true,
                            freq_label,
                            percent);

          if (canceled)
            break;

          if (!fine.sync)
            continue;

          float score = quality_score(fine);

          if (has_zero_ber(fine))
          {
            best = fine;
            best_score = score;
            log_info(__func__,
                     ": accepting HD fine gain=",
                     fine.gain,
                     " at ",
                     freq_label,
                     frequency_unit,
                     " after BER reached 0");
            break;
          }

          if (!best.sync || gain_is_better(fine, best, score, best_score))
          {
            best = fine;
            best_score = score;
          }
        }

        if (canceled)
          break;

        log_info(__func__,
                 ": best gain for ",
                 freq_label,
                 frequency_unit,
                 " is ",
                 best.gain,
                 " score=",
                 best_score,
                 " ber=",
                 best.muxdata.ber_valid ? std::to_string(best.muxdata.cber) : "n/a",
                 " mer=",
                 best.muxdata.mer_valid
                     ? std::to_string((best.muxdata.mer_lower + best.muxdata.mer_upper) / 2.0f)
                     : "n/a");

        // Do the final full scan at the best gain to collect all subchannels.
        auto final =
            scan_one_gain(frequency,
                          best.gain,
                          final_scan_time,
                          final_minimum_time,
                          false,
                          freq_label,
                          percent);

        if (canceled)
          break;

        if (!final.sync || final.muxdata.subchannels.empty())
        {
          log_info(__func__,
                   ": lock existed at ",
                   freq_label,
                   frequency_unit,
                   " but final scan found no usable subchannels at gain=",
                   best.gain);
          continue;
        }

        std::string muxname =
            final.muxdata.name.empty()
                ? std::string(freq_label).append(" HD")
                : final.muxdata.name;

        struct channelprops channelprops = {};
        channelprops.frequency = frequency;
        channelprops.modulation = modulation::hd;
        channelprops.name = muxname;
        channelprops.logourl = "";
        channelprops.autogain = false;
        channelprops.manualgain = best.gain;
        channelprops.freqcorrection = 0;

        std::vector<subchannelprops> subchannels;

        for (auto const& scanned : final.muxdata.subchannels)
        {
          struct subchannelprops subchannel = {};
          subchannel.number = scanned.number;
          subchannel.logourl = "";

          if (!final.muxdata.name.empty())
            subchannel.name = muxname + " " + scanned.name;
          else
            subchannel.name = std::string(freq_label).append(" ").append(scanned.name);

          subchannels.emplace_back(std::move(subchannel));
        }

        bool const exists = channel_exists(dbhandle, channelprops);

        if (exists)
          update_channel(dbhandle, channelprops, subchannels);
        else
          add_channel(dbhandle, channelprops, subchannels);

        muxes_found++;
        subchannels_found += static_cast<int>(subchannels.size());

        update_progress(
            percent,
            std::string("Found ") + muxname,
            std::to_string(muxes_found) +
                " station(s), " +
                std::to_string(subchannels_found) +
                " HD channel(s) found",
            std::string("Best gain ") +
                std::to_string(best.gain) +
                ", BER " +
                (best.muxdata.ber_valid ? std::to_string(best.muxdata.cber) : "n/a"));

        log_info(__func__,
                 ": found ",
                 muxname,
                 " at ",
                 freq_label,
                 frequency_unit,
                 " with ",
                 subchannels.size(),
                 " HD subchannel(s), best_gain=",
                 best.gain,
                 ", ber=",
                 final.muxdata.ber_valid ? std::to_string(final.muxdata.cber) : "n/a");
      }

      progress.SetPercentage(canceled ? static_cast<int>((frequency_index * 100) / total_frequencies) : 100);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));

      TriggerChannelUpdate();
      TriggerChannelGroupsUpdate();

      if (canceled)
      {
        kodi::gui::dialogs::OK::ShowAndGetInput(
            "HD Radio scan canceled",
            std::to_string(muxes_found) + " HD station(s) found before cancel.",
            "",
            std::to_string(subchannels_found) + " HD subchannel(s) added or updated.");
      }
      else
      {
        kodi::gui::dialogs::OK::ShowAndGetInput(
            "HD Radio scan complete",
            std::to_string(muxes_found) + " HD station(s) found.",
            "",
            std::to_string(subchannels_found) + " HD subchannel(s) added or updated.");
      }
    }
    catch (std::exception& ex)
    {
      handle_stdexception(__func__, ex);

      kodi::gui::dialogs::OK::ShowAndGetInput(
          "HD Radio scan failed",
          "An error occurred during automatic HD Radio scan:",
          "",
          ex.what());
    }
    catch (...)
    {
      handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
    }
  }).detach();

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::OpenDialogChannelSettings (CInstancePVRClient)
//
// Show the channel settings dialog
//
// Arguments:
//
//	channel		- The channel to show the dialog for

PVR_ERROR addon::OpenDialogChannelSettings(kodi::addon::PVRChannel const& channel)
{
  // Prevent manipulation of the PVR stream (OpenLiveStream/CloseLiveStream)
  std::unique_lock<std::mutex> lock(m_pvrstream_lock);

  // The channel settings dialog can't be shown when there is an active stream
  if (m_pvrstream)
  {

    // TODO: This message is terrible
    kodi::gui::dialogs::OK::ShowAndGetInput(
        kodi::addon::GetLocalizedString(30405),
        "Modifying PVR Radio channel settings requires "
        "exclusive access to the connected RTL-SDR tuner device.",
        "", "Active playback of PVR Radio streams must be stopped before continuing.");

    return PVR_ERROR::PVR_ERROR_NO_ERROR;
  }

  // Create a copy of the current addon settings structure
  struct settings settings = copy_settings();

  try
  {

    // Set up the tuner device properties
    struct tunerprops tunerprops = {};
    tunerprops.freqcorrection = settings.device_frequency_correction;

    channelid channelid(channel.GetUniqueId()); // Convert UniqueID back into a channelid

    // Get the properties of the channel to be manipulated
    struct channelprops channelprops = {};
    if (!get_channel_properties(connectionpool::handle(m_connpool), channelid.frequency(),
                                channelid.modulation(), channelprops))
      throw string_exception("Unable to retrieve properties for channel ",
                             channel.GetChannelName().c_str());

    // Create and initialize the dialog box against a new signal meter instance
    std::unique_ptr<channelsettings> dialog =
        channelsettings::create(create_device(settings), tunerprops, channelprops, false);
    dialog->DoModal();

    if (dialog->get_dialog_result())
    {

      // Retrieve the updated channel properties from the dialog box and persist them
      dialog->get_channel_properties(channelprops);
      update_channel(connectionpool::handle(m_connpool), channelprops);
    }
  }

  catch (std::exception& ex)
  {

    // Log the error and inform the user that the operation failed, do not return an error code
    handle_stdexception(__func__, ex);
    kodi::gui::dialogs::OK::ShowAndGetInput(kodi::addon::GetLocalizedString(30407),
                                            "An error occurred displaying the "
                                            "channel settings dialog:",
                                            "", ex.what());
  }

  catch (...)
  {
    return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::OpenLiveStream (CInstancePVRClient)
//
// Open a live stream on the backend
//
// Arguments:
//
//	channel		- Channel of the live stream to be opened

bool addon::OpenLiveStream(kodi::addon::PVRChannel const& channel)
{








  // Prevent race condition with GetSignalStatus()
  std::unique_lock<std::mutex> lock(m_pvrstream_lock);

  // Create a copy of the current addon settings structure
  struct settings settings = copy_settings();

  try
  {

    // Set up the tuner device properties
    struct tunerprops tunerprops = {};
    tunerprops.freqcorrection = settings.device_frequency_correction;

    channelid channelid(channel.GetUniqueId()); // Convert UniqueID back into a channelid

    // Retrieve the tuning properties for the channel from the database
    struct channelprops channelprops = {};
    if (!get_channel_properties(connectionpool::handle(m_connpool), channelid.frequency(),
                                channelid.modulation(), channelprops))
      throw string_exception("channel ", channel.GetUniqueId(), " (",
                             channel.GetChannelName().c_str(), ") was not found in the database");

    // FM Radio
    //
    if ((channelprops.modulation == modulation::fm) ||
        (channelprops.modulation == modulation::am))
    {

      // Set up the FM digital signal processor properties
      struct fmprops fmprops = {};
      fmprops.decoderds = (channelprops.modulation == modulation::fm) &&
                          settings.fmradio_enable_rds;
      fmprops.isnorthamerica = is_region_northamerica(settings);
      fmprops.samplerate = settings.fmradio_sample_rate;
      fmprops.downsamplequality = static_cast<int>(settings.fmradio_downsample_quality);
      fmprops.outputrate = settings.fmradio_output_samplerate;
      fmprops.outputgain = settings.fmradio_output_gain;

      // Log information about the stream for diagnostic purposes
      log_info(__func__, ": Creating ",
               (channelprops.modulation == modulation::am) ? "AM" : "FM",
               " stream for channel \"", channelprops.name, "\"");
      log_info(__func__, ": tunerprops.freqcorrection = ", tunerprops.freqcorrection, " PPM");
      log_info(__func__, ": fmprops.decoderds = ", (fmprops.decoderds) ? "true" : "false");
      log_info(__func__,
               ": fmprops.isnorthamerica = ", (fmprops.isnorthamerica) ? "true" : "false");
      log_info(__func__, ": fmrops.samplerate = ", fmprops.samplerate, " Hz");
      log_info(__func__, ": fmprops.downsamplequality = ",
               downsample_quality_to_string(
                   static_cast<enum downsample_quality>(fmprops.downsamplequality)));
      log_info(__func__, ": fmprops.outputgain = ", fmprops.outputgain, " dB");
      log_info(__func__, ": fmprops.outputrate = ", fmprops.outputrate, " Hz");
      log_info(__func__, ": channelprops.frequency = ", channelprops.frequency, " Hz");
      log_info(__func__, ": channelprops.autogain = ", (channelprops.autogain) ? "true" : "false");
      log_info(__func__, ": channelprops.manualgain = ", channelprops.manualgain / 10, " dB");
      log_info(__func__, ": channelprops.freqcorrection = ", channelprops.freqcorrection, " PPM");

      // Create the FM Radio stream
      m_pvrstream = fmstream::create(create_device(settings), tunerprops, channelprops, fmprops);
    }

    // HD Radio
    //
    else if (channelprops.modulation == modulation::hd)
    {

      // Set up the HD Radio digital signal processor properties
      struct hdprops hdprops = {};
      hdprops.outputgain = settings.hdradio_output_gain;

      // Log information about the stream for diagnostic purposes
      log_info(__func__, ": Creating hdstream for channel \"", channelprops.name, "\"");
      log_info(__func__, ": subchannel = ", channelid.subchannel());
      log_info(__func__, ": tunerprops.freqcorrection = ", tunerprops.freqcorrection, " PPM");
      log_info(__func__, ": hdprops.outputgain = ", hdprops.outputgain, " dB");
      log_info(__func__, ": channelprops.frequency = ", channelprops.frequency, " Hz");
      log_info(__func__, ": channelprops.autogain = ", (channelprops.autogain) ? "true" : "false");
      log_info(__func__, ": channelprops.manualgain = ", channelprops.manualgain / 10, " dB");
      log_info(__func__, ": channelprops.freqcorrection = ", channelprops.freqcorrection, " PPM");

      // Create the HD Radio stream
      m_pvrstream = hdstream::create(create_device(settings), tunerprops, channelprops, hdprops,
                                     channelid.subchannel());
    }

    // DAB
    //
    else if (channelprops.modulation == modulation::dab)
    {

      // Set up the DAB digital signal processor properties
      struct dabprops dabprops = {};
      dabprops.outputgain = settings.dabradio_output_gain;
      dabprops.coarse_corrector = settings.dabradio_coarse_corrector;
      dabprops.coarse_corrector_type = settings.dabradio_coarse_corrector_type;

      // Log information about the stream for diagnostic purposes
      log_info(__func__, ": Creating dabstream for channel \"", channelprops.name, "\"");
      log_info(__func__, ": subchannel = ", channelid.subchannel());
      log_info(__func__, ": tunerprops.freqcorrection = ", tunerprops.freqcorrection, " PPM");
      log_info(__func__, ": dabrops.outputgain = ", dabprops.outputgain, " dB");
      log_info(__func__, ": dabrops.coarse_corrector = ", dabprops.coarse_corrector);
      log_info(__func__, ": dabrops.coarse_corrector_type = ", dabprops.coarse_corrector_type);
      log_info(__func__, ": channelprops.frequency = ", channelprops.frequency, " Hz");
      log_info(__func__, ": channelprops.autogain = ", (channelprops.autogain) ? "true" : "false");
      log_info(__func__, ": channelprops.manualgain = ", channelprops.manualgain / 10, " dB");
      log_info(__func__, ": channelprops.freqcorrection = ", channelprops.freqcorrection, " PPM");

      // Create the DAB stream
      m_pvrstream = dabstream::create(create_device(settings), tunerprops, channelprops, dabprops,
                                      channelid.subchannel());
    }

    // Weather Radio
    //
    else if (channelprops.modulation == modulation::wx)
    {

      // Set up the FM digital signal processor properties
      struct wxprops wxprops = {};
      wxprops.samplerate = settings.wxradio_sample_rate;
      wxprops.outputrate = settings.wxradio_output_samplerate;
      wxprops.outputgain = settings.wxradio_output_gain;

      // Log information about the stream for diagnostic purposes
      log_info(__func__, ": Creating wxstream for channel \"", channelprops.name, "\"");
      log_info(__func__, ": tunerprops.freqcorrection = ", tunerprops.freqcorrection, " PPM");
      log_info(__func__, ": wxprops.samplerate = ", wxprops.samplerate, " Hz");
      log_info(__func__, ": wxprops.outputgain = ", wxprops.outputgain, " dB");
      log_info(__func__, ": wxprops.outputrate = ", wxprops.outputrate, " Hz");
      log_info(__func__, ": channelprops.frequency = ", channelprops.frequency, " Hz");
      log_info(__func__, ": channelprops.autogain = ", (channelprops.autogain) ? "true" : "false");
      log_info(__func__, ": channelprops.manualgain = ", channelprops.manualgain / 10, " dB");
      log_info(__func__, ": channelprops.freqcorrection = ", channelprops.freqcorrection, " PPM");

      // Create the Weather Radio stream
      m_pvrstream = wxstream::create(create_device(settings), tunerprops, channelprops, wxprops);
    }

    else
      throw string_exception("channel ", channel.GetUniqueId(), " (",
                             channel.GetChannelName().c_str(), ") has an unknown modulation type");
  }

  // Queue a notification for the user when a live stream cannot be opened, don't just silently log it
  catch (std::exception& ex)
  {

    kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, "Live Stream creation failed (%s).",
                                     ex.what());
    return handle_stdexception(__func__, ex, false);
  }

  catch (...)
  {
    return handle_generalexception(__func__, false);
  }

  return true;
}

//-----------------------------------------------------------------------------
// addon::ReadLiveStream (CInstancePVRClient)
//
// Read from an open live stream
//
// Arguments:
//
//	buffer		- The buffer to store the data in
//	size		- The number of bytes to read into the buffer

int addon::ReadLiveStream(unsigned char* /*buffer*/, unsigned int /*size*/)
{
  return -1;
}

//-----------------------------------------------------------------------------
// addon::RenameChannel (CInstancePVRClient)
//
// Renames a channel on the backend
//
// Arguments:
//
//	channel		- Channel to be renamed

PVR_ERROR addon::RenameChannel(kodi::addon::PVRChannel const& channel)
{
  channelid channelid(channel.GetUniqueId()); // Convert UniqueID back into a channelid

  try
  {
    rename_channel(connectionpool::handle(m_connpool), channelid.frequency(),
                   channelid.modulation(), channel.GetChannelName().c_str());
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED);
  }
  catch (...)
  {
    return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED);
  }

  return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::SeekLiveStream (CInstancePVRClient)
//
// Seek in a live stream on a backend that supports timeshifting
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

int64_t addon::SeekLiveStream(int64_t position, int whence)
{
  try
  {
    return (m_pvrstream) ? m_pvrstream->seek(position, whence) : -1;
  }
  catch (std::exception& ex)
  {
    return handle_stdexception(__func__, ex, -1);
  }
  catch (...)
  {
    return handle_generalexception(__func__, -1);
  }
}

//---------------------------------------------------------------------------

#pragma warning(pop)
