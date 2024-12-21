/*
 *  Copyright (C) 2005-2023 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "pvrclient-nextpvr.h"

#include "BackendRequest.h"
#include "utilities/XMLUtils.h"
#include "kodi/General.h"
#include <kodi/Network.h>

#include <ctime>
#include <memory>
#include <stdio.h>
#include <stdlib.h>

#include <kodi/tools/StringUtils.h>

using namespace NextPVR::utilities;
#include <algorithm>

const char SAFE[256] = {
    /*      0 1 2 3  4 5 6 7  8 9 A B  C D E F */
    /* 0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 1 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 2 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 3 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,

    /* 4 */ 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 5 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
    /* 6 */ 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 7 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,

    /* 8 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 9 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* A */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* B */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

    /* C */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* D */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* E */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* F */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

std::string UriEncode(const std::string sSrc)
{
  const char DEC2HEX[16 + 1] = "0123456789ABCDEF";
  const unsigned char* pSrc = (const unsigned char*)sSrc.c_str();
  const int SRC_LEN = sSrc.length();
  unsigned char* const pStart = new unsigned char[SRC_LEN * 3];
  unsigned char* pEnd = pStart;
  const unsigned char* const SRC_END = pSrc + SRC_LEN;

  for (; pSrc < SRC_END; ++pSrc)
  {
    if (SAFE[*pSrc])
    {
      *pEnd++ = *pSrc;
    }
    else
    {
      // escape this char
      *pEnd++ = '%';
      *pEnd++ = DEC2HEX[*pSrc >> 4];
      *pEnd++ = DEC2HEX[*pSrc & 0x0F];
    }
  }

  std::string sResult((char*)pStart, (char*)pEnd);
  delete[] pStart;
  return sResult;
}

#define SLOW_CONNECT_POLL 60
#define FAST_CONNECT_POLL 5

/* needs extra time to start of zeroconf tuner connection fails
* this timeout is 60 seconds so wait a short time afterwards */

#define FAST_SLOW_POLL_TRANSITION 65

/************************************************************/
/** Class interface */

cPVRClientNextPVR::cPVRClientNextPVR(const CNextPVRAddon& base, const kodi::addon::IInstanceInfo& instance, bool first) :
  kodi::addon::CInstancePVRClient(instance),
  m_base(base),
  m_settings(new InstanceSettings(*this, instance, first)),
  m_request(m_settings),
  m_channels(m_settings, m_request),
  m_timers(m_settings, m_request, m_channels, *this),
  m_recordings(m_settings, m_request, m_timers, m_channels,m_genreMapper, *this),
  m_menuhook(m_settings, m_recordings, m_channels, *this),
  m_genreMapper(m_settings),
  m_epg(m_settings, m_request, m_recordings, m_channels, m_genreMapper)
{
  if (!kodi::vfs::DirectoryExists(m_settings->m_instanceDirectory))
  {
    // check new installation of the first instance, upgrades will migrate
    if (first && !kodi::vfs::FileExists("special://profile/addon_data/pvr.nextpvr/settings.xml"))
    {
      m_request.OneTimeSetup();
    }
    kodi::vfs::CreateDirectory(m_settings->m_instanceDirectory);
  }

  m_bConnected = false;
  m_supportsLiveTimeshift = false;
  m_lastRecordingUpdateTime = std::numeric_limits<time_t>::max(); // time of last recording check - force forever
  m_timeshiftBuffer = new timeshift::DummyBuffer(m_settings, m_request);
  m_realTimeBuffer = new timeshift::DummyBuffer(m_settings, m_request);
  m_livePlayer = nullptr;
  m_nowPlaying = NotPlaying;
  m_running = true;
  m_thread = std::thread([&] { Process(); });
}

cPVRClientNextPVR::~cPVRClientNextPVR()
{
  if (m_nowPlaying != NotPlaying)
  {
    // this is likley only needed for transcoding but include all cases
    if (m_nowPlaying == Recording)
    {
      std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
      std::map<int64_t, timeshift::RecordingBuffer*>::iterator  itr = m_multistreamRecording.begin();
      while (itr != m_multistreamRecording.end())
      {
        CloseRecordedStream(itr->first);
        itr = m_multistreamRecording.begin();
      }
    }
    else
      CloseLiveStream();
  }

  m_running = false;
  if (m_thread.joinable())
    m_thread.join();

  kodi::Log(ADDON_LOG_DEBUG, "->~cPVRClientNextPVR()");
  if (m_bConnected)
    Disconnect();
  delete m_timeshiftBuffer;
  delete m_realTimeBuffer;
  m_recordings.m_hostFilenames.clear();
  m_channels.m_channelDetails.clear();
  m_channels.m_liveStreams.clear();
}

