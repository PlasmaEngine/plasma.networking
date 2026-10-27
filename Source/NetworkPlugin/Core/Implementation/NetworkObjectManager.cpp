#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <NetworkPlugin/Core/NetworkMessages.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>
#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/Components/NetworkComponent.h>

#include <Core/Prefabs/PrefabResource.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Foundation/IO/MemoryStream.h>

plNetworkObjectManager::plNetworkObjectManager()
{
}

plNetworkObjectManager::~plNetworkObjectManager()
{
  Shutdown();
}

void plNetworkObjectManager::Initialize(plNetworkWorldModule* pNetworkModule)
{
  PL_ASSERT_DEV(pNetworkModule != nullptr, "NetworkWorldModule cannot be null");
  m_pNetworkModule = pNetworkModule;
  m_uiNextNetworkID = 1;
}

void plNetworkObjectManager::Shutdown()
{
  // Clear all tracked objects (don't destroy them, let world handle that)
  m_NetworkObjects.Clear();
  m_ObjectToNetworkID.Clear();
  m_pNetworkModule = nullptr;
}

//////////////////////////////////////////////////////////////////////////
// Prefab Registration
//////////////////////////////////////////////////////////////////////////

void plNetworkObjectManager::RegisterNetworkPrefab(const plHashedString& sPrefabGuid, plUInt32 uiPrefabID)
{
  if (uiPrefabID == 0)
  {
    // Auto-generate ID based on hash (use full 32 bits for better collision resistance)
    uiPrefabID = static_cast<plUInt32>(sPrefabGuid.GetHash() & 0xFFFFFFFF);
    // Ensure non-zero ID
    if (uiPrefabID == 0)
      uiPrefabID = 1;
  }

  m_RegisteredPrefabs[sPrefabGuid] = uiPrefabID;
  m_PrefabIDToGuid[uiPrefabID] = sPrefabGuid;

  plLog::Dev("PL Net: Registered network prefab '{}' with ID {}", sPrefabGuid.GetString(), uiPrefabID);
}

void plNetworkObjectManager::UnregisterNetworkPrefab(const plHashedString& sPrefabGuid)
{
  plUInt32 prefabID = 0;
  if (m_RegisteredPrefabs.TryGetValue(sPrefabGuid, prefabID))
  {
    m_PrefabIDToGuid.Remove(prefabID);
    m_RegisteredPrefabs.Remove(sPrefabGuid);
  }
}

bool plNetworkObjectManager::IsNetworkPrefab(const plHashedString& sPrefabGuid) const
{
  return m_RegisteredPrefabs.Contains(sPrefabGuid);
}

//////////////////////////////////////////////////////////////////////////
// Spawning
//////////////////////////////////////////////////////////////////////////

plUInt32 plNetworkObjectManager::GenerateNetworkID()
{
  return m_uiNextNetworkID++;
}

plUInt32 plNetworkObjectManager::SpawnNetworkObject(
  const plHashedString& sPrefabGuid,
  const plTransform& transform,
  plUInt32 uiOwnerClientID,
  plNetworkObjectRole::Enum authority)
{
  return SpawnNetworkObject(sPrefabGuid, transform, plGameObjectHandle(), uiOwnerClientID, authority);
}

