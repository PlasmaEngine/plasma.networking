#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkTime.h>
#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>

plNetworkTime::plNetworkTime()
  : m_SyncInterval(plTime::MakeFromSeconds(5.0))
  , m_ServerOffset(plTime::MakeZero())
  , m_RoundTripTime(plTime::MakeZero())
  , m_SmoothedRTT(plTime::MakeZero())
  , m_LastSyncTime(plTime::MakeZero())
  , m_PendingSyncRequestTime(plTime::MakeZero())
{
  for (plUInt32 i = 0; i < RTT_HISTORY_SIZE; ++i)
  {
    m_RTTHistory[i] = plTime::MakeZero();
  }
}

void plNetworkTime::Initialize(plNetworkWorldModule* pNetworkModule)
{
  PL_ASSERT_DEV(pNetworkModule != nullptr, "NetworkWorldModule cannot be null");
  m_pNetworkModule = pNetworkModule;

  // Reset state
  m_ServerOffset = plTime::MakeZero();
  m_RoundTripTime = plTime::MakeZero();
  m_SmoothedRTT = plTime::MakeZero();
  m_bIsSynchronized = false;
  m_uiSyncCount = 0;
  m_bSyncPending = false;
  m_LastSyncTime = plTime::MakeZero();
  m_uiNextSyncSequence = 1;
  m_uiPendingSyncSequence = 0;
  m_LastServerResponseByClient.Clear();

  plLog::Dev("PL Net: NetworkTime initialized");
}

void plNetworkTime::Shutdown()
{
  m_pNetworkModule = nullptr;
  m_bIsSynchronized = false;
  m_uiSyncCount = 0;
}

void plNetworkTime::Update()
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  // Server doesn't need to sync - it IS the time authority
  if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
  {
    m_bIsSynchronized = true;
    return;
  }

  // Client: periodically request time sync
  if (!m_pNetworkModule->IsClient())
    return;

  plTime now = plTime::Now();

  // Check if we need to send a sync request
  if (!m_bSyncPending)
  {
    plTime timeSinceLastSync = now - m_LastSyncTime;
    
    // Sync more frequently at startup, then settle into normal interval
    plTime effectiveInterval = m_bIsSynchronized ? m_SyncInterval : plTime::MakeFromSeconds(0.5);

    if (timeSinceLastSync >= effectiveInterval)
    {
      SendSyncRequest();
    }
  }
  else
  {
    // Check for sync timeout (if we haven't received a response in 2 seconds)
    if (now - m_PendingSyncRequestTime > plTime::MakeFromSeconds(2.0))
    {
      plLog::Warning("PL Net: Time sync request timed out");
      m_bSyncPending = false;
    }
  }
}

plTime plNetworkTime::GetNetworkTime() const
{
  if (!m_pNetworkModule)
    return plTime::Now();

  // Server/Host: local time IS network time
  if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
  {
    return plTime::Now();
  }

  // Client: apply offset to get estimated server time
  return plTime::Now() - m_ServerOffset;
}

void plNetworkTime::RequestSync()
{
  if (!m_bSyncPending)
  {
    SendSyncRequest();
  }
}

void plNetworkTime::SendSyncRequest()
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  plNetworkMessage msg = pPeer->BeginSendMessage();

  msg.WriteUInt8(plNetworkMessageIDs::TimeSync);
  m_uiPendingSyncSequence = m_uiNextSyncSequence++;
  if (m_uiNextSyncSequence == 0)
    m_uiNextSyncSequence = 1;
  msg.WriteUInt16(m_uiPendingSyncSequence);
  
  // Write our local time when sending the request
  plTime now = plTime::Now();
  msg.WriteDouble(now.GetSeconds());

  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  pPeer->SendMessages(msg);

  m_PendingSyncRequestTime = now;
  m_bSyncPending = true;
  m_LastSyncTime = now;
}

void plNetworkTime::HandleTimeSyncRequest(plNetworkMessage& msg, plUInt32 uiClientID)
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  const plUInt16 uiSequence = msg.ReadUInt16();

  // T0: client's send time (from the request)
  const double clientRequestTimeSeconds = msg.ReadDouble();
  if (msg.HasReadError())
    return;

  // T1: server receive time (record immediately so transform traffic cannot inflate this)
  const plTime serverReceiveTime = plTime::Now();

  // Send response to the specific client
  plNetworkClient* pClient = m_pNetworkModule->GetClient(uiClientID);
  if (!pClient)
    return;

  const plTime now = plTime::Now();
  auto itLastResponse = m_LastServerResponseByClient.Find(uiClientID);
  if (itLastResponse.IsValid() && now - itLastResponse.Value() < plTime::MakeFromMilliseconds(200.0))
    return;
  m_LastServerResponseByClient[uiClientID] = now;

  // T2: server send time
  const plTime serverSendTime = plTime::Now();

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  plNetworkMessage response = pPeer->BeginSendMessage();

  response.WriteUInt8(plNetworkMessageIDs::TimeSyncResponse);
  response.WriteUInt16(uiSequence);
  response.WriteDouble(clientRequestTimeSeconds);
  response.WriteDouble(serverReceiveTime.GetSeconds());
  response.WriteDouble(serverSendTime.GetSeconds());

  response.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  pPeer->SendMessages(response, pClient->m_Connection);
}

