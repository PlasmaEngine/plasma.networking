// Networked blackboard replication (ObjectReplicate only — no custom message IDs).
//
// Authority modes:
//   ServerAuthoritative — host/server polls the board and broadcasts; clients apply only.
//   OwnerWritable       — owning client uploads; server validates sender via ObjectReplicate authority.
//
// Entry set: all entries by default, or only those with plBlackboardEntryFlags::Save when filtered.

#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Components/NetworkedBlackboardComponent.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/Stream.h>
#include <GameEngine/Gameplay/BlackboardComponent.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <NetworkPlugin/Core/NetworkVariant.h>

namespace
{
  bool TryWriteVariantBlob(plNetworkMessage& msg, const plVariant& value)
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    writer << value;

    const plUInt64 uiSize = storage.GetStorageSize64();
    if (uiSize == 0 || uiSize > 64 * 1024)
      return false;

    const plUInt32 uiSize32 = static_cast<plUInt32>(uiSize);
    msg.WriteUInt32(uiSize32);

    plMemoryStreamReader reader(&storage);
    plHybridArray<plUInt8, 256> temp;
    temp.SetCountUninitialized(uiSize32);
    if (reader.ReadBytes(temp.GetData(), uiSize32) != uiSize32)
      return false;

    msg.WriteBytes(temp.GetData(), uiSize32);
    return true;
  }

  bool TryReadVariantBlob(plNetworkMessage& msg, plVariant& out_value)
  {
    return plReadNetworkVariant(msg, out_value);
  }
} // namespace

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkBlackboardAuthority, 1)
  PL_ENUM_CONSTANTS(plNetworkBlackboardAuthority::ServerAuthoritative, plNetworkBlackboardAuthority::OwnerWritable)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_COMPONENT_TYPE(plNetworkedBlackboardComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Authority", plNetworkBlackboardAuthority, m_Authority),
    PL_MEMBER_PROPERTY("SendRate", m_fSendRate)->AddAttributes(new plDefaultValueAttribute(15.0f), new plClampValueAttribute(1.0f, 60.0f)),
    PL_MEMBER_PROPERTY("BlackboardName", m_sBlackboardName),
    PL_MEMBER_PROPERTY("ReplicateSaveFlaggedOnly", m_bReplicateSaveFlaggedOnly),
    PL_MEMBER_PROPERTY("SyncEntryFlags", m_bSyncEntryFlags)->AddAttributes(new plDefaultValueAttribute(true)),
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

plNetworkedBlackboardComponent::plNetworkedBlackboardComponent() = default;

void plNetworkedBlackboardComponent::ResolveBlackboard()
{
  if (m_pBoard != nullptr)
    return;

  plBlackboardComponent* pBbComp = nullptr;
  if (GetOwner()->TryGetComponentOfBaseType(pBbComp))
  {
    m_pBoard = pBbComp->GetBoard();
    return;
  }

  if (!m_sBlackboardName.IsEmpty())
  {
    m_pBoard = plBlackboardComponent::FindBlackboard(GetOwner(), m_sBlackboardName);
    return;
  }

  if (!m_bLoggedMissingBoard)
  {
    m_bLoggedMissingBoard = true;
    plLog::Warning("PL Net: NetworkedBlackboard on '{}' has no plBlackboardComponent and BlackboardName is empty.",
      GetOwner()->GetName());
  }
}

bool plNetworkedBlackboardComponent::HasReplicationAuthority() const
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return false;

  if (m_Authority == plNetworkBlackboardAuthority::ServerAuthoritative)
    return m_pNetworkModule->IsHost() || m_pNetworkModule->IsServer();

  return IsLocallyOwned();
}

bool plNetworkedBlackboardComponent::ShouldReplicateEntry(const plBlackboard::Entry& entry) const
{
  if (!m_bReplicateSaveFlaggedOnly)
    return true;

  return entry.m_Flags.IsSet(plBlackboardEntryFlags::Save);
}