plUInt32 plNetworkObjectManager::SpawnNetworkObject(
  const plHashedString& sPrefabGuid,
  const plTransform& localTransform,
  plGameObjectHandle hParent,
  plUInt32 uiOwnerClientID,
  plNetworkObjectRole::Enum authority)
{
  if (!m_pNetworkModule)
  {
    plLog::Error("PL Net: Cannot spawn - NetworkObjectManager not initialized");
    return 0;
  }

  // Only server/host can spawn objects
  if (!m_pNetworkModule->IsServer() && !m_pNetworkModule->IsHost())
  {
    plLog::Warning("PL Net: Only server can spawn network objects");
    return 0;
  }

  // Broadcast spawn validation message -- handlers can set m_bAllowSpawn to false to reject
  {
    plMsgNetworkValidateSpawn validateMsg;
    validateMsg.m_uiClientID = uiOwnerClientID;
    validateMsg.m_sPrefabGuid = sPrefabGuid;
    m_pNetworkModule->BroadcastToNetworkComponents(validateMsg);
    if (!validateMsg.m_bAllowSpawn)
    {
      plLog::Warning("PL Net: Spawn validation rejected for prefab '{}'", sPrefabGuid.GetString());
      return 0;
    }
  }

  // Check if prefab is registered
  if (!IsNetworkPrefab(sPrefabGuid))
  {
    // Clients only accept registered prefab IDs, so an unregistered spawn would disconnect every client.
    plLog::Warning("PL Net: Prefab '{}' is not registered for network spawning", sPrefabGuid.GetString());
    return 0;
  }

  // Generate network ID first so components can use it during spawn
  plUInt32 networkID = GenerateNetworkID();
  m_uiPendingSpawnNetworkID = networkID;

  // Spawn the prefab locally
  plGameObject* pObject = SpawnPrefabInternal(sPrefabGuid, localTransform, hParent);
  
  // Clear pending ID
  m_uiPendingSpawnNetworkID = 0;
  
  if (!pObject)
  {
    plLog::Error("PL Net: Failed to spawn prefab '{}'", sPrefabGuid.GetString());
    return 0;
  }

  // Create network object info
  plNetworkObjectInfo info;
  info.m_uiNetworkID = networkID;
  info.m_hGameObject = pObject->GetHandle();
  info.m_uiOwnerClientID = uiOwnerClientID;
  info.m_Authority = authority;
  info.m_sPrefabGuid = sPrefabGuid;
  info.m_SpawnTime = plTime::Now();

  // Store in tracking tables
  m_NetworkObjects[networkID] = info;
  m_ObjectToNetworkID[pObject->GetHandle()] = networkID;

  // Update component NetIDs for components that were registered during spawn
  for (auto it = pObject->GetChildren(); it.IsValid(); ++it)
  {
    UpdateComponentNetIDs(it, networkID);
  }
  UpdateComponentNetIDs(pObject, networkID);

  // Broadcast post-spawn message to all network components
  {
    plMsgNetworkObjectSpawned spawnedMsg;
    spawnedMsg.m_pSpawnedObject = pObject;
    spawnedMsg.m_uiNetworkID = networkID;
    spawnedMsg.m_uiOwnerClientID = uiOwnerClientID;
    m_pNetworkModule->BroadcastToNetworkComponents(spawnedMsg);
  }

  // Broadcast spawn to all connected clients
  BroadcastSpawn(info, localTransform);

  plLog::Dev("PL Net: Spawned network object {} (prefab: {}, owner: {})",
    networkID, sPrefabGuid.GetString(), uiOwnerClientID);

  return networkID;
}

plGameObject* plNetworkObjectManager::SpawnPrefabInternal(
  const plHashedString& sPrefabGuid,
  const plTransform& transform,
  plGameObjectHandle hParent)
{
  if (!m_pNetworkModule)
    return nullptr;

  plWorld* pWorld = m_pNetworkModule->GetWorld();
  if (!pWorld)
    return nullptr;

  // Load the prefab resource
  plPrefabResourceHandle hPrefab = plResourceManager::LoadResource<plPrefabResource>(sPrefabGuid.GetString());
  if (!hPrefab.IsValid())
  {
    plLog::Error("PL Net: Failed to load prefab resource '{}'", sPrefabGuid.GetString());
    return nullptr;
  }

  plResourceLock<plPrefabResource> pPrefab(hPrefab, plResourceAcquireMode::BlockTillLoaded);
  if (pPrefab.GetAcquireResult() != plResourceAcquireResult::Final)
  {
    plLog::Error("PL Net: Failed to acquire prefab resource '{}'", sPrefabGuid.GetString());
    return nullptr;
  }

  // Instantiate the prefab
  plPrefabInstantiationOptions options;
  options.m_hParent = hParent;
  options.m_bForceDynamic = true; // Network objects should be dynamic

  plDynamicArray<plGameObject*> createdObjects;
  options.m_pCreatedRootObjectsOut = &createdObjects;

  pPrefab->InstantiatePrefab(*pWorld, transform, options, nullptr);

  if (createdObjects.IsEmpty())
  {
    plLog::Error("PL Net: Prefab '{}' created no objects", sPrefabGuid.GetString());
    return nullptr;
  }

  return createdObjects[0];
}