ADDON_STATUS cPVRClientNextPVR::Connect(bool sendWOL)
{
  m_bConnected = false;
  ADDON_STATUS status = ADDON_STATUS_UNKNOWN;
  // initiate session
  if (sendWOL)
    SendWakeOnLan();

  if (m_connectionState == PVR_CONNECTION_STATE_UNKNOWN)
    SetConnectionState(PVR_CONNECTION_STATE_CONNECTING);

  m_request.ClearSID();
  tinyxml2::XMLDocument doc;
  if (m_firstSessionInitiate == 0)
    m_firstSessionInitiate = time(nullptr);
  if (m_request.DoMethodRequest("session.initiate&ver=1.0&device=xbmc", doc) == tinyxml2::XML_SUCCESS)
  {
    std::string salt;
    std::string sid;
    if (XMLUtils::GetString(doc.RootElement(), "salt", salt) && XMLUtils::GetString(doc.RootElement(), "sid", sid))
    {
      // a bit of debug
      kodi::Log(ADDON_LOG_DEBUG, "session.initiate returns: sid=%s salt=%s", sid.c_str(), salt.c_str());
      std::string pinMD5 = kodi::GetMD5(m_settings->m_PIN);
      kodi::tools::StringUtils::ToLower(pinMD5);

      // calculate combined MD5
      std::string combinedMD5;
      combinedMD5.append(":");
      combinedMD5.append(pinMD5);
      combinedMD5.append(":");
      combinedMD5.append(salt);

      // get digest
      std::string md5 = kodi::GetMD5(combinedMD5);

      // login session
      std::string loginResponse;
      std::string request = kodi::tools::StringUtils::Format("session.login&sid=%s&md5=%s", sid.c_str(), md5.c_str());
      doc.Clear();
      if (m_request.DoMethodRequest(request, doc) == tinyxml2::XML_SUCCESS)
      {
        m_request.SetSID(sid);
        doc.Clear();
        if (m_request.DoMethodRequest("setting.list", doc) == tinyxml2::XML_SUCCESS)
        {
          if (m_settings->ReadBackendSettings(doc) != ADDON_STATUS_OK)
          {
            m_request.DoActionRequest("session.logout");
            SetConnectionState(PVR_CONNECTION_STATE_VERSION_MISMATCH, kodi::addon::GetLocalizedString(30050));
            status = ADDON_STATUS_PERMANENT_FAILURE;
            return status;
          }
        }
        // set additional options based on the backend
        ConfigurePostConnectionOptions();
        m_settings->SetConnection(true);
        kodi::Log(ADDON_LOG_DEBUG, "session.login successful");
        status = ADDON_STATUS_OK;
        // don't notify core could be before addon is created
        m_bConnected = true;
        SetConnectionState(PVR_CONNECTION_STATE_CONNECTED);
      }
      else
      {
        kodi::Log(ADDON_LOG_DEBUG, "session.login failed");
        SetConnectionState(PVR_CONNECTION_STATE_ACCESS_DENIED, kodi::addon::GetLocalizedString(30052));
        status = ADDON_STATUS_PERMANENT_FAILURE;
      }
    }
  }
  else
  {
    if (m_settings->m_connectionConfirmed || !m_settings->m_instancePriority)
    {
      status = ADDON_STATUS_OK;
      // backend should continue to connnect and ignore client until reachable
      UpdateServerCheck();
      m_connectionState = PVR_CONNECTION_STATE_SERVER_UNREACHABLE;
    }
    else
    {
      status = ADDON_STATUS_PERMANENT_FAILURE;
    }
  }

  return status;
}

void cPVRClientNextPVR::UpdateServerCheck()
{
  if (time(nullptr) > m_firstSessionInitiate + FAST_SLOW_POLL_TRANSITION)
    m_nextServerCheck = time(nullptr) + SLOW_CONNECT_POLL;
  else
    m_nextServerCheck = time(nullptr) + FAST_CONNECT_POLL;
}


void cPVRClientNextPVR::ResetConnection()
{
  m_nextServerCheck = 0;
  m_connectionState = PVR_CONNECTION_STATE_DISCONNECTED;
  m_bConnected = false;
}

void cPVRClientNextPVR::Disconnect()
{
  if (m_bConnected)
    m_request.DoActionRequest("session.logout");
  if (m_settings->CheckInstanceSettings())
  {
    SetConnectionState(PVR_CONNECTION_STATE_DISCONNECTED);
  }
  m_bConnected = false;
}