bool plNetworkedBlackboardComponent::HasEntryChanges() const
{
  if (!m_pBoard)
    return false;

  const plUInt32 uiBoardCounter = m_pBoard->GetBlackboardChangeCounter();
  if (uiBoardCounter != m_uiLastBlackboardChangeCounter)
    return true;

  for (auto it = m_pBoard->GetAllEntries().GetIterator(); it.IsValid(); ++it)
  {
    if (!ShouldReplicateEntry(it.Value()))
      continue;

    plUInt32 uiCached = 0;
    if (!m_LastEntryChangeCounters.TryGetValue(it.Key(), uiCached) || uiCached != it.Value().m_uiChangeCounter)
      return true;
  }

  return false;
}

void plNetworkedBlackboardComponent::RefreshChangeCacheFromBoard()
{
  if (!m_pBoard)
    return;

  m_uiLastBlackboardChangeCounter = m_pBoard->GetBlackboardChangeCounter();

  for (auto it = m_pBoard->GetAllEntries().GetIterator(); it.IsValid(); ++it)
  {
    if (ShouldReplicateEntry(it.Value()))
      m_LastEntryChangeCounters[it.Key()] = it.Value().m_uiChangeCounter;
  }

  PruneStaleCacheEntries();
}

void plNetworkedBlackboardComponent::PruneStaleCacheEntries()
{
  if (!m_pBoard)
    return;

  plHybridArray<plHashedString, 16> toRemove;

  for (auto it = m_LastEntryChangeCounters.GetIterator(); it.IsValid(); ++it)
  {
    if (!m_pBoard->GetAllEntries().Contains(it.Key()))
      toRemove.PushBack(it.Key());
  }

  for (const plHashedString& key : toRemove)
    m_LastEntryChangeCounters.Remove(key);
}

void plNetworkedBlackboardComponent::OnSimulationStarted()
{
  plNetworkComponent::OnSimulationStarted();

  // Network-spawned objects give every plNetworkComponent the same NetID (e.g. FirstPersonPlayer shares it).
  // ObjectReplicate delivers to the first matching component only — use a derived ID for this component.
  if (m_pNetworkModule)
  {
    const plUInt32 uiObjectNetID = m_pNetworkModule->GetObjectManager()->GetNetworkID(GetOwner()->GetHandle());
    const plUInt32 uiRegisteredNetID = GetNetID();

    if (uiObjectNetID != 0 && uiObjectNetID == uiRegisteredNetID)
    {
      m_uiOwnerObjectNetID = uiObjectNetID;
      const plUInt32 uiComponentNetID = plHashingUtils::CombineHashValues32(
        uiObjectNetID, plHashingUtils::StringHashTo32(plHashingUtils::StringHash(GetDynamicRTTI()->GetTypeName())));
      SetNetID(uiComponentNetID);
    }
  }

  SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);

  if (m_Authority == plNetworkBlackboardAuthority::ServerAuthoritative)
  {
    if (m_pNetworkModule && (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost()))
      SetNetworkRole(plNetworkRoleEnum::Host);
    else
      SetNetworkRole(plNetworkRoleEnum::Server);
  }
  else
  {
    SetNetworkRole(plNetworkRoleEnum::Client);
  }

  m_bForceFullSnapshot = true;
  ResolveBlackboard();
  RefreshChangeCacheFromBoard();
}

void plNetworkedBlackboardComponent::OnNetworkStateChanged(plNetworkConnectionState::Enum state)
{
  plNetworkComponent::OnNetworkStateChanged(state);

  if (state == plNetworkConnectionState::Connected && HasReplicationAuthority())
  {
    m_bForceFullSnapshot = true;
    ResolveBlackboard();
    RefreshChangeCacheFromBoard();
  }
}

