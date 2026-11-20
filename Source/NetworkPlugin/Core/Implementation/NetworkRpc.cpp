#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkRpc.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>

#include <Foundation/Reflection/Reflection.h>

// Reflection for RPC types
PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkRpcTarget, 1)
  PL_ENUM_CONSTANT(plNetworkRpcTarget::Server),
  PL_ENUM_CONSTANT(plNetworkRpcTarget::Owner),
  PL_ENUM_CONSTANT(plNetworkRpcTarget::AllClients),
  PL_ENUM_CONSTANT(plNetworkRpcTarget::AllClientsAndServer),
  PL_ENUM_CONSTANT(plNetworkRpcTarget::Multicast),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkRpcReliability, 1)
  PL_ENUM_CONSTANT(plNetworkRpcReliability::Unreliable),
  PL_ENUM_CONSTANT(plNetworkRpcReliability::UnreliableOrdered),
  PL_ENUM_CONSTANT(plNetworkRpcReliability::Reliable),
  PL_ENUM_CONSTANT(plNetworkRpcReliability::ReliableOrdered),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plNetworkRpcAttribute, 1, plRTTIDefaultAllocator<plNetworkRpcAttribute>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Target", plNetworkRpcTarget, m_Target),
    PL_ENUM_MEMBER_PROPERTY("Reliability", plNetworkRpcReliability, m_Reliability),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

namespace
{
  /// \brief The variant type an RPC argument of this reflected type travels as; Invalid for types RPCs do not support.
  plVariantType::Enum GetArgumentVariantType(const plRTTI* pType)
  {
    if (pType == plGetStaticRTTI<bool>())
      return plVariantType::Bool;
    if (pType == plGetStaticRTTI<plInt8>())
      return plVariantType::Int8;
    if (pType == plGetStaticRTTI<plUInt8>())
      return plVariantType::UInt8;
    if (pType == plGetStaticRTTI<plInt16>())
      return plVariantType::Int16;
    if (pType == plGetStaticRTTI<plUInt16>())
      return plVariantType::UInt16;
    if (pType == plGetStaticRTTI<plInt32>())
      return plVariantType::Int32;
    if (pType == plGetStaticRTTI<plUInt32>())
      return plVariantType::UInt32;
    if (pType == plGetStaticRTTI<plInt64>())
      return plVariantType::Int64;
    if (pType == plGetStaticRTTI<plUInt64>())
      return plVariantType::UInt64;
    if (pType == plGetStaticRTTI<float>())
      return plVariantType::Float;
    if (pType == plGetStaticRTTI<double>())
      return plVariantType::Double;
    if (pType == plGetStaticRTTI<plVec2>())
      return plVariantType::Vector2;
    if (pType == plGetStaticRTTI<plVec3>())
      return plVariantType::Vector3;
    if (pType == plGetStaticRTTI<plVec4>())
      return plVariantType::Vector4;
    if (pType == plGetStaticRTTI<plQuat>())
      return plVariantType::Quaternion;
    if (pType == plGetStaticRTTI<plString>() || pType == plGetStaticRTTI<plStringView>() || pType == plGetStaticRTTI<const char*>())
      return plVariantType::String;
    return plVariantType::Invalid;
  }

  bool IsStringType(plVariantType::Enum type) { return type == plVariantType::String || type == plVariantType::StringView; }
} // namespace

plNetworkRpcManager::plNetworkRpcManager()
{
}

plNetworkRpcManager::~plNetworkRpcManager()
{
  Shutdown();
}

void plNetworkRpcManager::Initialize(plNetworkWorldModule* pNetworkModule)
{
  PL_ASSERT_DEV(pNetworkModule != nullptr, "NetworkWorldModule cannot be null");
  m_pNetworkModule = pNetworkModule;

  // Auto-register all known component RPCs
  RegisterAllRpcs();
}

void plNetworkRpcManager::Shutdown()
{
  m_RegisteredRpcs.Clear();
  m_RpcLookup.Clear();
  m_pNetworkModule = nullptr;
}

//////////////////////////////////////////////////////////////////////////
// RPC Registration
//////////////////////////////////////////////////////////////////////////

