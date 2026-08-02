#include <NetworkPlugin/NetworkPluginPCH.h>

#include <Foundation/Utilities/AssetFileHeader.h>
#include <NetworkPlugin/Core/NetworkReplicationProfileResource.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_TYPE(plNetworkReplicationProfileDescriptor, plNoBase, 1, plRTTIDefaultAllocator<plNetworkReplicationProfileDescriptor>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("SendRate", m_fSendRate)->AddAttributes(new plDefaultValueAttribute(20.0f), new plClampValueAttribute(0.1f, 60.0f)),
    PL_ENUM_MEMBER_PROPERTY("Condition", plReplicateCondition, m_Condition),
    PL_ENUM_MEMBER_PROPERTY("Target", plReplicateTarget, m_Target),
    PL_ENUM_MEMBER_PROPERTY("Encoding", plNetSerializeEncoding, m_Encoding),
    PL_MEMBER_PROPERTY("Priority", m_uiPriority)->AddAttributes(new plDefaultValueAttribute(128)),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plNetworkReplicationProfileResource, 1, plRTTIDefaultAllocator<plNetworkReplicationProfileResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plNetworkReplicationProfileResource);
PL_RESOURCE_IMPLEMENT_CREATEABLE(plNetworkReplicationProfileResource, plNetworkReplicationProfileDescriptor)
{
  m_Descriptor = descriptor;

  plResourceLoadDesc result;
  result.m_uiQualityLevelsDiscardable = 0;
  result.m_uiQualityLevelsLoadable = 0;
  result.m_State = plResourceState::Loaded;
  return result;
}
// clang-format on

plNetworkReplicationProfileResource::plNetworkReplicationProfileResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
}

plResourceLoadDesc plNetworkReplicationProfileResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);
  m_Descriptor = plNetworkReplicationProfileDescriptor();
  return {plResourceState::Unloaded, 0, 0};
}

plResourceLoadDesc plNetworkReplicationProfileResource::UpdateContent(plStreamReader* pStream)
{
  if (pStream == nullptr)
    return {plResourceState::LoadedResourceMissing, 0, 0};

  plStringBuilder absolutePath;
  (*pStream) >> absolutePath;
  plAssetFileHeader assetHeader;
  assetHeader.Read(*pStream).IgnoreResult();
  m_Descriptor.Load(*pStream);
  return {plResourceState::Loaded, 0, 0};
}

void plNetworkReplicationProfileResource::UpdateMemoryUsage(MemoryUsage& out_memoryUsage)
{
  out_memoryUsage.m_uiMemoryCPU = sizeof(*this);
  out_memoryUsage.m_uiMemoryGPU = 0;
}

void plNetworkReplicationProfileDescriptor::Save(plStreamWriter& inout_stream) const
{
  inout_stream << static_cast<plUInt8>(1);
  inout_stream << m_fSendRate;
  inout_stream << static_cast<plUInt8>(m_Condition.GetValue());
  inout_stream << static_cast<plUInt8>(m_Target.GetValue());
  inout_stream << static_cast<plUInt8>(m_Encoding.GetValue());
  inout_stream << m_uiPriority;
}

void plNetworkReplicationProfileDescriptor::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  if (uiVersion != 1)
    return;

  plUInt8 uiCondition = 0;
  plUInt8 uiTarget = 0;
  plUInt8 uiEncoding = 0;
  inout_stream >> m_fSendRate;
  inout_stream >> uiCondition;
  inout_stream >> uiTarget;
  inout_stream >> uiEncoding;
  inout_stream >> m_uiPriority;
  m_fSendRate = plMath::Clamp(m_fSendRate, 0.1f, 60.0f);
  m_Condition = static_cast<plReplicateCondition::Enum>(plMath::Min<plUInt8>(uiCondition, plReplicateCondition::InitialOnly));
  m_Target = static_cast<plReplicateTarget::Enum>(plMath::Min<plUInt8>(uiTarget, plReplicateTarget::NonOwner));
  m_Encoding = static_cast<plNetSerializeEncoding::Enum>(plMath::Min<plUInt8>(uiEncoding, plNetSerializeEncoding::Compressed));
}

PL_STATICLINK_FILE(NetworkPlugin, NetworkPlugin_Core_Implementation_NetworkReplicationProfileResource);
