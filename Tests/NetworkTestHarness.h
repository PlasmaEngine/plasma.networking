#pragma once

#include "NetworkTestPCH.h"

#include <Core/Prefabs/PrefabResource.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Core/ResourceManager/ResourceTypeLoader.h>
#include <Core/World/World.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <GameEngine/PlatformServices/Interfaces/PlatformProviders.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Components/NetworkedPropertiesComponent.h>
#include <NetworkPlugin/Components/NetworkedTransformComponent.h>
#include <NetworkPlugin/Core/Driver/enet/DriverEnet.h>
#include <NetworkPlugin/Core/Driver/platform/DriverPlatform.h>
#include <NetworkPlugin/Core/NetworkLevelManager.h>
#include <NetworkPlugin/Core/NetworkRpc.h>
#include <NetworkPlugin/Core/NetworkSimulation.h>

#include <initializer_list>

/// \brief Checks a wait and reports it with a message whose values are read after the wait ended. The arguments of
/// PL_TEST_BOOL_MSG are evaluated in no fixed order, so a wait passed to it directly can be reported with stale values.
#define NETWORK_TEST_WAIT(wait, msg, ...)        \
  do                                             \
  {                                              \
    const bool bWaitHeld_ = (wait);              \
    PL_TEST_BOOL_MSG(bWaitHeld_, msg, ##__VA_ARGS__); \
  } while (false)

class plNetworkSecurityComponent;
using plNetworkSecurityComponentManager = plComponentManager<plNetworkSecurityComponent, plBlockStorageType::Compact>;

/// \brief A replicated component with one serialized value and one RPC target, used by every endpoint in the tests.
class plNetworkSecurityComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkSecurityComponent, plNetworkComponent, plNetworkSecurityComponentManager);

public:
  plUInt32 m_uiValue = 0;
  plUInt32 m_uiCalls = 0;
  void SetValue(plUInt32 uiValue)
  {
    m_uiValue = uiValue;
    ++m_uiCalls;
  }
  void NetworkSerialize(plNetworkMessage& msg) override { msg.WriteUInt32(m_uiValue); }
  void NetworkDeserialize(plNetworkMessage& msg) override
  {
    const plUInt32 uiValue = msg.ReadUInt32();
    if (!msg.HasReadError())
      m_uiValue = uiValue;
  }
};

class plNetworkReplicaTargetComponent;
using plNetworkReplicaTargetComponentManager = plComponentManager<plNetworkReplicaTargetComponent, plBlockStorageType::Compact>;

/// \brief A plain gameplay component whose reflected members a plNetworkedPropertiesComponent replicates.
class plNetworkReplicaTargetComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkReplicaTargetComponent, plComponent, plNetworkReplicaTargetComponentManager);

public:
  plInt32 m_iValue = 0;
  plVec3 m_vPosition = plVec3::MakeZero();
};

class plNetworkRpcProbeComponent;
using plNetworkRpcProbeComponentManager = plComponentManager<plNetworkRpcProbeComponent, plBlockStorageType::Compact>;

/// \brief One RPC per target. Each counts how often it ran on this peer and records the arguments in arrival order.
class plNetworkRpcProbeComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkRpcProbeComponent, plNetworkComponent, plNetworkRpcProbeComponentManager);

public:
  plUInt32 m_uiServer = 0;
  plUInt32 m_uiOwner = 0;
  plUInt32 m_uiAllClients = 0;
  plUInt32 m_uiAll = 0;
  plUInt32 m_uiMulticast = 0;
  plDynamicArray<plInt32> m_Arguments;

  void RpcServer(plInt32 iValue) { Record(m_uiServer, iValue); }
  void RpcOwner(plInt32 iValue) { Record(m_uiOwner, iValue); }
  void RpcAllClients(plInt32 iValue) { Record(m_uiAllClients, iValue); }
  void RpcAll(plInt32 iValue) { Record(m_uiAll, iValue); }
  void RpcMulticast(plInt32 iValue) { Record(m_uiMulticast, iValue); }

  void NetworkSerialize(plNetworkMessage&) override {}
  void NetworkDeserialize(plNetworkMessage&) override {}

