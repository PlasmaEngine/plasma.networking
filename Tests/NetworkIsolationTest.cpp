#include "NetworkTestPCH.h"

#include <Foundation/Threading/AtomicInteger.h>
#include <NetworkPlugin/Core/NetworkPeer.h>
#include <TestFramework/Utilities/TestLogInterface.h>

#include "NetworkTestHarness.h"

#include <thread>

using namespace NetworkTestHarness;

namespace
{
  /// \brief Creates a raw peer and makes it listen on the port, the way a host does, but without the world module's
  /// address rewriting, so the test controls exactly which address is bound.
  bool ListenRaw(plStringView sAddress, plUInt16 uiPort)
  {
    plNetworkConfig config;
    config.m_sServerAddress = sAddress;
    config.m_uiServerPort = uiPort;
    config.m_uiMessagePoolSize = 64;

    plNetworkPeer* pPeer = plNetworkPeer::CreatePeer(config);
    if (pPeer == nullptr)
      return false;

    const bool bListening = pPeer->Listen().Succeeded();
    plNetworkPeer::DestroyPeer(pPeer);
    return bListening;
  }

  void Publish(NetworkEndpoint& host, plUInt32 uiValue)
  {
    host.m_pComponent->m_uiValue = uiValue;
    host.m_pComponent->MarkDirty();
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, TwoSessions)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B46: two hosts with two clients each share nothing")
  {
    const plUInt32 uiPeersBefore = plNetworkPeer::GetPeerCount();
    {
      NetworkEndpoint hostA, a1, a2, hostB, b1, b2;
      if (!PL_TEST_BOOL(hostA.Start(true) && hostB.Start(true)))
        return;

      const plUInt16 uiPortA = hostA.GetPort();
      const plUInt16 uiPortB = hostB.GetPort();
      PL_TEST_BOOL(uiPortA != uiPortB);
      if (!PL_TEST_BOOL(a1.Start(false, uiPortA) && a2.Start(false, uiPortA) && b1.Start(false, uiPortB) && b2.Start(false, uiPortB)))
        return;

      // Every endpoint stays serviced in every wait, so neither host times the other session's clients out.
      const Pumped all({&hostA, &a1, &a2, &hostB, &b1, &b2});
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return a1.m_pModule->IsConnected() && a2.m_pModule->IsConnected() && b1.m_pModule->IsConnected() && b2.m_pModule->IsConnected(); })))
        return;

      PL_TEST_INT(hostA.m_pModule->GetClients().GetCount(), 2);
      PL_TEST_INT(hostB.m_pModule->GetClients().GetCount(), 2);
      PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore + 6);

      // Both sessions replicate the same object (same deterministic network ID) with different values.
      Publish(hostA, 73);
      Publish(hostB, 91);
      PL_TEST_BOOL(PumpAll(all, [&] { return a1.m_pComponent->m_uiValue == 73 && a2.m_pComponent->m_uiValue == 73 && b1.m_pComponent->m_uiValue == 91 && b2.m_pComponent->m_uiValue == 91; }));

      // Stopping host A ends only session A.
      hostA.Stop();
      PL_TEST_BOOL(PumpAll(all, [&] { return !a1.m_pModule->IsConnected() && !a2.m_pModule->IsConnected(); }));
      PL_TEST_BOOL(b1.m_pModule->IsConnected() && b2.m_pModule->IsConnected());
      PL_TEST_INT(hostB.m_pModule->GetClients().GetCount(), 2);

      Publish(hostB, 92);
      PL_TEST_BOOL(PumpAll(all, [&] { return b1.m_pComponent->m_uiValue == 92 && b2.m_pComponent->m_uiValue == 92; }));
      PL_TEST_INT(a1.m_pComponent->m_uiValue, 73);
      PL_TEST_INT(a2.m_pComponent->m_uiValue, 73);
    }
    PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore);
  }
}

PL_CREATE_SIMPLE_TEST(Network, SamePortRefused)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B46: a second host cannot bind a port a host holds")
  {
    const plUInt32 uiPeersBefore = plNetworkPeer::GetPeerCount();
    {
      NetworkEndpoint host, client;
      if (!PL_TEST_BOOL(host.Start(true)))
        return;
      const plUInt16 uiPort = host.GetPort();
      if (!PL_TEST_BOOL(client.Start(false, uiPort)))
        return;

      const Pumped all({&host, &client});
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return client.m_pModule->IsConnected(); })))
        return;

      {
        plTestLogInterface log;
        plTestLogSystemScope logScope(&log);
        log.ExpectMessage("Failed to create host on port", plLogMsgType::ErrorMsg, 2);

        // The host binds every interface. Neither the same wildcard nor the loopback address may be bound next to it:
        // either would split the port's traffic between two hosts.
        PL_TEST_BOOL(!ListenRaw("any", uiPort));
        PL_TEST_BOOL(!ListenRaw("127.0.0.1", uiPort));
      }

      // A world module host on the same port fails to start and leaves no peer behind.
      {
        plTestLogInterface log;
        plTestLogSystemScope logScope(&log);
        log.ExpectMessage("Failed to create host on port", plLogMsgType::ErrorMsg);

        NetworkEndpoint second;
        PL_TEST_BOOL(!second.Start(true, uiPort));
        PL_TEST_BOOL(second.m_pModule->GetPeer() == nullptr);
      }

      PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore + 2);
      PL_TEST_BOOL(client.m_pModule->IsConnected());
      Publish(host, 41);
      PL_TEST_BOOL(PumpAll(all, [&] { return client.m_pComponent->m_uiValue == 41; }));
    }
    PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B46: a wildcard host cannot bind a port a loopback host holds")
  {
    plNetworkConfig config;
    config.m_sServerAddress = "127.0.0.1";
    config.m_uiServerPort = 0;
    config.m_uiMessagePoolSize = 64;

    plNetworkPeer* pFirst = plNetworkPeer::CreatePeer(config);
    if (!PL_TEST_BOOL(pFirst != nullptr && pFirst->Listen().Succeeded()))
    {
      plNetworkPeer::DestroyPeer(pFirst);
      return;
    }

    const plUInt16 uiPort = plNetworkTestAccess::Host(pFirst)->address.port;
    {
      plTestLogInterface log;
      plTestLogSystemScope logScope(&log);
      log.ExpectMessage("Failed to create host on port", plLogMsgType::ErrorMsg, 2);
      PL_TEST_BOOL(!ListenRaw("any", uiPort));
      PL_TEST_BOOL(!ListenRaw("127.0.0.1", uiPort));
    }

    plNetworkPeer::DestroyPeer(pFirst);

    // Once the first host is gone the port is free again.
    PL_TEST_BOOL(ListenRaw("127.0.0.1", uiPort));
  }
}