bool plNetworkObjectManager::DespawnNetworkObject(plUInt32 uiNetworkID)
{
  if (!m_pNetworkModule)
    return false;

  // Only server/host can despawn
  if (!m_pNetworkModule->IsServer() && !m_pNetworkModule->IsHost())
  {
    plLog::Warning("PL Net: Only server can despawn network objects");
    return false;
  }

  plNetworkObjectInfo* pInfo = nullptr;
  if (!m_NetworkObjects.TryGetValue(uiNetworkID, pInfo))
  {
    plLog::Warning("PL Net: Cannot despawn unknown network object {}", uiNetworkID);
    return false;
  }

  // Broadcast despawn to clients first
  BroadcastDespawn(uiNetworkID);

  // Destroy the game object
  plWorld* pWorld = m_pNetworkModule->GetWorld();
  if (pWorld)
  {
    plGameObject* pObject = nullptr;
    if (pWorld->TryGetObject(pInfo->m_hGameObject, pObject))
    {
      pWorld->DeleteObjectDelayed(pInfo->m_hGameObject);
    }
  }

  // Remove from tracking
  m_ObjectToNetworkID.Remove(pInfo->m_hGameObject);
  m_NetworkObjects.Remove(uiNetworkID);

  plLog::Dev("PL Net: Despawned network object {}", uiNetworkID);

  return true;
}

void plNetworkObjectManager::DespawnObjectsOwnedBy(plUInt32 uiClientID)
{
  plDynamicArray<plUInt32> objectsToDespawn;

  for (auto it = m_NetworkObjects.GetIterator(); it.IsValid(); ++it)
  {
    if (it.Value().m_uiOwnerClientID == uiClientID)
    {
      objectsToDespawn.PushBack(it.Key());
    }
  }

  for (plUInt32 networkID : objectsToDespawn)
  {
    DespawnNetworkObject(networkID);
  }

  plLog::Dev("PL Net: Despawned {} objects owned by client {}", objectsToDespawn.GetCount(), uiClientID);
}

//////////////////////////////////////////////////////////////////////////
// Ownership & Authority
//////////////////////////////////////////////////////////////////////////

bool plNetworkObjectManager::TransferOwnership(plUInt32 uiNetworkID, plUInt32 uiNewOwnerClientID)
{
  plNetworkObjectInfo* pInfo = nullptr;
  if (!m_NetworkObjects.TryGetValue(uiNetworkID, pInfo))
  {
    return false;
  }

  plUInt32 oldOwner = pInfo->m_uiOwnerClientID;
  pInfo->m_uiOwnerClientID = uiNewOwnerClientID;

  // Broadcast ownership change
  if (m_pNetworkModule && m_pNetworkModule->GetPeer())
  {
    plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
    plNetworkMessage msg = pPeer->BeginSendMessage();

    msg.WriteUInt8(plNetworkMessageIDs::ObjectRole);
    msg.WriteUInt32(uiNetworkID);
    msg.WriteUInt32(uiNewOwnerClientID);
    msg.WriteUInt8(static_cast<plUInt8>(pInfo->m_Authority.GetValue()));

    msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
    pPeer->SendMessages(msg);
  }

  plLog::Dev("PL Net: Transferred ownership of object {} from {} to {}",
    uiNetworkID, oldOwner, uiNewOwnerClientID);

  return true;
}

bool plNetworkObjectManager::SetAuthority(plUInt32 uiNetworkID, plNetworkObjectRole::Enum authority)
{
  plNetworkObjectInfo* pInfo = nullptr;
  if (!m_NetworkObjects.TryGetValue(uiNetworkID, pInfo))
  {
    return false;
  }

  pInfo->m_Authority = authority;
  return true;
}

plUInt32 plNetworkObjectManager::GetOwner(plUInt32 uiNetworkID) const
{
  const plNetworkObjectInfo* pInfo = nullptr;
  if (m_NetworkObjects.TryGetValue(uiNetworkID, pInfo))
  {
    return pInfo->m_uiOwnerClientID;
  }
  return 0;
}

plNetworkObjectRole::Enum plNetworkObjectManager::GetAuthority(plUInt32 uiNetworkID) const
{
  const plNetworkObjectInfo* pInfo = nullptr;
  if (m_NetworkObjects.TryGetValue(uiNetworkID, pInfo))
  {
    return pInfo->m_Authority;
  }
  return plNetworkObjectRole::None;
}

