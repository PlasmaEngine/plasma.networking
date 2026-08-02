#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Components/NetworkedRigidbodyComponent.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <RendererCore/Debug/DebugRenderer.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plNetworkedRigidbodyComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("InterpolationSpeed", m_fInterpolationSpeed)->AddAttributes(new plDefaultValueAttribute(10.0f)),
    PL_MEMBER_PROPERTY("TeleportThreshold", m_fTeleportThreshold)->AddAttributes(new plDefaultValueAttribute(5.0f)),
    PL_MEMBER_PROPERTY("SyncVelocity", m_bSyncVelocity)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("SyncAngularVelocity", m_bSyncAngularVelocity)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Networking"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

plNetworkedRigidbodyComponent::plNetworkedRigidbodyComponent()
{
  m_NetworkTransform.SetIdentity();
  m_InterpolatedTransform.SetIdentity();
}

void plNetworkedRigidbodyComponent::OnSimulationStarted()
{
  plNetworkComponent::OnSimulationStarted();

  // Set network role so server/host replicates to clients
  SetNetworkRole(plNetworkRoleEnum::Host);

  m_NetworkTransform = GetOwner()->GetGlobalTransform();
  m_InterpolatedTransform = m_NetworkTransform;

  // Only clients (non-authority) should have kinematic physics
  // Server/host keeps dynamic physics for simulation
  if (m_pNetworkModule && m_pNetworkModule->GetPeer())
  {
    bool bIsClient = m_pNetworkModule->IsClient() && !m_pNetworkModule->IsHost();
    if (bIsClient)
    {
      MakePhysicsKinematic(true);
    }
  }
}

bool plNetworkedRigidbodyComponent::HasStateAuthority() const
{
  if (!m_pNetworkModule)
    return false;

  // Use base class NetID which is set by the spawn system
  plUInt32 netID = GetNetID();
  
  // If we don't have a network ID yet, fallback to server/host
  if (netID == 0)
  {
    return m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost();
  }

  auto* pObjectManager = m_pNetworkModule->GetObjectManager();
  if (!pObjectManager)
    return false;

  const plNetworkObjectInfo* pInfo = pObjectManager->GetObjectInfo(netID);
  if (!pInfo)
  {
    // Object not found in manager - fallback to server/host authority
    return m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost();
  }

  switch (pInfo->m_Authority.GetValue())
  {
    case plNetworkObjectRole::OwnedAuthoritative:
      return pInfo->m_uiOwnerClientID == m_pNetworkModule->GetLocalClientID();

    case plNetworkObjectRole::Replicated:
    case plNetworkObjectRole::ReplicatedSimulated:
      return m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost();

    default:
      return false;
  }
}

bool plNetworkedRigidbodyComponent::HasInputAuthority() const
{
  if (!m_pNetworkModule)
    return false;

  plUInt32 netID = GetNetID();
  if (netID == 0)
    return false;

  auto* pObjectManager = m_pNetworkModule->GetObjectManager();
  if (!pObjectManager)
    return false;

  const plNetworkObjectInfo* pInfo = pObjectManager->GetObjectInfo(netID);
  if (!pInfo)
    return false;

  return pInfo->m_uiOwnerClientID == m_pNetworkModule->GetLocalClientID();
}

void plNetworkedRigidbodyComponent::Update()
{
  if (!m_pNetworkModule)
  {
    // Try to get it now
    m_pNetworkModule = GetWorld()->GetModule<plNetworkWorldModule>();
    if (!m_pNetworkModule)
      return;
  }
  
  if (!m_pNetworkModule->GetPeer())
    return;

  float deltaTime = GetWorld()->GetClock().GetTimeDiff().AsFloatInSeconds();

  if (HasStateAuthority())
  {
    // We have authority: read from physics and replicate to others
    UpdateAsAuthority();
  }
  else
  {
    // We don't have authority: interpolate to network transform
    UpdateAsRemote(deltaTime);
  }
}

