#include <NetworkPlugin/NetworkPluginPCH.h>

#define ENET_DLL
#include <enet/enet.h>

#include <NetworkPlugin/Core/NetworkTypes.h>
#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/Core/Driver/enet/DriverEnet.h>


namespace
{
  plUInt32 s_uiEnetUsers = 0;
}

plDriverEnet::plDriverEnet()
{

}

plResult plDriverEnet::Initalize(plNetworkPeer* pNetworkPeer, const plNetworkConfig& config)
{
  m_pNetworkPeer = pNetworkPeer;
  m_Config = config;
  m_PeerMap.Clear();

  if(s_uiEnetUsers == 0 && enet_initialize() != 0)
  {
    plLog::Error("ENet: Failed to initialize ENet.");
    return PL_FAILURE;
  }

  ++s_uiEnetUsers;
  m_bInitialized = true;

  plLog::Info("ENet: ENet initialized.");
  return PL_SUCCESS;
}

void plDriverEnet::Shutdown()
{
  if(m_pEnetPeer)
  {
    enet_peer_disconnect_now(m_pEnetPeer, 0);
  }

  enet_host_destroy(m_pEnetHost);

  if (m_bInitialized && --s_uiEnetUsers == 0)
    enet_deinitialize();
  m_bInitialized = false;

  m_PeerMap.Clear();
  m_ConnectionIDs.Clear();

  m_pEnetPeer = nullptr;
  m_pEnetHost = nullptr;

  plLog::Info("ENet: ENet shutdown.");
}

plResult plDriverEnet::Listen()
{
  ENetAddress address = {0};
  address.host = ENET_HOST_ANY;
  address.port = m_Config.m_uiServerPort;

  plLog::Info("ENet: Attempting to listen on port {0} with max {1} connections...", 
    m_Config.m_uiServerPort, m_Config.m_uiConnectionLimit);

  if(!m_Config.m_sServerAddress.IsEmpty() && m_Config.m_sServerAddress != "any")
  {
    enet_address_set_host(&address, m_Config.m_sServerAddress.GetData());
  }

  m_pEnetHost = enet_host_create(&address, m_Config.m_uiConnectionLimit, 1, 0, 0);
  if(!m_pEnetHost)
  {
    plLog::Error("ENet: Failed to create host on port {0}. Port may be in use or invalid.", m_Config.m_uiServerPort);
    return PL_FAILURE;
  }

  plLog::Info("ENet: Host created on port {0}.", m_Config.m_uiServerPort);
  // ENet checks these limits before allocating packet reassembly storage. Bound pending
  // payloads independently of the local message pool (at most 64 messages per peer).
  m_pEnetHost->maximumPacketSize = m_Config.m_uiMessageSize;
  m_pEnetHost->maximumWaitingData = static_cast<size_t>(m_Config.m_uiMessageSize) * plMath::Min<plUInt16>(m_Config.m_uiMessagePoolSize, 64);
  return PL_SUCCESS;
}

plResult plDriverEnet::Connect()
{
  plLog::Info("ENet: Connecting...");

  ENetAddress address = {0};
  address.port = m_Config.m_uiServerPort;
  enet_address_set_host(&address, m_Config.m_sServerAddress.GetData());

  m_pEnetHost = enet_host_create(nullptr, 1, 1, 0, 0);
  if(!m_pEnetHost)
  {
    plLog::Error("ENet: Failed to create host.");
    return PL_FAILURE;
  }

  m_pEnetHost->maximumPacketSize = m_Config.m_uiMessageSize;
  m_pEnetHost->maximumWaitingData = static_cast<size_t>(m_Config.m_uiMessageSize) * plMath::Min<plUInt16>(m_Config.m_uiMessagePoolSize, 64);
  m_pEnetPeer = enet_host_connect(m_pEnetHost, &address, 1, 0);
  if(!m_pEnetPeer)
  {
    plLog::Error("ENet: Failed to connect.");
    enet_host_destroy(m_pEnetHost);
    m_pEnetHost = nullptr;
    return PL_FAILURE;
  }

  plLog::Info("ENet: Connected.");
  return PL_SUCCESS;
}

void plDriverEnet::Disconnect()
{
  if(m_pEnetPeer)
  {
    enet_peer_disconnect_now(m_pEnetPeer, 0);
    m_pEnetPeer = nullptr;
    plLog::Info("ENet: Disconnected.");
  }
}

