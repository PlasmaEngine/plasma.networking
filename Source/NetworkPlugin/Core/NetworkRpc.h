#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkTypes.h>
#include <NetworkPlugin/Core/NetworkMessage.h>

#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Types/Variant.h>

#include <type_traits>

class plNetworkWorldModule;
class plComponent;
class plNetworkComponent;
class plNetworkPeer;

/// @brief RPC execution target - which machines should execute the RPC
struct plNetworkRpcTarget
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    Server = 0,       ///< Execute on server only
    Owner,            ///< Execute on owning client only
    AllClients,       ///< Execute on all clients (not server)
    AllClientsAndServer, ///< Execute on all machines
    Multicast,        ///< Execute on specific list of clients
    Default = Server
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkRpcTarget);

/// @brief RPC reliability mode
struct plNetworkRpcReliability
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    Unreliable = 0,
    UnreliableOrdered,
    Reliable,
    ReliableOrdered,
    Default = ReliableOrdered
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkRpcReliability);

/// @brief Attribute to mark a function as a Network RPC
/// Usage: PL_NETWORK_RPC(MyFunction, plNetworkRpcTarget::Server, plNetworkRpcReliability::Reliable)
class PL_NETWORKPLUGIN_DLL plNetworkRpcAttribute : public plPropertyAttribute
{
  PL_ADD_DYNAMIC_REFLECTION(plNetworkRpcAttribute, plPropertyAttribute);

public:
  plNetworkRpcAttribute() = default;
  plNetworkRpcAttribute(plNetworkRpcTarget::Enum target, plNetworkRpcReliability::Enum reliability = plNetworkRpcReliability::ReliableOrdered)
    : m_Target(target)
    , m_Reliability(reliability)
  {
  }

  plNetworkRpcTarget::Enum m_Target = plNetworkRpcTarget::Server;
  plNetworkRpcReliability::Enum m_Reliability = plNetworkRpcReliability::ReliableOrdered;
};

/// @brief Information about a registered RPC
struct PL_NETWORKPLUGIN_DLL plNetworkRpcInfo
{
  plUInt32 m_uiRpcID = 0;                      ///< Unique RPC identifier
  plStringView m_sFunctionName;                ///< Function name for lookup
  const plRTTI* m_pComponentType = nullptr;    ///< Component type that owns this RPC
  const plAbstractFunctionProperty* m_pFunction = nullptr; ///< Function property from reflection
  plNetworkRpcTarget::Enum m_Target = plNetworkRpcTarget::Server;
  plNetworkRpcReliability::Enum m_Reliability = plNetworkRpcReliability::ReliableOrdered;
};

/// @brief Manages RPC registration, invocation, and message handling
/// 
/// This system provides:
/// - Automatic RPC discovery via reflection attributes
/// - Type-safe parameter serialization using variants
/// - Server/client/multicast targeting
/// - Reliable and unreliable delivery options
class PL_NETWORKPLUGIN_DLL plNetworkRpcManager
{
public:
  plNetworkRpcManager();
  ~plNetworkRpcManager();

  /// @brief Initialize the RPC manager with the network module
  void Initialize(plNetworkWorldModule* pNetworkModule);

  /// @brief Shutdown and cleanup
  void Shutdown();

  //////////////////////////////////////////////////////////////////////////
  // RPC Registration
  //////////////////////////////////////////////////////////////////////////

  /// @brief Scan a component type for RPC functions and register them
  void RegisterComponentRpcs(const plRTTI* pComponentType);

  /// @brief Manually register an RPC for a component type
  /// Use this when the function property doesn't carry plNetworkRpcAttribute (e.g. plugin attribute not available in editor)
  void RegisterRpc(const plRTTI* pComponentType, plStringView sFunctionName,
    plNetworkRpcTarget::Enum target,
    plNetworkRpcReliability::Enum reliability = plNetworkRpcReliability::ReliableOrdered);

