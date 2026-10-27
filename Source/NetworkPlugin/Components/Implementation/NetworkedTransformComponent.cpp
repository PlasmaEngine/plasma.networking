#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Components/NetworkedTransformComponent.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <NetworkPlugin/Core/NetworkMessage.h>
#include <NetworkPlugin/Core/NetworkBandwidth.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <RendererCore/Debug/DebugRenderer.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plNetworkedTransformComponent, 2, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("SendRate", m_fSendRate)->AddAttributes(new plDefaultValueAttribute(20.0f)),
    PL_MEMBER_PROPERTY("TeleportThreshold", m_fTeleportThreshold)->AddAttributes(new plDefaultValueAttribute(5.0f)),
    PL_MEMBER_PROPERTY("SyncVelocity", m_bSyncVelocity)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("FullPrecision", m_bFullPrecision)->AddAttributes(new plDefaultValueAttribute(false)),
    PL_MEMBER_PROPERTY("UseBoundedPosition", m_bUseBoundedPosition)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("WorldBoundsMin", m_vWorldBoundsMin),
    PL_MEMBER_PROPERTY("WorldBoundsMax", m_vWorldBoundsMax),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(GetRenderedStateTime),
  }
  PL_END_FUNCTIONS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Networking"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

plNetworkedTransformComponent::plNetworkedTransformComponent()
{
  m_qReplicatedRotation = plQuat::MakeIdentity();
  m_qLastAppliedRotation = plQuat::MakeIdentity();
}

void plNetworkedTransformComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_fSendRate;
  s << m_fTeleportThreshold;
  s << m_bSyncVelocity;
  s << m_bFullPrecision;
  s << m_bUseBoundedPosition;
  s << m_vWorldBoundsMin;
  s << m_vWorldBoundsMax;
}

void plNetworkedTransformComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);

  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());
  if (uiVersion < 2)
    return; // exported before this component wrote any data; re-transform the scene

  plStreamReader& s = inout_stream.GetStream();

  s >> m_fSendRate;
  s >> m_fTeleportThreshold;
  s >> m_bSyncVelocity;
  s >> m_bFullPrecision;
  s >> m_bUseBoundedPosition;
  s >> m_vWorldBoundsMin;
  s >> m_vWorldBoundsMax;
}

void plNetworkedTransformComponent::OnSimulationStarted()
{
  plNetworkComponent::OnSimulationStarted();

  m_vReplicatedPosition = GetOwner()->GetGlobalPosition();
  m_qReplicatedRotation = GetOwner()->GetGlobalRotation();
  m_vPrevPosition = m_vReplicatedPosition;

  // Configure the interpolator. The adaptive-rate playback system handles
  // jitter absorption automatically via playback speed control -- no
  // extrapolation or complex curve fitting needed.
  plSnapshotInterpolationConfig config = m_Interpolator.GetConfig();
  config.m_fTeleportThreshold = m_fTeleportThreshold;
  config.m_InterpolationDelay = plTime::MakeFromMilliseconds(
    (m_fSendRate > 0.0f) ? (2000.0 / m_fSendRate) : 100.0);
  m_Interpolator.SetConfig(config);
}

void plNetworkedTransformComponent::Update()
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  // NetID may be assigned after OnSimulationStarted (network spawn); register High priority once.
  if (!m_bBandwidthPrioritySet && GetNetID() != 0)
  {
    m_pNetworkModule->GetBandwidthManager()->SetComponentPriority(GetNetID(), plNetworkPriority::High);
    m_bBandwidthPrioritySet = true;
  }

  // Determine authority
  bool bIsLocal = false;
  if (GetNetID() != 0)
  {
    bIsLocal = IsLocallyOwned();
  }
  else if (m_pNetworkModule->IsHost())
  {
    bIsLocal = true;
  }

  // One-time authority notification
  if (!m_bAuthorityInitialized && GetNetID() != 0)
  {
    m_bAuthorityInitialized = true;
    OnAuthorityDetermined(bIsLocal);
  }

  if (bIsLocal)
  {
    UpdateAsAuthority();
  }
  else
  {
    UpdateAsRemote();
  }
}

void plNetworkedTransformComponent::CaptureTransformState()
{
  m_vReplicatedPosition = GetOwner()->GetGlobalPosition();
  m_qReplicatedRotation = GetOwner()->GetGlobalRotation();

  // Derive velocity from position delta (still useful for game logic
  // like ground state detection, even though the interpolation system
  // no longer uses it for curve tangents)
  float fDt = GetWorld()->GetClock().GetTimeDiff().AsFloatInSeconds();
  if (fDt > 0.0f)
  {
    m_vReplicatedVelocity = (m_vReplicatedPosition - m_vPrevPosition) / fDt;
  }
  m_vPrevPosition = m_vReplicatedPosition;
}

