#include "NetworkTestPCH.h"

#include <Foundation/Math/Random.h>
#include <NetworkPlugin/Core/NetworkVariant.h>

#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

namespace
{
  // Fixed so a failing run can be replayed exactly.
  constexpr plUInt64 s_uiFuzzSeed = 0x4E57463031;

  constexpr plUInt32 s_uiPayloadShapes = 4;

  /// \brief Fills the payload with one of a few shapes: empty, random, all 0xFF (maximal lengths and counts), all zero.
  void MakePayload(plRandom& ref_rng, plUInt32 uiShape, plDynamicArray<plUInt8>& out_payload)
  {
    out_payload.Clear();
    if (uiShape == 0)
      return;

    const plUInt32 uiLength = 1 + ref_rng.UIntInRange(uiShape == 1 ? 192 : 64);
    out_payload.SetCountUninitialized(uiLength);
    for (plUInt8& value : out_payload)
    {
      value = (uiShape == 1) ? static_cast<plUInt8>(ref_rng.UInt()) : (uiShape == 2 ? 0xFF : 0x00);
    }
  }

  void DisconnectHostile(NetworkEndpoint& host, HostileClient& hostile)
  {
    hostile.Disconnect();
    PumpHostile(host, hostile, 5);
  }

  /// \brief Sends every message ID with every payload shape. A client that is kicked on the way is replaced by a new one.
  void SweepMessageIDs(NetworkEndpoint& host, plRandom& ref_rng, bool bHandshake)
  {
    plDynamicArray<plUInt8> payload;
    plUInt32 uiID = 1;

    while (uiID < 256)
    {
      HostileClient hostile(host.GetPort(), 0);
      PumpHostile(host, hostile, 20);
      if (!PL_TEST_BOOL(hostile.m_bConnected))
        return;

      if (bHandshake)
      {
        hostile.Handshake();
        PumpHostile(host, hostile, 10);
      }

      while (uiID < 256 && !hostile.m_bDisconnected)
      {
        for (plUInt32 uiShape = 0; uiShape < s_uiPayloadShapes; ++uiShape)
        {
          MakePayload(ref_rng, uiShape, payload);
          hostile.SendWithID(static_cast<plUInt8>(uiID), payload.GetData(), payload.GetCount());
        }

        PumpHostile(host, hostile, 2);
        ++uiID;
      }

      DisconnectHostile(host, hostile);
    }
  }

  /// \brief Builds an RPC message with a valid header followed by an arbitrary type byte and tail.
  plUInt32 BuildRpc(plUInt8* pBuffer, plUInt32 uiCapacity, plUInt32 uiRpcID, plUInt32 uiComponentID, plUInt8 uiCount, plUInt8 uiType, plUInt32 uiTail)
  {
    plNetworkMessage msg(pBuffer, 0, uiCapacity, 0, 0);
    msg.WriteUInt32(uiRpcID);
    msg.WriteUInt32(uiComponentID);
    msg.WriteUInt8(uiCount);
    msg.WriteUInt8(uiType);
    msg.WriteUInt32(uiTail);
    return msg.m_uiLength;
  }

