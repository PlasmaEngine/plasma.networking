#include "NetworkTestPCH.h"

#include <NetworkPlugin/Components/NetworkedEventComponent.h>

#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

namespace
{
  using Role = NetworkEndpoint::Role;

  struct RpcPrefabs
  {
    TestPrefabs m_Library;
    plHashedString m_sProbe; ///< An RPC probe and a twin type with an RPC of the same name.
    plHashedString m_sEvent; ///< A networked event component.

    RpcPrefabs()
    {
      m_sProbe = m_Library.Add("Probe", [](plWorld& ref_world, plGameObject* pRoot)
        {
          AddComponent<plNetworkRpcProbeComponent>(ref_world, pRoot);
          AddComponent<plNetworkRpcTwinComponent>(ref_world, pRoot);
        });

      m_sEvent = m_Library.Add("Event", [](plWorld& ref_world, plGameObject* pRoot)
        { AddComponent<plNetworkedEventComponent>(ref_world, pRoot); });
    }

    void Register(std::initializer_list<NetworkEndpoint*> endpoints)
    {
      for (NetworkEndpoint* pEndpoint : endpoints)
      {
        pEndpoint->RegisterPrefab(m_sProbe);
        pEndpoint->RegisterPrefab(m_sEvent);
      }
    }
  };

  /// \brief Server S and clients A and B with a probe owned by A, spawned and started everywhere.
  struct RpcSession
  {
    RpcPrefabs m_Prefabs;
    NetworkEndpoint m_Server, m_A, m_B;
    Pumped m_All{{&m_Server, &m_A, &m_B}};
    plUInt32 m_uiObject = 0;

    bool Start(Role role, const plHashedString& sPrefab)
    {
      m_Prefabs.Register({&m_Server, &m_A, &m_B});
      if (!ConnectAllAs(role, m_All, m_Server, {&m_A, &m_B}))
        return false;

      m_uiObject = m_Server.Spawn(sPrefab, m_A.m_pModule->GetLocalClientID());
      if (m_uiObject == 0)
        return false;

      // The spawned components start on the next update of each world; give every peer a few.
      if (!PumpAll(m_All, [&] { return m_A.FindSpawned(m_uiObject) && m_B.FindSpawned(m_uiObject); }))
        return false;
      PumpFor(m_All, plTime::MakeFromMilliseconds(100));
      return true;
    }

    plNetworkRpcProbeComponent* Probe(NetworkEndpoint& endpoint) { return endpoint.FindSpawnedComponent<plNetworkRpcProbeComponent>(m_uiObject); }
    plNetworkRpcTwinComponent* Twin(NetworkEndpoint& endpoint) { return endpoint.FindSpawnedComponent<plNetworkRpcTwinComponent>(m_uiObject); }

    void Reset()
    {
      for (NetworkEndpoint* pEndpoint : {&m_Server, &m_A, &m_B})
      {
        PL_LOCK(pEndpoint->m_World.GetWriteMarker());
        if (plNetworkRpcProbeComponent* pProbe = Probe(*pEndpoint))
        {
          pProbe->m_uiServer = pProbe->m_uiOwner = pProbe->m_uiAllClients = pProbe->m_uiAll = pProbe->m_uiMulticast = 0;
          pProbe->m_Arguments.Clear();
        }
      }
    }
  };

  plUInt32 Count(plNetworkRpcProbeComponent* pProbe, plNetworkRpcTarget::Enum target)
  {
    if (pProbe == nullptr)
      return 1000;

    switch (target)
    {
      case plNetworkRpcTarget::Server:
        return pProbe->m_uiServer;
      case plNetworkRpcTarget::Owner:
        return pProbe->m_uiOwner;
      case plNetworkRpcTarget::AllClients:
        return pProbe->m_uiAllClients;
      case plNetworkRpcTarget::AllClientsAndServer:
        return pProbe->m_uiAll;
      default:
        return pProbe->m_uiMulticast;
    }
  }

  const char* FunctionName(plNetworkRpcTarget::Enum target)
  {
    switch (target)
    {
      case plNetworkRpcTarget::Server:
        return "RpcServer";
      case plNetworkRpcTarget::Owner:
        return "RpcOwner";
      case plNetworkRpcTarget::AllClients:
        return "RpcAllClients";
      case plNetworkRpcTarget::AllClientsAndServer:
        return "RpcAll";
      default:
        return "RpcMulticast";
    }
  }

