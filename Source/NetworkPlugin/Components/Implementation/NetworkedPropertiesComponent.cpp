#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Components/NetworkedPropertiesComponent.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <Core/ResourceManager/Implementation/ResourceLock.h>
#include <Core/ResourceManager/ResourceManager.h>

#include <Core/World/GameObject.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Reflection/ReflectionUtils.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plReplicateAuthority, 1)
  PL_ENUM_CONSTANTS(plReplicateAuthority::Server, plReplicateAuthority::Owner, plReplicateAuthority::Shared)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_TYPE(plNetworkedPropertyEntry, plNoBase, 1, plRTTIDefaultAllocator<plNetworkedPropertyEntry>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("ComponentType", m_sComponentType),
    PL_MEMBER_PROPERTY("ComponentIndex", m_uiComponentIndex)->AddAttributes(new plDefaultValueAttribute(0)),
    PL_MEMBER_PROPERTY("Property", m_sProperty),
    PL_MEMBER_PROPERTY("Profile", m_sProfile)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_NetworkReplicationProfile")),
    PL_ENUM_MEMBER_PROPERTY("Condition", plReplicateCondition, m_Condition),
    PL_ENUM_MEMBER_PROPERTY("Target", plReplicateTarget, m_Target),
    PL_ENUM_MEMBER_PROPERTY("Encoding", plNetSerializeEncoding, m_Encoding),
    PL_MEMBER_PROPERTY("Priority", m_uiPriority)->AddAttributes(new plDefaultValueAttribute(128)),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_COMPONENT_TYPE(plNetworkedPropertiesComponent, 2, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("SendRate", m_fSendRate)->AddAttributes(new plDefaultValueAttribute(20.0f), new plClampValueAttribute(1.0f, 60.0f)),
    PL_ENUM_MEMBER_PROPERTY("Authority", plReplicateAuthority, m_Authority),
    PL_MEMBER_PROPERTY("TakeOverRemotePhysics", m_bTakeOverRemotePhysics)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("InterpolationTime", m_fInterpolationTime)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 2.0f)),
    PL_ARRAY_MEMBER_PROPERTY("Properties", m_Properties)->AddAttributes(new plMaxArraySizeAttribute(32)),
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

void plNetworkedPropertiesComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_fSendRate;
  s << static_cast<plUInt8>(m_Authority.GetValue());
  s << m_bTakeOverRemotePhysics;
  s << m_fInterpolationTime;

  s << m_Properties.GetCount();
  for (const plNetworkedPropertyEntry& entry : m_Properties)
  {
    s << entry.m_sComponentType;
    s << entry.m_uiComponentIndex;
    s << entry.m_sProperty;
    s << entry.m_sProfile;
    s << static_cast<plUInt8>(entry.m_Condition.GetValue());
    s << static_cast<plUInt8>(entry.m_Target.GetValue());
    s << static_cast<plUInt8>(entry.m_Encoding.GetValue());
    s << entry.m_uiPriority;
  }
}

void plNetworkedPropertiesComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);

  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());
  if (uiVersion < 2)
    return; // exported before this component wrote any data; re-transform the scene

  plStreamReader& s = inout_stream.GetStream();

  s >> m_fSendRate;
  plUInt8 uiAuthority = 0;
  s >> uiAuthority;
  m_Authority = static_cast<plReplicateAuthority::Enum>(uiAuthority);
  s >> m_bTakeOverRemotePhysics;
  s >> m_fInterpolationTime;

  plUInt32 uiCount = 0;
  s >> uiCount;
  m_Properties.SetCount(uiCount);
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    plNetworkedPropertyEntry& entry = m_Properties[i];
    s >> entry.m_sComponentType;
    s >> entry.m_uiComponentIndex;
    s >> entry.m_sProperty;
    s >> entry.m_sProfile;
    plUInt8 uiCondition = 0, uiTarget = 0, uiEncoding = 0;
    s >> uiCondition;
    s >> uiTarget;
    s >> uiEncoding;
    entry.m_Condition = static_cast<plReplicateCondition::Enum>(uiCondition);
    entry.m_Target = static_cast<plReplicateTarget::Enum>(uiTarget);
    entry.m_Encoding = static_cast<plNetSerializeEncoding::Enum>(uiEncoding);
    s >> entry.m_uiPriority;
  }
}

