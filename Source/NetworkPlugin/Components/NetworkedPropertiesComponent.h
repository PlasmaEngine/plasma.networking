#pragma once

#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Core/NetworkSerialize.h>
#include <NetworkPlugin/Core/NetworkReplicationProfileResource.h>

#include <Foundation/Containers/Map.h>

/// Who simulates the replicated properties and is allowed to send them.
struct plReplicateAuthority
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    Server, ///< The server/host simulates; clients only apply received values.
    Owner,  ///< The owning client simulates and uploads; the server relays to everyone else.
    Shared, ///< Server and owning client may both write (legacy behavior).

    Default = Server
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plReplicateAuthority);

struct PL_NETWORKPLUGIN_DLL plNetworkedPropertyEntry
{
  plString m_sComponentType;
  plUInt8 m_uiComponentIndex = 0;
  plString m_sProperty;
  plString m_sProfile;

  plEnum<plReplicateCondition> m_Condition = plReplicateCondition::OnChange;
  plEnum<plReplicateTarget> m_Target = plReplicateTarget::AllClients;
  plEnum<plNetSerializeEncoding> m_Encoding = plNetSerializeEncoding::Default;
  plUInt8 m_uiPriority = 128;
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkedPropertyEntry);

using plNetworkedPropertiesComponentManager = plComponentManagerSimple<class plNetworkedPropertiesComponent, plComponentUpdateType::Always>;

/// Replicates selected top-level properties on sibling components.
class PL_NETWORKPLUGIN_DLL plNetworkedPropertiesComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkedPropertiesComponent, plNetworkComponent, plNetworkedPropertiesComponentManager);

public:
  float m_fSendRate = 20.0f;
  plEnum<plReplicateAuthority> m_Authority;

  /// On peers without authority, switch sibling dynamic physics actors to kinematic so the
  /// local simulation stops fighting the replicated transform. Only applies when a
  /// plGameObject transform property is replicated. Restored on disconnect.
  bool m_bTakeOverRemotePhysics = true;

  /// Seconds over which received float/vector/quaternion values are blended in on
  /// non-authoritative peers. 0 = snap immediately.
  float m_fInterpolationTime = 0.1f;

  plDynamicArray<plNetworkedPropertyEntry> m_Properties;

  void SerializeComponent(plWorldWriter& inout_stream) const override;
  void DeserializeComponent(plWorldReader& inout_stream) override;

  void NetworkSerialize(plNetworkMessage& msg) override;
  void NetworkSerializeForClient(plNetworkMessage& msg, plUInt32 uiRecipientClientID, bool bInitialSync) override;
  void NetworkDeserialize(plNetworkMessage& msg) override;
  bool RequiresPerRecipientSerialization() const override { return true; }

  void OnNetworkStateChanged(plNetworkConnectionState::Enum state) override;

protected:
  void OnSimulationStarted() override;
  void Update() override;

private:
  struct ResolvedEntry
  {
    enum class Kind : plUInt8
    {
      Invalid,
      Member,
      GameObjectTags
    };

    void* m_pInstance = nullptr;
    const plAbstractMemberProperty* m_pProperty = nullptr;
    plVariantType::Enum m_VariantType = plVariantType::Invalid;
    plVariant m_LastObserved;
    bool m_bResolved = false;
    Kind m_Kind = Kind::Invalid;
    plNetworkReplicationProfileResourceHandle m_hProfile;

    // receive-side smoothing state
    plVariant m_InterpStart;
    plVariant m_InterpTarget;
    plTime m_InterpStartTime;
    bool m_bInterpolating = false;
  };

  void ResolveEntries();
  plUInt32 GetOwnerClientID() const;
  plNetworkReplicationProfileDescriptor GetSettings(plUInt32 uiEntry) const;
  plVariant GetCurrentValue(const ResolvedEntry& entry) const;
  void WriteValue(plNetworkMessage& msg, const ResolvedEntry& entry, plNetSerializeEncoding::Enum encoding) const;
  plVariant ReadValue(plNetworkMessage& msg, const ResolvedEntry& entry, plNetSerializeEncoding::Enum encoding) const;
  void ApplyValue(ResolvedEntry& entry, const plVariant& value);

  static bool IsInterpolatableType(plVariantType::Enum type);
  static plVariant LerpVariant(const plVariant& a, const plVariant& b, float fT);
  bool ReplicatesOwnerTransform() const;
  void TakeOverRemotePhysics();
  void RestoreRemotePhysics();
  void TickInterpolation();

  plDynamicArray<ResolvedEntry> m_ResolvedEntries;
  plMap<plUInt32, plDynamicArray<plVariant>> m_LastSentValues;
  plMap<plUInt32, plDynamicArray<plTime>> m_LastSentTimes;
  plTime m_LastPollTime;
  plDynamicArray<plComponentHandle> m_TakenOverActors;
  bool m_bRemotePhysicsTakenOver = false;
  plUInt8 m_uiDebugSendLogs = 0;
  plUInt8 m_uiDebugRecvLogs = 0;
};