private:
  void Record(plUInt32& ref_uiCount, plInt32 iValue)
  {
    ++ref_uiCount;
    m_Arguments.PushBack(iValue);
  }
};

class plNetworkRpcTwinComponent;
using plNetworkRpcTwinComponentManager = plComponentManager<plNetworkRpcTwinComponent, plBlockStorageType::Compact>;

/// \brief A second component type with an RPC named like one on plNetworkRpcProbeComponent.
class plNetworkRpcTwinComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkRpcTwinComponent, plNetworkComponent, plNetworkRpcTwinComponentManager);

public:
  plUInt32 m_uiServer = 0;
  void RpcServer(plInt32) { ++m_uiServer; }

  void NetworkSerialize(plNetworkMessage&) override {}
  void NetworkDeserialize(plNetworkMessage&) override {}
};

class plNetworkCounterComponent;
using plNetworkCounterComponentManager = plComponentManager<plNetworkCounterComponent, plBlockStorageType::Compact>;

/// \brief A server-authoritative value on the default channel and priority that counts every state message it applies.
class plNetworkCounterComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkCounterComponent, plNetworkComponent, plNetworkCounterComponentManager);

public:
  plUInt32 m_uiValue = 0;
  plUInt32 m_uiReceived = 0;

  void Set(plUInt32 uiValue)
  {
    m_uiValue = uiValue;
    MarkDirty();
  }

  void NetworkSerialize(plNetworkMessage& msg) override { msg.WriteUInt32(m_uiValue); }
  void NetworkDeserialize(plNetworkMessage& msg) override
  {
    const plUInt32 uiValue = msg.ReadUInt32();
    if (msg.HasReadError())
      return;

    m_uiValue = uiValue;
    ++m_uiReceived;
  }

protected:
  void OnSimulationStarted() override
  {
    plNetworkComponent::OnSimulationStarted();
    SetNetworkRole(plNetworkRoleEnum::Server);
  }
};

/// \brief Narrow test seams for constructing ownership and transport faults without loading gameplay assets.
class plNetworkTestAccess
{
public:
  static ENetHost* Host(plNetworkPeer* pPeer) { return static_cast<plDriverEnet*>(pPeer->GetTransportDriver())->m_pEnetHost; }

  /// \brief UDP payload bytes this peer's ENet host has sent or received, all connections and protocol overhead included.
  static plUInt64 BytesSent(plNetworkPeer* pPeer) { return pPeer ? Host(pPeer)->totalSentData : 0; }
  static plUInt64 BytesReceived(plNetworkPeer* pPeer) { return pPeer ? Host(pPeer)->totalReceivedData : 0; }

  static void Role(plNetworkWorldModule* pModule, plNetworkRoleEnum::Enum role) { pModule->m_Role = role; }
  static void Own(plNetworkWorldModule* pModule, plGameObject* pObject, plUInt32 uiObjectID, plUInt32 uiClientID)
  {
    plNetworkObjectInfo info;
    info.m_uiNetworkID = uiObjectID;
    info.m_hGameObject = pObject->GetHandle();
    info.m_uiOwnerClientID = uiClientID;
    pModule->m_ObjectManager.m_NetworkObjects.Insert(uiObjectID, info);
    pModule->m_ObjectManager.m_ObjectToNetworkID.Insert(pObject->GetHandle(), uiObjectID);
  }
  static void Expire(plNetworkClient* pClient) { pClient->m_AdmissionStartTime = plNetworkClock::Now() - plTime::MakeFromSeconds(20); }