void plNetworkRpcManager::RegisterComponentRpcs(const plRTTI* pComponentType)
{
  if (!pComponentType)
    return;

  for (const plAbstractFunctionProperty* pFunction : pComponentType->GetFunctions())
  {
    const plNetworkRpcAttribute* pRpcAttr = pFunction->GetAttributeByType<plNetworkRpcAttribute>();
    if (!pRpcAttr)
      continue;

    RegisterRpcInfo(pComponentType, pFunction, pRpcAttr->m_Target, pRpcAttr->m_Reliability);
  }

  // Also check parent types
  if (pComponentType->GetParentType())
  {
    RegisterComponentRpcs(pComponentType->GetParentType());
  }
}

void plNetworkRpcManager::RegisterRpc(
  const plRTTI* pComponentType,
  plStringView sFunctionName,
  plNetworkRpcTarget::Enum target,
  plNetworkRpcReliability::Enum reliability)
{
  if (!pComponentType)
    return;

  const plAbstractFunctionProperty* pFunction = nullptr;
  for (const plAbstractFunctionProperty* pFunc : pComponentType->GetFunctions())
  {
    if (sFunctionName.IsEqual(pFunc->GetPropertyName()))
    {
      pFunction = pFunc;
      break;
    }
  }

  if (!pFunction)
  {
    plLog::Error("PL Net: RegisterRpc - function '{}' not found on type '{}'",
      sFunctionName, pComponentType->GetTypeName());
    return;
  }

  RegisterRpcInfo(pComponentType, pFunction, target, reliability);
}

void plNetworkRpcManager::RegisterRpcInfo(const plRTTI* pComponentType, const plAbstractFunctionProperty* pFunction,
  plNetworkRpcTarget::Enum target, plNetworkRpcReliability::Enum reliability)
{
  const plUInt32 rpcID = GenerateRpcID(pComponentType, pFunction->GetPropertyName());

  if (const plNetworkRpcInfo* pExisting = GetRpcInfo(rpcID))
  {
    if (pExisting->m_pComponentType != pComponentType || pExisting->m_pFunction != pFunction)
    {
      plLog::Error("PL Net: RPC '{}::{}' has the same ID {} as '{}::{}'; rename one of them", pComponentType->GetTypeName(), pFunction->GetPropertyName(),
        rpcID, pExisting->m_pComponentType->GetTypeName(), pExisting->m_sFunctionName);
    }
    return;
  }

  plNetworkRpcInfo info;
  info.m_uiRpcID = rpcID;
  info.m_sFunctionName = pFunction->GetPropertyName();
  info.m_pComponentType = pComponentType;
  info.m_pFunction = pFunction;
  info.m_Target = target;
  info.m_Reliability = reliability;
  m_RegisteredRpcs[rpcID] = info;

  // Lookup key: high 32 bits = type hash, low 32 bits = function name hash
  const plUInt64 lookupKey = (static_cast<plUInt64>(pComponentType->GetTypeNameHash()) << 32) | plHashingUtils::StringHash(pFunction->GetPropertyName());
  m_RpcLookup[lookupKey] = rpcID;

  plLog::Dev("PL Net: Registered RPC '{}::{}' with ID {}", pComponentType->GetTypeName(), pFunction->GetPropertyName(), rpcID);
}

void plNetworkRpcManager::RegisterAllRpcs()
{
  // Find all types derived from plNetworkComponent
  plRTTI::ForEachDerivedType<plNetworkComponent>([this](const plRTTI* pRtti) {
    RegisterComponentRpcs(pRtti);
  });
}

const plNetworkRpcInfo* plNetworkRpcManager::GetRpcInfo(plUInt32 uiRpcID) const
{
  const plNetworkRpcInfo* pInfo = nullptr;
  m_RegisteredRpcs.TryGetValue(uiRpcID, pInfo);
  return pInfo;
}

const plNetworkRpcInfo* plNetworkRpcManager::GetRpcInfo(const plRTTI* pComponentType, plStringView sFunctionName) const
{
  plUInt64 lookupKey = (static_cast<plUInt64>(pComponentType->GetTypeNameHash()) << 32) |
                       plHashingUtils::StringHash(sFunctionName);

  plUInt32 rpcID = 0;
  if (m_RpcLookup.TryGetValue(lookupKey, rpcID))
  {
    return GetRpcInfo(rpcID);
  }

  // Try parent types
  if (pComponentType->GetParentType())
  {
    return GetRpcInfo(pComponentType->GetParentType(), sFunctionName);
  }

  return nullptr;
}

