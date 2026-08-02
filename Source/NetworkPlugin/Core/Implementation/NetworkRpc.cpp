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

  // Iterate through all functions on this type
  for (const plAbstractFunctionProperty* pFunction : pComponentType->GetFunctions())
  {
    // Check for NetworkRpc attribute
    const plNetworkRpcAttribute* pRpcAttr = pFunction->GetAttributeByType<plNetworkRpcAttribute>();
    if (!pRpcAttr)
      continue;

    // Generate or lookup RPC ID
    plUInt32 rpcID = GenerateRpcID(pComponentType, pFunction->GetPropertyName());

    // Check if already registered
    if (m_RegisteredRpcs.Contains(rpcID))
      continue;

    // Register the RPC
    plNetworkRpcInfo info;
    info.m_uiRpcID = rpcID;
    info.m_sFunctionName = pFunction->GetPropertyName();
    info.m_pComponentType = pComponentType;
    info.m_pFunction = pFunction;
    info.m_Target = pRpcAttr->m_Target;
    info.m_Reliability = pRpcAttr->m_Reliability;

    m_RegisteredRpcs[rpcID] = info;

    // Create lookup key: high 32 bits = type hash, low 32 bits = function name hash
    plUInt64 lookupKey = (static_cast<plUInt64>(pComponentType->GetTypeNameHash()) << 32) |
                         plHashingUtils::StringHash(pFunction->GetPropertyName());
    m_RpcLookup[lookupKey] = rpcID;

    plLog::Dev("PL Net: Registered RPC '{}::{}' with ID {}",
      pComponentType->GetTypeName(), pFunction->GetPropertyName(), rpcID);
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

  // Find the function property by name
  const plAbstractFunctionProperty* pFunction = nullptr;
  for (const plAbstractFunctionProperty* pFunc : pComponentType->GetFunctions())
  {
    if (plStringUtils::IsEqual(pFunc->GetPropertyName(), sFunctionName.GetStartPointer()))
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

  plUInt32 rpcID = GenerateRpcID(pComponentType, sFunctionName);

  if (m_RegisteredRpcs.Contains(rpcID))
    return;

  plNetworkRpcInfo info;
  info.m_uiRpcID = rpcID;
  info.m_sFunctionName = pFunction->GetPropertyName();
  info.m_pComponentType = pComponentType;
  info.m_pFunction = pFunction;
  info.m_Target = target;
  info.m_Reliability = reliability;

  m_RegisteredRpcs[rpcID] = info;

  plUInt64 lookupKey = (static_cast<plUInt64>(pComponentType->GetTypeNameHash()) << 32) |
                       plHashingUtils::StringHash(sFunctionName);
  m_RpcLookup[lookupKey] = rpcID;

  plLog::Dev("PL Net: Registered RPC '{}::{}' with ID {} (manual)",
    pComponentType->GetTypeName(), sFunctionName, rpcID);
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
  // Create a deterministic ID from type + function name for network compatibility
  plUInt64 hash = (static_cast<plUInt64>(pComponentType->GetTypeNameHash()) << 32) |
                  plHashingUtils::StringHash(sFunctionName);

  // Use lower 32 bits, ensure non-zero
  plUInt32 id = static_cast<plUInt32>(hash & 0xFFFFFFFF);
  if (id == 0)
    id = 1;

  return id;
}

//////////////////////////////////////////////////////////////////////////
// RPC Invocation
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
  if (!m_pNetworkModule || !pComponent)
  {
    plLog::Warning("PL Net: Cannot call RPC - invalid state");
    return false;
  }

  // Find the RPC info
  const plNetworkRpcInfo* pRpcInfo = GetRpcInfo(pComponent->GetDynamicRTTI(), sFunctionName);
  if (!pRpcInfo)
  {
    plLog::Warning("PL Net: RPC '{}' not found on component type '{}'",
      sFunctionName, pComponent->GetDynamicRTTI()->GetTypeName());
    return false;
  }

  // Validate argument count
  if (pRpcInfo->m_pFunction->GetArgumentCount() != arguments.GetCount())
  {
    plLog::Error("PL Net: RPC '{}' expects {} arguments, got {}",
      sFunctionName, pRpcInfo->m_pFunction->GetArgumentCount(), arguments.GetCount());
    return false;
  }

  // Get the network ID for this component
  plUInt32 componentNetID = pComponent->GetNetID();
  if (componentNetID == static_cast<plUInt32>(-1))
  {
    plLog::Warning("PL Net: Cannot call RPC on non-networked component");
    return false;
  }

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  if (!pPeer)
  {
    plLog::Warning("PL Net: No network peer available for RPC");
    return false;
  }

  // Build the RPC message
  plNetworkMessage msg = pPeer->BeginSendMessage();

  msg.WriteUInt8(plNetworkMessageIDs::ObjectRpc);
  msg.WriteUInt32(pRpcInfo->m_uiRpcID);
  msg.WriteUInt32(componentNetID);

  // Serialize arguments
  SerializeArguments(msg, pRpcInfo->m_pFunction, arguments);

  // Set channel type based on reliability
  msg.SetChannelType(GetChannelType(pRpcInfo->m_Reliability));

  // Determine targets based on RPC target type
  switch (pRpcInfo->m_Target)
  {
    case plNetworkRpcTarget::Server:
    {
      if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
      {
        // We are the server, execute locally
        plVariant returnValue;
        plDynamicArray<plVariant> args;
        args.SetCount(arguments.GetCount());
        for (plUInt32 i = 0; i < arguments.GetCount(); ++i)
          args[i] = arguments[i];

        pRpcInfo->m_pFunction->Execute(pComponent, args, returnValue);
      }
      else
      {
        // Send to server
        pPeer->SendMessages(msg);
      }
      break;
    }

    case plNetworkRpcTarget::Owner:
    {
      // Get owner from the network object manager
      plUInt32 ownerClientID = 0;
      plNetworkComponent* pNetComp = nullptr;
      if (pComponent->GetOwner()->TryGetComponentOfBaseType(pNetComp) && pNetComp->GetNetID() != 0)
      {
        const plNetworkObjectInfo* pObjInfo = m_pNetworkModule->GetObjectManager()->GetObjectInfo(pNetComp->GetNetID());
        if (pObjInfo)
        {
          ownerClientID = pObjInfo->m_uiOwnerClientID;
        }
      }

      if (m_pNetworkModule->GetLocalClientID() == ownerClientID)
      {
        // Execute locally
        plVariant returnValue;
        plDynamicArray<plVariant> args;
        args.SetCount(arguments.GetCount());
        for (plUInt32 i = 0; i < arguments.GetCount(); ++i)
          args[i] = arguments[i];

        pRpcInfo->m_pFunction->Execute(pComponent, args, returnValue);
      }
      else if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
      {
        // Send to owner client
        plNetworkClient* pOwnerClient = m_pNetworkModule->GetClient(ownerClientID);
        if (pOwnerClient)
        {
          pPeer->SendMessages(msg, pOwnerClient->m_Connection);
        }
      }
      break;
    }

    case plNetworkRpcTarget::AllClients:
    {
      if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
      {
        // Broadcast to all clients
        plDynamicArray<plNetworkConnection> targets;
        for (plNetworkClient* pClient : m_pNetworkModule->GetClients())
        {
          if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
          {
            targets.PushBack(pClient->m_Connection);
          }
        }
        if (!targets.IsEmpty())
        {
          pPeer->SendMessages(msg, targets);
        }
      }
      else
      {
        // Client needs to ask server to broadcast
        pPeer->SendMessages(msg);
      }
      break;
    }

    case plNetworkRpcTarget::AllClientsAndServer:
    {
      // Execute locally
      plVariant returnValue;
      plDynamicArray<plVariant> args;
      args.SetCount(arguments.GetCount());
      for (plUInt32 i = 0; i < arguments.GetCount(); ++i)
        args[i] = arguments[i];

      pRpcInfo->m_pFunction->Execute(pComponent, args, returnValue);

      if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
      {
        // Broadcast to all clients
        plDynamicArray<plNetworkConnection> targets;
        for (plNetworkClient* pClient : m_pNetworkModule->GetClients())
        {
          if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
          {
            targets.PushBack(pClient->m_Connection);
          }
        }
        if (!targets.IsEmpty())
        {
          pPeer->SendMessages(msg, targets);
        }
      }
      else
      {
        // Send to server for relay
        pPeer->SendMessages(msg);
      }
      break;
    }

    case plNetworkRpcTarget::Multicast:
    {
      if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
      {
        // Send to specific clients
        plDynamicArray<plNetworkConnection> targets;
        for (plUInt32 clientID : targetClients)
        {
          plNetworkClient* pClient = m_pNetworkModule->GetClient(clientID);
          if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
          {
            targets.PushBack(pClient->m_Connection);
          }
        }
        if (!targets.IsEmpty())
        {
          pPeer->SendMessages(msg, targets);
        }
      }
      break;
    }
  }

  return true;
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

  // Find the RPC info
  const plNetworkRpcInfo* pRpcInfo = GetRpcInfo(pComponent->GetDynamicRTTI(), sFunctionName);
  if (!pRpcInfo)
  {
    plLog::Warning("PL Net: RPC '{}' not found on component type '{}'",
      sFunctionName, pComponent->GetDynamicRTTI()->GetTypeName());
    return false;
  }

  // Validate argument count
  if (pRpcInfo->m_pFunction->GetArgumentCount() != arguments.GetCount())
  {
    plLog::Error("PL Net: RPC '{}' expects {} arguments, got {}",
      sFunctionName, pRpcInfo->m_pFunction->GetArgumentCount(), arguments.GetCount());
    return false;
  }

  // Use the provided network ID
  if (uiNetID == plMath::MaxValue<plUInt32>())
  {
    plLog::Warning("PL Net: Cannot call RPC on component with invalid net ID");
    return false;
  }

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  if (!pPeer)
  {
    plLog::Warning("PL Net: No network peer available for RPC");
    return false;
  }

  // Build the RPC message
  plNetworkMessage msg = pPeer->BeginSendMessage();

  msg.WriteUInt8(plNetworkMessageIDs::ObjectRpc);
  msg.WriteUInt32(pRpcInfo->m_uiRpcID);
  msg.WriteUInt32(uiNetID);

  // Serialize arguments
  SerializeArguments(msg, pRpcInfo->m_pFunction, arguments);

  // Set channel type based on reliability
  msg.SetChannelType(GetChannelType(pRpcInfo->m_Reliability));

  // Determine targets based on RPC target type
  switch (pRpcInfo->m_Target)
  {
    case plNetworkRpcTarget::Server:
    {
      if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
      {
        // We are the server, execute locally
        plVariant returnValue;
        plDynamicArray<plVariant> args;
        args.SetCount(arguments.GetCount());
        for (plUInt32 i = 0; i < arguments.GetCount(); ++i)
          args[i] = arguments[i];

        pRpcInfo->m_pFunction->Execute(pComponent, args, returnValue);
      }
      else
      {
        // Send to server
        pPeer->SendMessages(msg);
      }
      break;
    }

    case plNetworkRpcTarget::Owner:
    case plNetworkRpcTarget::AllClients:
    case plNetworkRpcTarget::AllClientsAndServer:
    case plNetworkRpcTarget::Multicast:
    {
      // For non-server targets, we need to check if we're on the server
      if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
      {
        // Execute locally first for AllClientsAndServer
        if (pRpcInfo->m_Target == plNetworkRpcTarget::AllClientsAndServer)
        {
          plVariant returnValue;
          plDynamicArray<plVariant> args;
          args.SetCount(arguments.GetCount());
          for (plUInt32 i = 0; i < arguments.GetCount(); ++i)
            args[i] = arguments[i];

          pRpcInfo->m_pFunction->Execute(pComponent, args, returnValue);
        }

        // Broadcast to appropriate clients
        plDynamicArray<plNetworkConnection> targets;
        for (plNetworkClient* pClient : m_pNetworkModule->GetClients())
        {
          if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
          {
            if (pRpcInfo->m_Target == plNetworkRpcTarget::Multicast)
            {
              // Only send to specified clients
              if (targetClients.Contains(pClient->m_uiClientID))
              {
                targets.PushBack(pClient->m_Connection);
              }
            }
            else
            {
              targets.PushBack(pClient->m_Connection);
            }
          }
        }
        if (!targets.IsEmpty())
        {
          pPeer->SendMessages(msg, targets);
        }
      }
      else
      {
        // Client needs to ask server to broadcast
        pPeer->SendMessages(msg);
      }
      break;
    }
  }

  return true;
}

