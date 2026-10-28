#pragma once

#include "NetworkTestPCH.h"

#include <Core/World/World.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <GameEngine/PlatformServices/Interfaces/PlatformProviders.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Core/Driver/enet/DriverEnet.h>
#include <NetworkPlugin/Core/Driver/platform/DriverPlatform.h>
#include <NetworkPlugin/Core/NetworkLevelManager.h>

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
  static void Platform(plDriverPlatform& driver, plPlatformTransport* pTransport, plNetworkPeer* pPeer)
  {
    driver.m_pTransport = pTransport;
    driver.m_pNetworkPeer = pPeer;
    driver.m_bListening = true;
    driver.m_Config.m_uiConnectionLimit = 1;
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

    plUInt16 GetPort() { return plNetworkTestAccess::Host(m_pModule->GetPeer())->address.port; }
  };

  /// \brief Pumps the host and up to two clients until the predicate holds or five seconds pass.
  template <typename Predicate>
  bool PumpUntil(NetworkEndpoint& host, NetworkEndpoint* pA, NetworkEndpoint* pB, Predicate predicate)
  {
    const plTime deadline = plTime::Now() + plTime::MakeFromSeconds(5);
    do
    {
      host.Pump();
      if (pA)
        pA->Pump();
      if (pB)
        pB->Pump();
      if (predicate())
        return true;
      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1));
    } while (plTime::Now() < deadline);
    return false;
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

  inline bool PumpHostile(NetworkEndpoint& host, HostileClient& client, plUInt32 uiCount = 50)
  {
    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      host.Pump();
      client.Pump();
      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1));
    }
    return client.m_bConnected;
  }
} // namespace NetworkTestHarness