  /// \brief How often one call runs on S, A and B. The defined routing for every target and sender, with B the only
  /// client a multicast names:
  ///
  ///   target              | from the server              | from the owning client A
  ///   Server              | S                            | S
  ///   Owner               | A                            | A, locally
  ///   AllClients          | A, B, and S when it is a host | A locally, B, and S when it is a host
  ///   AllClientsAndServer | S, A, B                      | A locally, S, B
  ///   Multicast [B]       | B                            | rejected: CallRpc returns false
  struct Expected
  {
    plUInt32 m_uiServer, m_uiA, m_uiB;
    bool m_bAccepted = true;
  };

  Expected ExpectedRouting(Role role, bool bFromServer, plNetworkRpcTarget::Enum target)
  {
    const plUInt32 uiHost = role == Role::Host ? 1 : 0;
    switch (target)
    {
      case plNetworkRpcTarget::Server:
        return {1, 0, 0};
      case plNetworkRpcTarget::Owner:
        return {0, 1, 0};
      case plNetworkRpcTarget::AllClients:
        return {uiHost, 1, 1};
      case plNetworkRpcTarget::AllClientsAndServer:
        return {1, 1, 1};
      default:
        return bFromServer ? Expected{0, 0, 1} : Expected{0, 0, 0, false};
    }
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, RpcRouting)
{
  for (Role role : {Role::Host, Role::Server})
  {
    PL_TEST_BLOCK(plTestBlock::Enabled, "D08: every RPC target from the server and from the owning client")
    {
      RpcSession session;
      if (!PL_TEST_BOOL(session.Start(role, session.m_Prefabs.m_sProbe)))
        return;

      const plUInt32 uiB = session.m_B.m_pModule->GetLocalClientID();
      for (bool bFromServer : {true, false})
      {
        NetworkEndpoint& sender = bFromServer ? session.m_Server : session.m_A;
        for (plNetworkRpcTarget::Enum target : {plNetworkRpcTarget::Server, plNetworkRpcTarget::Owner, plNetworkRpcTarget::AllClients,
               plNetworkRpcTarget::AllClientsAndServer, plNetworkRpcTarget::Multicast})
        {
          session.Reset();
          const Expected expected = ExpectedRouting(role, bFromServer, target);

          bool bAccepted = false;
          {
            PL_LOCK(sender.m_World.GetWriteMarker());
            plVariant argument(plInt32(7));
            plDynamicArray<plUInt32> targets;
            targets.PushBack(uiB);
            bAccepted = sender.m_pModule->GetRpcManager()->CallRpc(session.Probe(sender), FunctionName(target), plArrayPtr<plVariant>(&argument, 1), targets);
          }

          auto Matches = [&]
          {
            return Count(session.Probe(session.m_Server), target) == expected.m_uiServer && Count(session.Probe(session.m_A), target) == expected.m_uiA &&
                   Count(session.Probe(session.m_B), target) == expected.m_uiB;
          };
          PumpAll(session.m_All, Matches, plTime::MakeFromSeconds(1));
          PumpFor(session.m_All, plTime::MakeFromMilliseconds(100)); // a call that runs twice shows up late

          PL_TEST_BOOL_MSG(Matches() && bAccepted == expected.m_bAccepted,
            "%s, %s sends %s: expected S=%u A=%u B=%u (accepted %d), got S=%u A=%u B=%u (accepted %d)", role == Role::Host ? "host" : "dedicated server",
            bFromServer ? "the server" : "client A", FunctionName(target), expected.m_uiServer, expected.m_uiA, expected.m_uiB, expected.m_bAccepted,
            Count(session.Probe(session.m_Server), target), Count(session.Probe(session.m_A), target), Count(session.Probe(session.m_B), target), bAccepted);
        }
      }
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "D08: an event fired from a client reaches every peer once")
    {
      RpcSession session;
      if (!PL_TEST_BOOL(session.Start(role, session.m_Prefabs.m_sEvent)))
        return;

      plUInt32 calls[3] = {};
      NetworkEndpoint* endpoints[3] = {&session.m_Server, &session.m_A, &session.m_B};
      for (plUInt32 i = 0; i < 3; ++i)
      {
        PL_LOCK(endpoints[i]->m_World.GetWriteMarker());
        plNetworkedEventComponent* pEvent = endpoints[i]->FindSpawnedComponent<plNetworkedEventComponent>(session.m_uiObject);
        if (!PL_TEST_BOOL(pEvent != nullptr))
          return;
        plUInt32* pCalls = &calls[i];
        pEvent->SetEventReceivedCallback([pCalls](const plString&, const plVariant& param, const plVariant&)
          {
            if (param.IsA<plInt32>() && param.Get<plInt32>() == 3)
              ++(*pCalls);
          });
      }

      {
        PL_LOCK(session.m_A.m_World.GetWriteMarker());
        session.m_A.FindSpawnedComponent<plNetworkedEventComponent>(session.m_uiObject)->FireEventInt("Hit", 3);
      }

      PumpAll(session.m_All, [&] { return calls[0] == 1 && calls[1] == 1 && calls[2] == 1; });
      PumpFor(session.m_All, plTime::MakeFromMilliseconds(100));
      PL_TEST_BOOL_MSG(calls[0] == 1 && calls[1] == 1 && calls[2] == 1, "%s: server %u, A %u, B %u", role == Role::Host ? "host" : "dedicated server", calls[0], calls[1], calls[2]);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D32: RPCs of the same name on two component types stay apart")
  {
    RpcSession session;
    if (!PL_TEST_BOOL(session.Start(Role::Host, session.m_Prefabs.m_sProbe)))
      return;

    {
      PL_LOCK(session.m_A.m_World.GetWriteMarker());
      PL_TEST_BOOL(session.m_A.m_pModule->GetRpcManager()->CallRpc(session.Twin(session.m_A), "RpcServer", 1));
      PL_TEST_BOOL(session.m_A.m_pModule->GetRpcManager()->CallRpc(session.Probe(session.m_A), "RpcServer", 2));
    }

    PumpAll(session.m_All, [&] { return session.Twin(session.m_Server)->m_uiServer == 1 && session.Probe(session.m_Server)->m_uiServer == 1; });
    PL_TEST_INT(session.Twin(session.m_Server)->m_uiServer, 1);
    PL_TEST_INT(session.Probe(session.m_Server)->m_uiServer, 1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D08: an argument of the wrong type drops the call and keeps the receiver connected")
  {
    RpcSession session;
    if (!PL_TEST_BOOL(session.Start(Role::Host, session.m_Prefabs.m_sProbe)))
      return;

    // A well-formed call whose argument is a float where the function takes an int, as an older or newer build would send.
    const plNetworkRpcInfo* pRpc = session.m_A.m_pModule->GetRpcManager()->GetRpcInfo(plGetStaticRTTI<plNetworkRpcProbeComponent>(), "RpcAllClients");
    if (!PL_TEST_BOOL(pRpc != nullptr))
      return;

    {
      PL_LOCK(session.m_Server.m_World.GetWriteMarker());
      plNetworkPeer* pPeer = session.m_Server.m_pModule->GetPeer();
      plNetworkMessage msg = pPeer->BeginSendMessage();
      msg.WriteUInt8(plNetworkMessageIDs::ObjectRpc);
      msg.WriteUInt32(pRpc->m_uiRpcID);
      msg.WriteUInt32(session.Probe(session.m_A)->GetNetID());
      msg.WriteUInt8(1);
      msg.WriteUInt8(plVariantType::Float);
      msg.WriteFloat(1.5f);
      msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
      pPeer->SendTo(msg, session.m_Server.FindClient(session.m_A)->m_Connection).IgnoreResult();
    }
    PumpFor(session.m_All, plTime::MakeFromMilliseconds(100));

    {
      PL_LOCK(session.m_Server.m_World.GetWriteMarker());
      session.m_Server.m_pModule->GetRpcManager()->CallRpc(session.Probe(session.m_Server), "RpcAllClients", 4);
    }

    PumpAll(session.m_All, [&] { return !session.m_A.m_pModule->IsConnected() || Count(session.Probe(session.m_A), plNetworkRpcTarget::AllClients) == 1; });
    PL_TEST_BOOL_MSG(session.m_A.m_pModule->IsConnected(), "The client disconnected itself over a mistyped RPC.");
    PL_TEST_INT(Count(session.Probe(session.m_A), plNetworkRpcTarget::AllClients), 1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "An argument that does not convert to the parameter type refuses the call; one that converts is sent converted")
  {
    RpcSession session;
    if (!PL_TEST_BOOL(session.Start(Role::Host, session.m_Prefabs.m_sProbe)))
      return;

    {
      PL_LOCK(session.m_Server.m_World.GetWriteMarker());
      plNetworkRpcManager* pRpc = session.m_Server.m_pModule->GetRpcManager();
      PL_TEST_BOOL(!pRpc->CallRpc(session.Probe(session.m_Server), "RpcServer", plVec3(1.0f)));
      PL_TEST_BOOL(pRpc->CallRpc(session.Probe(session.m_Server), "RpcAllClients", 2.0f));
    }

    PL_TEST_INT(session.Probe(session.m_Server)->m_uiServer, 0);
    NETWORK_TEST_WAIT(PumpAll(session.m_All, [&] { return Count(session.Probe(session.m_A), plNetworkRpcTarget::AllClients) == 1; }), "A ran it %u times",
      Count(session.Probe(session.m_A), plNetworkRpcTarget::AllClients));
    PL_TEST_INT(session.Probe(session.m_A)->m_Arguments.IsEmpty() ? -1 : session.Probe(session.m_A)->m_Arguments[0], 2);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D12: reliable RPCs queued in one frame arrive in order")
  {
    RpcSession session;
    if (!PL_TEST_BOOL(session.Start(Role::Host, session.m_Prefabs.m_sProbe)))
      return;

    // More than the 64-message pool of the test endpoints, queued within microseconds of each other.
    const plUInt32 uiCalls = 200;
    {
      PL_LOCK(session.m_Server.m_World.GetWriteMarker());
      for (plUInt32 i = 0; i < uiCalls; ++i)
        session.m_Server.m_pModule->GetRpcManager()->CallRpc(session.Probe(session.m_Server), "RpcAll", static_cast<plInt32>(i));
    }

    PumpAll(session.m_All, [&] { return session.Probe(session.m_A)->m_Arguments.GetCount() == uiCalls; });
    const auto& arguments = session.Probe(session.m_A)->m_Arguments;
    bool bInOrder = arguments.GetCount() == uiCalls;
    for (plUInt32 i = 0; bInOrder && i < arguments.GetCount(); ++i)
      bInOrder = arguments[i] == static_cast<plInt32>(i);
    PL_TEST_BOOL_MSG(bInOrder, "%u calls arrived; order broken", arguments.GetCount());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D13: a burst larger than the message pool arrives complete")
  {
    RpcSession session;
    if (!PL_TEST_BOOL(session.Start(Role::Host, session.m_Prefabs.m_sProbe)))
      return;

    {
      PL_LOCK(session.m_Server.m_World.GetWriteMarker());
      for (plInt32 i = 0; i < 200; ++i)
        session.m_Server.m_pModule->GetRpcManager()->CallRpc(session.Probe(session.m_Server), "RpcAllClients", i);
    }

    PumpAll(session.m_All, [&] { return session.Probe(session.m_A)->m_Arguments.GetCount() == 200; });
    PL_TEST_INT(session.Probe(session.m_A)->m_Arguments.GetCount(), 200);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D13: a receiver whose message pool is in use keeps incoming reliable messages")
  {
    RpcSession session;
    if (!PL_TEST_BOOL(session.Start(Role::Host, session.m_Prefabs.m_sProbe)))
      return;

    {
      PL_LOCK(session.m_Server.m_World.GetWriteMarker());
      for (plInt32 i = 0; i < 20; ++i)
        session.m_Server.m_pModule->GetRpcManager()->CallRpc(session.Probe(session.m_Server), "RpcAllClients", i);
    }
    session.m_Server.Pump(); // on the wire to A

    {
      // A queues as many messages as its pool holds before it next reads the network.
      PL_LOCK(session.m_A.m_World.GetWriteMarker());
      for (plInt32 i = 0; i < 64; ++i)
        session.m_A.m_pModule->GetRpcManager()->CallRpc(session.Probe(session.m_A), "RpcServer", i);
    }

    PumpAll(session.m_All, [&] { return session.Probe(session.m_A)->m_Arguments.GetCount() == 20; });
    PL_TEST_INT(session.Probe(session.m_A)->m_Arguments.GetCount(), 20);
    PL_TEST_BOOL(PumpAll(session.m_All, [&] { return session.Probe(session.m_Server)->m_uiServer == 64; }));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D13: RPCs that run only on the server do not hold message pool slots")
  {
    RpcSession session;
    if (!PL_TEST_BOOL(session.Start(Role::Host, session.m_Prefabs.m_sProbe)))
      return;

    const plUInt32 uiFree = plNetworkTestAccess::FreeMessageCount(session.m_Server.m_pModule->GetPeer());
    {
      PL_LOCK(session.m_Server.m_World.GetWriteMarker());
      for (plInt32 i = 0; i < 100; ++i)
        session.m_Server.m_pModule->GetRpcManager()->CallRpc(session.Probe(session.m_Server), "RpcServer", i);
    }
    PumpFor(session.m_All, plTime::MakeFromMilliseconds(50));

    PL_TEST_INT(session.Probe(session.m_Server)->m_uiServer, 100);
    PL_TEST_INT(plNetworkTestAccess::FreeMessageCount(session.m_Server.m_pModule->GetPeer()), uiFree);
  }
}
