#include <NetworkPlugin/NetworkPluginPCH.h>

#include <GameEngine/PlatformServices/Interfaces/PlatformProviders.h>
#include <GameEngine/PlatformServices/PlatformServices.h>
#include <NetworkPlugin/Core/Driver/platform/DriverPlatform.h>
#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/Core/NetworkTypes.h>


namespace
{
  bool IsReliableChannel(plNetworkChannelTypeEnum::Enum channel)
  {
    return channel == plNetworkChannelTypeEnum::Reliable || channel == plNetworkChannelTypeEnum::ReliableOrdered;
  }
} // namespace

plDriverPlatform::plDriverPlatform()
{
}

plResult plDriverPlatform::Initalize(plNetworkPeer* pNetworkPeer, const plNetworkConfig& config)
{
  m_pNetworkPeer = pNetworkPeer;
  m_Config = config;
  m_pTransport = nullptr;
  m_bListening = false;
  m_ServerUser = plPlatformUserId();
  m_uiNextConnectionID = 1;
  m_UserToConnection.Clear();
  m_ConnectionToUser.Clear();

  plPlatformServices* pHub = plPlatformServices::GetInstance();
  if (pHub == nullptr || !pHub->IsInitialized())
  {
    plLog::Error("Platform: no platform services hub - the Platform driver needs a platform backend package.");
    return PL_FAILURE;
  }

  m_pTransport = pHub->GetTransport();
  if (m_pTransport == nullptr)
  {
    plLog::Error("Platform: backend '{}' provides no transport.", pHub->GetBackendId());
    return PL_FAILURE;
  }

  plLog::Info("Platform: transport of backend '{}' ready (max payload {} bytes).", pHub->GetBackendId(), m_pTransport->GetMaxPayloadBytes());

  return PL_SUCCESS;
}

void plDriverPlatform::Shutdown()
{
  if (m_pTransport != nullptr)
  {
    m_pTransport->DisconnectAll();

    if (m_bListening)
    {
      m_pTransport->StopListening();
    }
  }

  m_pTransport = nullptr;
  m_bListening = false;
  m_ServerUser = plPlatformUserId();
  m_UserToConnection.Clear();
  m_ConnectionToUser.Clear();

  plLog::Info("Platform: driver shut down.");
}

plResult plDriverPlatform::Listen()
{
  if (m_pTransport == nullptr)
    return PL_FAILURE;

  if (m_pTransport->Listen().Failed())
  {
    plLog::Error("Platform: transport refused to listen.");
    return PL_FAILURE;
  }

  m_bListening = true;
  plLog::Info("Platform: listening as '{}' with max {} connections.", GetLocalUserAddress(), m_Config.m_uiConnectionLimit);

  return PL_SUCCESS;
}

plResult plDriverPlatform::Connect()
{
  if (m_pTransport == nullptr)
    return PL_FAILURE;

  const plPlatformUserId user = ParseUserAddress(m_Config.m_sServerAddress);
  if (!user.IsValid())
  {
    plLog::Error("Platform: '{}' is not a user address (expected 'user:<backend>:<id>', '<backend>:<id>' or a native id).", m_Config.m_sServerAddress);
    return PL_FAILURE;
  }

  plLog::Info("Platform: connecting to '{}'...", user.m_sCanonical);

  if (m_pTransport->Connect(user).Failed())
  {
    plLog::Error("Platform: transport refused to connect to '{}'.", user.m_sCanonical);
    return PL_FAILURE;
  }

  // The server is connection 0 on a client, matching what the world module expects from ENet.
  m_ServerUser = user;
  m_UserToConnection[user.m_uiHash] = 0;
  m_ConnectionToUser[0] = user;

  return PL_SUCCESS;
}

void plDriverPlatform::Disconnect()
{
  if (m_pTransport != nullptr && m_ServerUser.IsValid())
  {
    m_pTransport->Disconnect(m_ServerUser);
    RemoveConnection(0);
    m_ServerUser = plPlatformUserId();
    plLog::Info("Platform: disconnected from server.");
  }
}

void plDriverPlatform::Disconnect(const plNetworkConnection& connection)
{
  if (m_pTransport == nullptr)
    return;

  if (const plPlatformUserId* pUser = FindUser(connection.m_uiConnectionID))
  {
    m_pTransport->Disconnect(*pUser);
    plLog::Info("Platform: disconnected Id: {} ('{}').", connection.m_uiConnectionID, pUser->m_sCanonical);
    RemoveConnection(connection.m_uiConnectionID);
  }
}