plUInt32 plNetworkRpcManager::GenerateRpcID(const plRTTI* pComponentType, plStringView sFunctionName)
{
  // Type and function name both go in, so two component types can have RPCs of the same name.
  const plUInt32 uiTypeHash = plHashingUtils::StringHashTo32(pComponentType->GetTypeNameHash());
  const plUInt32 id = plHashingUtils::CombineHashValues32(uiTypeHash, plHashingUtils::StringHashTo32(plHashingUtils::StringHash(sFunctionName)));
  return id == 0 ? 1 : id;
}

//////////////////////////////////////////////////////////////////////////
// RPC Invocation
//
// Where a call runs, by target and caller. A host counts as a client; B is the only client a multicast names.
//
//   target              | called on the server          | called on the owning client A
//   Server              | the server                    | the server
//   Owner               | the owner                     | A, locally
//   AllClients          | every client, the host too    | A locally, then every other client and a host
//   AllClientsAndServer | everyone                      | A locally, then everyone else
//   Multicast           | the listed clients            | refused: CallRpc returns false
//
// A client may only call RPCs on objects it owns; the server checks that before it runs or relays a call.
//////////////////////////////////////////////////////////////////////////

bool plNetworkRpcManager::CallRpc(plNetworkComponent* pComponent, plStringView sFunctionName, plArrayPtr<plVariant> arguments)
{
  return CallRpc(pComponent, sFunctionName, arguments, plDynamicArray<plUInt32>());
}

bool plNetworkRpcManager::CallRpc(
  plNetworkComponent* pComponent,
  plStringView sFunctionName,
  plArrayPtr<plVariant> arguments,
  const plDynamicArray<plUInt32>& targetClients)
{
  if (!pComponent)
  {
    plLog::Warning("PL Net: Cannot call RPC - invalid state");
    return false;
  }

  return CallRpc(static_cast<plComponent*>(pComponent), pComponent->GetNetID(), sFunctionName, arguments, targetClients);
}

bool plNetworkRpcManager::CallRpc(plComponent* pComponent, plUInt32 uiNetID, plStringView sFunctionName, plArrayPtr<plVariant> arguments)
{
  return CallRpc(pComponent, uiNetID, sFunctionName, arguments, plDynamicArray<plUInt32>());
}

bool plNetworkRpcManager::CallRpc(
  plComponent* pComponent,
  plUInt32 uiNetID,
  plStringView sFunctionName,
  plArrayPtr<plVariant> arguments,
  const plDynamicArray<plUInt32>& targetClients)
{
  if (!m_pNetworkModule || !pComponent)
  {
    plLog::Warning("PL Net: Cannot call RPC - invalid state");
    return false;
  }

  const plNetworkRpcInfo* pRpcInfo = GetRpcInfo(pComponent->GetDynamicRTTI(), sFunctionName);
  if (!pRpcInfo)
  {
    plLog::Warning("PL Net: RPC '{}' not found on component type '{}'", sFunctionName, pComponent->GetDynamicRTTI()->GetTypeName());
    return false;
  }

  if (uiNetID == 0 || uiNetID == plMath::MaxValue<plUInt32>())
  {
    plLog::Warning("PL Net: Cannot call RPC '{}' on a component without a NetID", sFunctionName);
    return false;
  }

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  if (!pPeer)
  {
    plLog::Warning("PL Net: No network peer available for RPC");
    return false;
  }

  plDynamicArray<plVariant> args;
  if (ConvertArguments(*pRpcInfo, arguments, args).Failed())
    return false;

  const bool bServer = !m_pNetworkModule->IsClient();
  if (!bServer && pRpcInfo->m_Target == plNetworkRpcTarget::Multicast)
  {
    plLog::Warning("PL Net: RPC '{}' targets a list of clients, which only the server can call", sFunctionName);
    return false;
  }

  plNetworkMessage msg = pPeer->BeginSendMessage();
  msg.WriteUInt8(plNetworkMessageIDs::ObjectRpc);
  msg.WriteUInt32(pRpcInfo->m_uiRpcID);
  msg.WriteUInt32(uiNetID);
  SerializeArguments(msg, pRpcInfo->m_pFunction, args);
  msg.SetChannelType(GetChannelType(pRpcInfo->m_Reliability));

  if (bServer)
  {
    RouteOnServer(*pRpcInfo, pComponent, GetOwnerOf(pComponent, uiNetID), args, msg, m_pNetworkModule->GetServerClientID(), targetClients);
    return true;
  }

  // A client runs what it is part of right away and hands the rest to the server.
  switch (pRpcInfo->m_Target)
  {
    case plNetworkRpcTarget::Owner:
      if (GetOwnerOf(pComponent, uiNetID) == m_pNetworkModule->GetLocalClientID())
      {
        Execute(*pRpcInfo, pComponent, args);
        return true;
      }
      break;

    case plNetworkRpcTarget::AllClients:
    case plNetworkRpcTarget::AllClientsAndServer:
      Execute(*pRpcInfo, pComponent, args);
      break;

    default:
      break;
  }

  return pPeer->SendToServer(msg).Succeeded();
}