void cPVRClientNextPVR::ConfigurePostConnectionOptions()
{
  m_settings->SetVersionSpecificSettings();
  if (m_settings->m_liveStreamingMethod != eStreamingMethod::RealTime)
  {
    delete m_timeshiftBuffer;
    m_supportsLiveTimeshift = true;

    if (m_settings->m_liveStreamingMethod == eStreamingMethod::Transcoded && m_settings->m_transcodedTimeshift)
    {
      std::string version;
      bool enabled;
      const std::string addonName = "inputstream.ffmpegdirect";

      if (kodi::IsAddonAvailable(addonName, version, enabled))
      {
        if (!enabled)
        {
          kodi::Log(ADDON_LOG_INFO, "%s installed but not enabled at startup", addonName.c_str());
          kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, kodi::addon::GetLocalizedString(30191).c_str(), addonName.c_str());
        }
      }
      else // Not installed
      {
        kodi::Log(ADDON_LOG_INFO, "%s not installed", addonName.c_str());
        kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, kodi::addon::GetLocalizedString(30192).c_str(), addonName.c_str());
      }
    }

    if (m_settings->m_liveStreamingMethod == eStreamingMethod::Transcoded)
    {
      m_supportsLiveTimeshift = false;
      m_timeshiftBuffer = new timeshift::TranscodedBuffer(m_settings, m_request);
    }
    else if (m_settings->m_liveStreamingMethod == eStreamingMethod::ClientTimeshift)
    {
      m_timeshiftBuffer = new timeshift::ClientTimeShift(m_settings, m_request);
    }
  }

  if (m_settings->m_useLiveStreams)
      m_channels.LoadLiveStreams();

  if (m_lastEPGUpdateTime == 0)
    m_request.GetLastUpdate("system.epg.summary", m_lastEPGUpdateTime);

  m_channels.CacheAllChannels(m_lastEPGUpdateTime);
}

/* IsUp()
 * \brief   Check if we have a valid session to nextpvr
 * \return  True when a session is active
 */
bool cPVRClientNextPVR::IsUp()
{
  // check time since last time Recordings were updated, update if it has been awhile
  if (m_bConnected == true)
  {
    if (m_nowPlaying == NotPlaying && m_lastRecordingUpdateTime != std::numeric_limits<time_t>::max() && time(nullptr) > (m_lastRecordingUpdateTime + m_settings->m_heartbeatInterval))
    {
      time_t update_time;
      if (m_request.GetLastUpdate("recording.lastupdated", update_time) == tinyxml2::XML_SUCCESS)
      {
        if (m_connectionState == PVR_CONNECTION_STATE_SERVER_UNREACHABLE)
        {
          // one time failure resolved do nothing
          m_connectionState = PVR_CONNECTION_STATE_CONNECTED;
        }
        if (update_time > m_lastRecordingUpdateTime)
        {
          m_lastRecordingUpdateTime = std::numeric_limits<time_t>::max();
          time_t lastUpdate;
          if (m_request.GetLastUpdate("system.epg.summary", lastUpdate) == tinyxml2::XML_SUCCESS)
          {
            if (lastUpdate > m_lastEPGUpdateTime)
            {
              // trigger EPG updates for all channels with a guide source
              kodi::Log(ADDON_LOG_DEBUG, "Trigger EPG update start");
              int channels = 0;
              for (const auto& updateChannel : m_channels.m_channelDetails)
              {
                if (updateChannel.second.first == false)
                {
                  channels++;
                  TriggerEpgUpdate(updateChannel.first);
                }
              }
              kodi::Log(ADDON_LOG_DEBUG, "Triggered %d channel updates", channels);

              m_lastEPGUpdateTime = lastUpdate;
              m_lastRecordingUpdateTime = update_time;
              return m_bConnected;
            }
          }
          if (m_settings->m_accessLevel | ACCESS_NONE)
          {
            if (update_time <= m_timers.m_lastTimerUpdateTime + 1)
            {
              // we already updated this one in Kodi
              m_lastRecordingUpdateTime = time(nullptr);
              return m_bConnected;
            }
            if (m_request.GetLastUpdate("recording.lastupdated&ignore_resume=true", lastUpdate) == tinyxml2::XML_SUCCESS)
            {
              if (lastUpdate <= m_timers.m_lastTimerUpdateTime)
              {
                if (m_settings->m_backendResume)
                {
                  // only resume position changed
                  m_recordings.GetRecordingsLastPlayedPosition();
                  m_lastRecordingUpdateTime = update_time;
                }
                return m_bConnected;
              }
            }
            TriggerRecordingUpdate();
            if (m_settings->m_accessLevel & ACCESS_TIMERS)
              TriggerTimerUpdate();
          }
          else
          {
            m_lastRecordingUpdateTime = time(nullptr);
          }
        }
        else
        {
          m_lastRecordingUpdateTime = time(nullptr);
        }
      }
      else
      {
        if (m_connectionState == PVR_CONNECTION_STATE_CONNECTED)
        {
          if (m_settings->m_heartbeatInterval == DEFAULT_HEARTBEAT)
          {
          // allow a one time retry in 60 seconds for a default check
            m_connectionState = PVR_CONNECTION_STATE_SERVER_UNREACHABLE;
            m_lastRecordingUpdateTime = time(nullptr);
          }
          else
          {
            SetConnectionState(PVR_CONNECTION_STATE_DISCONNECTED);
            UpdateServerCheck();
            m_bConnected = false;
          }
        }
        else if (m_connectionState == PVR_CONNECTION_STATE_SERVER_UNREACHABLE)
        {
          SetConnectionState(PVR_CONNECTION_STATE_DISCONNECTED);
          UpdateServerCheck();
          m_bConnected = false;
        }
      }
    }
    else if (m_nowPlaying != NotPlaying)
    {
      m_request.RenewSID();
      if (m_nowPlaying == Transcoding)
      {
        if (m_livePlayer->IsRealTimeStream() == false)
        {
          //m_livePlayer->Close();
          m_nowPlaying = NotPlaying;
          m_livePlayer = nullptr;
        }
      }
    }
  }
  else if (m_connectionState == PVR_CONNECTION_STATE_SERVER_UNREACHABLE || m_connectionState == PVR_CONNECTION_STATE_DISCONNECTED)
  {
    if (time(nullptr) > m_nextServerCheck)
    {
      Connect(false);
      if (m_coreState == PVR_CONNECTION_STATE_CONNECTING || m_coreState == PVR_CONNECTION_STATE_DISCONNECTED && (time(nullptr) > m_firstSessionInitiate + FAST_SLOW_POLL_TRANSITION))
        SetConnectionState(PVR_CONNECTION_STATE_SERVER_UNREACHABLE);
    }
  }
  return m_bConnected;
}

