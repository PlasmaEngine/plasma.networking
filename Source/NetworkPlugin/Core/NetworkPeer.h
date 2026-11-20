#pragma once

#include <NetworkPlugin/Core/NetworkEvent.h>
#include <NetworkPlugin/Core/NetworkTypes.h>
#include <NetworkPlugin/Core/NetworkMessage.h>
#include <NetworkPlugin/NetworkPluginDLL.h>
#include <Foundation/Time/Time.h>
#include <NetworkPlugin/Core/NetworkTime.h>

class plNetworkDriverInterface;
class plNetworkSimulator;

/// \brief One message waiting in the outgoing queue of a peer. It owns a copy of the bytes.
struct PL_NETWORKPLUGIN_DLL plBufferMessage
{
  bool m_bToServer = false;
  plDynamicArray<plNetworkConnection> m_Targets;
  plDynamicArray<plUInt8> m_Data;
  plEnum<plNetworkChannelTypeEnum> m_ChannelType;
};

/// \brief Owns one driver and the buffers messages are built in.
///
/// Outgoing messages: BeginSendMessage() hands out a scratch buffer, and a send copies the bytes into the outgoing queue,
/// which grows as needed and is handed to the driver in Update(), first in, first out. Scratch buffers are recycled in
/// Update(), so a message that is begun and never sent costs nothing beyond the frame. Incoming messages use the fixed
/// pool of m_uiMessagePoolSize buffers; while it is empty the driver leaves received packets queued in the transport.
class PL_NETWORKPLUGIN_DLL plNetworkPeer
{
public:
  /// \brief Number of peers created by CreatePeer and not yet destroyed. Safe to call from any thread.
  static plUInt32 GetPeerCount();

  PL_NODISCARD plResult Initialize(const plNetworkConfig& config);
  void Shutdown();

  PL_NODISCARD plResult Listen();

  PL_NODISCARD plResult Connect();

  void Disconnect();

  void Disconnect(const plNetworkConnection& connection);

  PL_NODISCARD plResult PopEvent(plNetworkEvent& event);

  /// \brief Queues a message for the server. Only valid on a client peer.
  /// Fails when the message is invalid or overflowed its buffer; the caller then still owns whatever it meant to send.
  plResult SendToServer(const plNetworkMessage& message);

  /// \brief Queues a message for one connection. Only valid on a listening peer.
  plResult SendTo(const plNetworkMessage& message, const plNetworkConnection& target);

  /// \brief Queues a message for every given connection. Only valid on a listening peer. An empty list sends nothing.
  plResult SendTo(const plNetworkMessage& message, plArrayPtr<const plNetworkConnection> targets);

  static plNetworkPeer* CreatePeer(const plNetworkConfig& config);
  static void DestroyPeer(plNetworkPeer* pPeer);

  /// \brief Takes a buffer from the incoming pool. Only drivers call this.
  plNetworkMessage CreateMessage();

  /// \brief Returns an incoming message to the pool. Messages from BeginSendMessage need no release.
  void DestroyMessage(plNetworkMessage& message);

  /// \brief True while the incoming pool has a free buffer.
  bool HasFreeMessage() const { return !m_MessagePool.IsEmpty(); }

  /// \brief A scratch buffer of m_uiMessageSize bytes to build an outgoing message in.
  plNetworkMessage BeginSendMessage();

  /// \brief Discards a message from BeginSendMessage that will not be sent.
  void CancelSendMessage(plNetworkMessage& message);

  plUInt8* GetMessageBuffer(const plUInt32 messageID) const
  {
    if (messageID >= m_Config.m_uiMessagePoolSize) return nullptr;
    return m_MessageBuffer + m_Config.m_uiMessageSize * messageID;
  }

  plNetworkConfig m_Config;

  plEnum<plNetworkRoleEnum> m_Role;

  plUInt8* m_MessageBuffer = nullptr;
  plHybridArray<plUInt32, 128> m_MessagePool;

  /// \brief Hands the outgoing queue to the driver, flushes it and recycles the scratch buffers. Called once per update.
  void Update(float dt);

  /// \brief Hands the outgoing queue to the driver and flushes it, keeping the scratch buffers of messages being built.
  void Flush();

  /// @brief Get the network driver owned by this peer. It is the simulating driver that wraps the transport.
  plNetworkDriverInterface* GetDriver() const { return m_pDriver; }

  /// @brief The ENet or platform driver below the simulating driver.
  plNetworkDriverInterface* GetTransportDriver() const;

  /// @brief The network conditions this peer simulates. Settings applied here replace the net_sim_* CVars for this peer.
  plNetworkSimulator* GetSimulator() const;

  /// @brief Messages queued and not yet handed to the driver.
  plUInt32 GetQueuedMessageCount() const { return m_OutgoingBuffer.GetCount(); }

private:
  /// @brief Every live peer. Worlds can update on different threads, so it is only touched under s_PeersMutex.
  static plDynamicArray<plNetworkPeer*> s_Peers;

  /// @brief The network driver instance owned by this peer
  plNetworkDriverInterface* m_pDriver = nullptr;

  plResult Enqueue(const plNetworkMessage& message, bool bToServer, plArrayPtr<const plNetworkConnection> targets);
  void DispatchBufferedMessage(plBufferMessage& bufferMessage);
  void FlushOutgoingBuffer();
  plDynamicArray<plBufferMessage> m_OutgoingBuffer;

  /// @brief Scratch buffers for outgoing messages; the first m_uiScratchInUse are handed out this frame.
  plDynamicArray<plUInt8*> m_ScratchBuffers;
  plUInt32 m_uiScratchInUse = 0;
  plTime m_LastSendErrorLog;

  void CreateMessageBuffers();
  void DestroyMessageBuffers();

  bool IsValidMessageID(plUInt32 messageID) const;
};