  /// \brief Recipients the properties component keeps a last-sent record for.
  static plUInt32 SentRecordCount(const plNetworkedPropertiesComponent* pComponent) { return pComponent->m_LastSentValues.GetCount(); }
  static plUInt32 TimeSyncRecordCount(plNetworkWorldModule* pModule) { return pModule->GetNetworkTime()->m_LastServerResponseByClient.GetCount(); }
  static plUInt32 BandwidthStateCount(plNetworkWorldModule* pModule) { return pModule->GetBandwidthManager()->m_ComponentStates.GetCount(); }
  static plUInt32 FreeMessageCount(plNetworkPeer* pPeer) { return pPeer->m_MessagePool.GetCount(); }
  static plResult Platform(plDriverPlatform& driver, plPlatformTransport* pTransport, plNetworkPeer* pPeer)
  {
    if (driver.ClaimTransport(pTransport).Failed())
      return PL_FAILURE;

    driver.m_pNetworkPeer = pPeer;
    driver.m_bListening = true;
    driver.m_Config.m_uiConnectionLimit = 1;
    return PL_SUCCESS;
  }
};

namespace NetworkTestHarness
{
  /// \brief One network peer: its own world, world module and replicated object. Any number can exist in one process.
  struct NetworkEndpoint
  {
    plWorldDesc m_WorldDesc{"NetworkSecurity"};
    plWorld m_World{m_WorldDesc};
    plNetworkWorldModule* m_pModule;
    plNetworkSecurityComponent* m_pComponent;

    NetworkEndpoint()
    {
      PL_LOCK(m_World.GetWriteMarker());
      m_pModule = m_World.GetOrCreateModule<plNetworkWorldModule>();
      m_pModule->SetTickRate(0.0f);
      plGameObjectDesc desc;
      desc.m_sName.Assign("Replicated");
      desc.m_bDynamic = true;
      plGameObject* pObject = nullptr;
      m_World.CreateObject(desc, pObject);
      m_World.GetOrCreateComponentManager<plNetworkSecurityComponentManager>()->CreateComponent(pObject, m_pComponent);
      m_pComponent->SetNetworkRole(plNetworkRoleEnum::Server);
      m_World.Update();
    }

    /// \brief A scene object whose target component's Value and Position are replicated by a plNetworkedPropertiesComponent.
    struct Replica
    {
      plGameObject* m_pObject = nullptr;
      plNetworkReplicaTargetComponent* m_pTarget = nullptr;
      plNetworkedPropertiesComponent* m_pProperties = nullptr;
    };

    plHybridArray<Replica, 16> m_Replicas;

    /// \brief Adds a replica named sName. Every endpoint must add the same names in the same order, because scene NetIDs
    /// are hashed from the object path. Interpolation is off, so a received value is applied as it arrives.
    Replica& AddReplica(plStringView sName, plReplicateAuthority::Enum authority)
    {
      PL_LOCK(m_World.GetWriteMarker());
      plGameObjectDesc desc;
      desc.m_sName.Assign(sName);
      desc.m_bDynamic = true;

      Replica& replica = m_Replicas.ExpandAndGetRef();
      m_World.CreateObject(desc, replica.m_pObject);
      m_World.GetOrCreateComponentManager<plNetworkReplicaTargetComponentManager>()->CreateComponent(replica.m_pObject, replica.m_pTarget);
      m_World.GetOrCreateComponentManager<plNetworkedPropertiesComponentManager>()->CreateComponent(replica.m_pObject, replica.m_pProperties);

      plNetworkedPropertiesComponent* pProperties = replica.m_pProperties;
      pProperties->m_Authority = authority;
      pProperties->m_fInterpolationTime = 0.0f;
      pProperties->m_fSendRate = 60.0f;
      for (const char* szProperty : {"Value", "Position"})
      {
        plNetworkedPropertyEntry& entry = pProperties->m_Properties.ExpandAndGetRef();
        entry.m_sComponentType = plGetStaticRTTI<plNetworkReplicaTargetComponent>()->GetTypeName();
        entry.m_sProperty = szProperty;
      }

      m_World.Update();
      return replica;
    }

