#include "NetworkTestPCH.h"

#include <Core/World/World.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <GameEngine/PlatformServices/Interfaces/PlatformProviders.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Components/NetworkedTransformComponent.h>
#include <NetworkPlugin/Core/Driver/enet/DriverEnet.h>
#include <NetworkPlugin/Core/Driver/platform/DriverPlatform.h>
#include <NetworkPlugin/Core/NetworkLevelManager.h>
#include <NetworkPlugin/Core/NetworkVariant.h>

#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

PL_BEGIN_COMPONENT_TYPE(plNetworkSecurityComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_FUNCTIONS
  {
    PL_FUNCTION_PROPERTY(SetValue),
  }
  PL_END_FUNCTIONS;
}
PL_END_COMPONENT_TYPE

PL_CREATE_SIMPLE_TEST(Network, AdmissionAndLifetime)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-F01: host and two clients replicate and tear down")
  {
    const plUInt32 uiPeersBefore = plNetworkPeer::m_Peers.GetCount();
    {
      NetworkEndpoint host, a, b;
      if (!PL_TEST_BOOL(host.Start(true))) return;
      const plUInt16 port = plNetworkTestAccess::Host(host.m_pModule->GetPeer())->address.port;
      if (!PL_TEST_BOOL(a.Start(false, port) && b.Start(false, port))) return;
      PL_TEST_BOOL(PumpUntil(host, &a, &b, [&] { return a.m_pModule->IsConnected() && b.m_pModule->IsConnected(); }));
      host.m_pComponent->m_uiValue = 73;
      host.m_pComponent->MarkDirty();
      PL_TEST_BOOL(PumpUntil(host, &a, &b, [&] { return a.m_pComponent->m_uiValue == 73 && b.m_pComponent->m_uiValue == 73; }));
    }
    PL_TEST_INT(plNetworkPeer::m_Peers.GetCount(), uiPeersBefore);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B01/02/08/11/12/16: hostile admission and teardown")
  {
    NetworkEndpoint host;
    if (!PL_TEST_BOOL(host.Start(true))) return;
    AdmissionObserver observer;
    observer.m_pModule = host.m_pModule;
    host.m_pModule->RegisterNetworkObject(&observer);
    const plUInt16 port = plNetworkTestAccess::Host(host.m_pModule->GetPeer())->address.port;
    {
      HostileClient first(port, 0), second(port, 0);
      for (plUInt32 i = 0; i < 100; ++i) { host.Pump(); first.Pump(); second.Pump(); plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1)); }
      if (!PL_TEST_BOOL(host.m_pModule->GetClients().GetCount() == 2)) return;
      auto* pFirst = host.m_pModule->GetClients()[0];
      auto* pSecond = host.m_pModule->GetClients()[1];
      PL_TEST_BOOL(pFirst->m_Connection.m_uiConnectionID != 0 && pSecond->m_Connection.m_uiConnectionID != 0);
      PL_TEST_BOOL(pFirst->m_Connection.m_uiConnectionID != pSecond->m_Connection.m_uiConnectionID);
      plUInt8 bytes[1] = {plNetworkMessageIDs::UserMessageStart};
      plNetworkMessage message(bytes, 0, 1, 1, 0);
      first.Send(message);
      PumpHostile(host, first);
      PL_TEST_INT(observer.m_uiMessages, 0);
      first.Handshake();
      PumpHostile(host, first);
      PL_TEST_INT(observer.m_uiConnected, 1);
      first.Handshake();
      PumpHostile(host, first);
      PL_TEST_INT(observer.m_uiConnected, 1);
      for (auto* pClient : host.m_pModule->GetClients())
      {
        if (pClient->m_ConnectionState == plNetworkConnectionState::Connecting)
          plNetworkTestAccess::Expire(pClient);
      }
      PumpHostile(host, second);
      if (!PL_TEST_BOOL(host.m_pModule->GetClients().GetCount() == 1)) return;
      const plUInt32 uiKickedID = host.m_pModule->GetClients()[0]->m_uiClientID;
      host.m_pModule->KickClient(uiKickedID);
      PumpHostile(host, first);
      PL_TEST_BOOL(host.m_pModule->GetClients().IsEmpty());
      PL_TEST_INT(observer.m_uiMessages, 0);
    }
    {
      HostileClient rejected(port, 0);
      PumpHostile(host, rejected);
      observer.m_bStopOnAdmission = true;
      rejected.Handshake();
      PumpHostile(host, rejected);
      PL_TEST_BOOL(host.m_pModule->GetPeer() == nullptr);
    }
    host.m_pModule->RemoveNetworkObject(&observer);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B15/16: a server cannot send a client-admission handshake")
  {
    NetworkEndpoint host, client;
    if (!PL_TEST_BOOL(host.Start(true))) return;
    const plUInt16 port = plNetworkTestAccess::Host(host.m_pModule->GetPeer())->address.port;
    if (!PL_TEST_BOOL(client.Start(false, port))) return;
    PL_TEST_BOOL(PumpUntil(host, &client, nullptr, [&] { return client.m_pModule->IsConnected(); }));
    if (host.m_pModule->GetClients().IsEmpty()) return;
    plNetworkMessage msg = host.m_pModule->GetPeer()->BeginSendMessage();
    msg.WriteUInt8(plNetworkMessageIDs::Handshake);
    host.m_pModule->GetPeer()->SendMessages(msg, host.m_pModule->GetClients()[0]->m_Connection);
    PL_TEST_BOOL(PumpUntil(host, &client, nullptr, [&] { return client.m_pModule->GetPeer() == nullptr; }));
  }
}

