#pragma once

#include <Core/ResourceManager/Resource.h>
#include <NetworkPlugin/NetworkPluginDLL.h>

using plNetworkReplicationProfileResourceHandle = plTypedResourceHandle<class plNetworkReplicationProfileResource>;

struct PL_NETWORKPLUGIN_DLL plNetworkReplicationProfileDescriptor
{
  float m_fSendRate = 20.0f;
  plEnum<plReplicateCondition> m_Condition = plReplicateCondition::OnChange;
  plEnum<plReplicateTarget> m_Target = plReplicateTarget::AllClients;
  plEnum<plNetSerializeEncoding> m_Encoding = plNetSerializeEncoding::Default;
  plUInt8 m_uiPriority = 128;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkReplicationProfileDescriptor);

class PL_NETWORKPLUGIN_DLL plNetworkReplicationProfileResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plNetworkReplicationProfileResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plNetworkReplicationProfileResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plNetworkReplicationProfileResource, plNetworkReplicationProfileDescriptor);

public:
  plNetworkReplicationProfileResource();
  const plNetworkReplicationProfileDescriptor& GetDescriptor() const { return m_Descriptor; }

private:
  plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  plResourceLoadDesc UpdateContent(plStreamReader* pStream) override;
  void UpdateMemoryUsage(MemoryUsage& out_memoryUsage) override;

  plNetworkReplicationProfileDescriptor m_Descriptor;
};
