#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>

#include <Core/World/ComponentManager.h>
#include <NetworkPlugin/Core/NetworkClient.h>
#include <NetworkPlugin/Core/NetworkEvent.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>

struct plNetworkClientConnectionData;

using plNetworkComponentManager = plComponentManagerSimple<class plNetworkComponent, plComponentUpdateType::Always>;

class PL_NETWORKPLUGIN_DLL plNetworkComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkComponent, plComponent, plNetworkComponentManager);

public:
  plNetworkComponent() = default;
  virtual ~plNetworkComponent() = default;

  virtual void OnNetworkStateChanged(plNetworkConnectionState::Enum state);
  virtual void OnClientConnecting(plNetworkClientConnectionData& pClient) {}
  virtual void OnClientConnected(plNetworkClient* pClient) {}
  virtual void OnClientDisconnected(plNetworkClient* pClient) {}

  /// @brief Serialize component state to network message
  /// Default implementation serializes all properties marked with plNetSerializeAttribute
  /// Override for custom serialization logic
  virtual void NetworkSerialize(plNetworkMessage& msg);
  
  /// @brief Deserialize component state from network message
  /// Default implementation deserializes all properties marked with plNetSerializeAttribute
  /// Override for custom deserialization logic
  virtual void NetworkDeserialize(plNetworkMessage& msg);

  /// @brief Serialize for one recipient. Components with target-filtered state override this.
  virtual void NetworkSerializeForClient(plNetworkMessage& msg, plUInt32 uiRecipientClientID, bool bInitialSync)
  {
    PL_IGNORE_UNUSED(uiRecipientClientID);
    PL_IGNORE_UNUSED(bInitialSync);
    NetworkSerialize(msg);
  }

  virtual bool RequiresPerRecipientSerialization() const { return false; }

  PL_NODISCARD PL_FORCE_INLINE bool NeedReplication() const { return m_bNeedReplication; }
  void MarkDirty() { m_bNeedReplication = true; }
  void ClearDirty() { m_bNeedReplication = false; }

  void SetChannelType(plEnum<plNetworkChannelTypeEnum> channelType) { m_ChannelType = channelType; }
  PL_NODISCARD PL_FORCE_INLINE plEnum<plNetworkChannelTypeEnum> GetChannelType() const { return m_ChannelType; }

  void SetNetworkRole(plEnum<plNetworkRoleEnum> networkRole) { m_NetworkRole = networkRole; }
  PL_NODISCARD PL_FORCE_INLINE plEnum<plNetworkRoleEnum> GetNetworkRole() { return m_NetworkRole; }

  void SetNetID(plUInt32 uiNetNetID) { m_uiNetID = uiNetNetID; }
  plUInt32 GetNetID() const { return m_uiNetID; }

  /// @brief Returns true if the local client is the owner of this component's network object.
  /// Queries the ObjectManager's ownership table for this component's NetID.
  bool IsLocallyOwned() const;

protected:
  virtual void Update() {}
  virtual void OnSimulationStarted() override;
  virtual void Deinitialize() override;

  plNetworkWorldModule* m_pNetworkModule = nullptr;


private:
  plEnum<plNetworkRoleEnum> m_NetworkRole = plNetworkRoleEnum::Undefined;
  plEnum<plNetworkChannelTypeEnum> m_ChannelType;
  bool m_bNeedReplication = false;
  plUInt32 m_uiNetID = 0;
};
