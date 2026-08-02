#pragma once

#include <Core/Utils/Blackboard.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/NetworkPluginDLL.h>

/// \brief Who may write blackboard state onto the network (ObjectReplicate channel).
struct plNetworkBlackboardAuthority
{
  using StorageType = plUInt8;

  enum Enum
  {
    ServerAuthoritative, ///< Host/server serializes; all peers apply incoming updates.
    OwnerWritable,       ///< Owning client serializes; host/server and other peers apply updates.

    Default = ServerAuthoritative
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkBlackboardAuthority);

using plNetworkedBlackboardComponentManager = plComponentManagerSimple<class plNetworkedBlackboardComponent, plComponentUpdateType::Always>;

/// \brief Replicates all (or Save-flagged) entries on a bound plBlackboard via ObjectReplicate.
///
/// Bind to a board on the same object (plBlackboardComponent) or by name (FindBlackboard / global).
/// Uses per-entry change counters for deltas; full snapshots on connect and when entries are added/removed.
class PL_NETWORKPLUGIN_DLL plNetworkedBlackboardComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkedBlackboardComponent, plNetworkComponent, plNetworkedBlackboardComponentManager);

public:
  plNetworkedBlackboardComponent();
  virtual ~plNetworkedBlackboardComponent() = default;

  virtual void NetworkSerialize(plNetworkMessage& msg) override;
  virtual void NetworkDeserialize(plNetworkMessage& msg) override;
  virtual void OnNetworkStateChanged(plNetworkConnectionState::Enum state) override;

protected:
  virtual void OnSimulationStarted() override;
  virtual void Update() override;

private:
  void ResolveBlackboard();
  bool HasReplicationAuthority() const;
  bool ShouldReplicateEntry(const plBlackboard::Entry& entry) const;
  bool HasEntryChanges() const;
  void RefreshChangeCacheFromBoard();
  void PruneStaleCacheEntries();

  plEnum<plNetworkBlackboardAuthority> m_Authority = plNetworkBlackboardAuthority::ServerAuthoritative;
  float m_fSendRate = 15.0f;
  plString m_sBlackboardName;
  bool m_bReplicateSaveFlaggedOnly = false;
  bool m_bSyncEntryFlags = true;

  plSharedPtr<plBlackboard> m_pBoard;
  plHashTable<plHashedString, plUInt32> m_LastEntryChangeCounters;
  plUInt32 m_uiLastBlackboardChangeCounter = 0;
  plTime m_LastSendTime;
  bool m_bForceFullSnapshot = true;
  bool m_bLoggedMissingBoard = false;

  /// Spawned objects assign the same NetID to every plNetworkComponent on the owner; we derive a unique ID for routing.
  plUInt32 m_uiOwnerObjectNetID = 0;
};