void plNetworkRpcManager::RouteOnServer(const plNetworkRpcInfo& rpcInfo, plComponent* pComponent, plUInt32 uiOwnerClientID, plArrayPtr<plVariant> arguments,
  const plNetworkMessage& msg, plUInt32 uiCallerClientID, const plDynamicArray<plUInt32>& targetClients)
{
  // A client that called it has already run its own part; the server runs its part and sends the rest on.
  const plUInt32 uiServerID = m_pNetworkModule->GetServerClientID();
  const bool bHost = m_pNetworkModule->IsHost();

  switch (rpcInfo.m_Target)
  {
    case plNetworkRpcTarget::Server:
      Execute(rpcInfo, pComponent, arguments);
      break;

    case plNetworkRpcTarget::Owner:
      if (uiOwnerClientID == uiServerID)
        Execute(rpcInfo, pComponent, arguments);
      else if (uiOwnerClientID != uiCallerClientID)
        m_pNetworkModule->SendToClient(msg, uiOwnerClientID).IgnoreResult();
      break;

    case plNetworkRpcTarget::AllClients:
      if (bHost)
        Execute(rpcInfo, pComponent, arguments);
      m_pNetworkModule->SendToConnectedClients(msg, uiCallerClientID).IgnoreResult();
      break;

    case plNetworkRpcTarget::AllClientsAndServer:
      Execute(rpcInfo, pComponent, arguments);
      m_pNetworkModule->SendToConnectedClients(msg, uiCallerClientID).IgnoreResult();
      break;

    case plNetworkRpcTarget::Multicast:
      for (plUInt32 uiClientID : targetClients)
      {
        if (uiClientID == uiServerID)
          Execute(rpcInfo, pComponent, arguments);
        else
          m_pNetworkModule->SendToClient(msg, uiClientID).IgnoreResult();
      }
      break;
  }
}

void plNetworkRpcManager::Execute(const plNetworkRpcInfo& rpcInfo, plComponent* pComponent, plArrayPtr<plVariant> arguments)
{
  plVariant returnValue;
  plDynamicArray<plVariant> args;
  args = arguments;
  rpcInfo.m_pFunction->Execute(pComponent, args, returnValue);
}

plUInt32 plNetworkRpcManager::GetOwnerOf(plComponent* pComponent, plUInt32 uiNetID) const
{
  if (const plNetworkComponent* pNetworkComponent = m_pNetworkModule->FindNetworkComponent(uiNetID))
    return m_pNetworkModule->GetOwnerClientID(pNetworkComponent);

  if (pComponent->IsInstanceOf<plNetworkComponent>())
    return m_pNetworkModule->GetOwnerClientID(static_cast<plNetworkComponent*>(pComponent));

  return m_pNetworkModule->GetServerClientID();
}