PL_CREATE_SIMPLE_TEST(Network, AuthorityAndRpc)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B03/04/06/09/24: explicit component ownership")
  {
    NetworkEndpoint endpoint;
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    auto* module = endpoint.m_pModule;
    auto* component = endpoint.m_pComponent;
    component->SetNetworkRole(plNetworkRoleEnum::Client);
    plNetworkTestAccess::Role(module, plNetworkRoleEnum::Host);
    PL_TEST_BOOL(!module->HasAuthority(component, 1));
    plNetworkTestAccess::Own(module, component->GetOwner(), 99, 1);
    PL_TEST_BOOL(module->HasAuthority(component, 1));
    PL_TEST_BOOL(!module->HasAuthority(component, 2));
    PL_TEST_BOOL(module->ValidateStateUpdate(component->GetNetID(), 1));
    PL_TEST_BOOL(!module->ValidateStateUpdate(component->GetNetID(), 2));
    PL_TEST_BOOL(module->FindNetworkComponent(component->GetNetID()) == component);
    PL_TEST_INT(module->FindOwningObject(component)->m_uiNetworkID, 99);

    plUInt8 bytes[64] = {};
    plNetworkMessage msg(bytes, 0, sizeof(bytes), 0, 0);
    msg.WriteUInt32(99); msg.WriteUInt32(2); msg.WriteUInt8(plNetworkObjectRole::OwnedAuthoritative);
    msg.m_uiPosition = 0;
    module->GetObjectManager()->HandleOwnershipMessage(msg);
    PL_TEST_INT(module->GetObjectManager()->GetOwner(99), 1);
    msg.m_uiPosition = 0;
    module->GetObjectManager()->HandleDespawnMessage(msg);
    PL_TEST_BOOL(module->GetObjectManager()->GetObjectInfo(99) != nullptr);
    msg.m_uiPosition = 0;
    module->GetObjectManager()->HandleSpawnMessage(msg);
    PL_TEST_INT(module->GetObjectManager()->GetAllObjects().GetCount(), 1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B05/24: malformed arity/type/truncation never invokes the exact target")
  {
    NetworkEndpoint endpoint;
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    auto* module = endpoint.m_pModule;
    auto* component = endpoint.m_pComponent;
    plNetworkTestAccess::Role(module, plNetworkRoleEnum::Host);
    module->GetRpcManager()->RegisterRpc(component->GetDynamicRTTI(), "SetValue", plNetworkRpcTarget::Server);
    const auto* rpc = module->GetRpcManager()->GetRpcInfo(component->GetDynamicRTTI(), "SetValue");
    if (!PL_TEST_BOOL(rpc != nullptr)) return;
    for (plUInt8 count : {plUInt8(0), plUInt8(200), plUInt8(1)})
    {
      plUInt8 bytes[64] = {};
      plNetworkMessage msg(bytes, 0, sizeof(bytes), 0, 0);
      msg.WriteUInt32(rpc->m_uiRpcID); msg.WriteUInt32(component->GetNetID()); msg.WriteUInt8(count);
      msg.WriteUInt8(plVariantType::UInt32); // Missing value for the correct arity.
      msg.m_uiPosition = 0;
      module->GetRpcManager()->HandleRpcMessage(msg, 0);
      PL_TEST_BOOL(msg.HasReadError());
      PL_TEST_INT(component->m_uiCalls, 0);
    }
    for (plUInt8 type : {plUInt8(plVariantType::Float), plUInt8(plVariantType::UInt32)})
    {
      plUInt8 bytes[64] = {};
      plNetworkMessage msg(bytes, 0, sizeof(bytes), 0, 0);
      msg.WriteUInt32(rpc->m_uiRpcID); msg.WriteUInt32(component->GetNetID()); msg.WriteUInt8(1);
      msg.WriteUInt8(type); msg.WriteUInt32(42); msg.m_uiPosition = 0;
      module->GetRpcManager()->HandleRpcMessage(msg, 0);
      PL_TEST_BOOL(msg.HasReadError() == (type == plVariantType::Float));
    }
    PL_TEST_INT(component->m_uiCalls, 1);
    PL_TEST_INT(component->m_uiValue, 42);
  }
}

PL_CREATE_SIMPLE_TEST(Network, VariantAllocationBounds)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B10: outer and nested lengths are validated before allocation")
  {
    for (plUInt32 length : {0u, 1u, 65537u, 0xFFFFFFFEu})
    {
      plUInt8 bytes[16] = {};
      plNetworkMessage msg(bytes, 0, sizeof(bytes), 0, 0);
      msg.WriteUInt32(length); msg.m_uiPosition = 0;
      plVariant value;
      PL_TEST_BOOL(!plReadNetworkVariant(msg, value));
      PL_TEST_BOOL(msg.HasReadError());
    }
    for (auto type : {plVariantType::String, plVariantType::DataBuffer, plVariantType::VariantArray, plVariantType::VariantDictionary})
    {
      plUInt8 bytes[16] = {};
      plNetworkMessage msg(bytes, 0, sizeof(bytes), 0, 0);
      msg.WriteUInt32(6); msg.WriteUInt8(plGetStaticRTTI<plVariant>()->GetTypeVersion());
      msg.WriteUInt8(type); msg.WriteUInt32(0xFFFFFFFE); msg.m_uiPosition = 0;
      plVariant value;
      PL_TEST_BOOL(!plReadNetworkVariant(msg, value));
      PL_TEST_BOOL(msg.HasReadError());
    }
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    plVariantArray source; source.PushBack(plVariant(plUInt32(42))); source.PushBack(plVariant("hello"));
    writer << plVariant(source);
    plUInt8 bytes[256] = {};
    plNetworkMessage msg(bytes, 0, sizeof(bytes), 0, 0);
    msg.WriteUInt32(static_cast<plUInt32>(storage.GetStorageSize64()));
    plMemoryStreamReader reader(&storage);
    const plUInt32 uiSize = static_cast<plUInt32>(storage.GetStorageSize64());
    reader.ReadBytes(bytes + msg.m_uiPosition, uiSize);
    msg.m_uiLength += uiSize; msg.m_uiPosition = 0;
    plVariant result;
    PL_TEST_BOOL(plReadNetworkVariant(msg, result));
    PL_TEST_BOOL(result == plVariant(source));
  }
}