  /// @brief Register all RPCs from all known network component types
  void RegisterAllRpcs();

  /// @brief Get RPC info by ID
  const plNetworkRpcInfo* GetRpcInfo(plUInt32 uiRpcID) const;

  /// @brief Get RPC info by function name and component type
  const plNetworkRpcInfo* GetRpcInfo(const plRTTI* pComponentType, plStringView sFunctionName) const;

  //////////////////////////////////////////////////////////////////////////
  // RPC Invocation
  //////////////////////////////////////////////////////////////////////////

  /// @brief Call an RPC on a network component
  /// @param pComponent The component to invoke the RPC on
  /// @param sFunctionName The name of the RPC function
  /// @param arguments Array of variant arguments
  /// @return True if the RPC was successfully queued for sending
  bool CallRpc(plNetworkComponent* pComponent, plStringView sFunctionName, plArrayPtr<plVariant> arguments);

  /// @brief Call an RPC with specific targets (for multicast)
  bool CallRpc(plNetworkComponent* pComponent, plStringView sFunctionName, 
               plArrayPtr<plVariant> arguments, const plDynamicArray<plUInt32>& targetClients);

  /// @brief Call an RPC on any component with an explicit net ID
  /// @param pComponent The component to invoke the RPC on
  /// @param uiNetID The network ID for this component
  /// @param sFunctionName The name of the RPC function
  /// @param arguments Array of variant arguments
  /// @return True if the RPC was successfully queued for sending
  bool CallRpc(plComponent* pComponent, plUInt32 uiNetID, plStringView sFunctionName, plArrayPtr<plVariant> arguments);

  /// @brief Call an RPC on any component with an explicit net ID and specific targets
  bool CallRpc(plComponent* pComponent, plUInt32 uiNetID, plStringView sFunctionName, 
               plArrayPtr<plVariant> arguments, const plDynamicArray<plUInt32>& targetClients);

  /// @brief Convenience template for calling RPCs with automatic variant conversion
  /// Note: Uses enable_if to avoid conflicting with explicit overloads
  template <typename... Args, 
    typename = std::enable_if_t<(sizeof...(Args) > 0) && 
      !std::is_same_v<std::tuple<std::decay_t<Args>...>, std::tuple<plArrayPtr<plVariant>, plDynamicArray<plUInt32>>>>>
  bool CallRpc(plNetworkComponent* pComponent, plStringView sFunctionName, Args&&... args)
  {
    plVariant arguments[] = {plVariant(std::forward<Args>(args))...};
    return CallRpc(pComponent, sFunctionName, plArrayPtr<plVariant>(arguments, sizeof...(Args)));
  }

  /// @brief Convenience template for calling RPCs on any component with an explicit net ID
  template <typename... Args, 
    typename = std::enable_if_t<(sizeof...(Args) > 0) && 
      !std::is_same_v<std::tuple<std::decay_t<Args>...>, std::tuple<plArrayPtr<plVariant>, plDynamicArray<plUInt32>>>>>
  bool CallRpc(plComponent* pComponent, plUInt32 uiNetID, plStringView sFunctionName, Args&&... args)
  {
    plVariant arguments[] = {plVariant(std::forward<Args>(args))...};
    return CallRpc(pComponent, uiNetID, sFunctionName, plArrayPtr<plVariant>(arguments, sizeof...(Args)));
  }

  //////////////////////////////////////////////////////////////////////////
  // Message Handling
  //////////////////////////////////////////////////////////////////////////

  /// @brief Handle incoming RPC message
  void HandleRpcMessage(plNetworkMessage& msg, plUInt32 uiSenderClientID);

private:
  /// @brief Generate unique RPC ID from component type and function name
  plUInt32 GenerateRpcID(const plRTTI* pComponentType, plStringView sFunctionName);