void cPVRClientNextPVR::Process()
{
  while (m_running)
  {
    IsUp();
    if (m_settings->m_heartbeatInterval == DEFAULT_HEARTBEAT)
      std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    else
      std::this_thread::sleep_for(std::chrono::seconds(10));
  }
}

PVR_ERROR cPVRClientNextPVR::OnSystemSleep()
{
  m_bConnected = false;
  m_lastRecordingUpdateTime = std::numeric_limits<time_t>::max();
  m_nextServerCheck = std::numeric_limits<time_t>::max();
  m_connectionState = PVR_CONNECTION_STATE_DISCONNECTED;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR cPVRClientNextPVR::OnSystemWake()
{
  m_firstSessionInitiate = time(nullptr);
  m_nextServerCheck = m_firstSessionInitiate + FAST_SLOW_POLL_TRANSITION;
  kodi::Log(ADDON_LOG_DEBUG, "NextPVR wake");
  // allow time for core to reset
  m_lastRecordingUpdateTime = time(nullptr) + SLOW_CONNECT_POLL;

  if (m_request.IsActiveSID() && m_request.PingBackend())
  {
    m_connectionState = PVR_CONNECTION_STATE_CONNECTED;
    m_bConnected = true;
    return PVR_ERROR_NO_ERROR;
  }
  // Core only allows PVR_CONNECTION_STATE_CONNECTING once
  SetConnectionState(PVR_CONNECTION_STATE_DISCONNECTED);
  m_connectionState = PVR_CONNECTION_STATE_CONNECTING;

  if (Connect() != ADDON_STATUS_OK)
  {
    SetConnectionState(PVR_CONNECTION_STATE_ACCESS_DENIED);
    return PVR_ERROR_SERVER_ERROR;
  }

  kodi::Log(ADDON_LOG_INFO, "On NextPVR Wake %d %d", m_bConnected, m_connectionState);
  return PVR_ERROR_NO_ERROR;
}

void cPVRClientNextPVR::SendWakeOnLan()
{
  if (m_settings->m_enableWOL == true)
  {
    if (kodi::network::IsLocalHost(m_settings->m_hostname) || !kodi::network::IsHostOnLAN(m_settings->m_hostname, true))
    {
      return;
    }
    int count = 0;
    for (; count < m_settings->m_timeoutWOL; count++)
    {
      if (m_request.PingBackend())
      {
        return;
      }
      kodi::network::WakeOnLan(m_settings->m_hostMACAddress);
      kodi::Log(ADDON_LOG_DEBUG, "WOL sent %d", count);
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
  }
}

void cPVRClientNextPVR::SetConnectionState(PVR_CONNECTION_STATE state, std::string displayMessage)
{
  ConnectionStateChange("", state, displayMessage);
  if (state == PVR_CONNECTION_STATE_CONNECTED && m_coreState != PVR_CONNECTION_STATE_UNKNOWN)
    TriggerChannelGroupsUpdate();
  m_connectionState = state;
  m_coreState = state;
}

/************************************************************/
/** General handling */

// Used among others for the server name string in the "Recordings" view
PVR_ERROR cPVRClientNextPVR::GetBackendName(std::string& name)
{
  name = "NextPVR:" + m_settings->m_instanceName;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR cPVRClientNextPVR::GetBackendHostname(std::string& hostname)
{
  hostname = m_settings->m_hostname;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR cPVRClientNextPVR::GetBackendVersion(std::string& version)
{
  if (m_bConnected)
    version = std::to_string(m_settings->m_backendVersion);
  else
    version = kodi::addon::GetLocalizedString(13205);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR cPVRClientNextPVR::GetConnectionString(std::string& connection)
{
  connection = m_settings->m_hostname;
  if (!m_bConnected)
    connection += ": " + kodi::addon::GetLocalizedString(15208);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR cPVRClientNextPVR::GetDriveSpace(uint64_t& total, uint64_t& used)
{
  if (!m_bConnected)
  {
    total = 0;
    used = 0;
    return PVR_ERROR_NO_ERROR;
  }
  return m_recordings.GetDriveSpace(total, used);
}

PVR_ERROR cPVRClientNextPVR::GetChannelStreamProperties(const kodi::addon::PVRChannel& channel, PVR_SOURCE source, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  bool liveStream = m_channels.IsChannelAPlugin(channel.GetUniqueId());
  if (liveStream)
  {
    properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, m_channels.m_liveStreams[channel.GetUniqueId()]);
    properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");
    return PVR_ERROR_NO_ERROR;
  }
  else if (m_settings->m_liveStreamingMethod == Transcoded && !channel.GetIsRadio())
  {
    if (m_livePlayer != nullptr)
    {
      m_livePlayer->Close();
      m_nowPlaying = NotPlaying;
      m_livePlayer = nullptr;
    }
    const std::string line = kodi::tools::StringUtils::Format("%s/service?method=channel.transcode.m3u8&sid=%s", m_settings->m_urlBase, m_request.GetSID());
    m_livePlayer = m_timeshiftBuffer;
    m_livePlayer->Channel(channel.GetUniqueId());
    if (m_livePlayer->Open(line))
    {
      m_nowPlaying = Transcoding;
    }
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "Transcoding Error");
      return PVR_ERROR_FAILED;
    }
    if (m_settings->m_transcodedTimeshift)
    {
      properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
      properties.emplace_back("inputstream.ffmpegdirect.stream_mode", "timeshift");
      properties.emplace_back("inputstream.ffmpegdirect.manifest_type", "hls");
    }
    properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, line);
    properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");
    properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/x-mpegURL");
    return PVR_ERROR_NO_ERROR;
  }
  return PVR_ERROR_NOT_IMPLEMENTED;
}

/************************************************************/
/** Live stream handling */
bool cPVRClientNextPVR::OpenLiveStream(const kodi::addon::PVRChannel& channel)
{
  if (!m_bConnected && !m_settings->m_enableWOL)
  {
    m_nextServerCheck = std::numeric_limits<time_t>::max();
    Connect(true);
    if (m_bConnected)
    {
      SetConnectionState(PVR_CONNECTION_STATE_CONNECTED);
    }
  }

  std::string line;
  if (channel.GetIsRadio() == false)
  {
    m_nowPlaying = TV;
  }
  else
  {
    m_nowPlaying = Radio;
  }
  if (m_channels.m_liveStreams.count(channel.GetUniqueId()) != 0)
  {
    line = m_channels.m_liveStreams[channel.GetUniqueId()];
    m_livePlayer = m_realTimeBuffer;
    return m_livePlayer->Open(line, ADDON_READ_CACHED);
  }
  else if (m_settings->m_liveStreamingMethod == ClientTimeshift)
  {
    line = kodi::tools::StringUtils::Format("%s/live?channeloid=%d&client=%s&sid=%s", m_settings->m_urlBase, channel.GetUniqueId(), m_request.GetSID(), m_request.GetSID());
    m_livePlayer = m_timeshiftBuffer;
    m_livePlayer->Channel(channel.GetUniqueId());
  }
  else
  {
    line = kodi::tools::StringUtils::Format("%s/live?channeloid=%d&client=XBMC-%s", m_settings->m_urlBase, channel.GetUniqueId(), m_request.GetSID());
    m_livePlayer = m_realTimeBuffer;
  }
  kodi::Log(ADDON_LOG_INFO, "Calling Open(%s) on tsb!", line.c_str());
  if (m_livePlayer->Open(line))
  {
    return true;
  }
  return false;
}

int cPVRClientNextPVR::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
  if (IsServerStreamingLive())
  {
    return m_livePlayer->Read(pBuffer, iBufferSize);
  }
  return -1;
}

void cPVRClientNextPVR::CloseLiveStream(void)
{
  kodi::Log(ADDON_LOG_DEBUG, "CloseLiveStream");
  if (IsServerStreamingLive())
  {
    m_livePlayer->Close();
    m_livePlayer = nullptr;
  }
  m_nowPlaying = NotPlaying;
}

int64_t cPVRClientNextPVR::SeekLiveStream(int64_t iPosition, int iWhence)
{
  if (IsServerStreamingLive())
  {
    return m_livePlayer->Seek(iPosition, iWhence);
  }
  return -1;
}

int64_t cPVRClientNextPVR::LengthLiveStream(void)
{
  if (IsServerStreamingLive())
  {
    kodi::Log(ADDON_LOG_DEBUG, "seek length(%lli)", m_livePlayer->Length());
    return m_livePlayer->Length();
  }
  return -1;
}

PVR_ERROR cPVRClientNextPVR::GetSignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus)
{
  // Not supported yet
  if (m_nowPlaying == Transcoding)
  {
    m_livePlayer->Lease();
  }
  return PVR_ERROR_NO_ERROR;
}