    /// \brief Adds a scene object with a plNetworkedTransformComponent. Bounds are tight so 16-bit positions resolve to ~2 mm.
    plGameObject* AddMover(plStringView sName, plNetworkedTransformComponent** out_pComponent = nullptr)
    {
      PL_LOCK(m_World.GetWriteMarker());
      plGameObjectDesc desc;
      desc.m_sName.Assign(sName);
      desc.m_bDynamic = true;
      desc.m_LocalPosition.Set(1.0f, 2.0f, 3.0f);
      plGameObject* pObject = nullptr;
      m_World.CreateObject(desc, pObject);

      plNetworkedTransformComponent* pComponent = nullptr;
      m_World.GetOrCreateComponentManager<plNetworkedTransformComponentManager>()->CreateComponent(pObject, pComponent);
      pComponent->m_vWorldBoundsMin = plVec3(-64.0f);
      pComponent->m_vWorldBoundsMax = plVec3(64.0f);
      if (out_pComponent)
        *out_pComponent = pComponent;

      m_World.Update();
      return pObject;
    }

    /// \brief The host's record of the client that this endpoint is, or null.
    plNetworkClient* FindClient(const NetworkEndpoint& client)
    {
      for (plNetworkClient* pClient : m_pModule->GetClients())
      {
        if (pClient && pClient->m_uiClientID == client.m_pModule->GetLocalClientID())
          return pClient;
      }
      return nullptr;
    }

    plUInt64 BytesSent() const { return plNetworkTestAccess::BytesSent(m_pModule->GetPeer()); }
    plUInt64 BytesReceived() const { return plNetworkTestAccess::BytesReceived(m_pModule->GetPeer()); }

    void Pump()
    {
      PL_LOCK(m_World.GetWriteMarker());
      m_World.Update();
    }

    /// \brief Conditions this endpoint simulates on everything it sends and receives, applied when it starts.
    plNetworkSimulationSettings m_Simulation;
    plUInt32 m_uiGameVersion = 0;
    plUInt16 m_uiMessagePoolSize = 64;

    enum class Role
    {
      Host,   ///< Server with a local player.
      Server, ///< Dedicated server.
      Client,
    };

    bool StartAs(Role role, plUInt16 uiPort = 0)
    {
      PL_LOCK(m_World.GetWriteMarker());
      plNetworkConfig config;
      config.m_sServerAddress = "127.0.0.1";
      config.m_uiServerPort = uiPort;
      config.m_uiMessagePoolSize = m_uiMessagePoolSize;
      config.m_Simulation = m_Simulation;
      m_pModule->SetNetworkConfig(config);
      m_pModule->SetGameVersion(m_uiGameVersion);

      switch (role)
      {
        case Role::Host:
          return m_pModule->StartHost();
        case Role::Server:
          return m_pModule->StartServer();
        case Role::Client:
        default:
          return m_pModule->StartClient();
      }
    }

    bool Start(bool bHost, plUInt16 uiPort = 0) { return StartAs(bHost ? Role::Host : Role::Client, uiPort); }

    /// \brief Changes the conditions of a running endpoint.
    void Simulate(const plNetworkSimulationSettings& settings)
    {
      m_Simulation = settings;
      if (m_pModule->GetPeer())
        m_pModule->GetPeer()->GetSimulator()->SetSettings(settings);
    }

    void RegisterPrefab(const plHashedString& sPrefab) { m_pModule->GetObjectManager()->RegisterNetworkPrefab(sPrefab); }

    /// \brief Spawns a registered prefab on this server or host; returns its object NetID, 0 on failure.
    plUInt32 Spawn(const plHashedString& sPrefab, plUInt32 uiOwnerClientID, const plVec3& vPosition = plVec3::MakeZero())
    {
      PL_LOCK(m_World.GetWriteMarker());
      return m_pModule->GetObjectManager()->SpawnNetworkObject(sPrefab, plTransform(vPosition), uiOwnerClientID);
    }