void plNetworkedPropertiesComponent::OnSimulationStarted()
{
  plNetworkComponent::OnSimulationStarted();

  switch (m_Authority)
  {
    case plReplicateAuthority::Server:
      SetNetworkRole(plNetworkRoleEnum::Server);
      break;
    case plReplicateAuthority::Owner:
      SetNetworkRole(plNetworkRoleEnum::Client);
      break;
    case plReplicateAuthority::Shared:
    default:
      SetNetworkRole(plNetworkRoleEnum::Both);
      break;
  }

  SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  ResolveEntries();
}

void plNetworkedPropertiesComponent::ResolveEntries()
{
  m_ResolvedEntries.Clear();
  m_ResolvedEntries.SetCount(plMath::Min<plUInt32>(m_Properties.GetCount(), 32));

  if (m_Properties.GetCount() > 32)
  {
    plLog::Error("PL Net: NetworkedProperties on '{}' has {} entries; only the first 32 are supported.",
      GetOwner()->GetName(), m_Properties.GetCount());
  }

  for (plUInt32 uiEntry = 0; uiEntry < m_ResolvedEntries.GetCount(); ++uiEntry)
  {
    const plNetworkedPropertyEntry& entry = m_Properties[uiEntry];

    if (entry.m_sComponentType.IsEqual("plGameObject"))
    {
      const plAbstractProperty* pProperty = plGetStaticRTTI<plGameObject>()->FindPropertyByName(entry.m_sProperty);
      ResolvedEntry& resolved = m_ResolvedEntries[uiEntry];

      if (pProperty != nullptr && pProperty->GetCategory() == plPropertyCategory::Member)
      {
        resolved.m_pInstance = GetOwner();
        resolved.m_pProperty = static_cast<const plAbstractMemberProperty*>(pProperty);
        resolved.m_VariantType = plNetworkSerializer::GetVariantTypeForProperty(resolved.m_pProperty);
        resolved.m_bResolved = plNetworkSerializer::IsVariantTypeSupported(resolved.m_VariantType);
        resolved.m_Kind = resolved.m_bResolved ? ResolvedEntry::Kind::Member : ResolvedEntry::Kind::Invalid;
      }
      else if (pProperty != nullptr && pProperty->GetCategory() == plPropertyCategory::Set && entry.m_sProperty.IsEqual("Tags"))
      {
        resolved.m_pInstance = GetOwner();
        resolved.m_bResolved = true;
        resolved.m_Kind = ResolvedEntry::Kind::GameObjectTags;
      }

      if (resolved.m_bResolved)
      {
        resolved.m_LastObserved = GetCurrentValue(resolved);
        if (!entry.m_sProfile.IsEmpty())
          resolved.m_hProfile = plResourceManager::LoadResource<plNetworkReplicationProfileResource>(entry.m_sProfile);
      }
    }
    else
    {
      plUInt32 uiTypeIndex = 0;

      for (plComponent* pComponent : GetOwner()->GetComponents())
      {
        if (!entry.m_sComponentType.IsEqual(pComponent->GetDynamicRTTI()->GetTypeName()))
          continue;

        if (uiTypeIndex++ != entry.m_uiComponentIndex)
          continue;

        const plAbstractProperty* pProperty = pComponent->GetDynamicRTTI()->FindPropertyByName(entry.m_sProperty);
        if (pProperty == nullptr || pProperty->GetCategory() != plPropertyCategory::Member)
          break;

        ResolvedEntry& resolved = m_ResolvedEntries[uiEntry];
        resolved.m_pInstance = pComponent;
        resolved.m_pProperty = static_cast<const plAbstractMemberProperty*>(pProperty);
        resolved.m_VariantType = plNetworkSerializer::GetVariantTypeForProperty(resolved.m_pProperty);
        resolved.m_bResolved = plNetworkSerializer::IsVariantTypeSupported(resolved.m_VariantType);
        resolved.m_Kind = resolved.m_bResolved ? ResolvedEntry::Kind::Member : ResolvedEntry::Kind::Invalid;
        if (resolved.m_bResolved)
        {
          resolved.m_LastObserved = GetCurrentValue(resolved);
          if (!entry.m_sProfile.IsEmpty())
            resolved.m_hProfile = plResourceManager::LoadResource<plNetworkReplicationProfileResource>(entry.m_sProfile);
        }
        break;
      }
    }

    if (!m_ResolvedEntries[uiEntry].m_bResolved)
    {
      plLog::Warning("PL Net: Could not resolve replicated property '{}[{}].{}' on '{}'; entry is disabled.",
        entry.m_sComponentType, entry.m_uiComponentIndex, entry.m_sProperty, GetOwner()->GetName());
    }
  }
}