bool plNetworkObjectManager::HasAuthority(plUInt32 uiNetworkID, plUInt32 uiClientID) const
{
  const plNetworkObjectInfo* pInfo = nullptr;
  if (!m_NetworkObjects.TryGetValue(uiNetworkID, pInfo))
  {
    return false;
  }

  switch (pInfo->m_Authority.GetValue())
  {
    case plNetworkObjectRole::OwnedAuthoritative:
      return pInfo->m_uiOwnerClientID == uiClientID;

    case plNetworkObjectRole::Replicated:
      // Only server has authority
      return uiClientID == m_pNetworkModule->GetServerClientID();

    case plNetworkObjectRole::ReplicatedSimulated:
      // Owner or server can modify
      return pInfo->m_uiOwnerClientID == uiClientID ||
             uiClientID == m_pNetworkModule->GetServerClientID();

    default:
      return false;
  }
}

//////////////////////////////////////////////////////////////////////////
// Object Lookup
//////////////////////////////////////////////////////////////////////////

const plNetworkObjectInfo* plNetworkObjectManager::GetObjectInfo(plUInt32 uiNetworkID) const
{
  const plNetworkObjectInfo* pInfo = nullptr;
  m_NetworkObjects.TryGetValue(uiNetworkID, pInfo);
  return pInfo;
}

plUInt32 plNetworkObjectManager::GetNetworkID(plGameObjectHandle hObject) const
{
  plUInt32 networkID = 0;
  m_ObjectToNetworkID.TryGetValue(hObject, networkID);
  return networkID;
}

plGameObjectHandle plNetworkObjectManager::GetGameObject(plUInt32 uiNetworkID) const
{
  const plNetworkObjectInfo* pInfo = nullptr;
  if (m_NetworkObjects.TryGetValue(uiNetworkID, pInfo))
  {
    return pInfo->m_hGameObject;
  }
  return plGameObjectHandle();
}

//////////////////////////////////////////////////////////////////////////
// Message Handling
//////////////////////////////////////////////////////////////////////////

void plNetworkObjectManager::BroadcastSpawn(const plNetworkObjectInfo& info, const plTransform& transform)
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();

  // Get all connected clients
  plDynamicArray<plNetworkConnection> targets;
  for (plNetworkClient* pClient : m_pNetworkModule->GetClients())
  {
    if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
    {
      targets.PushBack(pClient->m_Connection);
    }
  }

  if (targets.IsEmpty())
    return;

  plNetworkMessage msg = pPeer->BeginSendMessage();

  msg.WriteUInt8(plNetworkMessageIDs::ObjectSpawn);
  msg.WriteUInt32(info.m_uiNetworkID);
  msg.WriteUInt32(info.m_uiOwnerClientID);
  msg.WriteUInt8(static_cast<plUInt8>(info.m_Authority.GetValue()));

  // Write prefab ID for bandwidth optimization
  plUInt32 prefabID = 0;
  m_RegisteredPrefabs.TryGetValue(info.m_sPrefabGuid, prefabID);
  msg.WriteUInt32(prefabID);

  // Write transform
  msg.WriteVec3(transform.m_vPosition);
  msg.WriteQuat(transform.m_qRotation);
  msg.WriteVec3(transform.m_vScale);

  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  pPeer->SendMessages(msg, targets);
}

void plNetworkObjectManager::SendSpawnToClient(const plNetworkObjectInfo& info, const plTransform& transform, plUInt32 uiClientID)
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  plNetworkClient* pClient = m_pNetworkModule->GetClient(uiClientID);
  if (!pClient)
    return;

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  plNetworkMessage msg = pPeer->BeginSendMessage();

  msg.WriteUInt8(plNetworkMessageIDs::ObjectSpawn);
  msg.WriteUInt32(info.m_uiNetworkID);
  msg.WriteUInt32(info.m_uiOwnerClientID);
  msg.WriteUInt8(static_cast<plUInt8>(info.m_Authority.GetValue()));

  plUInt32 prefabID = 0;
  m_RegisteredPrefabs.TryGetValue(info.m_sPrefabGuid, prefabID);
  msg.WriteUInt32(prefabID);

  msg.WriteVec3(transform.m_vPosition);
  msg.WriteQuat(transform.m_qRotation);
  msg.WriteVec3(transform.m_vScale);

  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  pPeer->SendMessages(msg, pClient->m_Connection);
}