    bool Despawn(plUInt32 uiObjectID)
    {
      PL_LOCK(m_World.GetWriteMarker());
      return m_pModule->GetObjectManager()->DespawnNetworkObject(uiObjectID);
    }

    /// \brief The local instance of a spawned object, or null.
    plGameObject* FindSpawned(plUInt32 uiObjectID)
    {
      PL_LOCK(m_World.GetReadMarker());
      plGameObject* pObject = nullptr;
      m_World.TryGetObject(m_pModule->GetObjectManager()->GetGameObject(uiObjectID), pObject);
      return pObject;
    }

    /// \brief A component of type T on a spawned object, or on its child named sChild.
    template <typename T>
    T* FindSpawnedComponent(plUInt32 uiObjectID, plStringView sChild = {})
    {
      PL_LOCK(m_World.GetReadMarker());
      plGameObject* pObject = FindSpawned(uiObjectID);
      if (pObject != nullptr && !sChild.IsEmpty())
        pObject = pObject->FindChildByName(plTempHashedString(sChild), false);

      T* pComponent = nullptr;
      if (pObject != nullptr)
        pObject->TryGetComponentOfBaseType(pComponent);
      return pComponent;
    }

    plUInt32 GetOwnerOf(plUInt32 uiObjectID) const { return m_pModule->GetObjectManager()->GetOwner(uiObjectID); }

    void Stop()
    {
      PL_LOCK(m_World.GetWriteMarker());
      m_pModule->Stop();
    }

    plUInt16 GetPort() { return plNetworkTestAccess::Host(m_pModule->GetPeer())->address.port; }
  };

  /// \brief Prefabs built in memory for one test. Releasing the library frees them, so no test leaks a resource.
  struct TestPrefabs
  {
    plDynamicArray<plPrefabResourceHandle> m_Handles;

    ~TestPrefabs()
    {
      m_Handles.Clear();
      plResourceManager::FreeAllUnusedResources();
    }

    /// \brief Writes a prefab whose root object is named sName; build(world, pRoot) adds its components and children.
    template <typename Build>
    plHashedString Add(plStringView sName, Build build)
    {
      plStringBuilder sId("NetworkTestPrefab/", sName);

      plUniquePtr<plResourceLoaderFromMemory> pLoader = PL_DEFAULT_NEW(plResourceLoaderFromMemory);
      pLoader->m_sResourceDescription = sId;
      {
        plWorldDesc desc("NetworkTestPrefabSource");
        plWorld world(desc);
        PL_LOCK(world.GetWriteMarker());

        plGameObjectDesc root;
        root.m_sName.Assign(sName);
        root.m_bDynamic = true;
        plGameObject* pRoot = nullptr;
        world.CreateObject(root, pRoot);
        build(world, pRoot);

        plMemoryStreamWriter stream(&pLoader->m_CustomData);
        stream << plString(sId);

        plAssetFileHeader header;
        header.SetFileHashAndVersion(1, 6);
        header.Write(stream).IgnoreResult();

        const char szSceneTag[16] = "[plBinaryScene]";
        stream.WriteBytes(szSceneTag, sizeof(szSceneTag)).IgnoreResult();

        plWorldWriter writer;
        writer.WriteWorld(stream, world);
        stream << static_cast<plUInt32>(0); // no exposed parameters
      }

      plPrefabResourceHandle hPrefab = plResourceManager::GetExistingResourceOrCreateAsync<plPrefabResource>(sId, std::move(pLoader));
      plResourceLock<plPrefabResource> pPrefab(hPrefab, plResourceAcquireMode::BlockTillLoaded);
      m_Handles.PushBack(hPrefab);

      plHashedString sPrefab;
      sPrefab.Assign(sId);
      return sPrefab;
    }
  };