  void RunParsers(plRandom& ref_rng, NetworkEndpoint& endpoint, plUInt32 uiRpcID, plUInt32 uiIterations)
  {
    plNetworkWorldModule* pModule = endpoint.m_pModule;

    // Rejecting hostile input is logged as an error; the test framework would count each one as a failure.
    plMuteLog mute;
    plLogSystemScope logScope(&mute);

    for (plUInt32 i = 0; i < uiIterations; ++i)
    {
      plUInt8 bytes[96] = {};
      const plUInt32 uiLength = ref_rng.UIntInRange(sizeof(bytes) + 1);
      for (plUInt32 b = 0; b < uiLength; ++b)
        bytes[b] = static_cast<plUInt8>(ref_rng.UInt());

      // Every other iteration starts with a header that selects the registered RPC and the real component.
      if (i % 2 == 0 && uiLength >= 8)
      {
        plMemoryUtils::Copy(bytes, reinterpret_cast<const plUInt8*>(&uiRpcID), 4);
        const plUInt32 uiComponentID = endpoint.m_pComponent->GetNetID();
        plMemoryUtils::Copy(bytes + 4, reinterpret_cast<const plUInt8*>(&uiComponentID), 4);
      }

      {
        plNetworkMessage msg(bytes, 0, sizeof(bytes), uiLength, 0);
        pModule->GetRpcManager()->HandleRpcMessage(msg, 0);
      }
      {
        plNetworkMessage msg(bytes, 0, sizeof(bytes), uiLength, 0);
        pModule->GetObjectManager()->HandleOwnershipMessage(msg);
      }
      {
        plNetworkMessage msg(bytes, 0, sizeof(bytes), uiLength, 0);
        pModule->GetObjectManager()->HandleDespawnMessage(msg);
      }
      {
        plNetworkMessage msg(bytes, 0, sizeof(bytes), uiLength, 0);
        pModule->GetObjectManager()->HandleSpawnMessage(msg);
      }
      {
        plNetworkMessage msg(bytes, 0, sizeof(bytes), uiLength, 0);
        plVariant value;
        plReadNetworkVariant(msg, value);
        PL_TEST_BOOL(msg.m_uiPosition <= msg.m_uiLength);
      }
      {
        plNetworkMessage msg(bytes, 0, sizeof(bytes), uiLength, 0);
        msg.ReadString();
        PL_TEST_BOOL(msg.m_uiPosition <= msg.m_uiLength);
      }
    }
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, HostileMessageIDs)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-F01: every message ID and payload shape from an unadmitted client")
  {
    NetworkEndpoint host, client;
    if (!PL_TEST_BOOL(host.Start(true)))
      return;
    if (!PL_TEST_BOOL(client.Start(false, host.GetPort())))
      return;
    PL_TEST_BOOL(PumpUntil(host, &client, nullptr, [&] { return client.m_pModule->IsConnected(); }));

    plRandom rng;
    rng.Initialize(s_uiFuzzSeed);
    SweepMessageIDs(host, rng, false);

    // The well-behaved client was not disturbed and replication still works.
    PL_TEST_BOOL(host.m_pModule->GetPeer() != nullptr);
    PL_TEST_BOOL(client.m_pModule->IsConnected());
    host.m_pComponent->m_uiValue = 91;
    host.m_pComponent->MarkDirty();
    PL_TEST_BOOL(PumpUntil(host, &client, nullptr, [&] { return client.m_pComponent->m_uiValue == 91; }));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-F01: every message ID and payload shape from an admitted client")
  {
    NetworkEndpoint host, client;
    if (!PL_TEST_BOOL(host.Start(true)))
      return;
    if (!PL_TEST_BOOL(client.Start(false, host.GetPort())))
      return;
    PL_TEST_BOOL(PumpUntil(host, &client, nullptr, [&] { return client.m_pModule->IsConnected(); }));

    plRandom rng;
    rng.Initialize(s_uiFuzzSeed + 1);
    SweepMessageIDs(host, rng, true);

    PL_TEST_BOOL(host.m_pModule->GetPeer() != nullptr);
    PL_TEST_BOOL(client.m_pModule->IsConnected());
    host.m_pComponent->m_uiValue = 92;
    host.m_pComponent->MarkDirty();
    PL_TEST_BOOL(PumpUntil(host, &client, nullptr, [&] { return client.m_pComponent->m_uiValue == 92; }));
  }
}

PL_CREATE_SIMPLE_TEST(Network, ParserFuzz)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-F01: random buffers into the host-side parsers")
  {
    NetworkEndpoint endpoint;
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    auto* pModule = endpoint.m_pModule;
    plNetworkTestAccess::Role(pModule, plNetworkRoleEnum::Host);
    plNetworkTestAccess::Own(pModule, endpoint.m_pComponent->GetOwner(), 99, 1);
    pModule->GetRpcManager()->RegisterRpc(endpoint.m_pComponent->GetDynamicRTTI(), "SetValue", plNetworkRpcTarget::Server);
    const auto* pRpc = pModule->GetRpcManager()->GetRpcInfo(endpoint.m_pComponent->GetDynamicRTTI(), "SetValue");
    if (!PL_TEST_BOOL(pRpc != nullptr))
      return;

    plRandom rng;
    rng.Initialize(s_uiFuzzSeed + 2);
    RunParsers(rng, endpoint, pRpc->m_uiRpcID, 3000);

    // Host-side ownership and lifetime commands from a peer change nothing.
    PL_TEST_INT(pModule->GetObjectManager()->GetOwner(99), 1);
    PL_TEST_BOOL(pModule->GetObjectManager()->GetObjectInfo(99) != nullptr);
    PL_TEST_INT(pModule->GetObjectManager()->GetAllObjects().GetCount(), 1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-F01: random buffers into the client-side parsers")
  {
    NetworkEndpoint endpoint;
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    plNetworkTestAccess::Role(endpoint.m_pModule, plNetworkRoleEnum::Client);

    plRandom rng;
    rng.Initialize(s_uiFuzzSeed + 3);
    RunParsers(rng, endpoint, 0, 3000);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-F01: every variant type byte in an RPC with the right header")
  {
    NetworkEndpoint endpoint;
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    auto* pModule = endpoint.m_pModule;
    plNetworkTestAccess::Role(pModule, plNetworkRoleEnum::Host);
    pModule->GetRpcManager()->RegisterRpc(endpoint.m_pComponent->GetDynamicRTTI(), "SetValue", plNetworkRpcTarget::Server);
    const auto* pRpc = pModule->GetRpcManager()->GetRpcInfo(endpoint.m_pComponent->GetDynamicRTTI(), "SetValue");
    if (!PL_TEST_BOOL(pRpc != nullptr))
      return;

    plMuteLog mute;
    plLogSystemScope logScope(&mute);

    for (plUInt32 uiType = 0; uiType < 256; ++uiType)
    {
      for (plUInt8 uiCount : {plUInt8(0), plUInt8(1), plUInt8(2), plUInt8(255)})
      {
        plUInt8 bytes[64] = {};
        const plUInt32 uiLength = BuildRpc(bytes, sizeof(bytes), pRpc->m_uiRpcID, endpoint.m_pComponent->GetNetID(), uiCount, static_cast<plUInt8>(uiType), 0xFFFFFFFF);
        plNetworkMessage msg(bytes, 0, sizeof(bytes), uiLength, 0);
        pModule->GetRpcManager()->HandleRpcMessage(msg, 0);
        PL_TEST_BOOL(msg.m_uiPosition <= msg.m_uiLength);
      }
    }
  }
}