void plNetworkedTransformComponent::UpdateAsAuthority()
{
  CaptureTransformState();

  // Throttle network sends to configured rate
  plTime now = plTime::Now();
  double fSendInterval = (m_fSendRate > 0.0f) ? (1000.0 / m_fSendRate) : 50.0;
  if ((now - m_LastSendTime).GetMilliseconds() < fSendInterval)
    return;
  m_LastSendTime = now;

  if (m_pNetworkModule->IsHost())
  {
    BroadcastStateToClients(true);
  }
  else
  {
    SendStateToHost();
  }
}

void plNetworkedTransformComponent::UpdateAsRemote()
{
  // Compute frame delta from wall-clock time to stay in sync with
  // snapshot timestamps (which are also wall-clock based via GetSyncedTime/plTime::Now).
  // Using game clock (GetWorld()->GetClock().GetTimeDiff()) can drift due to
  // fixed timestep, time scaling, or pausing.
  plTime now = plTime::Now();
  float frameDt = 0.0f;
  if (m_LastWallClockUpdateTime.GetSeconds() > 0.0)
  {
    frameDt = static_cast<float>((now - m_LastWallClockUpdateTime).GetSeconds());
    frameDt = plMath::Clamp(frameDt, 0.0f, 0.1f); // Safety cap at 100ms
  }
  m_LastWallClockUpdateTime = now;

  // Advance the interpolator timeline. The adaptive-rate playback system
  // adjusts speed based on buffer fullness to absorb jitter.
  m_Interpolator.Update(frameDt);

  plTransform interpolated;
  plVec3 velocity;
  bool bHasData = m_Interpolator.GetInterpolatedTransform(m_pNetworkModule->GetSyncedTime(), interpolated, &velocity);

  plVec3 targetPos;
  plQuat targetRot;
  bool bHasTarget = false;

  if (bHasData)
  {
    targetPos = interpolated.m_vPosition;
    targetRot = interpolated.m_qRotation;
    bHasTarget = true;
  }
  else if (m_vReplicatedPosition != plVec3::MakeZero())
  {
    // Interpolator has no data yet -- use last known replicated position
    targetPos = m_vReplicatedPosition;
    targetRot = m_qReplicatedRotation;
    bHasTarget = true;
  }

  if (!bHasTarget)
    return;

  // Initialize tracked state on first valid target. This avoids the
  // entity appearing to slide from the spawn position to its real location.
  if (!m_bFirstTargetReceived)
  {
    m_vLastAppliedPosition = targetPos;
    m_qLastAppliedRotation = targetRot;
    m_bFirstTargetReceived = true;
  }

  // Update tracked state. The adaptive-rate linear interpolation already
  // provides smooth, consistent-speed movement with no overshoot.
  m_vLastAppliedPosition = targetPos;
  m_qLastAppliedRotation = targetRot;

  ApplyRemotePosition(m_vLastAppliedPosition);
  ApplyRemoteRotation(m_qLastAppliedRotation);
}

void plNetworkedTransformComponent::ApplyRemotePosition(const plVec3& vPosition)
{
  GetOwner()->SetGlobalPosition(vPosition);
}

void plNetworkedTransformComponent::ApplyRemoteRotation(const plQuat& qRotation)
{
  GetOwner()->SetGlobalRotation(qRotation);
}

void plNetworkedTransformComponent::SendStateToHost()
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  plNetworkMessage msg = m_pNetworkModule->GetPeer()->BeginSendMessage();
  WriteTransformState(msg);

  msg.SetChannelType(plNetworkChannelTypeEnum::UnreliableOrdered);
  msg.SetPriority(plNetworkPriority::High);
  m_pNetworkModule->GetPeer()->SendMessages(msg);
}

void plNetworkedTransformComponent::BroadcastStateToClients(bool bIncludeOwner)
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  plUInt32 uiOwnerClientID = 0;
  if (const plNetworkObjectInfo* pInfo = m_pNetworkModule->GetObjectManager()->GetObjectInfo(GetNetID()))
  {
    uiOwnerClientID = pInfo->m_uiOwnerClientID;
  }

  auto& clients = m_pNetworkModule->GetClients();
  plDynamicArray<plNetworkConnection> connections;
  for (auto* pClient : clients)
  {
    if (pClient->m_uiClientID == m_pNetworkModule->GetLocalClientID())
      continue;

    if (!bIncludeOwner && pClient->m_uiClientID == uiOwnerClientID)
      continue;

    connections.PushBack(pClient->m_Connection);
  }
  if (connections.IsEmpty())
    return;

  plNetworkMessage msg = m_pNetworkModule->GetPeer()->BeginSendMessage();
  WriteTransformState(msg);

  msg.SetChannelType(plNetworkChannelTypeEnum::UnreliableOrdered);
  msg.SetPriority(plNetworkPriority::High);
  m_pNetworkModule->GetPeer()->SendMessages(msg, connections);
}