bool cPVRClientNextPVR::CanPauseStream(void)
{
  // not called for recordings
  if (IsServerStreaming())
  {
    if (m_nowPlaying == Recording)
      return true;
    else
      return m_livePlayer->CanPauseStream();
  }
  return false;
}

void cPVRClientNextPVR::PauseStream(bool bPaused)
{
  if (IsServerStreaming())
  {
    if (m_nowPlaying == Recording)
    {
      std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
      m_multistreamRecording[m_streamCount]->PauseStream(bPaused);
    }
    else
      m_livePlayer->PauseStream(bPaused);
  }
}


PVR_ERROR cPVRClientNextPVR::PauseRecordedStream(int64_t streamId, bool bPaused)
{
  if (IsServerStreaming())
  {
    if (m_nowPlaying == Recording)
    {
      std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
      m_multistreamRecording[streamId]->PauseStream(bPaused);
    }
    else
      m_livePlayer->PauseStream(bPaused);
  }
  return PVR_ERROR_NO_ERROR;
}

bool cPVRClientNextPVR::CanSeekStream(void)
{
  if (IsServerStreamingLive())
  {
    return m_livePlayer->CanSeekStream();
  }
  return false;
}


/************************************************************/
/** Record stream handling */


bool cPVRClientNextPVR::OpenRecordedStream(const kodi::addon::PVRRecording& recording, int64_t& streamId)
{
  kodi::addon::PVRRecording copyRecording = recording;
  copyRecording.SetDirectory(m_recordings.m_hostFilenames[recording.GetRecordingId()]);
  const std::string line = kodi::tools::StringUtils::Format("%s/live?recording=%s&client=XBMC-%s", m_settings->m_urlBase, recording.GetRecordingId().c_str(), m_request.GetSID());
  std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
  m_nowPlaying = Recording;
  m_multistreamRecording.emplace(++m_streamCount, new timeshift::RecordingBuffer(m_settings, m_request));
  streamId = m_streamCount;
  bool ret = m_multistreamRecording[streamId]->Open(line, copyRecording, streamId);
  if (!ret)
  {
    CloseRecordedStream(streamId);
  }
  return ret;
}

