#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Core/NetworkSnapshot.h>

using plNetworkedTransformComponentManager = plComponentManagerSimple<class plNetworkedTransformComponent, plComponentUpdateType::Always>;

/// \brief General-purpose component for replicating game object transforms with smooth snapshot interpolation.
///
/// Supports two authority patterns:
/// - Client-upload: owning client calls UpdateAsAuthority() and SendStateToHost(); host relays to others.
/// - Server-sim: host/server simulates and BroadcastStateToClients(bIncludeOwner=true) so the owner can reconcile.
/// Remote peers use adaptive-rate linear interpolation for smooth rendering.
///
/// No dependency on any physics plugin. Position/rotation application is virtual so subclasses
/// can override for physics-aware behavior (e.g. character controller teleport).
class PL_NETWORKPLUGIN_DLL plNetworkedTransformComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkedTransformComponent, plNetworkComponent, plNetworkedTransformComponentManager);

public:
  plNetworkedTransformComponent();
  virtual ~plNetworkedTransformComponent() = default;

  //////////////////////////////////////////////////////////////////////////
  // Editor-visible configuration
  //////////////////////////////////////////////////////////////////////////

  float m_fSendRate = 20.0f;          ///< Hz, state send frequency
  float m_fTeleportThreshold = 5.0f;  ///< Snap instead of interpolate above this distance
  bool m_bSyncVelocity = true;        ///< Include derived velocity in state
  bool m_bFullPrecision = false;      ///< Send all transform fields as float32
  bool m_bUseBoundedPosition = true;  ///< Quantize positions inside WorldBounds
  plVec3 m_vWorldBoundsMin = plVec3(-1000.0f); ///< Shared minimum for 16-bit position quantization
  plVec3 m_vWorldBoundsMax = plVec3(1000.0f);  ///< Shared maximum for 16-bit position quantization

  void SerializeComponent(plWorldWriter& inout_stream) const override;
  void DeserializeComponent(plWorldReader& inout_stream) override;

  /// Server-time position currently shown by the interpolator.
  plTime GetRenderedStateTime() const { return m_Interpolator.GetTimeline(); }
  const plVec3& GetReplicatedVelocity() const { return m_vReplicatedVelocity; }

  //////////////////////////////////////////////////////////////////////////
  // Message handling
  //////////////////////////////////////////////////////////////////////////

  /// @brief Handle a received TransformState network message.
  /// Called by the engine's message dispatch (plNetworkWorldModule).
  /// Reads position/rotation/velocity, feeds the interpolator, and relays on host.
  virtual void HandleTransformStateMessage(plNetworkMessage& msg);

protected:
  //////////////////////////////////////////////////////////////////////////
  // Virtual hooks for subclasses
  //////////////////////////////////////////////////////////////////////////

  /// @brief Called once when authority is determined (local vs remote).
  /// Override to set up cameras, person tags, input, etc.
  virtual void OnAuthorityDetermined(bool bIsLocalAuthority) {}

  /// @brief Called each frame for the authoritative (local/owner) instance.
  /// Default captures transform, derives velocity, and sends state.
  /// Override to drive physics/input before calling base.
  virtual void UpdateAsAuthority();

  /// @brief Called each frame for remote (non-owner) instances.
  /// Default interpolates and calls ApplyRemotePosition/ApplyRemoteRotation.
  virtual void UpdateAsRemote();

  /// @brief Read current transform into replicated state. Override to add custom state.
  virtual void CaptureTransformState();

  /// @brief Apply a position received from the network to this game object.
  /// Default calls GetOwner()->SetGlobalPosition().
  /// Override for physics-aware behavior (e.g. character controller teleport).
  virtual void ApplyRemotePosition(const plVec3& vPosition);

  /// @brief Apply a rotation received from the network to this game object.
  /// Default calls GetOwner()->SetGlobalRotation().
  virtual void ApplyRemoteRotation(const plQuat& qRotation);

  //////////////////////////////////////////////////////////////////////////
  // Replicated state accessible to subclasses
  //////////////////////////////////////////////////////////////////////////

  plVec3 m_vReplicatedPosition = plVec3::MakeZero();
  plQuat m_qReplicatedRotation;
  plVec3 m_vReplicatedVelocity = plVec3::MakeZero();

  /// @brief Interpolator available to subclasses for debug info
  plNetworkInterpolator m_Interpolator;

  //////////////////////////////////////////////////////////////////////////
  // Component lifecycle
  //////////////////////////////////////////////////////////////////////////

  virtual void OnSimulationStarted() override;
  virtual void Update() override;

private:
  plTime m_LastSendTime;
  plVec3 m_vPrevPosition = plVec3::MakeZero();
  bool m_bAuthorityInitialized = false;

  /// @brief Internally tracked position/rotation for teleport detection.
  /// These are independent of the game object's actual transform (which may be
  /// modified by physics between our updates). Initialized from the first valid
  /// network target to avoid lerping from spawn.
  plVec3 m_vLastAppliedPosition = plVec3::MakeZero();
  plQuat m_qLastAppliedRotation;
  bool m_bFirstTargetReceived = false;

  /// @brief Wall-clock time of last UpdateAsRemote call.
  /// Used to compute frame delta in wall-clock time (matching snapshot timestamps)
  /// instead of game clock time which may drift due to fixed timestep or scaling.
  plTime m_LastWallClockUpdateTime;

  bool m_bBandwidthPrioritySet = false;
  plUInt32 m_uiSnapshotSequence = 0;

  void WriteTransformState(plNetworkMessage& msg) const;
  void SendStateToHost();
  void BroadcastStateToClients(bool bIncludeOwner = true);
};