plResult plNetworkRpcManager::ConvertArguments(const plNetworkRpcInfo& rpcInfo, plArrayPtr<plVariant> arguments, plDynamicArray<plVariant>& out_arguments) const
{
  const plAbstractFunctionProperty* pFunction = rpcInfo.m_pFunction;
  if (pFunction->GetArgumentCount() != arguments.GetCount())
  {
    plLog::Error("PL Net: RPC '{}' expects {} arguments, got {}", rpcInfo.m_sFunctionName, pFunction->GetArgumentCount(), arguments.GetCount());
    return PL_FAILURE;
  }

  // The function is called with exactly its parameter types, here and on every receiver.
  out_arguments.SetCount(arguments.GetCount());
  for (plUInt32 i = 0; i < arguments.GetCount(); ++i)
  {
    const plVariantType::Enum expected = GetArgumentVariantType(pFunction->GetArgumentType(i));
    if (expected == plVariantType::Invalid || !arguments[i].CanConvertTo(expected))
    {
      plLog::Warning("PL Net: Not calling RPC '{}': argument {} does not convert to the type the function takes", rpcInfo.m_sFunctionName, i);
      return PL_FAILURE;
    }

    out_arguments[i] = arguments[i].ConvertTo(expected);
  }

  return PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////
// Message Handling
//////////////////////////////////////////////////////////////////////////

void plNetworkRpcManager::HandleRpcMessage(plNetworkMessage& msg, plUInt32 uiSenderClientID)
{
  if (!m_pNetworkModule)
    return;

  const plUInt32 uiStart = msg.m_uiPosition;
  plUInt32 rpcID = msg.ReadUInt32();
  plUInt32 componentNetID = msg.ReadUInt32();
  if (msg.HasReadError())
    return;

  // A call from the server for a component that has not started yet (a fresh spawn) runs once it starts, and calls
  // behind it wait with it. Components register their RPCs when they start, so this comes before the RPC lookup.
  plNetworkComponent* pComponent = m_pNetworkModule->FindNetworkComponent(componentNetID);
  if (pComponent == nullptr || m_pNetworkModule->IsHoldingMessagesFor(componentNetID))
  {
    if (m_pNetworkModule->IsClient() && uiSenderClientID == m_pNetworkModule->GetServerClientID())
      m_pNetworkModule->HoldRpcForComponent(componentNetID, uiSenderClientID, plArrayPtr<const plUInt8>(msg.m_pBuffer + uiStart, msg.m_uiLength - uiStart));
    else
      plLog::Warning("PL Net: RPC target component {} not found", componentNetID);

    msg.m_uiPosition = msg.m_uiLength;
    return;
  }

  const plNetworkRpcInfo* pRpcInfo = GetRpcInfo(rpcID);
  if (!pRpcInfo)
  {
    plLog::Warning("PL Net: Received unknown RPC ID {}", rpcID);
    return;
  }

  const bool bServer = !m_pNetworkModule->IsClient();
  const plUInt32 uiOwnerClientID = m_pNetworkModule->GetOwnerClientID(pComponent);
  if (bServer)
  {
    // Clients may call RPCs only on what they own, and never one aimed at a list of clients.
    plNetworkClient* pSender = m_pNetworkModule->GetClient(uiSenderClientID);
    if (uiSenderClientID != m_pNetworkModule->GetServerClientID() &&
        (!pSender || pSender->m_ConnectionState != plNetworkConnectionState::Connected || uiOwnerClientID != uiSenderClientID ||
          pRpcInfo->m_Target == plNetworkRpcTarget::Multicast))
      return;
  }
  else if (uiSenderClientID != m_pNetworkModule->GetServerClientID() || pRpcInfo->m_Target == plNetworkRpcTarget::Server)
  {
    return;
  }

  if (!pComponent->GetDynamicRTTI()->IsDerivedFrom(pRpcInfo->m_pComponentType))
  {
    plLog::Warning("PL Net: RPC type mismatch - expected {}, got {}",
      pRpcInfo->m_pComponentType->GetTypeName(), pComponent->GetDynamicRTTI()->GetTypeName());
    return;
  }

  plDynamicArray<plVariant> arguments;
  const bool bArgumentsMatch = DeserializeArguments(msg, pRpcInfo->m_pFunction, arguments);
  if (msg.HasReadError())
    return;

  if (!bArgumentsMatch)
  {
    // A well-formed call from a build with another signature: dropped, the connection is fine.
    plLog::Warning("PL Net: Dropped RPC '{}::{}': its arguments do not match the function", pRpcInfo->m_pComponentType->GetTypeName(), pRpcInfo->m_sFunctionName);
    return;
  }

  if (!bServer)
  {
    // The server only sends a client what it is meant to run.
    Execute(*pRpcInfo, pComponent, arguments);
    return;
  }

  // Without a peer (the module is offline) the server's own part still runs and nothing is sent.
  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  plNetworkMessage relay = pPeer ? pPeer->BeginSendMessage() : plNetworkMessage();
  relay.WriteUInt8(plNetworkMessageIDs::ObjectRpc);
  relay.WriteUInt32(pRpcInfo->m_uiRpcID);
  relay.WriteUInt32(componentNetID);
  SerializeArguments(relay, pRpcInfo->m_pFunction, arguments);
  relay.SetChannelType(GetChannelType(pRpcInfo->m_Reliability));

  RouteOnServer(*pRpcInfo, pComponent, uiOwnerClientID, arguments, relay, uiSenderClientID, plDynamicArray<plUInt32>());
}

plNetworkChannelTypeEnum::Enum plNetworkRpcManager::GetChannelType(plNetworkRpcReliability::Enum reliability) const
{
  switch (reliability)
  {
    case plNetworkRpcReliability::Unreliable:
      return plNetworkChannelTypeEnum::Unreliable;
    case plNetworkRpcReliability::UnreliableOrdered:
      return plNetworkChannelTypeEnum::UnreliableOrdered;
    case plNetworkRpcReliability::Reliable:
      return plNetworkChannelTypeEnum::Reliable;
    case plNetworkRpcReliability::ReliableOrdered:
    default:
      return plNetworkChannelTypeEnum::ReliableOrdered;
  }
}

//////////////////////////////////////////////////////////////////////////
// Serialization
//////////////////////////////////////////////////////////////////////////

void plNetworkRpcManager::SerializeArguments(plNetworkMessage& msg, const plAbstractFunctionProperty* pFunction, plArrayPtr<plVariant> arguments)
{
  PL_IGNORE_UNUSED(pFunction);
  msg.WriteUInt8(static_cast<plUInt8>(arguments.GetCount()));

  for (plUInt32 i = 0; i < arguments.GetCount(); ++i)
  {
    WriteVariant(msg, arguments[i]);
  }
}

bool plNetworkRpcManager::DeserializeArguments(plNetworkMessage& msg, const plAbstractFunctionProperty* pFunction, plDynamicArray<plVariant>& out_arguments)
{
  const plUInt8 argCount = msg.ReadUInt8();
  if (msg.HasReadError())
    return false;

  // Every argument is read as the type it was written as, so a call with another signature is parsed to its end and
  // dropped; only a message that ends early or carries an unknown type is malformed.
  bool bMatches = argCount == pFunction->GetArgumentCount();
  out_arguments.SetCount(argCount);
  for (plUInt8 i = 0; i < argCount; ++i)
  {
    const plRTTI* pExpectedType = i < pFunction->GetArgumentCount() ? pFunction->GetArgumentType(i) : nullptr;
    bool bTypeMatches = true;
    out_arguments[i] = ReadVariant(msg, pExpectedType, bTypeMatches);
    if (!out_arguments[i].IsValid() || msg.HasReadError())
    {
      msg.MarkReadError();
      out_arguments.Clear();
      return false;
    }

    bMatches = bMatches && bTypeMatches;
  }

  if (msg.GetRemainingBytes() != 0)
    msg.MarkReadError();

  if (!bMatches)
    out_arguments.Clear();
  return bMatches;
}

void plNetworkRpcManager::WriteVariant(plNetworkMessage& msg, const plVariant& value)
{
  // Write variant type
  msg.WriteUInt8(static_cast<plUInt8>(value.GetType()));

  // Write value based on type
  switch (value.GetType())
  {
    case plVariantType::Invalid:
      break;

    case plVariantType::Bool:
      msg.WriteBool(value.Get<bool>());
      break;

    case plVariantType::Int8:
      msg.WriteInt8(value.Get<plInt8>());
      break;

    case plVariantType::UInt8:
      msg.WriteUInt8(value.Get<plUInt8>());
      break;

    case plVariantType::Int16:
      msg.WriteInt16(value.Get<plInt16>());
      break;

    case plVariantType::UInt16:
      msg.WriteUInt16(value.Get<plUInt16>());
      break;

    case plVariantType::Int32:
      msg.WriteInt32(value.Get<plInt32>());
      break;

    case plVariantType::UInt32:
      msg.WriteUInt32(value.Get<plUInt32>());
      break;

    case plVariantType::Int64:
      msg.WriteInt64(value.Get<plInt64>());
      break;

    case plVariantType::UInt64:
      msg.WriteUInt64(value.Get<plUInt64>());
      break;

    case plVariantType::Float:
      msg.WriteFloat(value.Get<float>());
      break;

    case plVariantType::Double:
      msg.WriteDouble(value.Get<double>());
      break;

    case plVariantType::Vector2:
      msg.WriteVec2(value.Get<plVec2>());
      break;

    case plVariantType::Vector3:
      msg.WriteVec3(value.Get<plVec3>());
      break;

    case plVariantType::Vector4:
      msg.WriteVec4(value.Get<plVec4>());
      break;

    case plVariantType::Quaternion:
      msg.WriteQuat(value.Get<plQuat>());
      break;

    case plVariantType::String:
      msg.WriteString(value.Get<plString>());
      break;

    case plVariantType::StringView:
      msg.WriteStringView(value.Get<plStringView>());
      break;

    default:
      plLog::Warning("PL Net: Unsupported variant type {} in RPC serialization", static_cast<int>(value.GetType()));
      break;
  }
}

plVariant plNetworkRpcManager::ReadVariant(plNetworkMessage& msg, const plRTTI* pExpectedType, bool& out_bTypeMatches)
{
  plVariantType::Enum variantType = static_cast<plVariantType::Enum>(msg.ReadUInt8());

  // Validate variant type is in valid range
  if (variantType < plVariantType::Invalid || variantType >= plVariantType::LastExtendedType)
  {
    plLog::Warning("PL Net: Invalid variant type {} received in RPC - possible corruption or attack", static_cast<int>(variantType));
    msg.MarkReadError();
    return plVariant();
  }

  // A value of another type than the function takes is still read, so the rest of the message parses; the call is not run.
  const plVariantType::Enum expectedVariantType = pExpectedType != nullptr ? GetArgumentVariantType(pExpectedType) : plVariantType::Invalid;
  out_bTypeMatches = expectedVariantType != plVariantType::Invalid &&
                     (variantType == expectedVariantType || (IsStringType(expectedVariantType) && IsStringType(variantType)));

  switch (variantType)
  {
    case plVariantType::Invalid:
      return plVariant();

    case plVariantType::Bool:
      return plVariant(msg.ReadBool());

    case plVariantType::Int8:
      return plVariant(msg.ReadInt8());

    case plVariantType::UInt8:
      return plVariant(msg.ReadUInt8());

    case plVariantType::Int16:
      return plVariant(msg.ReadInt16());

    case plVariantType::UInt16:
      return plVariant(msg.ReadUInt16());

    case plVariantType::Int32:
      return plVariant(msg.ReadInt32());

    case plVariantType::UInt32:
      return plVariant(msg.ReadUInt32());

    case plVariantType::Int64:
      return plVariant(msg.ReadInt64());

    case plVariantType::UInt64:
      return plVariant(msg.ReadUInt64());

    case plVariantType::Float:
      return plVariant(msg.ReadFloat());

    case plVariantType::Double:
      return plVariant(msg.ReadDouble());

    case plVariantType::Vector2:
      return plVariant(msg.ReadVec2());

    case plVariantType::Vector3:
      return plVariant(msg.ReadVec3());

    case plVariantType::Vector4:
      return plVariant(msg.ReadVec4());

    case plVariantType::Quaternion:
      return plVariant(msg.ReadQuat());

    case plVariantType::String:
    case plVariantType::StringView:
      return plVariant(msg.ReadString());

    default:
      plLog::Warning("PL Net: Unsupported variant type {} in RPC deserialization", static_cast<int>(variantType));
      msg.MarkReadError();
      return plVariant();
  }
}
