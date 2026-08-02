#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkTypes.h>
#include <NetworkPlugin/Core/NetworkMessage.h>

#include <Foundation/Containers/HashTable.h>
#include <Foundation/Strings/HashedString.h>
#include <Core/World/World.h>
#include <Core/Prefabs/PrefabResource.h>

class plNetworkWorldModule;
class plNetworkPeer;
class plGameObject;

/// @brief Represents a network-spawned object with ownership and authority information
struct PL_NETWORKPLUGIN_DLL plNetworkObjectInfo
{
  plUInt32 m_uiNetworkID = 0;                              ///< Unique network identifier
  plGameObjectHandle m_hGameObject;                         ///< Handle to the game object
  plUInt32 m_uiOwnerClientID = 0;                          ///< Client that owns this object
  plEnum<plNetworkObjectRole> m_Authority;                  ///< Authority level
  plHashedString m_sPrefabGuid;                            ///< Prefab used to spawn this object
  plTime m_SpawnTime;                                      ///< When the object was spawned
  bool m_bPendingDestroy = false;                          ///< Marked for destruction
};

// Spawn validation and post-spawn notifications are delivered via plMsgNetworkValidateSpawn
// and plMsgNetworkObjectSpawned messages to all registered network components.
// See NetworkMessages.h for message definitions.

/// @brief Manages network object spawning, despawning, and ownership
/// 
/// This manager handles:
/// - Registering prefabs that can be spawned over the network
/// - Server-authoritative spawn/despawn with client replication
/// - Object ownership and authority tracking
/// - Late-joiner synchronization of existing objects
class PL_NETWORKPLUGIN_DLL plNetworkObjectManager
{
public:
  plNetworkObjectManager();
  ~plNetworkObjectManager();

  /// @brief Initialize the manager with the network module
  void Initialize(plNetworkWorldModule* pNetworkModule);

  /// @brief Shutdown and cleanup all tracked objects
  void Shutdown();

  //////////////////////////////////////////////////////////////////////////
  // Prefab Registration
  //////////////////////////////////////////////////////////////////////////

  /// @brief Register a prefab that can be spawned over the network
  /// @param sPrefabGuid The prefab GUID or asset path
  /// @param uiPrefabID Optional numeric ID for bandwidth optimization
  void RegisterNetworkPrefab(const plHashedString& sPrefabGuid, plUInt32 uiPrefabID = 0);

  /// @brief Unregister a network prefab
  void UnregisterNetworkPrefab(const plHashedString& sPrefabGuid);

  /// @brief Check if a prefab is registered for network spawning
  bool IsNetworkPrefab(const plHashedString& sPrefabGuid) const;

  //////////////////////////////////////////////////////////////////////////
  // Spawning
  //////////////////////////////////////////////////////////////////////////

  /// @brief Spawn a networked object (server-side)
  /// @param sPrefabGuid The prefab to spawn
  /// @param transform Initial world transform
  /// @param uiOwnerClientID Client that will own this object (0 = server)
  /// @param authority Authority level for the object
  /// @return Network ID of the spawned object, or 0 on failure
  plUInt32 SpawnNetworkObject(
    const plHashedString& sPrefabGuid,
    const plTransform& transform,
    plUInt32 uiOwnerClientID = 0,
    plNetworkObjectRole::Enum authority = plNetworkObjectRole::OwnedAuthoritative);

  /// @brief Spawn with parent object
  plUInt32 SpawnNetworkObject(
    const plHashedString& sPrefabGuid,
    const plTransform& localTransform,
    plGameObjectHandle hParent,
    plUInt32 uiOwnerClientID = 0,
    plNetworkObjectRole::Enum authority = plNetworkObjectRole::OwnedAuthoritative);

  /// @brief Despawn a networked object (server-side)
  /// @param uiNetworkID The network ID of the object to despawn
  /// @return True if the object was found and despawned
  bool DespawnNetworkObject(plUInt32 uiNetworkID);

  /// @brief Despawn all objects owned by a specific client
  void DespawnObjectsOwnedBy(plUInt32 uiClientID);

  //////////////////////////////////////////////////////////////////////////
  // Ownership & Authority
  //////////////////////////////////////////////////////////////////////////