plResult plDriverPlatform::PopEvent(plNetworkEvent& event)
{
  if (m_pTransport == nullptr)
    return PL_FAILURE;

  // Unusable events are consumed here so one bad datagram cannot stall the world module's pump.
  while (true)
  {
    plPlatformTransportEvent transportEvent;
    if (!m_pTransport->PollEvent(transportEvent))
    {
      return PL_FAILURE;
    }

    switch (transportEvent.m_Type)
    {
      case plPlatformTransportEvent::Type::Connected:
      {
        if (!transportEvent.m_User.IsValid() || (!m_bListening && transportEvent.m_User != m_ServerUser))
        {
          m_pTransport->Disconnect(transportEvent.m_User);
          continue;
        }
        if (m_bListening && m_uiNextConnectionID == 0)
        {
          m_pTransport->Disconnect(transportEvent.m_User);
          continue; // Identity space exhausted; never wrap to the server identity.
        }
        if (m_bListening && m_UserToConnection.Contains(transportEvent.m_User.m_uiHash))
          continue; // Duplicate transport notification must not create a second client record.
        // The transport has no connection cap of its own; ENet applies this limit at host creation.
        if (m_bListening && !m_UserToConnection.Contains(transportEvent.m_User.m_uiHash) &&
            m_ConnectionToUser.GetCount() >= m_Config.m_uiConnectionLimit)
        {
          plLog::Warning("Platform: refused '{}' - connection limit of {} reached.", transportEvent.m_User.m_sCanonical, m_Config.m_uiConnectionLimit);
          m_pTransport->Disconnect(transportEvent.m_User);
          continue;
        }

        const plUInt32 uiConnectionID = GetOrAllocateConnection(transportEvent.m_User);
        plLog::Debug("Platform: connected Id: {} ('{}').", uiConnectionID, transportEvent.m_User.m_sCanonical);

        event.m_Sender.m_uiConnectionID = uiConnectionID;
        event.m_EventType = plNetworkEventTypeEnum::Connected;
        break;
      }
      case plPlatformTransportEvent::Type::Disconnected:
      case plPlatformTransportEvent::Type::ConnectFailed:
      {
        plUInt32 uiConnectionID = 0;
        if (!m_UserToConnection.TryGetValue(transportEvent.m_User.m_uiHash, uiConnectionID))
        {
          continue; // Never connected from our point of view.
        }

        const bool bFailed = transportEvent.m_Type == plPlatformTransportEvent::Type::ConnectFailed;
        plLog::Debug("Platform: {} Id: {} ('{}').", bFailed ? "connect failed" : "disconnected", uiConnectionID, transportEvent.m_User.m_sCanonical);

        event.m_Sender.m_uiConnectionID = uiConnectionID;
        event.m_EventType = bFailed ? plNetworkEventTypeEnum::Timeout : plNetworkEventTypeEnum::Disconnected;

        RemoveConnection(uiConnectionID);

        if (transportEvent.m_User == m_ServerUser)
        {
          m_ServerUser = plPlatformUserId();
        }
        break;
      }
      case plPlatformTransportEvent::Type::Data:
      {
        if (m_pNetworkPeer == nullptr)
        {
          continue;
        }

        plUInt32 uiConnectionID = 0;
        if (!m_UserToConnection.TryGetValue(transportEvent.m_User.m_uiHash, uiConnectionID) ||
            FindUser(uiConnectionID) == nullptr || *FindUser(uiConnectionID) != transportEvent.m_User)
        {
          continue; // Data is never an admission event and must not consume the message pool.
        }
        event.m_Sender.m_uiConnectionID = uiConnectionID;
        event.m_EventType = plNetworkEventTypeEnum::Message;

        event.m_Message = m_pNetworkPeer->CreateMessage();
        if (!event.m_Message.IsValid())
        {
          plLog::Error("Platform: failed to create message buffer - pool may be exhausted");
          continue;
        }

        const plUInt32 uiLength = transportEvent.m_Data.GetCount();
        if (uiLength < 1 || uiLength > event.m_Message.m_uiBufferSize)
        {
          plLog::Warning("Platform: dropped invalid datagram of {} bytes (maximum {}) from connection {}", uiLength, event.m_Message.m_uiBufferSize, uiConnectionID);
          m_pNetworkPeer->DestroyMessage(event.m_Message);
          continue;
        }

        event.m_Message.m_uiLength = uiLength;
        plMemoryUtils::Copy(event.m_Message.m_pBuffer, transportEvent.m_Data.GetData(), uiLength);
        ++m_uiMessagesReceived;
        m_uiBytesReceived += uiLength;
        break;
      }
      default:
        continue;
    }

    return PL_SUCCESS;
  }
}

void plDriverPlatform::Flush()
{
  if (m_pTransport != nullptr)
  {
    m_pTransport->Flush();
  }
}