void cPVRClientNextPVR::CloseRecordedStream(int64_t streamId)
{
  if (IsServerStreamingRecording(streamId))
  {
    std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
    m_multistreamRecording[streamId]->Close();
    m_multistreamRecording.erase(streamId);
  }
  if (m_multistreamRecording.size() == 0)
    m_nowPlaying = NotPlaying;
  kodi::Log(ADDON_LOG_DEBUG, "Closed streamId %d remaining %d", streamId, m_multistreamRecording.size());
}

int cPVRClientNextPVR::ReadRecordedStream(int64_t streamId, unsigned char* pBuffer, unsigned int iBufferSize)
{
  if (IsServerStreamingRecording(streamId))
  {
    std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
    return m_multistreamRecording[streamId]->Read(pBuffer, iBufferSize);
  }
  return -1;
}

int64_t cPVRClientNextPVR::SeekRecordedStream(int64_t streamId, int64_t iPosition, int iWhence)
{
  if (IsServerStreamingRecording(streamId))
  {
    std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
    return m_multistreamRecording[streamId]->Seek(iPosition, iWhence);
  }
  return -1;
}

int64_t cPVRClientNextPVR::LengthRecordedStream(int64_t streamId)
{
  if (IsServerStreamingRecording(streamId))
  {
    return m_multistreamRecording[streamId]->Length();
  }
  return -1;
}

bool cPVRClientNextPVR::IsTimeshifting()
{
  if (IsServerStreamingLive())
  {
    return m_livePlayer->IsTimeshifting();
  }
  return false;
}