void plNetworkObjectManager::BroadcastDespawn(plUInt32 uiNetworkID)
{
  if (!m_pNetworkModule || !m_pNetworkModule->GetPeer())
    return;

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();

  plDynamicArray<plNetworkConnection> targets;
  for (plNetworkClient* pClient : m_pNetworkModule->GetClients())
  {
    if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
    {
      targets.PushBack(pClient->m_Connection);
    }
  }

  if (targets.IsEmpty())
    return;

  plNetworkMessage msg = pPeer->BeginSendMessage();

  msg.WriteUInt8(plNetworkMessageIDs::ObjectDespawn);
  msg.WriteUInt32(uiNetworkID);

  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  pPeer->SendMessages(msg, targets);
}

void plNetworkObjectManager::HandleSpawnMessage(plNetworkMessage& msg)
{
  if (!m_pNetworkModule)
    return;

  // Only clients should handle spawn messages
  if (!m_pNetworkModule->IsClient())
    return;

  plUInt32 networkID = msg.ReadUInt32();
  plUInt32 ownerClientID = msg.ReadUInt32();
  plNetworkObjectRole::Enum authority = static_cast<plNetworkObjectRole::Enum>(msg.ReadUInt8());
  plUInt32 prefabID = msg.ReadUInt32();

  if (m_NetworkObjects.Contains(networkID))
  {
    plLog::Dev("PL Net: Ignoring duplicate spawn for object {}", networkID);
    return;
  }

  plHashedString prefabGuid;
  if (prefabID != 0)
  {
    if (!m_PrefabIDToGuid.TryGetValue(prefabID, prefabGuid))
    {
      plLog::Error("PL Net: Unknown prefab ID {} in spawn message", prefabID);
      return;
    }
  }
  else
  {
    // Remote paths are not an asset whitelist. Both peers must register the prefab ID.
    msg.MarkReadError();
    return;
  }

  plVec3 position = msg.ReadVec3();
  plQuat rotation = msg.ReadQuat();
  plVec3 scale = msg.ReadVec3();
  if (msg.HasReadError() || networkID == 0 || authority > plNetworkObjectRole::ReplicatedSimulated ||
      !position.IsValid() || !rotation.IsValid() || !scale.IsValid())
  {
    msg.MarkReadError();
    return;
  }

  plTransform transform;
  transform.m_vPosition = position;
  transform.m_qRotation = rotation;
  transform.m_vScale = scale;

  // Pre-register the network ID so components can find it during spawn
  m_uiPendingSpawnNetworkID = networkID;

  // Spawn locally
  plGameObject* pObject = SpawnPrefabInternal(prefabGuid, transform, plGameObjectHandle());
  
  // Clear pending ID
  m_uiPendingSpawnNetworkID = 0;
  
  if (!pObject)
  {
    plLog::Error("PL Net: Failed to spawn replicated object {}", networkID);
    return;
  }

  // Track the object
  plNetworkObjectInfo info;
  info.m_uiNetworkID = networkID;
  info.m_hGameObject = pObject->GetHandle();
  info.m_uiOwnerClientID = ownerClientID;
  info.m_Authority = authority;
  info.m_sPrefabGuid = prefabGuid;
  info.m_SpawnTime = plTime::Now();

  m_NetworkObjects[networkID] = info;
  m_ObjectToNetworkID[pObject->GetHandle()] = networkID;

  // Update component NetIDs for components that were registered during spawn
  // They may have gotten 0 because the mapping wasn't ready yet
  for (auto it = pObject->GetChildren(); it.IsValid(); ++it)
  {
    UpdateComponentNetIDs(it, networkID);
  }
  UpdateComponentNetIDs(pObject, networkID);

  // Update next network ID to avoid conflicts
  if (networkID >= m_uiNextNetworkID)
  {
    m_uiNextNetworkID = networkID + 1;
  }

  // Broadcast post-spawn message to all network components
  {
    plMsgNetworkObjectSpawned spawnedMsg;
    spawnedMsg.m_pSpawnedObject = pObject;
    spawnedMsg.m_uiNetworkID = networkID;
    spawnedMsg.m_uiOwnerClientID = ownerClientID;
    m_pNetworkModule->BroadcastToNetworkComponents(spawnedMsg);
  }

  plLog::Dev("PL Net: Spawned replicated object {} (owner: {})", networkID, ownerClientID);
}

