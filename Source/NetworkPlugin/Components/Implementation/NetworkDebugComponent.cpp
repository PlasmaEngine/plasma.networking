#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Components/NetworkDebugComponent.h>
#include <RendererCore/Debug/DebugRenderer.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plNetworkDebugComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("ShowNetworkId", m_bShowNetworkId)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("ShowReplicationState", m_bShowReplicationState)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("ShowNetworkRole", m_bShowNetworkRole)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("TextScale", m_fTextScale)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("TextOffset", m_vTextOffset)->AddAttributes(new plDefaultValueAttribute(plVec3(0, 0, 1.0f))),
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

void plNetworkDebugComponent::OnSimulationStarted()
{
  plNetworkComponent::OnSimulationStarted();
}

void plNetworkDebugComponent::Update()
{
  DrawDebugInfo();
}

void plNetworkDebugComponent::DrawDebugInfo()
{
  if (m_bShowNetworkId)
    DrawNetworkId();
  if (m_bShowReplicationState)
    DrawReplicationState();
  if (m_bShowNetworkRole)
    DrawNetworkRole();
}

void plNetworkDebugComponent::DrawNetworkId()
{
  plVec3 pos = GetOwner()->GetGlobalPosition() + m_vTextOffset;
  
  plStringBuilder sb;
  sb.SetFormat("NetID: {}", GetNetID());
  
  plDebugRenderer::Draw3DText(GetWorld(), sb, pos, plColor::White, m_fTextScale * 16);
}

void plNetworkDebugComponent::DrawReplicationState()
{
  // Find any network component on this object
  plHybridArray<plNetworkComponent*, 4> networkComponents;
  GetOwner()->TryGetComponentsOfBaseType(networkComponents);

  bool anyDirty = false;
  for (auto* pComp : networkComponents)
  {
    if (pComp->NeedReplication())
    {
      anyDirty = true;
      break;
    }
  }

  plVec3 pos = GetOwner()->GetGlobalPosition() + m_vTextOffset + plVec3(0, 0, 0.4f);
  
  plStringBuilder sb;
  sb = anyDirty ? "State: DIRTY" : "State: Clean";
  
  plColor color = anyDirty ? m_DirtyColor : plColor::LimeGreen;
  plDebugRenderer::Draw3DText(GetWorld(), sb, pos, color, m_fTextScale * 14);
}

void plNetworkDebugComponent::DrawNetworkRole()
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  plVec3 pos = GetOwner()->GetGlobalPosition() + m_vTextOffset + plVec3(0, 0, 0.6f);
  
  plStringBuilder sb;
  plColor color;

  if (m_pNetworkModule->IsServer())
  {
    sb = "Role: Server";
    color = m_ServerColor;
  }
  else if (m_pNetworkModule->IsHost())
  {
    sb = "Role: Host";
    color = plColor::Yellow;
  }
  else if (m_pNetworkModule->IsClient())
  {
    sb = "Role: Client";
    color = m_ClientColor;
  }
  else
  {
    sb = "Role: Offline";
    color = plColor::Gray;
  }
  
  plDebugRenderer::Draw3DText(GetWorld(), sb, pos, color, m_fTextScale * 14);
}