//////////////////////////////////////////////////////////////////////////
// Message Handling
//////////////////////////////////////////////////////////////////////////

void plNetworkRpcManager::HandleRpcMessage(plNetworkMessage& msg, plUInt32 uiSenderClientID)
{
  if (!m_pNetworkModule)
    return;

  plUInt32 rpcID = msg.ReadUInt32();
  plUInt32 componentNetID = msg.ReadUInt32();

  // Find the RPC info
  const plNetworkRpcInfo* pRpcInfo = GetRpcInfo(rpcID);
  if (!pRpcInfo)
  {
    plLog::Warning("PL Net: Received unknown RPC ID {}", rpcID);
    return;
  }

  // Validate sender authority for server-targeted RPCs
  // Only the owner or server should be able to call certain RPCs
  if (pRpcInfo->m_Target == plNetworkRpcTarget::Server)
  {
    // Server-targeted RPCs from clients are allowed (that's the point)
    // but we'll validate ownership below if needed
  }
  else if (pRpcInfo->m_Target == plNetworkRpcTarget::Owner ||
           pRpcInfo->m_Target == plNetworkRpcTarget::AllClients ||
           pRpcInfo->m_Target == plNetworkRpcTarget::AllClientsAndServer ||
           pRpcInfo->m_Target == plNetworkRpcTarget::Multicast)
  {
    // Only server/host should be broadcasting these to clients
    // If we're the server receiving this from a client, validate they have authority
    if ((m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost()) &&
        uiSenderClientID != m_pNetworkModule->GetServerClientID())
    {
      // Client is trying to broadcast - check if they're the owner
      const plNetworkObjectInfo* pObjInfo = m_pNetworkModule->GetObjectManager()->GetObjectInfo(componentNetID);
      if (pObjInfo && pObjInfo->m_uiOwnerClientID != uiSenderClientID)
      {
        plLog::Warning("PL Net: Client {} attempted unauthorized RPC broadcast for object {} (owner: {})",
          uiSenderClientID, componentNetID, pObjInfo->m_uiOwnerClientID);
        return;
      }
    }
  }

  // Find the component by network ID using the object manager
  plNetworkComponent* pComponent = nullptr;

  plWorld* pWorld = m_pNetworkModule->GetWorld();
  if (!pWorld)
    return;

  // Try to find the component via the object manager
  const plNetworkObjectInfo* pObjectInfo = m_pNetworkModule->GetObjectManager()->GetObjectInfo(componentNetID);
  if (pObjectInfo)
  {
    plGameObject* pGameObject = nullptr;
    if (pWorld->TryGetObject(pObjectInfo->m_hGameObject, pGameObject))
    {
      plNetworkComponent* pComp = nullptr;
      if (pGameObject->TryGetComponentOfBaseType(pComp))
      {
        pComponent = pComp;
      }
    }
  }

  if (!pComponent)
  {
    plLog::Warning("PL Net: RPC target component {} not found", componentNetID);
    return;
  }

  // Verify the component type matches
  if (!pComponent->GetDynamicRTTI()->IsDerivedFrom(pRpcInfo->m_pComponentType))
  {
    plLog::Warning("PL Net: RPC type mismatch - expected {}, got {}",
      pRpcInfo->m_pComponentType->GetTypeName(), pComponent->GetDynamicRTTI()->GetTypeName());
    return;
  }

  // Check if we should execute this RPC locally
  plUInt32 ownerClientID = 0;
  plNetworkComponent* pNetComp = nullptr;
  if (pComponent->GetOwner()->TryGetComponentOfBaseType(pNetComp) && pNetComp->GetNetID() != 0)
  {
    const plNetworkObjectInfo* pObjInfo = m_pNetworkModule->GetObjectManager()->GetObjectInfo(pNetComp->GetNetID());
    if (pObjInfo)
    {
      ownerClientID = pObjInfo->m_uiOwnerClientID;
    }
  }
  
  if (!ShouldExecuteLocally(*pRpcInfo, uiSenderClientID, ownerClientID))
  {
    // Need to relay this RPC
    if (m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost())
    {
      // Server relays to appropriate targets based on RPC target type
      plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
      if (pPeer)
      {
        // Rebuild the RPC message for relay
        plNetworkMessage relayMsg = pPeer->BeginSendMessage();
        relayMsg.WriteUInt8(plNetworkMessageIDs::ObjectRpc);
        relayMsg.WriteUInt32(pRpcInfo->m_uiRpcID);
        relayMsg.WriteUInt32(componentNetID);
        
        // Re-serialize arguments from current position
        plDynamicArray<plVariant> args;
        DeserializeArguments(msg, pRpcInfo->m_pFunction, args);
        SerializeArguments(relayMsg, pRpcInfo->m_pFunction, plArrayPtr<plVariant>(args.GetData(), args.GetCount()));
        relayMsg.SetChannelType(GetChannelType(pRpcInfo->m_Reliability));
        
        // Send to appropriate targets
        plDynamicArray<plNetworkConnection> targets;
        for (plNetworkClient* pClient : m_pNetworkModule->GetClients())
        {
          if (!pClient || pClient->m_ConnectionState != plNetworkConnectionState::Connected)
            continue;
          
          // Don't send back to original sender
          if (pClient->m_uiClientID == uiSenderClientID)
            continue;
          
          bool shouldSend = false;
          switch (pRpcInfo->m_Target)
          {
            case plNetworkRpcTarget::Owner:
              shouldSend = (pClient->m_uiClientID == ownerClientID);
              break;
            case plNetworkRpcTarget::AllClients:
            case plNetworkRpcTarget::AllClientsAndServer:
              shouldSend = true;
              break;
            default:
              break;
          }
          
          if (shouldSend)
            targets.PushBack(pClient->m_Connection);
        }
        
        if (!targets.IsEmpty())
          pPeer->SendMessages(relayMsg, targets);
      }
    }
    return;
  }

  // Deserialize arguments
  plDynamicArray<plVariant> arguments;
  DeserializeArguments(msg, pRpcInfo->m_pFunction, arguments);

  // Execute the RPC
  plVariant returnValue;
  pRpcInfo->m_pFunction->Execute(pComponent, arguments, returnValue);

  plLog::Dev("PL Net: Executed RPC '{}' on component {}",
    pRpcInfo->m_sFunctionName, componentNetID);
}