void plDriverEnet::Disconnect(const plNetworkConnection& connection)
{
  ENetPeer* peer;

  if(m_PeerMap.TryGetValue(connection.m_uiConnectionID, peer))
  {
    enet_peer_disconnect_now(peer, 0);
    m_ConnectionIDs.Remove(peer);
    m_PeerMap.Remove(connection.m_uiConnectionID);
    plLog::Info("ENet: Disconnected Id: {}.", connection.m_uiConnectionID);
  }
}

plResult plDriverEnet::PopEvent(plNetworkEvent& event)
{
  if(!m_pEnetHost)
  {
    return PL_FAILURE;
  }

  // Invalid receive events are consumed here so one hostile packet cannot stall
  // the world module's event pump for the rest of the frame.
  while (true)
  {
    ENetEvent enetEvent;
    const int result = enet_host_service(m_pEnetHost, &enetEvent, 0);

    if(result <= 0)
    {
      return PL_FAILURE;
    }

    ENetPeer* peer = enetEvent.peer;
    plUInt32 connectionId = 0; // The remote server is always 0 on a client.
    if (IsServer())
    {
      if (enetEvent.type == ENET_EVENT_TYPE_CONNECT)
      {
        if (m_ConnectionIDs.Contains(peer) || m_uiNextConnectionID > plMath::MaxValue<plUInt32>())
        {
          enet_peer_disconnect_now(peer, 0);
          continue;
        }
        connectionId = static_cast<plUInt32>(m_uiNextConnectionID++);
        m_ConnectionIDs.Insert(peer, connectionId);
      }
      else if (!m_ConnectionIDs.TryGetValue(peer, connectionId))
      {
        if (enetEvent.type == ENET_EVENT_TYPE_RECEIVE)
          enet_packet_destroy(enetEvent.packet);
        continue;
      }
    }
    event.m_Sender.m_uiConnectionID = connectionId;

    switch (enetEvent.type)
    {
      case ENET_EVENT_TYPE_CONNECT:
      {
        plLog::Debug("ENet: Connected Id: {}.", connectionId);
        event.m_EventType = plNetworkEventTypeEnum::Connected;
        if (IsServer())
        {
          m_PeerMap.Insert(connectionId, enetEvent.peer);
        }
        break;
      }
      case ENET_EVENT_TYPE_DISCONNECT:
      {
        plLog::Debug("ENet: Disconnected Id: {}.", connectionId);
        event.m_EventType = plNetworkEventTypeEnum::Disconnected;
        if (IsServer())
        {
          m_PeerMap.Remove(connectionId);
          m_ConnectionIDs.Remove(peer);
        }
        break;
      }
      case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
      {
        plLog::Debug("ENet: Timeout Id: {}.", connectionId);
        event.m_EventType = plNetworkEventTypeEnum::Timeout;
        if (IsServer())
        {
          m_PeerMap.Remove(connectionId);
          m_ConnectionIDs.Remove(peer);
        }
        break;
      }
      case ENET_EVENT_TYPE_RECEIVE:
      {
        plLog::Debug("ENet: Received Id: {}.", connectionId);
        event.m_EventType = plNetworkEventTypeEnum::Message;
        if (!m_pNetworkPeer)
        {
          enet_packet_destroy(enetEvent.packet);
          continue;
        }
        event.m_Message = m_pNetworkPeer->CreateMessage();
        if (!event.m_Message.IsValid())
        {
          plLog::Error("ENet: Failed to create message buffer - pool may be exhausted");
          enet_packet_destroy(enetEvent.packet);
          continue;
        }
        if (enetEvent.packet->dataLength < 1 || enetEvent.packet->dataLength > event.m_Message.m_uiBufferSize)
        {
          plLog::Warning("ENet: Dropped invalid packet of {} bytes (maximum {}) from connection {}",
            enetEvent.packet->dataLength, event.m_Message.m_uiBufferSize, connectionId);
          m_pNetworkPeer->DestroyMessage(event.m_Message);
          enet_packet_destroy(enetEvent.packet);
          continue;
        }
        event.m_Message.m_uiLength = static_cast<plUInt32>(enetEvent.packet->dataLength);
        plMemoryUtils::Copy(event.m_Message.m_pBuffer, enetEvent.packet->data, event.m_Message.m_uiLength);
        enet_packet_destroy(enetEvent.packet);
        break;
      }
      default:
        continue;
    }
    return PL_SUCCESS;
  }
}