void plNetworkedRigidbodyComponent::UpdateAsAuthority()
{
  plTransform currentTransform = GetOwner()->GetGlobalTransform();

  // Check if transform changed enough to replicate
  float posDiff = (currentTransform.m_vPosition - m_NetworkTransform.m_vPosition).GetLength();
  bool rotEqual = currentTransform.m_qRotation.IsEqualRotation(m_NetworkTransform.m_qRotation, 0.001f);

  if (posDiff > 0.01f || !rotEqual)
  {
    m_NetworkTransform = currentTransform;

    // Read velocities from physics if syncing
    if (m_bSyncVelocity || m_bSyncAngularVelocity)
    {
      ReadPhysicsVelocities();
    }

    MarkDirty();
  }
}

void plNetworkedRigidbodyComponent::UpdateAsRemote(float deltaTime)
{
  plTransform currentTransform = GetOwner()->GetGlobalTransform();
  plVec3 positionDiff = m_NetworkTransform.m_vPosition - currentTransform.m_vPosition;
  float distance = positionDiff.GetLength();

  if (distance > m_fTeleportThreshold)
  {
    // Too far, teleport immediately
    GetOwner()->SetGlobalTransform(m_NetworkTransform);
    m_InterpolatedTransform = m_NetworkTransform;
  }
  else
  {
    // Smooth interpolation
    float lerpFactor = plMath::Clamp(m_fInterpolationSpeed * deltaTime, 0.0f, 1.0f);

    m_InterpolatedTransform.m_vPosition = plMath::Lerp(currentTransform.m_vPosition, m_NetworkTransform.m_vPosition, lerpFactor);
    m_InterpolatedTransform.m_qRotation = plQuat::MakeSlerp(currentTransform.m_qRotation, m_NetworkTransform.m_qRotation, lerpFactor);
    m_InterpolatedTransform.m_vScale = plMath::Lerp(currentTransform.m_vScale, m_NetworkTransform.m_vScale, lerpFactor);

    GetOwner()->SetGlobalTransform(m_InterpolatedTransform);
  }
}

void plNetworkedRigidbodyComponent::ReadPhysicsVelocities()
{
  const plRTTI* pJoltActorRTTI = plRTTI::FindTypeByName("plJoltDynamicActorComponent");
  if (pJoltActorRTTI == nullptr)
    return;

  plComponent* pPhysicsComponent = nullptr;
  if (!GetOwner()->TryGetComponentOfBaseType(pJoltActorRTTI, pPhysicsComponent) || pPhysicsComponent == nullptr)
    return;

  if (m_bSyncVelocity)
  {
    const plAbstractProperty* pVelProp = pJoltActorRTTI->FindPropertyByName("LinearVelocity");
    if (pVelProp != nullptr && pVelProp->GetCategory() == plPropertyCategory::Member)
    {
      auto pMemberProp = static_cast<const plAbstractMemberProperty*>(pVelProp);
      m_vLinearVelocity = plReflectionUtils::GetMemberPropertyValue(pMemberProp, pPhysicsComponent).ConvertTo<plVec3>();
    }
  }

  if (m_bSyncAngularVelocity)
  {
    const plAbstractProperty* pAngVelProp = pJoltActorRTTI->FindPropertyByName("AngularVelocity");
    if (pAngVelProp != nullptr && pAngVelProp->GetCategory() == plPropertyCategory::Member)
    {
      auto pMemberProp = static_cast<const plAbstractMemberProperty*>(pAngVelProp);
      m_vAngularVelocity = plReflectionUtils::GetMemberPropertyValue(pMemberProp, pPhysicsComponent).ConvertTo<plVec3>();
    }
  }
}

