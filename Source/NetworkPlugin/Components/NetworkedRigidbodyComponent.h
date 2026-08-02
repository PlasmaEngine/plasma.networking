#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Components/NetworkComponent.h>

using plNetworkedRigidbodyComponentManager = plComponentManagerSimple<class plNetworkedRigidbodyComponent, plComponentUpdateType::Always>;

/// \brief Component that synchronizes a rigidbody/physics actor across the network.
/// On the server/host (authority), it reads physics simulation results and replicates them.
/// On clients (non-authority), it makes the physics actor kinematic and applies received transforms.
/// Supports owner-based input authority for player-controlled objects.
class PL_NETWORKPLUGIN_DLL plNetworkedRigidbodyComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkedRigidbodyComponent, plNetworkComponent, plNetworkedRigidbodyComponentManager);

public:
  plNetworkedRigidbodyComponent();
  virtual ~plNetworkedRigidbodyComponent() = default;

  // plNetworkComponent interface
  virtual void NetworkSerialize(plNetworkMessage& msg) override;
  virtual void NetworkDeserialize(plNetworkMessage& msg) override;

  /// \brief Returns true if this machine has state authority (can send authoritative updates)
  /// For server-authoritative objects, only server/host returns true.
  /// For owner-authoritative objects, only the owning client returns true.
  bool HasStateAuthority() const;

  /// \brief Returns true if this machine has input authority (can control this object)
  /// Owner always has input authority for their objects.
  bool HasInputAuthority() const;

  /// \brief Get the network ID for this object (from base class plNetworkComponent)
  /// This is set automatically by the network spawn system.
  plUInt32 GetNetworkID() const { return GetNetID(); }

  // Properties
  void SetInterpolationSpeed(float fSpeed) { m_fInterpolationSpeed = fSpeed; }
  float GetInterpolationSpeed() const { return m_fInterpolationSpeed; }

  void SetSyncVelocity(bool bSync) { m_bSyncVelocity = bSync; }
  bool GetSyncVelocity() const { return m_bSyncVelocity; }

  void SetSyncAngularVelocity(bool bSync) { m_bSyncAngularVelocity = bSync; }
  bool GetSyncAngularVelocity() const { return m_bSyncAngularVelocity; }

  void SetTeleportThreshold(float fThreshold) { m_fTeleportThreshold = fThreshold; }
  float GetTeleportThreshold() const { return m_fTeleportThreshold; }

protected:
  virtual void OnSimulationStarted() override;
  virtual void Update() override;

private:
  void UpdateAsAuthority();
  void UpdateAsRemote(float deltaTime);
  void ReadPhysicsVelocities();
  void MakePhysicsKinematic(bool bKinematic);
  void ApplyPhysicsVelocity(const plVec3& vLinear, const plVec3& vAngular);

  // Network state
  plTransform m_NetworkTransform;
  plVec3 m_vLinearVelocity = plVec3::MakeZero();
  plVec3 m_vAngularVelocity = plVec3::MakeZero();

  // Interpolation
  plTransform m_InterpolatedTransform;
  float m_fInterpolationSpeed = 10.0f;
  float m_fTeleportThreshold = 5.0f; // If distance > threshold, teleport instead of interpolate

  // Sync options
  bool m_bSyncVelocity = true;
  bool m_bSyncAngularVelocity = true;
  bool m_bIsKinematic = false; // Track if we've made physics kinematic
};