void plNetworkedBlackboardComponent::Update()
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  ResolveBlackboard();
  if (!m_pBoard)
    return;

  if (!HasReplicationAuthority())
    return;

  const plTime now = m_pNetworkModule->GetWorld()->GetClock().GetAccumulatedTime();
  const float fMinInterval = 1.0f / plMath::Max(m_fSendRate, 1.0f);

  if (m_pBoard->GetBlackboardChangeCounter() != m_uiLastBlackboardChangeCounter)
    m_bForceFullSnapshot = true;

  if (!HasEntryChanges())
    return;

  if (!m_bForceFullSnapshot && (now - m_LastSendTime).AsFloatInSeconds() < fMinInterval)
    return;

  m_LastSendTime = now;
  MarkDirty();
}

void plNetworkedBlackboardComponent::NetworkSerialize(plNetworkMessage& msg)
{
  if (!m_pBoard)
    ResolveBlackboard();

  const bool bFullSnapshot = m_bForceFullSnapshot;
  msg.WriteUInt8(bFullSnapshot ? 1 : 0);

  plHybridArray<plHashedString, 32> entriesToSend;

  if (m_pBoard)
  {
    for (auto it = m_pBoard->GetAllEntries().GetIterator(); it.IsValid(); ++it)
    {
      if (!ShouldReplicateEntry(it.Value()))
        continue;

      if (bFullSnapshot)
      {
        entriesToSend.PushBack(it.Key());
        continue;
      }

      plUInt32 uiCached = 0;
      if (!m_LastEntryChangeCounters.TryGetValue(it.Key(), uiCached) || uiCached != it.Value().m_uiChangeCounter)
        entriesToSend.PushBack(it.Key());
    }
  }

  const plUInt16 uiCount = static_cast<plUInt16>(plMath::Min<plUInt32>(entriesToSend.GetCount(), 0xFFFF));
  msg.WriteUInt16(uiCount);

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const plHashedString& sName = entriesToSend[i];
    const plBlackboard::Entry* pEntry = m_pBoard->GetEntry(sName);
    if (pEntry == nullptr)
      continue;

    msg.WriteString(sName.GetView());

    const plUInt16 uiFlags = static_cast<plUInt16>(pEntry->m_Flags.GetValue());
    msg.WriteUInt16(uiFlags);

    if (!TryWriteVariantBlob(msg, pEntry->m_Value))
    {
      static bool s_bLoggedVariantFailure = false;
      if (!s_bLoggedVariantFailure)
      {
        s_bLoggedVariantFailure = true;
        plLog::Warning("PL Net: NetworkedBlackboard failed to serialize variant for entry '{}'.", sName.GetData());
      }
    }
  }

  m_bForceFullSnapshot = false;
  RefreshChangeCacheFromBoard();
}

void plNetworkedBlackboardComponent::NetworkDeserialize(plNetworkMessage& msg)
{
  const bool bFullSnapshot = msg.ReadUInt8() != 0;
  const plUInt16 uiCount = msg.ReadUInt16();
  if (msg.HasReadError() || uiCount > msg.GetRemainingBytes() / 10)
  {
    msg.MarkReadError();
    return;
  }

  if (!m_pBoard)
    ResolveBlackboard();

  if (!m_pBoard)
    return;

  PL_IGNORE_UNUSED(bFullSnapshot);

  for (plUInt16 i = 0; i < uiCount; ++i)
  {
    const plString sName = msg.ReadString();
    const plUInt16 uiFlags = msg.ReadUInt16();

    plVariant value;
    if (!TryReadVariantBlob(msg, value))
      return;

    plHashedString sHashedName;
    sHashedName.Assign(sName.GetData());

    m_pBoard->SetEntryValue(sHashedName, value);

    if (m_bSyncEntryFlags)
    {
      m_pBoard->SetEntryFlags(sHashedName, plBlackboardEntryFlags::Enum(uiFlags));
    }
  }

  RefreshChangeCacheFromBoard();
}