void plNetworkedRigidbodyComponent::NetworkSerialize(plNetworkMessage& msg)
{
  msg.WriteVec3(m_NetworkTransform.m_vPosition);
  msg.WriteQuat(m_NetworkTransform.m_qRotation);
  msg.WriteVec3(m_NetworkTransform.m_vScale);
  
  // Encode sync flags so receiver knows what to expect
  plUInt8 flags = 0;
  if (m_bSyncVelocity) flags |= 0x01;
  if (m_bSyncAngularVelocity) flags |= 0x02;
  msg.WriteUInt8(flags);
  
  if (m_bSyncVelocity)
    msg.WriteVec3(m_vLinearVelocity);
  
  if (m_bSyncAngularVelocity)
    msg.WriteVec3(m_vAngularVelocity);
}

void plNetworkedRigidbodyComponent::NetworkDeserialize(plNetworkMessage& msg)
{
  m_NetworkTransform.m_vPosition = msg.ReadVec3();
  m_NetworkTransform.m_qRotation = msg.ReadQuat();
  m_NetworkTransform.m_vScale = msg.ReadVec3();
  
  // Read sync flags from the message to know what was actually sent
  plUInt8 flags = msg.ReadUInt8();
  bool bHasVelocity = (flags & 0x01) != 0;
  bool bHasAngularVelocity = (flags & 0x02) != 0;
  
  if (bHasVelocity)
    m_vLinearVelocity = msg.ReadVec3();
  
  if (bHasAngularVelocity)
    m_vAngularVelocity = msg.ReadVec3();
}

void plNetworkedRigidbodyComponent::MakePhysicsKinematic(bool bKinematic)
{
  if (m_bIsKinematic == bKinematic)
    return;

  const plRTTI* pJoltActorRTTI = plRTTI::FindTypeByName("plJoltDynamicActorComponent");
  if (pJoltActorRTTI != nullptr)
  {
    plComponent* pPhysicsComponent = nullptr;
    if (GetOwner()->TryGetComponentOfBaseType(pJoltActorRTTI, pPhysicsComponent) && pPhysicsComponent != nullptr)
    {
      const plAbstractProperty* pKinematicProp = pJoltActorRTTI->FindPropertyByName("Kinematic");
      if (pKinematicProp != nullptr && pKinematicProp->GetCategory() == plPropertyCategory::Member)
      {
        auto pMemberProp = static_cast<const plAbstractMemberProperty*>(pKinematicProp);
        plReflectionUtils::SetMemberPropertyValue(pMemberProp, pPhysicsComponent, bKinematic);
        m_bIsKinematic = bKinematic;
        plLog::Dev("NetworkedRigidbody: Set physics kinematic to {}", bKinematic ? "true" : "false");
      }
    }
  }
}

void plNetworkedRigidbodyComponent::ApplyPhysicsVelocity(const plVec3& vLinear, const plVec3& vAngular)
{
  const plRTTI* pJoltActorRTTI = plRTTI::FindTypeByName("plJoltDynamicActorComponent");
  if (pJoltActorRTTI != nullptr)
  {
    plComponent* pPhysicsComponent = nullptr;
    if (GetOwner()->TryGetComponentOfBaseType(pJoltActorRTTI, pPhysicsComponent) && pPhysicsComponent != nullptr)
    {
      // Apply linear velocity
      const plAbstractProperty* pVelProp = pJoltActorRTTI->FindPropertyByName("LinearVelocity");
      if (pVelProp != nullptr && pVelProp->GetCategory() == plPropertyCategory::Member)
      {
        auto pMemberProp = static_cast<const plAbstractMemberProperty*>(pVelProp);
        plReflectionUtils::SetMemberPropertyValue(pMemberProp, pPhysicsComponent, vLinear);
      }

      // Apply angular velocity
      const plAbstractProperty* pAngVelProp = pJoltActorRTTI->FindPropertyByName("AngularVelocity");
      if (pAngVelProp != nullptr && pAngVelProp->GetCategory() == plPropertyCategory::Member)
      {
        auto pMemberProp = static_cast<const plAbstractMemberProperty*>(pAngVelProp);
        plReflectionUtils::SetMemberPropertyValue(pMemberProp, pPhysicsComponent, vAngular);
      }
    }
  }
}