void plNetworkedTransformComponent::HandleTransformStateMessage(plNetworkMessage& msg)
{
  enum : plUInt8
  {
    HasVelocity = PL_BIT(0),
    FullPrecision = PL_BIT(1),
    BoundedPosition = PL_BIT(2),
    FullPosition = PL_BIT(3),
  };

  const plUInt8 uiFlags = msg.ReadUInt8();
  const plTime serverSendTime = plTime::MakeFromSeconds(msg.ReadDouble());
  const bool bFullPosition = (uiFlags & (FullPrecision | FullPosition)) != 0;
  const plVec3 vPos = bFullPosition ? msg.ReadVec3() :
    ((uiFlags & BoundedPosition) != 0 ? msg.ReadBoundedVec3(m_vWorldBoundsMin, m_vWorldBoundsMax) : msg.ReadVec3Half());
  const plQuat qRot = (uiFlags & FullPrecision) != 0 ? msg.ReadQuat() : msg.ReadQuatSmallestThree();
  const plVec3 vVel = (uiFlags & HasVelocity) == 0 ? plVec3::MakeZero() :
    ((uiFlags & FullPrecision) != 0 ? msg.ReadVec3() : msg.ReadVec3Half());
  if (msg.HasReadError() || !vPos.IsValid() || !qRot.IsValid() || !vVel.IsValid())
    return;

  m_vReplicatedPosition = vPos;
  m_qReplicatedRotation = qRot;
  m_vReplicatedVelocity = vVel;

  // Feed the interpolator on ALL peers (host and client) so that UpdateAsRemote()
  // can smoothly interpolate remote entities at the rendering frame rate.
  if (m_pNetworkModule)
  {
    plTransform t;
    t.m_vPosition = vPos;
    t.m_qRotation = qRot;
    t.m_vScale.Set(1.0f);

    m_Interpolator.AddSnapshot(t, vVel, serverSendTime, plTime::Now(), ++m_uiSnapshotSequence);
  }

  // Host: relay client-uploaded state to other clients (exclude owner; they already have it).
  if (m_pNetworkModule && m_pNetworkModule->IsHost())
  {
    BroadcastStateToClients(false);
  }
}

void plNetworkedTransformComponent::WriteTransformState(plNetworkMessage& msg) const
{
  enum : plUInt8
  {
    HasVelocity = PL_BIT(0),
    FullPrecision = PL_BIT(1),
    BoundedPosition = PL_BIT(2),
    FullPosition = PL_BIT(3),
  };

  plUInt8 uiFlags = m_bSyncVelocity ? HasVelocity : 0;
  if (m_bFullPrecision)
    uiFlags |= FullPrecision;
  else if (m_bUseBoundedPosition && m_vWorldBoundsMin.IsValid() && m_vWorldBoundsMax.IsValid() &&
           m_vReplicatedPosition.x >= m_vWorldBoundsMin.x && m_vReplicatedPosition.x <= m_vWorldBoundsMax.x &&
           m_vReplicatedPosition.y >= m_vWorldBoundsMin.y && m_vReplicatedPosition.y <= m_vWorldBoundsMax.y &&
           m_vReplicatedPosition.z >= m_vWorldBoundsMin.z && m_vReplicatedPosition.z <= m_vWorldBoundsMax.z)
    uiFlags |= BoundedPosition;
  else if (m_bUseBoundedPosition)
    uiFlags |= FullPosition;

  msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::TransformState));
  msg.WriteUInt32(GetNetID());
  msg.WriteUInt8(uiFlags);
  msg.WriteDouble(m_pNetworkModule->GetSyncedTime().GetSeconds());

  if ((uiFlags & (FullPrecision | FullPosition)) != 0)
    msg.WriteVec3(m_vReplicatedPosition);
  else if ((uiFlags & BoundedPosition) != 0)
    msg.WriteBoundedVec3(m_vReplicatedPosition, m_vWorldBoundsMin, m_vWorldBoundsMax);
  else
    msg.WriteVec3Half(m_vReplicatedPosition);

  if ((uiFlags & FullPrecision) != 0)
    msg.WriteQuat(m_qReplicatedRotation);
  else
    msg.WriteQuatSmallestThree(m_qReplicatedRotation);

  if (m_bSyncVelocity)
  {
    if ((uiFlags & FullPrecision) != 0)
      msg.WriteVec3(m_vReplicatedVelocity);
    else
      msg.WriteVec3Half(m_vReplicatedVelocity);
  }
}
