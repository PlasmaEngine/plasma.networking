#include "NetworkTestPCH.h"

#include <Core/WorldSerializer/WorldReader.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <NetworkPlugin/Components/NetworkDebugComponent.h>
#include <NetworkPlugin/Components/NetworkPredictedComponent.h>
#include <NetworkPlugin/Components/NetworkedBlackboardComponent.h>
#include <NetworkPlugin/Components/NetworkedEventComponent.h>
#include <NetworkPlugin/Components/NetworkedRigidbodyComponent.h>

#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

namespace
{
  using Role = NetworkEndpoint::Role;

  plHashedString AddPawnPrefab(TestPrefabs& ref_prefabs)
  {
    return ref_prefabs.Add("LifecyclePawn", [](plWorld& ref_world, plGameObject* pRoot)
      {
        AddComponent<plNetworkReplicaTargetComponent>(ref_world, pRoot);
        ReplicateTarget(AddComponent<plNetworkedPropertiesComponent>(ref_world, pRoot), plReplicateAuthority::Server);
      });
  }

  /// \brief A value of the same type that differs from the given one, or an invalid variant for types this test skips.
  plVariant Different(const plAbstractMemberProperty* pProperty, const plVariant& current)
  {
    if (pProperty->GetFlags().IsAnySet(plPropertyFlags::IsEnum | plPropertyFlags::Bitflags))
    {
      plDynamicArray<plReflectionUtils::EnumKeyValuePair> values;
      plReflectionUtils::GetEnumKeysAndValues(pProperty->GetSpecificType(), values);
      for (const auto& value : values)
      {
        if (value.m_iValue != current.ConvertTo<plInt64>())
          return plVariant(plInt64(value.m_iValue));
      }
      return plVariant();
    }

    switch (current.GetType())
    {
      case plVariantType::Bool:
        return !current.Get<bool>();
      case plVariantType::Float:
        return current.Get<float>() + 1.25f;
      case plVariantType::Double:
        return current.Get<double>() + 1.25;
      case plVariantType::Int8:
      case plVariantType::UInt8:
      case plVariantType::Int16:
      case plVariantType::UInt16:
      case plVariantType::Int32:
      case plVariantType::UInt32:
      case plVariantType::Int64:
      case plVariantType::UInt64:
      {
        plVariant result = current.ConvertTo<plInt64>() + 3;
        return result.ConvertTo(current.GetType());
      }
      case plVariantType::Vector3:
        return current.Get<plVec3>() + plVec3(1.0f, 2.0f, 3.0f);
      case plVariantType::Color:
        return plColor(0.1f, 0.2f, 0.3f, 0.4f);
      case plVariantType::String:
        return plString("network-test");
      default:
        return plVariant();
    }
  }