void plDriverEnet::Flush()
{
  if (m_pEnetHost)
  {
    enet_host_flush(m_pEnetHost);
  }
}

ENetPacketFlag ChannelTypeToPacketFlag(const plNetworkChannelTypeEnum::Enum channel)
{
  int flag = 0;

  if (channel == plNetworkChannelTypeEnum::Reliable || channel == plNetworkChannelTypeEnum::ReliableOrdered)
    flag |= ENET_PACKET_FLAG_RELIABLE;

  if (channel == plNetworkChannelTypeEnum::Unreliable)
    flag |= ENET_PACKET_FLAG_UNSEQUENCED;

  // Note: We intentionally do NOT use ENET_PACKET_FLAG_NO_ALLOCATE here
  // because it would cause use-after-free when the message buffer is reused
  // before ENet finishes sending the packet asynchronously.

  return static_cast<ENetPacketFlag>(flag);
}

void SendPacketToPeer(ENetPeer* peer, const plNetworkChannelTypeEnum::Enum channelType, const plNetworkMessage& message)
{
  if (!peer || peer->state != ENET_PEER_STATE_CONNECTED)
    return;
  const ENetPacketFlag flag = ChannelTypeToPacketFlag(channelType);

  ENetPacket* packet = enet_packet_create_offset(message.m_pBuffer, message.m_uiLength, 0, flag);

  if (packet != nullptr && enet_peer_send(peer, 0, packet) < 0)
    enet_packet_destroy(packet);
}

void plDriverEnet::SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message)
{
  if(IsServer())
  {
    plLog::Error("ENet: This send message should be called only on clients.");
    return;
  }
  SendPacketToPeer(m_pEnetPeer, channelType, message);
}

void plDriverEnet::SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plNetworkConnection target)
{
  if(!IsServer())
  {
    plLog::Error("ENet: This send message should be called only on servers.");
    return;
  }
  ENetPeer* peer;
  if (m_PeerMap.TryGetValue(target.m_uiConnectionID, peer) && peer && peer->state == ENET_PEER_STATE_CONNECTED)
  {
    SendPacketToPeer(peer, channelType, message);
  }
}

void plDriverEnet::SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plDynamicArray<plNetworkConnection> target)
{
  if(!IsServer())
  {
    plLog::Error("ENet: This send message should be called only on servers.");
    return;
  }

  for(auto& connection : target)
  {
    ENetPeer* peer;
    if (m_PeerMap.TryGetValue(connection.m_uiConnectionID, peer) && peer && peer->state == ENET_PEER_STATE_CONNECTED)
    {
      SendPacketToPeer(peer, channelType, message);
    }
  }
}

bool plDriverEnet::IsServer() const
{
  return m_pEnetHost != nullptr && m_pEnetPeer == nullptr;
}

plNetworkStats plDriverEnet::GetStats() const
{
  plNetworkConnection connection;
  connection.m_uiConnectionID = 0;
  return GetStats(connection);
}

plNetworkStats plDriverEnet::GetStats(plNetworkConnection target) const
{
  plNetworkStats stats;
  ENetPeer* peer = m_pEnetPeer;
  if (!peer)
    m_PeerMap.TryGetValue(target.m_uiConnectionID, peer);
  if (!peer && m_pEnetHost && m_pEnetHost->peerCount > 0)
    peer = m_pEnetHost->peers;
  if (peer)
  {
    stats.m_fRountTripTime = (float)peer->roundTripTime;
    stats.m_uiPacketsLost = static_cast<plUInt32>(peer->packetsLost);
    stats.m_uiTotalPacketsSent = static_cast<plUInt64>(peer->totalPacketsSent);
    stats.m_uiTotalPacketsLost = static_cast<plUInt32>(peer->totalPacketsLost);
    stats.m_uiTotalBytesSent = static_cast<plUInt64>(peer->totalDataSent);
    stats.m_uiTotalBytesReceived = static_cast<plUInt64>(peer->totalDataReceived);
  }
  return stats;
}