bool cPVRClientNextPVR::IsRealTimeStream()
{
  if (IsServerStreaming())
  {
    if (m_nowPlaying == Recording)
    {
      std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
      return m_multistreamRecording[m_streamCount]->IsRealTimeStream();
    }
    else
      return m_livePlayer->IsRealTimeStream();
  }
  return false;
}
PVR_ERROR cPVRClientNextPVR::GetStreamTimes(kodi::addon::PVRStreamTimes& stimes)
{
  if (IsServerStreaming())
  {
    if (m_nowPlaying == Recording)
    {
      std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
      return m_multistreamRecording[m_streamCount]->GetStreamTimes(stimes);
    }
    else
      return m_livePlayer->GetStreamTimes(stimes);
  }
  return PVR_ERROR_UNKNOWN;
}


PVR_ERROR cPVRClientNextPVR::IsRecordedStreamRealTime(int64_t streamId, bool& isRealTime)
{
  if (IsServerStreaming())
  {
    if (m_nowPlaying == Recording)
    {
      std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
      isRealTime = m_multistreamRecording[streamId]->IsRealTimeStream();
    }
    else
      return PVR_ERROR_INVALID_PARAMETERS;
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR cPVRClientNextPVR::GetRecordedStreamTimes(int64_t streamId, kodi::addon::PVRStreamTimes& stimes)
{
  if (IsServerStreaming())
  {
    if (m_nowPlaying == Recording)
    {
      std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
      return m_multistreamRecording[streamId]->GetStreamTimes(stimes);
    }
    else
      return m_livePlayer->GetStreamTimes(stimes);
  }
  return PVR_ERROR_UNKNOWN;
}

PVR_ERROR cPVRClientNextPVR::GetStreamReadChunkSize(int& chunksize)
{
  if (IsServerStreaming())
  {
    if (m_nowPlaying == TV)
      return m_livePlayer->GetStreamReadChunkSize(chunksize);
    if (m_nowPlaying == Recording)
      chunksize = m_settings->m_chunkRecording * 1024;
    else if (m_nowPlaying == Radio)
      chunksize = 4096;
    return PVR_ERROR_NO_ERROR;
  }
  return PVR_ERROR_UNKNOWN;
}

bool cPVRClientNextPVR::IsServerStreaming()
{
  if (IsServerStreamingLive(false) || m_multistreamRecording.size() > 0)
  {
    return true;
  }
  kodi::Log(ADDON_LOG_ERROR, "Unknown streaming state %d %d %d", m_nowPlaying, m_multistreamRecording.size(), !m_livePlayer);
  return false;
}

bool cPVRClientNextPVR::IsServerStreamingLive(bool log)
{
  if ((m_nowPlaying == TV || m_nowPlaying == Radio) && m_livePlayer != nullptr)
  {
    return true;
  }
  if (log)
    kodi::Log(ADDON_LOG_ERROR, "Unknown live streaming state %d %d %d", m_nowPlaying, m_multistreamRecording.size(), !m_livePlayer);
  return false;
}

bool cPVRClientNextPVR::IsServerStreamingRecording(int64_t streamId, bool log)
{
  if (m_nowPlaying == Recording && m_multistreamRecording.size() > 0)
  {
    std::lock_guard<std::recursive_mutex> lock(m_multiStreamMutex);
    return m_multistreamRecording.find(streamId) != m_multistreamRecording.end();
  }
  if (log)
    kodi::Log(ADDON_LOG_ERROR, "Unknown recording streaming state %d %d %d", m_nowPlaying, m_multistreamRecording.size(), !m_livePlayer);
  return false;
}

PVR_ERROR cPVRClientNextPVR::CallChannelMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRChannel& item)
{
    return m_menuhook.CallChannelMenuHook(menuhook, item);
}

PVR_ERROR cPVRClientNextPVR::CallRecordingMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRRecording& item)
{
    return m_menuhook.CallRecordingsMenuHook(menuhook, item);
}

PVR_ERROR cPVRClientNextPVR::CallSettingsMenuHook(const kodi::addon::PVRMenuhook& menuhook)
{
    return m_menuhook.CallSettingsMenuHook(menuhook);
}

/*******************************************/
/** PVR EPG Functions                     **/

PVR_ERROR cPVRClientNextPVR::GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results)
{
  return m_epg.GetEPGForChannel(channelUid, start, end, results);
}


/*******************************************/
/** PVR Channel Functions                 **/
PVR_ERROR cPVRClientNextPVR::GetChannelsAmount(int& amount)
{
  if (m_connectionState != PVR_CONNECTION_STATE_CONNECTED)
  {
    kodi::Log(ADDON_LOG_ERROR, "GetChannelsAmount called while disconnected");
    return PVR_ERROR_SERVER_ERROR;
  }

  amount = m_channels.GetNumChannels();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR cPVRClientNextPVR::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
  return m_channels.GetChannels(radio, results);
}


/*******************************************/
/** PVR Channel group Functions           **/