  /// \brief Changes every editable property T declares below plNetworkComponent, writes the scene and reads it back.
  /// Returns the names of the properties whose value did not survive.
  template <typename T>
  plString RoundTrip(plUInt32& out_uiChecked)
  {
    out_uiChecked = 0;
    plDynamicArray<plString> lost;
    plHybridArray<const plAbstractProperty*, 32> properties;
    plHybridArray<plVariant, 32> expected;

    plDefaultMemoryStreamStorage storage;
    {
      plWorldDesc desc("SerializeSource");
      plWorld source(desc);
      PL_LOCK(source.GetWriteMarker());
      plGameObjectDesc objectDesc;
      objectDesc.m_bDynamic = true;
      plGameObject* pObject = nullptr;
      source.CreateObject(objectDesc, pObject);
      T* pComponent = AddComponent<T>(source, pObject);

      for (const plRTTI* pRtti = plGetStaticRTTI<T>(); pRtti != nullptr && pRtti != plGetStaticRTTI<plNetworkComponent>(); pRtti = pRtti->GetParentType())
      {
        for (const plAbstractProperty* pProperty : pRtti->GetProperties())
        {
          if (pProperty->GetCategory() != plPropertyCategory::Member || pProperty->GetFlags().IsSet(plPropertyFlags::ReadOnly))
            continue;

          const auto* pMember = static_cast<const plAbstractMemberProperty*>(pProperty);
          const plVariant value = Different(pMember, plReflectionUtils::GetMemberPropertyValue(pMember, pComponent));
          if (!value.IsValid())
            continue;

          plReflectionUtils::SetMemberPropertyValue(pMember, pComponent, value);
          properties.PushBack(pProperty);
          expected.PushBack(plReflectionUtils::GetMemberPropertyValue(pMember, pComponent));
        }
      }

      plMemoryStreamWriter stream(&storage);
      plWorldWriter writer;
      writer.WriteWorld(stream, source);
    }

    plWorldDesc desc("SerializeLoaded");
    plWorld loaded(desc);
    PL_LOCK(loaded.GetWriteMarker());
    plMemoryStreamReader input(&storage);
    plWorldReader reader;
    if (reader.ReadWorldDescription(input).Failed())
      return "the scene could not be read";
    reader.InstantiateWorld(loaded);

    T* pRestored = nullptr;
    for (auto it = loaded.GetObjects(); it.IsValid(); ++it)
    {
      if (it->TryGetComponentOfBaseType(pRestored))
        break;
    }
    if (pRestored == nullptr)
      return "the component was not restored";

    for (plUInt32 i = 0; i < properties.GetCount(); ++i)
    {
      ++out_uiChecked;
      const auto* pMember = static_cast<const plAbstractMemberProperty*>(properties[i]);
      if (plReflectionUtils::GetMemberPropertyValue(pMember, pRestored) != expected[i])
        lost.PushBack(properties[i]->GetPropertyName());
    }

    plStringBuilder sLost;
    for (const plString& sName : lost)
      sLost.AppendWithSeparator(", ", sName);
    return sLost;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, Lifecycle)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "D17: stopping clears the object table and the time sync")
  {
    TestPrefabs prefabs;
    const plHashedString sPawn = AddPawnPrefab(prefabs);
    NetworkEndpoint host, a;
    host.RegisterPrefab(sPawn);
    a.RegisterPrefab(sPawn);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    const plUInt32 uiObject = host.Spawn(sPawn, 0);
    if (!PL_TEST_BOOL(PumpAll(all, [&] { return a.FindSpawned(uiObject) && a.m_pModule->GetNetworkTime()->IsSynchronized(); })))
      return;

    a.Stop();
    PL_TEST_INT(a.m_pModule->GetObjectManager()->GetAllObjects().GetCount(), 0);
    PL_TEST_BOOL(!a.m_pModule->GetNetworkTime()->IsSynchronized());
    PL_TEST_BOOL(a.m_pModule->GetNetworkTime()->GetServerOffset().IsZero());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D17: a client that joins a new session gets objects whose IDs the old session used")
  {
    TestPrefabs prefabs;
    const plHashedString sPawn = AddPawnPrefab(prefabs);
    NetworkEndpoint a;
    a.RegisterPrefab(sPawn);

    plUInt32 uiFirst = 0;
    plGameObjectHandle hFirst;
    {
      NetworkEndpoint first;
      first.RegisterPrefab(sPawn);
      const Pumped all({&first, &a});
      if (!PL_TEST_BOOL(ConnectAll(all, first, {&a})))
        return;
      uiFirst = first.Spawn(sPawn, 0);
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return a.FindSpawned(uiFirst) != nullptr; })))
        return;
      hFirst = a.FindSpawned(uiFirst)->GetHandle();
      a.Stop();
    }

    NetworkEndpoint second;
    second.RegisterPrefab(sPawn);
    const Pumped all({&second, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, second, {&a})))
      return;

    const plUInt32 uiSecond = second.Spawn(sPawn, 0);
    PL_TEST_INT(uiSecond, uiFirst);
    NETWORK_TEST_WAIT(PumpAll(all, [&] { return a.FindSpawned(uiSecond) != nullptr && a.FindSpawned(uiSecond)->GetHandle() != hFirst; }),
      "The client took the new object for a duplicate of one from the old session.");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D27: the objects of a client that leaves are despawned")
  {
    TestPrefabs prefabs;
    const plHashedString sPawn = AddPawnPrefab(prefabs);
    NetworkEndpoint host, a, b;
    for (NetworkEndpoint* pEndpoint : {&host, &a, &b})
      pEndpoint->RegisterPrefab(sPawn);

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    const plUInt32 uiObject = host.Spawn(sPawn, a.m_pModule->GetLocalClientID());
    if (!PL_TEST_BOOL(PumpAll(all, [&] { return b.FindSpawned(uiObject) != nullptr; })))
      return;

    a.Stop();
    NETWORK_TEST_WAIT(PumpAll(all, [&] { return host.FindSpawned(uiObject) == nullptr && b.FindSpawned(uiObject) == nullptr; }),
      "The object of the client that left is still in the session.");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D28: a client the server rejects learns why")
  {
    NetworkEndpoint host, a;
    host.m_uiGameVersion = 1;
    a.m_uiGameVersion = 2;

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(host.Start(true) && a.Start(false, host.GetPort())))
      return;

    PumpAll(all, [&] { return a.m_pModule->GetPeer() == nullptr; });
    PL_TEST_BOOL(!a.m_pModule->IsConnected());
    PL_TEST_INT(a.m_pModule->GetLastDisconnectReason(), plNetworkDisconnectReason::VersionMismatch);
  }

  for (Role role : {Role::Host, Role::Server})
  {
    PL_TEST_BLOCK(plTestBlock::Enabled, "D31: clients learn that the server shut down")
    {
      NetworkEndpoint server, a;
      const Pumped all({&server, &a});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a})))
        return;

      server.Stop();
      PumpAll(all, [&] { return a.m_pModule->GetPeer() == nullptr; });
      PL_TEST_BOOL_MSG(a.m_pModule->GetLastDisconnectReason() == plNetworkDisconnectReason::ServerShutdown, "%s: the client recorded reason %d",
        role == Role::Host ? "host" : "dedicated server", a.m_pModule->GetLastDisconnectReason());
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D40: per-client records are pruned when clients leave")
  {
    NetworkEndpoint host;
    host.AddReplica("Crate", plReplicateAuthority::Server);
    if (!PL_TEST_BOOL(host.Start(true)))
      return;

    for (plInt32 i = 0; i < 8; ++i)
    {
      NetworkEndpoint client;
      client.AddReplica("Crate", plReplicateAuthority::Server);
      const Pumped all({&host, &client});
      if (!PL_TEST_BOOL(client.Start(false, host.GetPort()) && PumpAll(all, [&] { return client.m_pModule->GetNetworkTime()->IsSynchronized(); })))
        return;

      {
        PL_LOCK(host.m_World.GetWriteMarker());
        host.m_Replicas[0].m_pTarget->m_iValue = i + 1;
      }
      PumpAll(all, [&] { return client.m_Replicas[0].m_pTarget->m_iValue == i + 1; });
      client.Stop();
      PumpAll(Pumped({&host}), [&] { return host.m_pModule->GetClients().IsEmpty(); });
    }

    // One record for the server's own uploads may remain; none for the clients that left.
    PL_TEST_BOOL_MSG(plNetworkTestAccess::SentRecordCount(host.m_Replicas[0].m_pProperties) <= 1, "%u last-sent records",
      plNetworkTestAccess::SentRecordCount(host.m_Replicas[0].m_pProperties));
    PL_TEST_INT(plNetworkTestAccess::TimeSyncRecordCount(host.m_pModule), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D40: bandwidth records of despawned objects are pruned")
  {
    TestPrefabs prefabs;
    const plHashedString sPawn = AddPawnPrefab(prefabs);
    NetworkEndpoint host, a;
    host.RegisterPrefab(sPawn);
    a.RegisterPrefab(sPawn);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    const plUInt32 uiBefore = plNetworkTestAccess::BandwidthStateCount(host.m_pModule);
    for (plInt32 i = 0; i < 20; ++i)
    {
      const plUInt32 uiObject = host.Spawn(sPawn, 0);
      PumpAll(all, [&] { return a.FindSpawned(uiObject) != nullptr; });
      PumpFor(all, plTime::MakeFromMilliseconds(20));
      host.Despawn(uiObject);
    }
    PumpFor(all, plTime::MakeFromMilliseconds(50));

    PL_TEST_BOOL_MSG(plNetworkTestAccess::BandwidthStateCount(host.m_pModule) <= uiBefore + 1, "%u bandwidth records after 20 despawned objects (%u before)",
      plNetworkTestAccess::BandwidthStateCount(host.m_pModule), uiBefore);
  }
}

PL_CREATE_SIMPLE_TEST(Network, ComponentSerialization)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "D34: editor-set values of every networking component survive a scene export")
  {
    plUInt32 uiChecked = 0;
    plString sLost;

    sLost = RoundTrip<plNetworkedRigidbodyComponent>(uiChecked);
    PL_TEST_BOOL_MSG(sLost.IsEmpty(), "plNetworkedRigidbodyComponent lost %s (%u checked)", sLost.GetData(), uiChecked);

    sLost = RoundTrip<plNetworkPredictedComponent>(uiChecked);
    PL_TEST_BOOL_MSG(sLost.IsEmpty(), "plNetworkPredictedComponent lost %s (%u checked)", sLost.GetData(), uiChecked);

    sLost = RoundTrip<plNetworkedBlackboardComponent>(uiChecked);
    PL_TEST_BOOL_MSG(sLost.IsEmpty(), "plNetworkedBlackboardComponent lost %s (%u checked)", sLost.GetData(), uiChecked);

    sLost = RoundTrip<plNetworkedEventComponent>(uiChecked);
    PL_TEST_BOOL_MSG(sLost.IsEmpty(), "plNetworkedEventComponent lost %s (%u checked)", sLost.GetData(), uiChecked);

    sLost = RoundTrip<plNetworkDebugComponent>(uiChecked);
    PL_TEST_BOOL_MSG(sLost.IsEmpty(), "plNetworkDebugComponent lost %s (%u checked)", sLost.GetData(), uiChecked);

    sLost = RoundTrip<plNetworkedPropertiesComponent>(uiChecked);
    PL_TEST_BOOL_MSG(sLost.IsEmpty(), "plNetworkedPropertiesComponent lost %s (%u checked)", sLost.GetData(), uiChecked);

    sLost = RoundTrip<plNetworkedTransformComponent>(uiChecked);
    PL_TEST_BOOL_MSG(sLost.IsEmpty(), "plNetworkedTransformComponent lost %s (%u checked)", sLost.GetData(), uiChecked);
  }
}