plVariant plNetworkedPropertiesComponent::GetCurrentValue(const ResolvedEntry& entry) const
{
  if (!entry.m_bResolved)
    return plVariant();

  if (entry.m_Kind == ResolvedEntry::Kind::GameObjectTags)
  {
    plVariantArray tags;
    tags.Reserve(GetOwner()->GetTags().GetNumTagsSet());
    for (const plTag& tag : GetOwner()->GetTags())
      tags.PushBack(tag.GetTagString());
    return plVariant(tags);
  }

  return plReflectionUtils::GetMemberPropertyValue(entry.m_pProperty, entry.m_pInstance);
}

void plNetworkedPropertiesComponent::WriteValue(
  plNetworkMessage& msg, const ResolvedEntry& entry, plNetSerializeEncoding::Enum encoding) const
{
  if (entry.m_Kind == ResolvedEntry::Kind::GameObjectTags)
  {
    const plTagSet& tags = GetOwner()->GetTags();
    const plUInt32 uiCount = plMath::Min<plUInt32>(tags.GetNumTagsSet(), 64);
    msg.WriteUInt8(static_cast<plUInt8>(uiCount));

    plUInt32 uiWritten = 0;
    for (const plTag& tag : tags)
    {
      if (uiWritten++ >= uiCount)
        break;
      msg.WriteString(tag.GetTagString());
    }
    return;
  }

  plNetworkSerializer::WriteVariantEncoded(msg, GetCurrentValue(entry), encoding);
}

plVariant plNetworkedPropertiesComponent::ReadValue(
  plNetworkMessage& msg, const ResolvedEntry& entry, plNetSerializeEncoding::Enum encoding) const
{
  if (entry.m_Kind == ResolvedEntry::Kind::GameObjectTags)
  {
    const plUInt8 uiCount = msg.ReadUInt8();
    if (uiCount > 64)
    {
      msg.MarkReadError();
      return plVariant();
    }

    plVariantArray tags;
    tags.Reserve(uiCount);
    for (plUInt8 i = 0; i < uiCount; ++i)
      tags.PushBack(msg.ReadString());
    return msg.HasReadError() ? plVariant() : plVariant(tags);
  }

  return plNetworkSerializer::ReadVariantEncoded(msg, entry.m_VariantType, encoding);
}

void plNetworkedPropertiesComponent::ApplyValue(ResolvedEntry& entry, const plVariant& value)
{
  if (entry.m_Kind == ResolvedEntry::Kind::GameObjectTags)
  {
    plTagSet tags;
    for (const plVariant& tag : value.Get<plVariantArray>())
      tags.SetByName(tag.ConvertTo<plString>());
    GetOwner()->SetTags(tags);
    return;
  }

  if (entry.m_pProperty->GetFlags().IsSet(plPropertyFlags::IsEnum))
    plReflectionUtils::SetMemberPropertyValue(entry.m_pProperty, entry.m_pInstance, value.ConvertTo<plInt64>());
  else
    plReflectionUtils::SetMemberPropertyValue(entry.m_pProperty, entry.m_pInstance, value);
}