  /// \brief Adds a component of type T to pObject in a world under construction and returns it.
  template <typename T>
  T* AddComponent(plWorld& ref_world, plGameObject* pObject)
  {
    T* pComponent = nullptr;
    ref_world.GetOrCreateComponentManager<typename T::ComponentManagerType>()->CreateComponent(pObject, pComponent);
    return pComponent;
  }

  /// \brief Adds a child object named sName below pParent.
  inline plGameObject* AddChild(plWorld& ref_world, plGameObject* pParent, plStringView sName)
  {
    plGameObjectDesc desc;
    desc.m_sName.Assign(sName);
    desc.m_bDynamic = true;
    desc.m_hParent = pParent->GetHandle();
    plGameObject* pChild = nullptr;
    ref_world.CreateObject(desc, pChild);
    return pChild;
  }

  /// \brief Configures a properties component to replicate Value and Position of a sibling plNetworkReplicaTargetComponent.
  inline void ReplicateTarget(plNetworkedPropertiesComponent* pProperties, plReplicateAuthority::Enum authority)
  {
    pProperties->m_Authority = authority;
    pProperties->m_fInterpolationTime = 0.0f;
    pProperties->m_fSendRate = 60.0f;
    pProperties->m_Properties.Clear();
    for (const char* szProperty : {"Value", "Position"})
    {
      plNetworkedPropertyEntry& entry = pProperties->m_Properties.ExpandAndGetRef();
      entry.m_sComponentType = plGetStaticRTTI<plNetworkReplicaTargetComponent>()->GetTypeName();
      entry.m_sProperty = szProperty;
    }
  }

  struct HostileClient;

  /// \brief Everything one wait services.
  ///
  /// A host drops a client that is not serviced for its inactivity timeout, so a wait lists every endpoint and hostile
  /// client that is alive in the test, not only the ones it is waiting for. Null entries are skipped.
  struct Pumped
  {
    Pumped(std::initializer_list<NetworkEndpoint*> endpoints, std::initializer_list<HostileClient*> hostile = {})
    {
      for (NetworkEndpoint* pEndpoint : endpoints)
      {
        if (pEndpoint)
          m_Endpoints.PushBack(pEndpoint);
      }

      for (HostileClient* pClient : hostile)
      {
        if (pClient)
          m_Hostile.PushBack(pClient);
      }
    }

    plHybridArray<NetworkEndpoint*, 4> m_Endpoints;
    plHybridArray<HostileClient*, 4> m_Hostile;
  };

  inline void PumpOnce(const Pumped& pumped);

  /// \brief Services everything in the set until the predicate holds or the timeout passes.
  ///
  /// The loop yields instead of sleeping. A fixed sleep is a timer-granularity wait whose length depends on the machine,
  /// so a wait counted in sleeps is a wall-clock budget that changes from run to run.
  template <typename Predicate>
  bool PumpAll(const Pumped& pumped, Predicate predicate, plTime timeout = plTime::MakeFromSeconds(5))
  {
    const plTime deadline = plTime::Now() + timeout;
    do
    {
      PumpOnce(pumped);
      if (predicate())
        return true;
      plThreadUtils::YieldTimeSlice();
    } while (plTime::Now() < deadline);
    return false;
  }

  /// \brief Services everything in the set for a fixed wall-clock duration. Only for traffic that is itself time-driven
  /// (pings, time sync, send-rate throttles); waits for an outcome go through PumpAll.
  inline void PumpFor(const Pumped& pumped, plTime duration)
  {
    PumpAll(pumped, [] { return false; }, duration);
  }

