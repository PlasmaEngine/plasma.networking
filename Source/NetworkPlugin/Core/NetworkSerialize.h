#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkMessage.h>

#include <Foundation/Containers/Map.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/Variant.h>

// plNetSerializeAttribute and plNetSerializeEncoding are defined in Foundation/Reflection/Implementation/PropertyAttributes.h
// and included via Foundation/Reflection/Reflection.h

class plComponent;
class plNetworkMessage;

/// @brief Context for replication-condition-aware serialization
struct PL_NETWORKPLUGIN_DLL plNetworkSerializeContext
{
  bool m_bIsInitialSync = false;       ///< True during SyncComponentStateToClient (late-joiner)
  plUInt32 m_uiRecipientClientID = 0;  ///< The client we are serializing for (0 = all / unspecified)
  plUInt32 m_uiOwnerClientID = 0;      ///< The owner of the network object (from ObjectManager)
};

/// @brief Information about a serializable property discovered via reflection
struct PL_NETWORKPLUGIN_DLL plNetSerializePropertyInfo
{
  const plAbstractMemberProperty* m_pProperty = nullptr;
  const plNetSerializeAttribute* m_pAttribute = nullptr;
  plUInt32 m_uiPropertyHash = 0;  ///< Hash for delta tracking
};

/// @brief Handles automatic serialization of properties marked with plNetSerializeAttribute
/// 
/// This class provides reflection-based serialization for network components,
/// eliminating the need for manual NetworkSerialize/NetworkDeserialize overrides.
/// 
/// Property discovery is cached per-type on first access for optimal runtime performance.
///
/// Wire format (with context): [uint32 bitmask] [values for set bits, in property-list order]
/// Wire format (legacy, no context): [uint8 count] [all values in property-list order]
class PL_NETWORKPLUGIN_DLL plNetworkSerializer
{
public:
  /// @brief Serialize all properties with plNetSerializeAttribute on a component (legacy, no filtering)
  /// @param pComponent The component to serialize
  /// @param msg The network message to write to
  /// @return Number of properties serialized
  static plUInt32 SerializeProperties(const plComponent* pComponent, plNetworkMessage& msg);

  /// @brief Deserialize all properties with plNetSerializeAttribute on a component (legacy, no filtering)
  /// @param pComponent The component to deserialize into
  /// @param msg The network message to read from
  /// @return Number of properties deserialized
  static plUInt32 DeserializeProperties(plComponent* pComponent, plNetworkMessage& msg);

  /// @brief Serialize properties with replication condition/target filtering
  /// Uses bitmask wire format: [uint32 bitmask] [included property values]
  /// @param pComponent The component to serialize
  /// @param msg The network message to write to
  /// @param ctx Serialization context (initial sync, recipient, owner)
  /// @param pLastSentValues Optional last-sent values cache for OnChange tracking (updated in place)
  /// @return Number of properties serialized
  static plUInt32 SerializePropertiesWithContext(const plComponent* pComponent, plNetworkMessage& msg,
    const plNetworkSerializeContext& ctx, plDynamicArray<plVariant>* pLastSentValues = nullptr);

  /// @brief Deserialize properties from bitmask wire format
  /// Reads: [uint32 bitmask] [values for set bits]
  /// @param pComponent The component to deserialize into
  /// @param msg The network message to read from
  /// @return Number of properties deserialized
  static plUInt32 DeserializePropertiesWithContext(plComponent* pComponent, plNetworkMessage& msg);

  /// @brief Check if a component type has any OwnerOnly or NonOwner properties
  /// Used by the replication loop to decide if per-recipient serialization is needed
  static bool HasPerRecipientProperties(const plRTTI* pRtti);

  /// @brief Get all serializable properties for a component type (cached after first call)
  /// @param pRtti The RTTI of the component type
  /// @return Pointer to cached property list, or nullptr if type has no serializable properties
  static const plDynamicArray<plNetSerializePropertyInfo>* GetCachedProperties(const plRTTI* pRtti);

  /// @brief Check if a component type has any serializable properties (cached after first call)
  static bool HasSerializableProperties(const plRTTI* pRtti);

  /// @brief Clear the property cache (call on hot-reload if RTTI changes)
  static void ClearCache();

  /// @brief Return the plVariant type used to serialize a reflected member property.
  static plVariantType::Enum GetVariantTypeForProperty(const plAbstractMemberProperty* pProperty);

  static bool IsVariantTypeSupported(plVariantType::Enum type);

  /// @brief Encode/decode helpers shared by data-driven replication components.
  static void WriteVariantEncoded(plNetworkMessage& msg, const plVariant& value, plNetSerializeEncoding::Enum encoding);
  static plVariant ReadVariantEncoded(plNetworkMessage& msg, plVariantType::Enum expectedType, plNetSerializeEncoding::Enum encoding);

private:
  /// @brief Build the property list for a type (internal, populates cache)
  static void BuildPropertyCache(const plRTTI* pRtti);

  /// @brief Serialize a single property value to the message
  static bool SerializeProperty(const plComponent* pComponent, const plNetSerializePropertyInfo& propInfo, plNetworkMessage& msg);

  /// @brief Deserialize a single property value from the message
  static bool DeserializeProperty(plComponent* pComponent, const plNetSerializePropertyInfo& propInfo, plNetworkMessage& msg);

  /// @brief Mutex protecting the property cache
  static plMutex s_CacheMutex;

  /// @brief Cached property lists per RTTI type (empty array = no properties, not in map = not cached yet)
  static plMap<const plRTTI*, plDynamicArray<plNetSerializePropertyInfo>> s_PropertyCache;
};

/// @brief Helper macros for common network property patterns
/// These macros add plHiddenAttribute by default so properties don't clutter the editor
#define PL_NET_PROPERTY(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute(), new plHiddenAttribute())

#define PL_NET_PROPERTY_HALF(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute(plNetSerializeEncoding::Half), new plHiddenAttribute())

#define PL_NET_PROPERTY_QUAT(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute(plNetSerializeEncoding::SmallestThree), new plHiddenAttribute())

/// @brief Visible variants that show in the editor (for debugging or editable network properties)
#define PL_NET_PROPERTY_VISIBLE(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute())

#define PL_NET_PROPERTY_HALF_VISIBLE(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute(plNetSerializeEncoding::Half))

#define PL_NET_PROPERTY_QUAT_VISIBLE(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute(plNetSerializeEncoding::SmallestThree))

/// @brief Property replicated only during initial sync (late-joiner)
#define PL_NET_PROPERTY_INITIAL_ONLY(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute(\
    plNetSerializeEncoding::Default, 128, plReplicateCondition::InitialOnly, plReplicateTarget::AllClients), new plHiddenAttribute())

/// @brief Property replicated only when value changes
#define PL_NET_PROPERTY_ON_CHANGE(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute(\
    plNetSerializeEncoding::Default, 128, plReplicateCondition::OnChange, plReplicateTarget::AllClients), new plHiddenAttribute())

/// @brief Property replicated only to the owning client
#define PL_NET_PROPERTY_OWNER_ONLY(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute(\
    plNetSerializeEncoding::Default, 128, plReplicateCondition::Always, plReplicateTarget::OwnerOnly), new plHiddenAttribute())

/// @brief Property replicated to all clients except the owner
#define PL_NET_PROPERTY_NON_OWNER(name, member) \
  PL_MEMBER_PROPERTY(name, member)->AddAttributes(new plNetSerializeAttribute(\
    plNetSerializeEncoding::Default, 128, plReplicateCondition::Always, plReplicateTarget::NonOwner), new plHiddenAttribute())