void plNetworkedPropertiesComponent::Update()
{
  if (m_pNetworkModule == nullptr || m_pNetworkModule->GetPeer() == nullptr)
    return;

  if (!m_pNetworkModule->HasLocalAuthority(this))
  {
    // passive peer: keep local physics from fighting the replicated values, blend received values in
    if (m_bTakeOverRemotePhysics && !m_bRemotePhysicsTakenOver && m_pNetworkModule->IsConnected() && ReplicatesOwnerTransform())
      TakeOverRemotePhysics();

    TickInterpolation();
    return;
  }

  const plTime now = plTime::Now();
  const plTime interval = plTime::MakeFromSeconds(m_fSendRate > 0.0f ? 1.0 / m_fSendRate : 0.05);
  if (now - m_LastPollTime < interval)
    return;
  m_LastPollTime = now;

  bool bNeedsReplication = false;
  plUInt8 uiBestPriority = 255;
  for (plUInt32 i = 0; i < m_ResolvedEntries.GetCount(); ++i)
  {
    ResolvedEntry& resolved = m_ResolvedEntries[i];
    const plNetworkReplicationProfileDescriptor settings = GetSettings(i);
    uiBestPriority = plMath::Min(uiBestPriority, settings.m_uiPriority);
    if (!resolved.m_bResolved || settings.m_Condition == plReplicateCondition::InitialOnly)
      continue;

    const plVariant current = GetCurrentValue(resolved);
    if (!current.IsValid())
      continue;

    if (settings.m_Condition == plReplicateCondition::Always || !resolved.m_LastObserved.IsValid() || current != resolved.m_LastObserved)
    {
      bNeedsReplication = true;
      resolved.m_LastObserved = current;
    }
  }

  if (GetNetID() != 0 && uiBestPriority != 255)
  {
    const plNetworkPriority::Enum priority = uiBestPriority <= 51 ? plNetworkPriority::Critical :
      uiBestPriority <= 102 ? plNetworkPriority::High :
      uiBestPriority <= 153 ? plNetworkPriority::Normal :
      uiBestPriority <= 204 ? plNetworkPriority::Low : plNetworkPriority::VeryLow;
    m_pNetworkModule->GetBandwidthManager()->SetComponentPriority(GetNetID(), priority);
  }

  if (bNeedsReplication)
  {
    MarkDirty();

    if (m_uiDebugSendLogs < 5)
    {
      ++m_uiDebugSendLogs;
      plLog::Dev("PL Net: Props '{}' (NetID {}) marked dirty for replication.", GetOwner()->GetName(), GetNetID());
    }
  }
}