void plNetworkObjectManager::HandleDespawnMessage(plNetworkMessage& msg)
{
  if (!m_pNetworkModule)
    return;

  // Only clients should handle despawn messages
  if (!m_pNetworkModule->IsClient())
    return;

  plUInt32 networkID = msg.ReadUInt32();
  if (msg.HasReadError())
    return;

  plNetworkObjectInfo* pInfo = nullptr;
  if (!m_NetworkObjects.TryGetValue(networkID, pInfo))
  {
    plLog::Warning("PL Net: Received despawn for unknown object {}", networkID);
    return;
  }

  // Destroy the game object
  plWorld* pWorld = m_pNetworkModule->GetWorld();
  if (pWorld)
  {
    plGameObject* pObject = nullptr;
    if (pWorld->TryGetObject(pInfo->m_hGameObject, pObject))
    {
      pWorld->DeleteObjectDelayed(pInfo->m_hGameObject);
    }
  }

  // Remove from tracking
  m_ObjectToNetworkID.Remove(pInfo->m_hGameObject);
  m_NetworkObjects.Remove(networkID);

  plLog::Dev("PL Net: Despawned replicated object {}", networkID);
}

void plNetworkObjectManager::HandleOwnershipMessage(plNetworkMessage& msg)
{
  if (!m_pNetworkModule || !m_pNetworkModule->IsClient())
    return;
  plUInt32 networkID = msg.ReadUInt32();
  plUInt32 newOwnerClientID = msg.ReadUInt32();
  plNetworkObjectRole::Enum authority = static_cast<plNetworkObjectRole::Enum>(msg.ReadUInt8());
  if (msg.HasReadError() || authority > plNetworkObjectRole::ReplicatedSimulated)
  {
    msg.MarkReadError();
    return;
  }

  plNetworkObjectInfo* pInfo = nullptr;
  if (!m_NetworkObjects.TryGetValue(networkID, pInfo))
  {
    plLog::Warning("PL Net: Received ownership change for unknown object {}", networkID);
    return;
  }

  pInfo->m_uiOwnerClientID = newOwnerClientID;
  pInfo->m_Authority = authority;

  plLog::Dev("PL Net: Object {} ownership changed to client {}", networkID, newOwnerClientID);
}

void plNetworkObjectManager::SyncAllObjectsToClient(plUInt32 uiClientID)
{
  if (!m_pNetworkModule)
    return;

  plWorld* pWorld = m_pNetworkModule->GetWorld();
  if (!pWorld)
    return;

  plLog::Dev("PL Net: Syncing {} objects to client {}", m_NetworkObjects.GetCount(), uiClientID);

  for (auto it = m_NetworkObjects.GetIterator(); it.IsValid(); ++it)
  {
    const plNetworkObjectInfo& info = it.Value();

    plGameObject* pObject = nullptr;
    if (pWorld->TryGetObject(info.m_hGameObject, pObject))
    {
      plTransform transform = pObject->GetGlobalTransform();
      SendSpawnToClient(info, transform, uiClientID);
    }
  }
}

void plNetworkObjectManager::UpdateComponentNetIDs(plGameObject* pObject, plUInt32 uiNetworkID)
{
  if (!pObject || !m_pNetworkModule)
    return;

  // Find all network components on this object and update their NetID
  plHybridArray<plNetworkComponent*, 4> components;
  pObject->TryGetComponentsOfBaseType(components);

  for (plNetworkComponent* pComp : components)
  {
    if (pComp->GetNetID() == 0 || pComp->GetNetID() != uiNetworkID)
    {
      pComp->SetNetID(uiNetworkID);
      plLog::Dev("PL Net: Updated component NetID to {} on object '{}'", uiNetworkID, pObject->GetName());
    }
  }
}

void plNetworkObjectManager::CleanupDestroyedObjects()
{
  if (!m_pNetworkModule)
    return;

  plWorld* pWorld = m_pNetworkModule->GetWorld();
  if (!pWorld)
    return;

  plDynamicArray<plUInt32> toRemove;

  for (auto it = m_NetworkObjects.GetIterator(); it.IsValid(); ++it)
  {
    plGameObject* pObject = nullptr;
    if (!pWorld->TryGetObject(it.Value().m_hGameObject, pObject))
    {
      toRemove.PushBack(it.Key());
    }
  }

  for (plUInt32 networkID : toRemove)
  {
    plNetworkObjectInfo* pInfo = nullptr;
    if (m_NetworkObjects.TryGetValue(networkID, pInfo))
    {
      m_ObjectToNetworkID.Remove(pInfo->m_hGameObject);
    }
    m_NetworkObjects.Remove(networkID);
  }
}