namespace
{
  void SendLevelMessage(HostileClient& client, plNetworkMessageIDs::Enum id, plUInt64 uiGeneration, const char* szPath = nullptr)
  {
    plUInt8 bytes[128] = {};
    plNetworkMessage msg(bytes, 0, sizeof(bytes), 0, 0);
    msg.WriteUInt8(id);
    msg.WriteUInt64(uiGeneration);
    if (szPath != nullptr)
      msg.WriteString(szPath);
    client.Send(msg);
  }

  // Scripted transport: events are queued by the test, disconnects are only counted.
  class FakeTransport : public plPlatformTransport
  {
  public:
    plDynamicArray<plPlatformTransportEvent> m_Events;
    plUInt32 m_uiDisconnects = 0;

    plResult Listen() override { return PL_SUCCESS; }
    void StopListening() override {}
    plResult Connect(const plPlatformUserId&) override { return PL_SUCCESS; }
    void Disconnect(const plPlatformUserId&) override { ++m_uiDisconnects; }
    void DisconnectAll() override {}
    plResult Send(const plPlatformUserId&, plArrayPtr<const plUInt8>, bool, plUInt8) override { return PL_SUCCESS; }
    plUInt32 GetMaxPayloadBytes() const override { return 1200; }
    bool PollEvent(plPlatformTransportEvent& out_event) override
    {
      if (m_Events.IsEmpty())
        return false;
      out_event = m_Events[0];
      m_Events.RemoveAtAndCopy(0);
      return true;
    }

