#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Core/NetworkSerialize.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <Core/World/World.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>

PL_BEGIN_COMPONENT_TYPE(plNetworkComponent, 1 /* version */, plComponentMode::Dynamic)
PL_END_COMPONENT_TYPE

void plNetworkComponent::OnSimulationStarted()
{
  plComponent::OnSimulationStarted();

  // Try to get the network module and register
  // Use GetOrCreateModule to ensure the module exists
  m_pNetworkModule = GetWorld()->GetOrCreateModule<plNetworkWorldModule>();

  if (m_pNetworkModule)
  {
    m_pNetworkModule->RegisterNetworkComponent(this);
  }
}

void plNetworkComponent::Deinitialize()
{
  // Unregister from network module
  if (m_pNetworkModule)
  {
    m_pNetworkModule->RemoveNetworkComponent(this);
  }

  plComponent::Deinitialize();
}

void plNetworkComponent::OnNetworkStateChanged(plNetworkConnectionState::Enum state)
{
  if (state == plNetworkConnectionState::Connected)
  {
    // If we didn't have the network module during simulation start, get it now
    if (!m_pNetworkModule)
    {
      m_pNetworkModule = GetWorld()->GetModule<plNetworkWorldModule>();

      // Register now if we weren't registered before
      if (m_pNetworkModule)
      {
        m_pNetworkModule->RegisterNetworkComponent(this);
      }
    }
  }
}

bool plNetworkComponent::IsLocallyOwned() const
{
  if (!m_pNetworkModule || m_uiNetID == 0)
    return false;

  const plNetworkObjectManager* pObjectManager = m_pNetworkModule->GetObjectManager();
  if (!pObjectManager)
    return false;

  // Owner client ID 0 is the server itself, so "no entry" must be told apart
  // from "owned by the server" via the object info, not the owner id value.
  const plNetworkObjectInfo* pInfo = pObjectManager->GetObjectInfo(m_uiNetID);
  if (pInfo == nullptr)
  {
    // Per-component NetIDs (derived from the spawn object id) are not registered in the object table.
    const plUInt32 uiObjectNetID = pObjectManager->GetNetworkID(GetOwner()->GetHandle());
    if (uiObjectNetID != 0)
    {
      pInfo = pObjectManager->GetObjectInfo(uiObjectNetID);
    }
  }

  if (pInfo == nullptr)
    return false;

  return pInfo->m_uiOwnerClientID == m_pNetworkModule->GetLocalClientID();
}

void plNetworkComponent::NetworkSerialize(plNetworkMessage& msg)
{
  // Default implementation: serialize all properties marked with plNetSerializeAttribute
  plNetworkSerializer::SerializeProperties(this, msg);
}

void plNetworkComponent::NetworkDeserialize(plNetworkMessage& msg)
{
  // Default implementation: deserialize all properties marked with plNetSerializeAttribute
  plNetworkSerializer::DeserializeProperties(this, msg);
}
