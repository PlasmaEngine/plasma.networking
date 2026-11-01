#pragma once

#include "NetworkTestPCH.h"

#include <Core/World/World.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <GameEngine/PlatformServices/Interfaces/PlatformProviders.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Core/Driver/enet/DriverEnet.h>
#include <NetworkPlugin/Core/Driver/platform/DriverPlatform.h>
#include <NetworkPlugin/Core/NetworkLevelManager.h>

#include <initializer_list>

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

/// \brief Narrow test seams for constructing ownership and transport faults without loading gameplay assets.
class plNetworkTestAccess
{
public:
  static ENetHost* Host(plNetworkPeer* pPeer) { return static_cast<plDriverEnet*>(pPeer->GetDriver())->m_pEnetHost; }
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
  static void Expire(plNetworkClient* pClient) { pClient->m_AdmissionStartTime = plTime::Now() - plTime::MakeFromSeconds(20); }
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

    void Pump()
    {
      PL_LOCK(m_World.GetWriteMarker());
      m_World.Update();
    }

    bool Start(bool bHost, plUInt16 uiPort = 0)
    {
      PL_LOCK(m_World.GetWriteMarker());
      plNetworkConfig config;
      config.m_sServerAddress = "127.0.0.1";
      config.m_uiServerPort = uiPort;
      config.m_uiMessagePoolSize = 64;
      m_pModule->SetNetworkConfig(config);
      return bHost ? m_pModule->StartHost() : m_pModule->StartClient();
    }

    void Stop()
    {
      PL_LOCK(m_World.GetWriteMarker());
      m_pModule->Stop();
    }

    plUInt16 GetPort() { return plNetworkTestAccess::Host(m_pModule->GetPeer())->address.port; }
  };

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