plNetworkReplicationProfileDescriptor plNetworkedPropertiesComponent::GetSettings(plUInt32 uiEntry) const
{
  plNetworkReplicationProfileDescriptor settings;
  settings.m_fSendRate = m_fSendRate;
  settings.m_Condition = m_Properties[uiEntry].m_Condition;
  settings.m_Target = m_Properties[uiEntry].m_Target;
  settings.m_Encoding = m_Properties[uiEntry].m_Encoding;
  settings.m_uiPriority = m_Properties[uiEntry].m_uiPriority;

  if (m_ResolvedEntries[uiEntry].m_hProfile.IsValid())
  {
    plResourceLock<plNetworkReplicationProfileResource> profile(
      m_ResolvedEntries[uiEntry].m_hProfile, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
    settings = profile->GetDescriptor();
    settings.m_fSendRate = plMath::Min(settings.m_fSendRate, m_fSendRate);
  }
  return settings;
}

plUInt32 plNetworkedPropertiesComponent::GetOwnerClientID() const
{
  if (m_pNetworkModule == nullptr)
    return 0;

  const plNetworkObjectManager* pObjectManager = m_pNetworkModule->GetObjectManager();
  if (const plNetworkObjectInfo* pInfo = pObjectManager->GetObjectInfo(GetNetID()))
    return pInfo->m_uiOwnerClientID;

  const plUInt32 uiObjectID = pObjectManager->GetNetworkID(GetOwner()->GetHandle());
  if (const plNetworkObjectInfo* pInfo = pObjectManager->GetObjectInfo(uiObjectID))
    return pInfo->m_uiOwnerClientID;

  return m_pNetworkModule->GetServerClientID();
}

void plNetworkedPropertiesComponent::NetworkSerialize(plNetworkMessage& msg)
{
  NetworkSerializeForClient(msg, 0, false);
}

void plNetworkedPropertiesComponent::NetworkSerializeForClient(plNetworkMessage& msg, plUInt32 uiRecipientClientID, bool bInitialSync)
{
  plDynamicArray<plVariant>& lastSent = m_LastSentValues[uiRecipientClientID];
  plDynamicArray<plTime>& lastSentTimes = m_LastSentTimes[uiRecipientClientID];
  lastSent.SetCount(m_ResolvedEntries.GetCount());
  lastSentTimes.SetCount(m_ResolvedEntries.GetCount());
  const plUInt32 uiOwnerClientID = GetOwnerClientID();
  const plTime now = plTime::Now();
  plUInt32 uiBitmask = 0;

  for (plUInt32 i = 0; i < m_ResolvedEntries.GetCount(); ++i)
  {
    const ResolvedEntry& resolved = m_ResolvedEntries[i];
    const plNetworkReplicationProfileDescriptor settings = GetSettings(i);
    if (!resolved.m_bResolved)
      continue;

    if (settings.m_Condition == plReplicateCondition::InitialOnly && !bInitialSync)
      continue;
    if (settings.m_Target == plReplicateTarget::OwnerOnly && uiRecipientClientID != 0 && uiRecipientClientID != uiOwnerClientID)
      continue;
    if (settings.m_Target == plReplicateTarget::NonOwner && uiRecipientClientID != 0 && uiRecipientClientID == uiOwnerClientID)
      continue;
    if (!bInitialSync && lastSentTimes[i].GetSeconds() > 0.0 &&
        now - lastSentTimes[i] < plTime::MakeFromSeconds(1.0 / plMath::Max(settings.m_fSendRate, 0.1f)))
      continue;

    const plVariant value = GetCurrentValue(resolved);
    if (!value.IsValid())
      continue;
    if (!bInitialSync && settings.m_Condition == plReplicateCondition::OnChange && lastSent[i].IsValid() && lastSent[i] == value)
      continue;

    uiBitmask |= PL_BIT(i);
  }

  msg.WriteUInt32(uiBitmask);
  for (plUInt32 i = 0; i < m_ResolvedEntries.GetCount(); ++i)
  {
    if ((uiBitmask & PL_BIT(i)) == 0)
      continue;

    const plVariant value = GetCurrentValue(m_ResolvedEntries[i]);
    const plNetworkReplicationProfileDescriptor settings = GetSettings(i);
    WriteValue(msg, m_ResolvedEntries[i], settings.m_Encoding);
    lastSent[i] = value;
    lastSentTimes[i] = now;
  }
}

void plNetworkedPropertiesComponent::NetworkDeserialize(plNetworkMessage& msg)
{
  const plUInt32 uiBitmask = msg.ReadUInt32();

  if (m_uiDebugRecvLogs < 5)
  {
    ++m_uiDebugRecvLogs;
    plLog::Dev("PL Net: Props '{}' (NetID {}) received replication (mask {}).", GetOwner()->GetName(), GetNetID(), uiBitmask);
  }
  for (plUInt32 i = 0; i < m_ResolvedEntries.GetCount(); ++i)
  {
    if ((uiBitmask & PL_BIT(i)) == 0)
      continue;

    ResolvedEntry& resolved = m_ResolvedEntries[i];
    if (!resolved.m_bResolved)
    {
      plLog::Warning("PL Net: Cannot consume unresolved replicated property at index {}.", i);
      return;
    }

    const plNetworkReplicationProfileDescriptor settings = GetSettings(i);
    const plVariant value = ReadValue(msg, resolved, settings.m_Encoding);
    if (msg.HasReadError() || !value.IsValid())
      return;

    // blend interpolatable values in over time on passive peers; snap everything else
    if (m_fInterpolationTime > 0.0f && IsInterpolatableType(value.GetType()) &&
        m_pNetworkModule != nullptr && !m_pNetworkModule->HasLocalAuthority(this))
    {
      const plVariant current = GetCurrentValue(resolved);
      if (current.IsValid() && current.GetType() == value.GetType())
      {
        resolved.m_InterpStart = current;
        resolved.m_InterpTarget = value;
        resolved.m_InterpStartTime = plTime::Now();
        resolved.m_bInterpolating = true;
        resolved.m_LastObserved = value;
        continue;
      }
    }

    resolved.m_bInterpolating = false;
    ApplyValue(resolved, value);
    resolved.m_LastObserved = value;
  }
}

bool plNetworkedPropertiesComponent::IsInterpolatableType(plVariantType::Enum type)
{
  switch (type)
  {
    case plVariantType::Float:
    case plVariantType::Double:
    case plVariantType::Vector2:
    case plVariantType::Vector3:
    case plVariantType::Vector4:
    case plVariantType::Quaternion:
      return true;
    default:
      return false;
  }
}

plVariant plNetworkedPropertiesComponent::LerpVariant(const plVariant& a, const plVariant& b, float fT)
{
  switch (b.GetType())
  {
    case plVariantType::Float:
      return plMath::Lerp(a.Get<float>(), b.Get<float>(), fT);
    case plVariantType::Double:
      return plMath::Lerp(a.Get<double>(), b.Get<double>(), static_cast<double>(fT));
    case plVariantType::Vector2:
      return plMath::Lerp(a.Get<plVec2>(), b.Get<plVec2>(), fT);
    case plVariantType::Vector3:
      return plMath::Lerp(a.Get<plVec3>(), b.Get<plVec3>(), fT);
    case plVariantType::Vector4:
      return plMath::Lerp(a.Get<plVec4>(), b.Get<plVec4>(), fT);
    case plVariantType::Quaternion:
      return plQuat::MakeSlerp(a.Get<plQuat>(), b.Get<plQuat>(), fT);
    default:
      return b;
  }
}

void plNetworkedPropertiesComponent::TickInterpolation()
{
  const plTime now = plTime::Now();
  const double fDuration = plMath::Max(0.001, static_cast<double>(m_fInterpolationTime));

  for (ResolvedEntry& resolved : m_ResolvedEntries)
  {
    if (!resolved.m_bInterpolating)
      continue;

    const double fT = plMath::Clamp((now - resolved.m_InterpStartTime).GetSeconds() / fDuration, 0.0, 1.0);
    ApplyValue(resolved, LerpVariant(resolved.m_InterpStart, resolved.m_InterpTarget, static_cast<float>(fT)));

    if (fT >= 1.0)
      resolved.m_bInterpolating = false;
  }
}

bool plNetworkedPropertiesComponent::ReplicatesOwnerTransform() const
{
  for (const ResolvedEntry& resolved : m_ResolvedEntries)
  {
    if (resolved.m_bResolved && resolved.m_Kind == ResolvedEntry::Kind::Member && resolved.m_pInstance == GetOwner())
      return true;
  }
  return false;
}

void plNetworkedPropertiesComponent::TakeOverRemotePhysics()
{
  m_bRemotePhysicsTakenOver = true;

  for (plComponent* pComponent : GetOwner()->GetComponents())
  {
    // resolve by type name to avoid a hard dependency on the Jolt plugin
    bool bIsDynamicActor = false;
    for (const plRTTI* pRtti = pComponent->GetDynamicRTTI(); pRtti != nullptr; pRtti = pRtti->GetParentType())
    {
      if (pRtti->GetTypeName() == "plJoltDynamicActorComponent")
      {
        bIsDynamicActor = true;
        break;
      }
    }

    if (!bIsDynamicActor)
      continue;

    const plAbstractProperty* pProperty = pComponent->GetDynamicRTTI()->FindPropertyByName("Kinematic");
    if (pProperty == nullptr || pProperty->GetCategory() != plPropertyCategory::Member)
      continue;

    const auto* pMember = static_cast<const plAbstractMemberProperty*>(pProperty);
    if (plReflectionUtils::GetMemberPropertyValue(pMember, pComponent).ConvertTo<bool>())
      continue; // already kinematic; nothing to take over or restore

    plReflectionUtils::SetMemberPropertyValue(pMember, pComponent, plVariant(true));
    m_TakenOverActors.PushBack(pComponent->GetHandle());
    plLog::Dev("PL Net: '{}' switched to kinematic on non-authoritative peer.", GetOwner()->GetName());
  }
}

void plNetworkedPropertiesComponent::RestoreRemotePhysics()
{
  for (const plComponentHandle& hActor : m_TakenOverActors)
  {
    plComponent* pComponent = nullptr;
    if (!GetWorld()->TryGetComponent(hActor, pComponent))
      continue;

    const plAbstractProperty* pProperty = pComponent->GetDynamicRTTI()->FindPropertyByName("Kinematic");
    if (pProperty != nullptr && pProperty->GetCategory() == plPropertyCategory::Member)
      plReflectionUtils::SetMemberPropertyValue(static_cast<const plAbstractMemberProperty*>(pProperty), pComponent, plVariant(false));
  }

  m_TakenOverActors.Clear();
  m_bRemotePhysicsTakenOver = false;
}

void plNetworkedPropertiesComponent::OnNetworkStateChanged(plNetworkConnectionState::Enum state)
{
  plNetworkComponent::OnNetworkStateChanged(state);

  if (state == plNetworkConnectionState::Disconnected || state == plNetworkConnectionState::Offline)
    RestoreRemotePhysics();
}

PL_STATICLINK_FILE(NetworkPlugin, NetworkPlugin_Components_Implementation_NetworkedPropertiesComponent);