PL_CREATE_SIMPLE_TEST(Network, StartStopChurn)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B46: a client restarting hundreds of times never disturbs its neighbour")
  {
    // More than 255 cycles, so nothing that counts peers in a byte can wrap without this test noticing.
    constexpr plUInt32 uiCycles = 260;

    const plUInt32 uiPeersBefore = plNetworkPeer::GetPeerCount();
    {
      NetworkEndpoint host, steady, churn;
      if (!PL_TEST_BOOL(host.Start(true)))
        return;
      const plUInt16 uiPort = host.GetPort();
      if (!PL_TEST_BOOL(steady.Start(false, uiPort)))
        return;

      const Pumped all({&host, &steady, &churn});
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return steady.m_pModule->IsConnected(); })))
        return;

      plUInt32 uiCompleted = 0;
      for (; uiCompleted < uiCycles; ++uiCompleted)
      {
        if (!churn.Start(false, uiPort))
          break;
        if (!PumpAll(all, [&] { return churn.m_pModule->IsConnected() && host.m_pModule->GetClients().GetCount() == 2; }))
          break;

        churn.Stop();
        if (!PumpAll(all, [&] { return churn.m_pModule->GetPeer() == nullptr && host.m_pModule->GetClients().GetCount() == 1; }))
          break;

        if (!steady.m_pModule->IsConnected())
          break;
      }

      PL_TEST_INT(uiCompleted, uiCycles);
      PL_TEST_BOOL(steady.m_pModule->IsConnected());
      PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore + 2);

      Publish(host, 260);
      PL_TEST_BOOL(PumpAll(all, [&] { return steady.m_pComponent->m_uiValue == 260; }));
    }
    PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore);
  }
}

PL_CREATE_SIMPLE_TEST(Network, ParallelPump)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "NW-B46: a host and three clients updated on four threads")
  {
    const plUInt32 uiPeersBefore = plNetworkPeer::GetPeerCount();
    {
      NetworkEndpoint host, a, b, c;
      if (!PL_TEST_BOOL(host.Start(true)))
        return;
      const plUInt16 uiPort = host.GetPort();
      if (!PL_TEST_BOOL(a.Start(false, uiPort) && b.Start(false, uiPort) && c.Start(false, uiPort)))
        return;

      if (!PL_TEST_BOOL(PumpAll(Pumped({&host, &a, &b, &c}), [&] { return a.m_pModule->IsConnected() && b.m_pModule->IsConnected() && c.m_pModule->IsConnected(); })))
        return;

      // Each world is only ever touched by its own thread while the threads run. The host publishes a new value on every
      // update, so replication traffic flows the whole time.
      const plTime end = plTime::Now() + plTime::MakeFromSeconds(2);
      plAtomicInteger32 iDisconnects;
      plAtomicInteger32 iHostUpdates;

      auto run = [&](NetworkEndpoint* pEndpoint, bool bHost)
      {
        plUInt32 uiValue = 1000;
        while (plTime::Now() < end)
        {
          {
            PL_LOCK(pEndpoint->m_World.GetWriteMarker());
            if (bHost)
            {
              pEndpoint->m_pComponent->m_uiValue = ++uiValue;
              pEndpoint->m_pComponent->MarkDirty();
              iHostUpdates.Increment();
            }

            pEndpoint->m_World.Update();

            if (!pEndpoint->m_pModule->IsConnected())
              iDisconnects.Increment();
          }
          plThreadUtils::YieldTimeSlice();
        }
      };

      std::thread threads[] = {
        std::thread(run, &host, true),
        std::thread(run, &a, false),
        std::thread(run, &b, false),
        std::thread(run, &c, false),
      };
      for (std::thread& thread : threads)
        thread.join();

      PL_TEST_INT(iDisconnects, 0);
      PL_TEST_BOOL(iHostUpdates > 10);
      PL_TEST_INT(host.m_pModule->GetClients().GetCount(), 3);

      // Back on one thread, every client converges on the host's final value.
      Publish(host, 5000);
      PL_TEST_BOOL(PumpAll(Pumped({&host, &a, &b, &c}), [&] { return a.m_pComponent->m_uiValue == 5000 && b.m_pComponent->m_uiValue == 5000 && c.m_pComponent->m_uiValue == 5000; }));
      PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore + 4);
    }
    PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore);
  }
}