  /// \brief Starts the server in the given role and every client, then waits until each client is admitted and the server
  /// lists all of them.
  inline bool ConnectAllAs(NetworkEndpoint::Role role, const Pumped& pumped, NetworkEndpoint& host, std::initializer_list<NetworkEndpoint*> clients,
    plTime timeout = plTime::MakeFromSeconds(5))
  {
    if (!host.StartAs(role))
      return false;

    for (NetworkEndpoint* pClient : clients)
    {
      if (!pClient->Start(false, host.GetPort()))
        return false;
    }

    return PumpAll(pumped, [&]
      {
        for (NetworkEndpoint* pClient : clients)
        {
          if (!pClient->m_pModule->IsConnected() || host.FindClient(*pClient) == nullptr)
            return false;
        }
        return true;
      },
      timeout);
  }

  /// \brief Starts the host and every client, then waits until each client is admitted and the host lists all of them.
  inline bool ConnectAll(const Pumped& pumped, NetworkEndpoint& host, std::initializer_list<NetworkEndpoint*> clients)
  {
    if (!host.Start(true))
      return false;

    for (NetworkEndpoint* pClient : clients)
    {
      if (!pClient->Start(false, host.GetPort()))
        return false;
    }

    return PumpAll(pumped, [&]
      {
        for (NetworkEndpoint* pClient : clients)
        {
          if (!pClient->m_pModule->IsConnected() || host.FindClient(*pClient) == nullptr)
            return false;
        }
        return true;
      });
  }

  /// \brief Pumps the host and up to two clients until the predicate holds or five seconds pass.
  template <typename Predicate>
  bool PumpUntil(NetworkEndpoint& host, NetworkEndpoint* pA, NetworkEndpoint* pB, Predicate predicate)
  {
    return PumpAll(Pumped({&host, pA, pB}), predicate);
  }

  struct AdmissionObserver : plNetworkObject
  {
    plNetworkWorldModule* m_pModule = nullptr;
    plUInt32 m_uiConnected = 0;
    plUInt32 m_uiMessages = 0;
    bool m_bStopOnAdmission = false;
    void OnClientConnecting(plNetworkClientConnectionData&) override
    {
      if (m_bStopOnAdmission)
        m_pModule->Stop();
    }
    void OnClientConnected(plNetworkClient*) override { ++m_uiConnected; }
    void OnNetworkMessage(plNetworkMessage&) override { ++m_uiMessages; }
  };

  /// \brief A raw ENet client that does not perform the application handshake; wire IDs and payloads are under test control.
  struct HostileClient
  {
    ENetHost* m_pHost = nullptr;
    ENetPeer* m_pPeer = nullptr;
    bool m_bConnected = false;
    bool m_bDisconnected = false;

    HostileClient(plUInt16 uiPort, plUInt32 uiConnectID)
    {
      m_pHost = enet_host_create(nullptr, 1, 1, 0, 0);
      ENetAddress address = {};
      enet_address_set_host(&address, "127.0.0.1");
      address.port = uiPort;
      m_pPeer = enet_host_connect(m_pHost, &address, 1, 0);
      m_pPeer->connectID = uiConnectID;
      auto* pConnect = reinterpret_cast<ENetOutgoingCommand*>(enet_list_begin(&m_pPeer->outgoingReliableCommands));
      pConnect->command.connect.connectID = uiConnectID;
    }

    ~HostileClient() { enet_host_destroy(m_pHost); }

    void Pump()
    {
      ENetEvent event;
      while (enet_host_service(m_pHost, &event, 0) > 0)
      {
        if (event.type == ENET_EVENT_TYPE_CONNECT)
          m_bConnected = true;
        if (event.type == ENET_EVENT_TYPE_DISCONNECT)
          m_bDisconnected = true;
        if (event.type == ENET_EVENT_TYPE_RECEIVE)
          enet_packet_destroy(event.packet);
      }
    }

    /// \brief True once the host has acknowledged everything this client sent reliably, including the connect, or once
    /// the connection is gone.
    bool IsSettled() const
    {
      return m_bDisconnected || (enet_list_empty(&m_pPeer->outgoingReliableCommands) && enet_list_empty(&m_pPeer->sentReliableCommands));
    }