  /// @brief Transfer ownership of an object to another client
  /// @param uiNetworkID The network ID of the object
  /// @param uiNewOwnerClientID The new owner's client ID
  /// @return True if the transfer was successful
  bool TransferOwnership(plUInt32 uiNetworkID, plUInt32 uiNewOwnerClientID);

  /// @brief Change the authority level of an object
  bool SetAuthority(plUInt32 uiNetworkID, plNetworkObjectRole::Enum authority);

  /// @brief Get the owner client ID for an object
  plUInt32 GetOwner(plUInt32 uiNetworkID) const;

  /// @brief Get the authority level for an object
  plNetworkObjectRole::Enum GetAuthority(plUInt32 uiNetworkID) const;

  /// @brief Check if a client has authority to modify an object
  bool HasAuthority(plUInt32 uiNetworkID, plUInt32 uiClientID) const;

  //////////////////////////////////////////////////////////////////////////
  // Object Lookup
  //////////////////////////////////////////////////////////////////////////

  /// @brief Get object info by network ID
  const plNetworkObjectInfo* GetObjectInfo(plUInt32 uiNetworkID) const;

  /// @brief Get the network ID for a game object
  plUInt32 GetNetworkID(plGameObjectHandle hObject) const;

  /// @brief Get pending spawn network ID (used during prefab instantiation)
  plUInt32 GetPendingSpawnNetworkID() const { return m_uiPendingSpawnNetworkID; }

  /// @brief Get the game object for a network ID
  plGameObjectHandle GetGameObject(plUInt32 uiNetworkID) const;

  /// @brief Get all currently tracked network objects
  const plHashTable<plUInt32, plNetworkObjectInfo>& GetAllObjects() const { return m_NetworkObjects; }

  //////////////////////////////////////////////////////////////////////////
  // Message Handling
  //////////////////////////////////////////////////////////////////////////

  /// @brief Handle incoming spawn message (client-side)
  void HandleSpawnMessage(plNetworkMessage& msg);

  /// @brief Handle incoming despawn message (client-side)
  void HandleDespawnMessage(plNetworkMessage& msg);

  /// @brief Handle ownership change message
  void HandleOwnershipMessage(plNetworkMessage& msg);

  /// @brief Send full object state to a newly connected client
  void SyncAllObjectsToClient(plUInt32 uiClientID);

private:
  /// @brief Generate a unique network ID
  plUInt32 GenerateNetworkID();

  /// @brief Send spawn message to all clients
  void BroadcastSpawn(const plNetworkObjectInfo& info, const plTransform& transform);

  /// @brief Send spawn message to specific client
  void SendSpawnToClient(const plNetworkObjectInfo& info, const plTransform& transform, plUInt32 uiClientID);

  /// @brief Send despawn message to all clients
  void BroadcastDespawn(plUInt32 uiNetworkID);

  /// @brief Internal spawn implementation
  plGameObject* SpawnPrefabInternal(const plHashedString& sPrefabGuid, const plTransform& transform, plGameObjectHandle hParent);

  /// @brief Update network component IDs after spawn
  void UpdateComponentNetIDs(plGameObject* pObject, plUInt32 uiNetworkID);

  /// @brief Cleanup destroyed objects
  void CleanupDestroyedObjects();

private:
  plNetworkWorldModule* m_pNetworkModule = nullptr;

  /// @brief All tracked network objects by network ID
  plHashTable<plUInt32, plNetworkObjectInfo> m_NetworkObjects;

  /// @brief Reverse lookup: game object handle -> network ID
  plHashTable<plGameObjectHandle, plUInt32> m_ObjectToNetworkID;

  /// @brief Registered prefabs: GUID -> numeric ID for bandwidth optimization
  plHashTable<plHashedString, plUInt32> m_RegisteredPrefabs;

  /// @brief Reverse lookup for prefabs: numeric ID -> GUID
  plHashTable<plUInt32, plHashedString> m_PrefabIDToGuid;

  /// @brief Counter for generating unique network IDs
  plUInt32 m_uiNextNetworkID = 1;

  /// @brief Network ID being assigned during spawn (for component registration)
  plUInt32 m_uiPendingSpawnNetworkID = 0;
};