bool plNetworkRpcManager::ShouldExecuteLocally(const plNetworkRpcInfo& rpcInfo, plUInt32 uiSenderClientID, plUInt32 uiOwnerClientID) const
{
  if (!m_pNetworkModule)
    return false;

  plUInt32 localClientID = m_pNetworkModule->GetLocalClientID();
  bool isServer = m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost();

  switch (rpcInfo.m_Target)
  {
    case plNetworkRpcTarget::Server:
      return isServer;

    case plNetworkRpcTarget::Owner:
      return localClientID == uiOwnerClientID;

    case plNetworkRpcTarget::AllClients:
      return !isServer || m_pNetworkModule->IsHost();

    case plNetworkRpcTarget::AllClientsAndServer:
      return true;

    case plNetworkRpcTarget::Multicast:
      // For multicast, the targeting is handled elsewhere
      return true;

    default:
      return false;
  }
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
  msg.WriteUInt8(static_cast<plUInt8>(arguments.GetCount()));

  for (plUInt32 i = 0; i < arguments.GetCount(); ++i)
  {
    WriteVariant(msg, arguments[i]);
  }
}

void plNetworkRpcManager::DeserializeArguments(plNetworkMessage& msg, const plAbstractFunctionProperty* pFunction, plDynamicArray<plVariant>& out_arguments)
{
  plUInt8 argCount = msg.ReadUInt8();
  out_arguments.SetCount(argCount);

  for (plUInt8 i = 0; i < argCount; ++i)
  {
    const plRTTI* pExpectedType = pFunction->GetArgumentType(i);
    out_arguments[i] = ReadVariant(msg, pExpectedType);
  }
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

plVariant plNetworkRpcManager::ReadVariant(plNetworkMessage& msg, const plRTTI* pExpectedType)
{
  plVariantType::Enum variantType = static_cast<plVariantType::Enum>(msg.ReadUInt8());

  // Validate variant type is in valid range
  if (variantType < plVariantType::Invalid || variantType >= plVariantType::LastExtendedType)
  {
    plLog::Warning("PL Net: Invalid variant type {} received in RPC - possible corruption or attack", static_cast<int>(variantType));
    return plVariant();
  }

  // Validate type matches expected if we have RTTI info
  // This prevents attackers from sending wrong types to crash or corrupt state
  if (pExpectedType != nullptr)
  {
    plVariantType::Enum expectedVariantType = plVariant::TypeDeduction<void>::value; // Invalid default
    
    // Map common RTTI types to variant types for validation
    if (pExpectedType == plGetStaticRTTI<bool>())
      expectedVariantType = plVariantType::Bool;
    else if (pExpectedType == plGetStaticRTTI<plInt8>())
      expectedVariantType = plVariantType::Int8;
    else if (pExpectedType == plGetStaticRTTI<plUInt8>())
      expectedVariantType = plVariantType::UInt8;
    else if (pExpectedType == plGetStaticRTTI<plInt16>())
      expectedVariantType = plVariantType::Int16;
    else if (pExpectedType == plGetStaticRTTI<plUInt16>())
      expectedVariantType = plVariantType::UInt16;
    else if (pExpectedType == plGetStaticRTTI<plInt32>())
      expectedVariantType = plVariantType::Int32;
    else if (pExpectedType == plGetStaticRTTI<plUInt32>())
      expectedVariantType = plVariantType::UInt32;
    else if (pExpectedType == plGetStaticRTTI<plInt64>())
      expectedVariantType = plVariantType::Int64;
    else if (pExpectedType == plGetStaticRTTI<plUInt64>())
      expectedVariantType = plVariantType::UInt64;
    else if (pExpectedType == plGetStaticRTTI<float>())
      expectedVariantType = plVariantType::Float;
    else if (pExpectedType == plGetStaticRTTI<double>())
      expectedVariantType = plVariantType::Double;
    else if (pExpectedType == plGetStaticRTTI<plVec2>())
      expectedVariantType = plVariantType::Vector2;
    else if (pExpectedType == plGetStaticRTTI<plVec3>())
      expectedVariantType = plVariantType::Vector3;
    else if (pExpectedType == plGetStaticRTTI<plVec4>())
      expectedVariantType = plVariantType::Vector4;
    else if (pExpectedType == plGetStaticRTTI<plQuat>())
      expectedVariantType = plVariantType::Quaternion;
    else if (pExpectedType == plGetStaticRTTI<plString>())
      expectedVariantType = plVariantType::String;
    
    // If we determined an expected type, validate it matches
    if (expectedVariantType != plVariant::TypeDeduction<void>::value && 
        variantType != expectedVariantType)
    {
      // Allow String/StringView interchangeability
      bool isStringCompatible = (expectedVariantType == plVariantType::String || expectedVariantType == plVariantType::StringView) &&
                                (variantType == plVariantType::String || variantType == plVariantType::StringView);
      
      if (!isStringCompatible)
      {
        plLog::Warning("PL Net: RPC argument type mismatch - expected {}, got {}. Possible attack or version mismatch.",
          static_cast<int>(expectedVariantType), static_cast<int>(variantType));
        return plVariant();
      }
    }
  }

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
      return plVariant();
  }
}