void plDriverPlatform::SendToUser(const plPlatformUserId& user, plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message)
{
  const plUInt32 uiMaxPayload = m_pTransport->GetMaxPayloadBytes();
  if (uiMaxPayload > 0 && message.m_uiLength > uiMaxPayload)
  {
    plLog::Error("Platform: dropped {} byte message to '{}' - transport payload limit is {} bytes.", message.m_uiLength, user.m_sCanonical, uiMaxPayload);
    return;
  }

  const plArrayPtr<const plUInt8> bytes(message.m_pBuffer, message.m_uiLength);
  // The channel index is the plNetworkChannelTypeEnum value, so the transport can keep per-type ordering.
  const plUInt8 uiChannel = static_cast<plUInt8>(channelType.GetValue());

  if (m_pTransport->Send(user, bytes, IsReliableChannel(channelType), uiChannel).Failed())
  {
    plLog::Warning("Platform: send to '{}' failed.", user.m_sCanonical);
    return;
  }

  ++m_uiMessagesSent;
  m_uiBytesSent += message.m_uiLength;
}

void plDriverPlatform::SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message)
{
  if (m_pTransport == nullptr)
    return;

  if (m_bListening)
  {
    plLog::Error("Platform: This send message should be called only on clients.");
    return;
  }

  if (!m_ServerUser.IsValid())
    return;

  SendToUser(m_ServerUser, channelType, message);
}

void plDriverPlatform::SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plNetworkConnection target)
{
  if (m_pTransport == nullptr)
    return;

  if (!m_bListening)
  {
    plLog::Error("Platform: This send message should be called only on servers.");
    return;
  }

  if (const plPlatformUserId* pUser = FindUser(target.m_uiConnectionID))
  {
    SendToUser(*pUser, channelType, message);
  }
}

void plDriverPlatform::SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plDynamicArray<plNetworkConnection> target)
{
  if (m_pTransport == nullptr)
    return;

  if (!m_bListening)
  {
    plLog::Error("Platform: This send message should be called only on servers.");
    return;
  }

  for (const plNetworkConnection& connection : target)
  {
    if (const plPlatformUserId* pUser = FindUser(connection.m_uiConnectionID))
    {
      SendToUser(*pUser, channelType, message);
    }
  }
}

plNetworkStats plDriverPlatform::GetStats() const
{
  plNetworkConnection connection;
  connection.m_uiConnectionID = 0;
  return GetStats(connection);
}

plNetworkStats plDriverPlatform::GetStats(plNetworkConnection target) const
{
  // The transport exposes no per-user counters; every connection reports the driver totals.
  PL_IGNORE_UNUSED(target);

  plNetworkStats stats;
  stats.m_uiTotalPacketsSent = m_uiMessagesSent;
  stats.m_uiTotalBytesSent = m_uiBytesSent;
  stats.m_uiTotalBytesReceived = m_uiBytesReceived;
  return stats;
}

plPlatformUserId plDriverPlatform::ParseUserAddress(plStringView sAddress)
{
  plStringView sTrimmed = sAddress;
  sTrimmed.Trim(" \t\r\n");
  if (sTrimmed.IsEmpty())
    return plPlatformUserId();

  if (sTrimmed.StartsWith_NoCase("user:"))
  {
    sTrimmed.Shrink(5, 0);
    return plPlatformUserId::FromCanonical(sTrimmed);
  }

  if (sTrimmed.FindSubString(":") != nullptr)
  {
    return plPlatformUserId::FromCanonical(sTrimmed);
  }

  plPlatformServices* pHub = plPlatformServices::GetInstance();
  if (pHub != nullptr && !pHub->GetBackendId().IsEmpty())
  {
    return plPlatformUserId::Make(pHub->GetBackendId(), sTrimmed);
  }

  return plPlatformUserId();
}

plString plDriverPlatform::GetLocalUserAddress()
{
  plPlatformServices* pHub = plPlatformServices::GetInstance();
  if (pHub == nullptr)
    return plString();

  const plPlatformUserId localUser = pHub->Identity().GetLocalUserId();
  if (!localUser.IsValid())
    return plString();

  plStringBuilder sAddress("user:", localUser.m_sCanonical);
  return sAddress;
}

plUInt32 plDriverPlatform::GetOrAllocateConnection(const plPlatformUserId& user)
{
  plUInt32 uiConnectionID = 0;
  if (m_UserToConnection.TryGetValue(user.m_uiHash, uiConnectionID))
  {
    return uiConnectionID;
  }

  uiConnectionID = m_uiNextConnectionID++;
  m_UserToConnection[user.m_uiHash] = uiConnectionID;
  m_ConnectionToUser[uiConnectionID] = user;

  return uiConnectionID;
}

void plDriverPlatform::RemoveConnection(plUInt32 uiConnectionID)
{
  auto it = m_ConnectionToUser.Find(uiConnectionID);
  if (!it.IsValid())
    return;

  m_UserToConnection.Remove(it.Value().m_uiHash);
  m_ConnectionToUser.Remove(it);
}

const plPlatformUserId* plDriverPlatform::FindUser(plUInt32 uiConnectionID) const
{
  auto it = m_ConnectionToUser.Find(uiConnectionID);
  return it.IsValid() ? &it.Value() : nullptr;
}