    /// \brief Tells the host this client is gone, so its connection slot is freed before the next hostile client connects.
    void Disconnect()
    {
      if (!m_bConnected || m_bDisconnected)
        return;

      enet_peer_disconnect_now(m_pPeer, 0);
      enet_host_flush(m_pHost);
      m_bDisconnected = true;
    }

    /// \brief Sends the bytes as one reliable packet, with no message framing applied.
    void SendRaw(const plUInt8* pBytes, plUInt32 uiLength)
    {
      ENetPacket* packet = enet_packet_create_offset(pBytes, uiLength, 0, ENET_PACKET_FLAG_RELIABLE);
      if (enet_peer_send(m_pPeer, 0, packet) < 0)
        enet_packet_destroy(packet);
      enet_host_flush(m_pHost);
    }

    void Send(const plNetworkMessage& msg) { SendRaw(msg.m_pBuffer, msg.m_uiLength); }

    /// \brief Sends one message whose ID byte and payload are chosen freely, independent of what the ID means.
    void SendWithID(plUInt8 uiMessageID, const plUInt8* pPayload, plUInt32 uiPayloadLength)
    {
      plDynamicArray<plUInt8> bytes;
      bytes.SetCountUninitialized(uiPayloadLength + 1);
      bytes[0] = uiMessageID;
      if (uiPayloadLength > 0)
        plMemoryUtils::Copy(bytes.GetData() + 1, pPayload, uiPayloadLength);
      SendRaw(bytes.GetData(), bytes.GetCount());
    }

    void Handshake()
    {
      plUInt8 bytes[128] = {};
      plNetworkMessage msg(bytes, 0, sizeof(bytes), 0, 0);
      msg.WriteUInt8(plNetworkMessageIDs::Handshake);
      msg.WriteUInt32(NETWORK_PROTOCOL_VERSION);
      msg.WriteUInt32(0);
      msg.WriteString("test");
      msg.WriteUInt16(0);
      Send(msg);
    }
  };

  inline void PumpOnce(const Pumped& pumped)
  {
    for (NetworkEndpoint* pEndpoint : pumped.m_Endpoints)
      pEndpoint->Pump();

    for (HostileClient* pClient : pumped.m_Hostile)
      pClient->Pump();
  }

  /// \brief Pumps until the host has acknowledged everything the hostile clients sent, then once more.
  ///
  /// ENet acknowledges a reliable packet in the same service call that receives it, and the world module handles every
  /// received event in that update, so the extra round only covers work the host defers to its next update.
  inline bool Settle(const Pumped& pumped)
  {
    const bool bSettled = PumpAll(pumped, [&]
      {
        for (const HostileClient* pClient : pumped.m_Hostile)
        {
          if (!pClient->IsSettled())
            return false;
        }
        return true;
      });

    if (!bSettled)
      plLog::Warning("NetworkTest: hostile traffic was still unacknowledged when the wait ran out.");

    PumpOnce(pumped);
    return bSettled;
  }

  /// \brief Settles one hostile client against the host and reports whether it is connected.
  inline bool PumpHostile(NetworkEndpoint& host, HostileClient& client)
  {
    Settle(Pumped({&host}, {&client}));
    return client.m_bConnected;
  }

  /// \brief Disconnects the hostile client and waits until the host has released its connection slot.
  ///
  /// This counts ENet peers rather than world module clients, so it also finishes when the host had already dropped the
  /// client and only the transport connection was left.
  inline void DisconnectHostile(const Pumped& pumped, NetworkEndpoint& host, HostileClient& client)
  {
    if (!client.m_bConnected || client.m_bDisconnected || host.m_pModule->GetPeer() == nullptr)
      return;

    const ENetHost* pHost = plNetworkTestAccess::Host(host.m_pModule->GetPeer());
    const size_t uiPeersBefore = pHost->connectedPeers;
    client.Disconnect();
    PumpAll(pumped, [&] { return pHost->connectedPeers < uiPeersBefore; });
  }
} // namespace NetworkTestHarness