PVR_ERROR cPVRClientNextPVR::GetChannelGroupsAmount(int& amount)
{
  if (m_connectionState != PVR_CONNECTION_STATE_CONNECTED)
  {
    kodi::Log(ADDON_LOG_ERROR, "GetChannelGroupsAmount called while disconnected");
    return PVR_ERROR_SERVER_ERROR;
  }

  m_channels.GetChannelGroupsAmount(amount);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR cPVRClientNextPVR::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
  return m_channels.GetChannelGroups(radio, results);
}

PVR_ERROR cPVRClientNextPVR::GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group, kodi::addon::PVRChannelGroupMembersResultSet& results)
{
  return m_channels.GetChannelGroupMembers(group, results);
}

/*******************************************/
/** PVR Recording Functions               **/

PVR_ERROR cPVRClientNextPVR::GetRecordingsAmount(bool deleted, int& amount)
{
  return m_recordings.GetRecordingsAmount(deleted, amount);
}

PVR_ERROR cPVRClientNextPVR::GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results)
{
  return m_recordings.GetRecordings(deleted, results);
}

PVR_ERROR cPVRClientNextPVR::DeleteRecording(const kodi::addon::PVRRecording& recording)
{
  return m_recordings.DeleteRecording(recording);
}

PVR_ERROR cPVRClientNextPVR::GetRecordingEdl(const kodi::addon::PVRRecording& recording, std::vector<kodi::addon::PVREDLEntry>& edl)
{
  return m_recordings.GetRecordingEdl(recording, edl);
}

PVR_ERROR cPVRClientNextPVR::GetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int& position)
{
  return m_recordings.GetRecordingLastPlayedPosition(recording, position);
}

PVR_ERROR cPVRClientNextPVR::SetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int lastplayedposition)
{
  return m_recordings.SetRecordingLastPlayedPosition(recording, lastplayedposition);
}

PVR_ERROR cPVRClientNextPVR::SetRecordingPlayCount(const kodi::addon::PVRRecording& recording, int count)
{
  return m_recordings.SetRecordingPlayCount(recording, count);
}

/*******************************************/
/** PVR Timer Functions                   **/
PVR_ERROR cPVRClientNextPVR::GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types)
{
  return m_timers.GetTimerTypes(types);
}

ADDON_STATUS cPVRClientNextPVR::SetInstanceSetting(const std::string& settingName,
  const kodi::addon::CSettingValue& settingValue)
{
  return m_settings->SetValue(settingName, settingValue);
}


PVR_ERROR cPVRClientNextPVR::GetTimersAmount(int& amount)
{
  return m_timers.GetTimersAmount(amount);
}

PVR_ERROR cPVRClientNextPVR::GetTimers(kodi::addon::PVRTimersResultSet& results)
{
  return m_timers.GetTimers(results);
}

PVR_ERROR cPVRClientNextPVR::AddTimer(const kodi::addon::PVRTimer& timer)
{
  return m_timers.AddTimer(timer);
}

PVR_ERROR cPVRClientNextPVR::DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete)
{
  return m_timers.DeleteTimer(timer, forceDelete);
}

PVR_ERROR cPVRClientNextPVR::UpdateTimer(const kodi::addon::PVRTimer& timer)
{
  return m_timers.UpdateTimer(timer);
}

//-- GetCapabilities -----------------------------------------------------
// Tell XBMC our requirements
//-----------------------------------------------------------------------------

PVR_ERROR cPVRClientNextPVR::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  kodi::Log(ADDON_LOG_DEBUG, "->GetCapabilities()");

  capabilities.SetSupportsEPG(true);
  capabilities.SetSupportsRecordings(m_settings->m_accessLevel & ACCESS_RECORDINGS);
  capabilities.SetSupportsRecordingsDelete(m_settings->m_accessLevel & ACCESS_RECORDINGS_DELETE);
  capabilities.SetSupportsRecordingsUndelete(false);
  capabilities.SetSupportsRecordingSize(m_settings->m_showRecordingSize);
  capabilities.SetSupportsTimers(m_settings->m_accessLevel & ACCESS_TIMERS);
  capabilities.SetSupportsTV(true);
  capabilities.SetSupportsRadio(m_settings->m_showRadio);
  capabilities.SetSupportsChannelGroups(true);
  capabilities.SetHandlesInputStream(true);
  capabilities.SetHandlesDemuxing(false);
  capabilities.SetSupportsChannelScan(false);
  capabilities.SetSupportsLastPlayedPosition(m_settings->m_backendResume);
  capabilities.SetSupportsRecordingEdl(m_settings->m_comskip);
  capabilities.SetSupportsRecordingsRename(false);
  capabilities.SetSupportsRecordingsLifetimeChange(false);
  capabilities.SetSupportsDescrambleInfo(false);
  capabilities.SetSupportsRecordingPlayCount(m_settings->m_backendResume);
  capabilities.SetSupportsProviders(false);
  capabilities.SetSupportsMultipleRecordedStreams(!m_settings->m_recordingPoster);
  return PVR_ERROR_NO_ERROR;
}
