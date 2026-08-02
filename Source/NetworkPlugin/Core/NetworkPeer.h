#pragma once

#include <NetworkPlugin/Core/NetworkEvent.h>
#include <NetworkPlugin/Core/NetworkTypes.h>
#include <NetworkPlugin/Core/NetworkMessage.h>
#include <NetworkPlugin/NetworkPluginDLL.h>
#include <Foundation/Time/Time.h>

class plNetworkDriverInterface;

struct PL_NETWORKPLUGIN_DLL plBufferMessage
{
public:
  bool m_bIsClient = false;
  plDynamicArray<plNetworkConnection> m_Targets;
  plNetworkMessage m_message;
  plTime m_QueueTime;

  float GetEffectivePriority() const
  {
    const float ageSeconds = (plTime::Now() - m_QueueTime).AsFloatInSeconds();
    return static_cast<float>(m_message.GetPriority()) - ageSeconds * 0.25f;
  }
};

class PL_NETWORKPLUGIN_DLL plNetworkPeer
{
public:
  static plDynamicArray<plNetworkPeer*> m_Peers;

  PL_NODISCARD plResult Initialize(const plNetworkConfig& config);
  void Shutdown();

  PL_NODISCARD plResult Listen();

  PL_NODISCARD plResult Connect();

  void Disconnect();

  void Disconnect(const plNetworkConnection& connection);

  PL_NODISCARD plResult PopEvent(plNetworkEvent& event);

  void SendMessages(const plNetworkMessage& message);
  void SendMessages(const plNetworkMessage& message, plNetworkConnection target);
  void SendMessages(const plNetworkMessage& message, plDynamicArray<plNetworkConnection> target);

  static plNetworkPeer* CreatePeer(const plNetworkConfig& config);
  static void DestroyPeer(plNetworkPeer* pPeer);

  plNetworkMessage CreateMessage();
  void DestroyMessage(plNetworkMessage& message);
  plNetworkMessage BeginSendMessage();
  void CancelSendMessage(plNetworkMessage& message);

  plUInt8* GetMessageBuffer(const plUInt32 messageID) const
  {
    if (messageID >= m_Config.m_uiMessagePoolSize) return nullptr;
    return m_MessageBuffer + m_Config.m_uiMessageSize * messageID;
  }

  plInt8 m_iHostID = -1;

  plNetworkConfig m_Config;

  plEnum<plNetworkRoleEnum> m_Role;

  plUInt8* m_MessageBuffer = nullptr;
  plHybridArray<plUInt32, 128> m_MessagePool;

  void Update(float dt);

  /// @brief Get the network driver owned by this peer
  plNetworkDriverInterface* GetDriver() const { return m_pDriver; }

private:
  /// @brief The network driver instance owned by this peer
  plNetworkDriverInterface* m_pDriver = nullptr;

  void DispatchBufferedMessage(plBufferMessage& bufferMessage);
  void FlushOutgoingBuffer();
  plDynamicArray<plBufferMessage> m_OutgoingBuffer;

  void CreateMessageBuffers();
  void DestroyMessageBuffers();

  bool IsValidMessageID(plUInt32 messageID) const;
};