    void Push(plPlatformTransportEvent::Type type, const plPlatformUserId& user, plUInt32 uiBytes = 0)
    {
      plPlatformTransportEvent& event = m_Events.ExpandAndGetRef();
      event.m_Type = type;
      event.m_User = user;
      event.m_Data.SetCount(uiBytes, 1);
    }
  };

  plUInt32 CountFreeMessages(plNetworkPeer* pPeer)
  {
    // Draining the pool on purpose logs an error when it runs dry.
    plMuteLog mute;
    plLogSystemScope logScope(&mute);

    plDynamicArray<plNetworkMessage> messages;
    for (plNetworkMessage message = pPeer->CreateMessage(); message.IsValid(); message = pPeer->CreateMessage())
      messages.PushBack(message);

    const plUInt32 uiCount = messages.GetCount();
    for (plNetworkMessage& message : messages)
      pPeer->DestroyMessage(message);
    return uiCount;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, HostileTraffic)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B07: level commands are host-only and acknowledgements bind to the load generation")
  {
    NetworkEndpoint host;
    if (!PL_TEST_BOOL(host.Start(true)))
      return;
    const plUInt16 port = plNetworkTestAccess::Host(host.m_pModule->GetPeer())->address.port;
    HostileClient hostile(port, 0);
    PumpHostile(host, hostile);
    hostile.Handshake();
    PumpHostile(host, hostile);
    if (!PL_TEST_BOOL(host.m_pModule->GetClients().GetCount() == 1))
      return;

    plNetworkLevelManager* pLevels = host.m_pModule->GetLevelManager();
    SendLevelMessage(hostile, plNetworkMessageIDs::LevelLoad, 7, "Forged.plScene");
    SendLevelMessage(hostile, plNetworkMessageIDs::LevelStart, 7);
    PumpHostile(host, hostile);
    PL_TEST_BOOL(pLevels->GetState() == plNetworkLevelState::None);
    PL_TEST_BOOL(pLevels->GetLoadGeneration() == 0);

    {
      PL_LOCK(host.m_World.GetWriteMarker());
      pLevels->RequestLevelLoad("Real.plScene");
    }
    PumpHostile(host, hostile);
    PL_TEST_BOOL(pLevels->GetState() == plNetworkLevelState::Loading);

    const plUInt64 uiGeneration = pLevels->GetLoadGeneration();
    SendLevelMessage(hostile, plNetworkMessageIDs::LevelLoadComplete, 0);
    SendLevelMessage(hostile, plNetworkMessageIDs::LevelLoadComplete, uiGeneration + 1);
    PumpHostile(host, hostile);
    PL_TEST_BOOL(pLevels->GetState() == plNetworkLevelState::Loading);

    SendLevelMessage(hostile, plNetworkMessageIDs::LevelLoadComplete, uiGeneration);
    PumpHostile(host, hostile);
    PL_TEST_BOOL(pLevels->GetState() == plNetworkLevelState::Ready);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B04: clients reject path-based spawns and servers never emit them")
  {
    NetworkEndpoint endpoint;
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    plNetworkObjectManager* pObjects = endpoint.m_pModule->GetObjectManager();

    plNetworkTestAccess::Role(endpoint.m_pModule, plNetworkRoleEnum::Client);
    plUInt8 bytes[128] = {};
    plNetworkMessage msg(bytes, 0, sizeof(bytes), 0, 0);
    msg.WriteUInt32(5);
    msg.WriteUInt32(1);
    msg.WriteUInt8(plNetworkObjectRole::OwnedAuthoritative);
    msg.WriteUInt32(0); // Unregistered prefab ID selects the free-form path.
    msg.WriteString("Prefabs/Arbitrary.plPrefab");
    msg.m_uiPosition = 0;
    pObjects->HandleSpawnMessage(msg);
    PL_TEST_BOOL(msg.HasReadError());
    PL_TEST_INT(pObjects->GetAllObjects().GetCount(), 0);

    plNetworkTestAccess::Role(endpoint.m_pModule, plNetworkRoleEnum::Host);
    plHashedString sUnregistered;
    sUnregistered.Assign("Prefabs/Unregistered.plPrefab");
    PL_TEST_INT(pObjects->SpawnNetworkObject(sUnregistered, plTransform::MakeIdentity(), 1), 0);
    PL_TEST_INT(pObjects->GetAllObjects().GetCount(), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B14: ENet reassembly is bounded by the declared message budget")
  {
    NetworkEndpoint host, client;
    if (!PL_TEST_BOOL(host.Start(true)))
      return;
    const plUInt16 port = plNetworkTestAccess::Host(host.m_pModule->GetPeer())->address.port;
    if (!PL_TEST_BOOL(client.Start(false, port)))
      return;

    const plNetworkConfig& config = host.m_pModule->GetPeer()->m_Config;
    const ENetHost* pServerHost = plNetworkTestAccess::Host(host.m_pModule->GetPeer());
    const ENetHost* pClientHost = plNetworkTestAccess::Host(client.m_pModule->GetPeer());
    const size_t uiBudget = static_cast<size_t>(config.m_uiMessageSize) * plMath::Min<plUInt16>(config.m_uiMessagePoolSize, 64);
    PL_TEST_BOOL(pServerHost->maximumPacketSize == config.m_uiMessageSize);
    PL_TEST_BOOL(pClientHost->maximumPacketSize == config.m_uiMessageSize);
    PL_TEST_BOOL(pServerHost->maximumWaitingData == uiBudget);
    PL_TEST_BOOL(pClientHost->maximumWaitingData == uiBudget);

    AdmissionObserver observer;
    observer.m_pModule = host.m_pModule;
    host.m_pModule->RegisterNetworkObject(&observer);
    HostileClient hostile(port, 0);
    PumpHostile(host, hostile);
    hostile.Handshake();
    PumpHostile(host, hostile);

    plDynamicArray<plUInt8> oversized;
    oversized.SetCount(config.m_uiMessageSize * 4u);
    plNetworkMessage message(oversized.GetData(), 0, oversized.GetCount(), oversized.GetCount(), 0);
    oversized[0] = static_cast<plUInt8>(plNetworkMessageIDs::UserMessageStart);
    hostile.Send(message);
    PumpHostile(host, hostile);
    PL_TEST_INT(observer.m_uiMessages, 0);
    PL_TEST_BOOL(host.m_pModule->GetPeer() != nullptr);
    host.m_pModule->RemoveNetworkObject(&observer);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B12/13: platform transport ignores stray data and enforces the admission limit")
  {
    NetworkEndpoint host;
    if (!PL_TEST_BOOL(host.Start(true)))
      return;
    plNetworkPeer* pPeer = host.m_pModule->GetPeer();

    FakeTransport transport;
    plDriverPlatform driver;
    plNetworkTestAccess::Platform(driver, &transport, pPeer);
    const plPlatformUserId userA = plPlatformUserId::Make("fake", "a");
    const plPlatformUserId userB = plPlatformUserId::Make("fake", "b");
    plNetworkEvent event;

    const plUInt32 uiFreeBefore = CountFreeMessages(pPeer);
    for (plUInt32 i = 0; i < 100; ++i)
      transport.Push(plPlatformTransportEvent::Type::Data, userA, 16);
    PL_TEST_BOOL(driver.PopEvent(event).Failed());
    PL_TEST_INT(CountFreeMessages(pPeer), uiFreeBefore);

    transport.Push(plPlatformTransportEvent::Type::Connected, plPlatformUserId());
    transport.Push(plPlatformTransportEvent::Type::Connected, userA);
    transport.Push(plPlatformTransportEvent::Type::Connected, userA);
    transport.Push(plPlatformTransportEvent::Type::Connected, userB);
    if (!PL_TEST_BOOL(driver.PopEvent(event).Succeeded()))
      return;
    PL_TEST_BOOL(event.m_EventType == plNetworkEventTypeEnum::Connected);
    PL_TEST_BOOL(event.m_Sender.m_uiConnectionID != 0);

    PL_TEST_BOOL(driver.PopEvent(event).Failed()); // Duplicate and over-limit connects produce no event.
    PL_TEST_INT(transport.m_uiDisconnects, 2);     // The invalid user and the over-limit user were refused.
  }
}