void plNetworkTime::HandleTimeSyncResponse(plNetworkMessage& msg)
{
  if (!m_bSyncPending)
  {
    // Unexpected response, ignore
    return;
  }

  const plTime t3ResponseReceived = plTime::Now();

  const plUInt16 uiSequence = msg.ReadUInt16();

  // T0, T1, T2 from the response (RFC 5905-style four-timestamp sync)
  const plTime t0ClientSend = plTime::MakeFromSeconds(msg.ReadDouble());
  const plTime t1ServerReceive = plTime::MakeFromSeconds(msg.ReadDouble());
  const plTime t2ServerSend = plTime::MakeFromSeconds(msg.ReadDouble());

  if (msg.HasReadError() || uiSequence != m_uiPendingSyncSequence)
  {
    plLog::Warning("PL Net: Received stale time sync response, ignoring");
    return;
  }

  m_bSyncPending = false;

  CalculateOffset(t0ClientSend, t1ServerReceive, t2ServerSend, t3ResponseReceived);
}

void plNetworkTime::CalculateOffset(plTime t0ClientSend, plTime t1ServerReceive, plTime t2ServerSend, plTime t3ClientReceive)
{
  // RTT and clock offset (immune to server processing delay between T1 and T2)
  const plTime rtt = (t3ClientReceive - t0ClientSend) - (t2ServerSend - t1ServerReceive);
  m_RoundTripTime = rtt;

  // Store in history for jitter calculation
  m_RTTHistory[m_uiRTTHistoryIndex] = rtt;
  m_uiRTTHistoryIndex = (m_uiRTTHistoryIndex + 1) % RTT_HISTORY_SIZE;

  // Update smoothed RTT with exponential moving average
  if (m_uiSyncCount == 0)
  {
    m_SmoothedRTT = rtt;
  }
  else
  {
    m_SmoothedRTT = m_SmoothedRTT * (1.0f - m_fSmoothingFactor) + rtt * m_fSmoothingFactor;
  }

  // Reject outliers: inflated RTT from bursty message queues looked like ~200ms "corrections"
  if (m_bIsSynchronized)
  {
    const plTime maxTrustedRtt = plMath::Max(plTime::MakeFromMilliseconds(50.0), m_SmoothedRTT * 4.0);
    if (rtt > maxTrustedRtt)
    {
      plLog::Dev("PL Net: Ignoring time sync sample (RTT {0}ms exceeds trusted {1}ms)",
        rtt.GetMilliseconds(), maxTrustedRtt.GetMilliseconds());
      return;
    }
  }

  // NTP offset: m_ServerOffset = local - server, so negate the standard theta term
  const plTime ntpOffset = ((t1ServerReceive - t0ClientSend) + (t2ServerSend - t3ClientReceive)) * 0.5;
  const plTime newOffset = ntpOffset * -1.0;

  // Smooth the offset change (unless this is the first sync)
  if (!m_bIsSynchronized)
  {
    m_ServerOffset = newOffset;
    m_bIsSynchronized = true;
    plLog::Dev("PL Net: Initial time sync complete. Offset: {0}ms, RTT: {1}ms",
      m_ServerOffset.GetMilliseconds(), m_RoundTripTime.GetMilliseconds());
  }
  else
  {
    const plTime offsetDelta = newOffset - m_ServerOffset;

    // Only snap when the sample is trustworthy (low RTT); otherwise smooth to avoid movement-driven spikes
    const plTime trustedRttThreshold = plMath::Max(plTime::MakeFromMilliseconds(50.0), m_SmoothedRTT * 2.0);
    if (plMath::Abs(offsetDelta.GetMilliseconds()) > 100.0 && rtt <= trustedRttThreshold)
    {
      m_ServerOffset = newOffset;
      plLog::Dev("PL Net: Large time correction: {0}ms (RTT {1}ms)", offsetDelta.GetMilliseconds(), rtt.GetMilliseconds());
    }
    else
    {
      m_ServerOffset = m_ServerOffset + offsetDelta * m_fSmoothingFactor;
    }
  }

  m_uiSyncCount++;

  if (m_uiSyncCount % 10 == 0)
  {
    plLog::Dev("PL Net: Time sync #{0}: Offset={1}ms, RTT={2}ms, SmoothedRTT={3}ms",
      m_uiSyncCount,
      m_ServerOffset.GetMilliseconds(),
      m_RoundTripTime.GetMilliseconds(),
      m_SmoothedRTT.GetMilliseconds());
  }
}

plTime plNetworkTime::LocalToNetworkTime(plTime localTime) const
{
  if (!m_pNetworkModule || m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
  {
    return localTime;
  }
  return localTime - m_ServerOffset;
}

plTime plNetworkTime::NetworkToLocalTime(plTime networkTime) const
{
  if (!m_pNetworkModule || m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
  {
    return networkTime;
  }
  return networkTime + m_ServerOffset;
}

plTime plNetworkTime::TimeSince(plTime networkTimestamp) const
{
  return GetNetworkTime() - networkTimestamp;
}

bool plNetworkTime::HasPassed(plTime networkTimestamp) const
{
  return GetNetworkTime() >= networkTimestamp;
}

PL_STATICLINK_FILE(NetworkPlugin, NetworkPlugin_Core_Implementation_NetworkTime);