  /// @brief Serialize RPC arguments to network message
  void SerializeArguments(plNetworkMessage& msg, const plAbstractFunctionProperty* pFunction, plArrayPtr<plVariant> arguments);

  /// @brief Deserialize RPC arguments from network message
  void DeserializeArguments(plNetworkMessage& msg, const plAbstractFunctionProperty* pFunction, plDynamicArray<plVariant>& out_arguments);

  /// @brief Write a variant to network message
  void WriteVariant(plNetworkMessage& msg, const plVariant& value);

  /// @brief Read a variant from network message
  plVariant ReadVariant(plNetworkMessage& msg, const plRTTI* pExpectedType);

  /// @brief Check if the local machine should execute this RPC
  bool ShouldExecuteLocally(const plNetworkRpcInfo& rpcInfo, plUInt32 uiSenderClientID, plUInt32 uiOwnerClientID) const;

  /// @brief Get the channel type for RPC reliability
  plNetworkChannelTypeEnum::Enum GetChannelType(plNetworkRpcReliability::Enum reliability) const;

private:
  plNetworkWorldModule* m_pNetworkModule = nullptr;

  /// @brief All registered RPCs by ID
  plHashTable<plUInt32, plNetworkRpcInfo> m_RegisteredRpcs;

  /// @brief RPC lookup by component type + function name hash
  plHashTable<plUInt64, plUInt32> m_RpcLookup;

  /// @brief Counter for RPC IDs (could also use hash-based IDs)
  plUInt32 m_uiNextRpcID = 1;
};

//////////////////////////////////////////////////////////////////////////
// RPC Macro Helpers
//////////////////////////////////////////////////////////////////////////

/// @brief Declare an RPC function in a component header
/// Usage in class declaration: PL_DECLARE_NETWORK_RPC(MyRpcFunction, plVec3, float)
#define PL_DECLARE_NETWORK_RPC(FunctionName, ...) \
  void FunctionName(__VA_ARGS__)

/// @brief Helper to call an RPC from a network component (requires component to inherit from plNetworkComponent)
/// Usage: PL_CALL_RPC(this, MyRpcFunction, position, damage)
#define PL_CALL_RPC(Component, FunctionName, ...) \
  do { \
    if (auto* pNetModule = (Component)->GetWorld()->GetModule<plNetworkWorldModule>()) { \
      pNetModule->GetRpcManager()->CallRpc((Component), #FunctionName, ##__VA_ARGS__); \
    } \
  } while(0)

/// @brief Helper to call an RPC from any component with a GetNetID() method
/// Usage: PL_CALL_RPC_EX(this, MyRpcFunction, position, damage)
#define PL_CALL_RPC_EX(Component, FunctionName, ...) \
  do { \
    if (auto* pNetModule = (Component)->GetWorld()->GetModule<plNetworkWorldModule>()) { \
      pNetModule->GetRpcManager()->CallRpc(static_cast<plComponent*>(Component), (Component)->GetNetID(), #FunctionName, ##__VA_ARGS__); \
    } \
  } while(0)

/// @brief Helper to call a server RPC specifically
#define PL_CALL_SERVER_RPC(Component, FunctionName, ...) \
  PL_CALL_RPC(Component, FunctionName, ##__VA_ARGS__)

/// @brief Helper to call a server RPC from any component with GetNetID()
#define PL_CALL_SERVER_RPC_EX(Component, FunctionName, ...) \
  PL_CALL_RPC_EX(Component, FunctionName, ##__VA_ARGS__)

/// @brief Helper to call an RPC on the owning client
#define PL_CALL_CLIENT_RPC(Component, FunctionName, ...) \
  PL_CALL_RPC(Component, FunctionName, ##__VA_ARGS__)

/// @brief Helper to call an RPC on the owning client from any component with GetNetID()
#define PL_CALL_CLIENT_RPC_EX(Component, FunctionName, ...) \
  PL_CALL_RPC_EX(Component, FunctionName, ##__VA_ARGS__)
