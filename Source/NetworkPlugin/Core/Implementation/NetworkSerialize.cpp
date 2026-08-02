#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkSerialize.h>
#include <Core/World/Component.h>
#include <Foundation/Reflection/ReflectionUtils.h>

// Note: plNetSerializeAttribute is now defined and registered in Foundation/Reflection/Implementation/PropertyAttributes.h/.cpp

//////////////////////////////////////////////////////////////////////////
// Property Cache
//////////////////////////////////////////////////////////////////////////

plMutex plNetworkSerializer::s_CacheMutex;
plMap<const plRTTI*, plDynamicArray<plNetSerializePropertyInfo>> plNetworkSerializer::s_PropertyCache;

void plNetworkSerializer::BuildPropertyCache(const plRTTI* pRtti)
{
  // This is called under lock, so no mutex needed here
  plDynamicArray<plNetSerializePropertyInfo>& properties = s_PropertyCache[pRtti];
  properties.Clear();

  // Get all properties including inherited ones
  plHybridArray<const plAbstractProperty*, 64> allProperties;
  pRtti->GetAllProperties(allProperties);

  for (const plAbstractProperty* pProp : allProperties)
  {
    // Only member properties are supported for now
    if (pProp->GetCategory() != plPropertyCategory::Member)
      continue;

    // Check for NetSerialize attribute
    const plNetSerializeAttribute* pNetAttr = pProp->GetAttributeByType<plNetSerializeAttribute>();
    if (!pNetAttr)
      continue;

    if (properties.GetCount() >= 32)
    {
      plLog::Error("PL Net: Type '{}' has more than 32 replicated properties; additional properties are ignored.", pRtti->GetTypeName());
      break;
    }

    plNetSerializePropertyInfo info;
    info.m_pProperty = static_cast<const plAbstractMemberProperty*>(pProp);
    info.m_pAttribute = pNetAttr;
    info.m_uiPropertyHash = plHashingUtils::StringHash(pProp->GetPropertyName());

    properties.PushBack(info);
  }

  // Sort by priority (higher priority first)
  properties.Sort([](const plNetSerializePropertyInfo& a, const plNetSerializePropertyInfo& b) {
    return a.m_pAttribute->m_iPriority > b.m_pAttribute->m_iPriority;
  });
}

const plDynamicArray<plNetSerializePropertyInfo>* plNetworkSerializer::GetCachedProperties(const plRTTI* pRtti)
{
  if (!pRtti)
    return nullptr;

  PL_LOCK(s_CacheMutex);

  // Check if already cached
  auto it = s_PropertyCache.Find(pRtti);
  if (it.IsValid())
  {
    return &it.Value();
  }

  // Build cache for this type
  BuildPropertyCache(pRtti);
  return &s_PropertyCache[pRtti];
}

bool plNetworkSerializer::HasSerializableProperties(const plRTTI* pRtti)
{
  const auto* pProps = GetCachedProperties(pRtti);
  return pProps && !pProps->IsEmpty();
}

void plNetworkSerializer::ClearCache()
{
  PL_LOCK(s_CacheMutex);
  s_PropertyCache.Clear();
}

//////////////////////////////////////////////////////////////////////////
// Context-Aware Serialization (Bitmask Wire Format)
//////////////////////////////////////////////////////////////////////////

plUInt32 plNetworkSerializer::SerializePropertiesWithContext(const plComponent* pComponent, plNetworkMessage& msg,
  const plNetworkSerializeContext& ctx, plDynamicArray<plVariant>* pLastSentValues)
{
  if (!pComponent)
    return 0;

  const plRTTI* pRtti = pComponent->GetDynamicRTTI();
  const plDynamicArray<plNetSerializePropertyInfo>* pProperties = GetCachedProperties(pRtti);
  if (!pProperties || pProperties->IsEmpty())
  {
    msg.WriteUInt32(0); // Empty bitmask
    return 0;
  }

  plUInt32 propCount = pProperties->GetCount();
  PL_ASSERT_DEV(propCount <= 32, "Context-aware serialization supports at most 32 properties per component.");

  // Initialize last-sent cache if needed
  if (pLastSentValues && pLastSentValues->GetCount() < propCount)
  {
    pLastSentValues->SetCount(propCount);
  }

  // Build bitmask of properties to include
  plUInt32 bitmask = 0;

  for (plUInt32 i = 0; i < propCount; ++i)
  {
    const plNetSerializePropertyInfo& propInfo = (*pProperties)[i];
    plReplicateCondition::Enum condition = propInfo.m_pAttribute->GetReplicateCondition();
    plReplicateTarget::Enum target = propInfo.m_pAttribute->GetReplicateTarget();

    // Check condition
    bool bInclude = true;

    if (condition == plReplicateCondition::InitialOnly)
    {
      // Only include during initial sync (late-joiner)
      bInclude = ctx.m_bIsInitialSync;
    }
    else if (condition == plReplicateCondition::OnChange && pLastSentValues)
    {
      // Compare current value with last-sent value
      plVariant currentValue = plReflectionUtils::GetMemberPropertyValue(propInfo.m_pProperty, pComponent);
      if ((*pLastSentValues)[i].IsValid() && (*pLastSentValues)[i] == currentValue)
      {
        bInclude = false; // Value hasn't changed
      }
    }

    // Check target
    if (bInclude && ctx.m_uiRecipientClientID != 0) // 0 means unspecified / all
    {
      if (target == plReplicateTarget::OwnerOnly)
      {
        bInclude = (ctx.m_uiRecipientClientID == ctx.m_uiOwnerClientID);
      }
      else if (target == plReplicateTarget::NonOwner)
      {
        bInclude = (ctx.m_uiRecipientClientID != ctx.m_uiOwnerClientID);
      }
    }

    if (bInclude)
    {
      bitmask |= (1u << i);
    }
  }

  // Write bitmask
  msg.WriteUInt32(bitmask);

  // Write included property values
  plUInt32 serializedCount = 0;
  for (plUInt32 i = 0; i < propCount; ++i)
  {
    if (!(bitmask & (1u << i)))
      continue;

    const plNetSerializePropertyInfo& propInfo = (*pProperties)[i];
    plVariant value = plReflectionUtils::GetMemberPropertyValue(propInfo.m_pProperty, pComponent);

    if (value.IsValid())
    {
      WriteVariantEncoded(msg, value, propInfo.m_pAttribute->GetEncoding());
      serializedCount++;

      // Update last-sent cache
      if (pLastSentValues)
      {
        (*pLastSentValues)[i] = value;
      }
    }
  }

  return serializedCount;
}

plUInt32 plNetworkSerializer::DeserializePropertiesWithContext(plComponent* pComponent, plNetworkMessage& msg)
{
  if (!pComponent)
    return 0;

  const plRTTI* pRtti = pComponent->GetDynamicRTTI();
  const plDynamicArray<plNetSerializePropertyInfo>* pProperties = GetCachedProperties(pRtti);
  if (!pProperties || pProperties->IsEmpty())
  {
    msg.ReadUInt32(); // consume the empty bitmask
    return 0;
  }

  plUInt32 bitmask = msg.ReadUInt32();
  plUInt32 propCount = pProperties->GetCount();

  plUInt32 deserializedCount = 0;
  for (plUInt32 i = 0; i < propCount; ++i)
  {
    if (!(bitmask & (1u << i)))
      continue;

    if (DeserializeProperty(pComponent, (*pProperties)[i], msg))
    {
      deserializedCount++;
    }
  }

  return deserializedCount;
}

bool plNetworkSerializer::HasPerRecipientProperties(const plRTTI* pRtti)
{
  const auto* pProperties = GetCachedProperties(pRtti);
  if (!pProperties)
    return false;

  for (const plNetSerializePropertyInfo& info : *pProperties)
  {
    if (info.m_pAttribute->GetReplicateTarget() != plReplicateTarget::AllClients)
    {
      return true;
    }
  }
  return false;
}

//////////////////////////////////////////////////////////////////////////
// Legacy Serialization (Count + All Values)
//////////////////////////////////////////////////////////////////////////

plUInt32 plNetworkSerializer::SerializeProperties(const plComponent* pComponent, plNetworkMessage& msg)
{
  if (!pComponent)
    return 0;

  const plRTTI* pRtti = pComponent->GetDynamicRTTI();
  
  const plDynamicArray<plNetSerializePropertyInfo>* pProperties = GetCachedProperties(pRtti);
  if (!pProperties || pProperties->IsEmpty())
    return 0;

  // Write property count
  msg.WriteUInt8(static_cast<plUInt8>(pProperties->GetCount()));

  plUInt32 serializedCount = 0;
  for (const plNetSerializePropertyInfo& propInfo : *pProperties)
  {
    if (SerializeProperty(pComponent, propInfo, msg))
    {
      serializedCount++;
    }
  }

  return serializedCount;
}

plUInt32 plNetworkSerializer::DeserializeProperties(plComponent* pComponent, plNetworkMessage& msg)
{
  if (!pComponent)
    return 0;

  const plRTTI* pRtti = pComponent->GetDynamicRTTI();

  const plDynamicArray<plNetSerializePropertyInfo>* pProperties = GetCachedProperties(pRtti);
  if (!pProperties || pProperties->IsEmpty())
    return 0;

  // Read property count
  plUInt8 propCount = msg.ReadUInt8();
  if (propCount != pProperties->GetCount())
  {
    plLog::Warning("PL Net: Property count mismatch during deserialization (expected {}, got {})",
      pProperties->GetCount(), propCount);
    // Still try to deserialize what we can
  }

  plUInt32 deserializedCount = 0;
  plUInt32 toDeserialize = plMath::Min<plUInt32>(propCount, pProperties->GetCount());
  
  for (plUInt32 i = 0; i < toDeserialize; ++i)
  {
    if (DeserializeProperty(pComponent, (*pProperties)[i], msg))
    {
      deserializedCount++;
    }
  }

  return deserializedCount;
}

//////////////////////////////////////////////////////////////////////////
// Single Property Serialization
//////////////////////////////////////////////////////////////////////////

bool plNetworkSerializer::SerializeProperty(const plComponent* pComponent, const plNetSerializePropertyInfo& propInfo, plNetworkMessage& msg)
{
  const plAbstractMemberProperty* pProp = propInfo.m_pProperty;
  
  // Get the property value as a variant
  plVariant value = plReflectionUtils::GetMemberPropertyValue(pProp, pComponent);
  
  if (!value.IsValid())
  {
    plLog::Warning("PL Net: Failed to get value for property '{}'", pProp->GetPropertyName());
    return false;
  }

  WriteVariantEncoded(msg, value, propInfo.m_pAttribute->GetEncoding());
  return true;
}

bool plNetworkSerializer::DeserializeProperty(plComponent* pComponent, const plNetSerializePropertyInfo& propInfo, plNetworkMessage& msg)
{
  const plAbstractMemberProperty* pProp = propInfo.m_pProperty;
  const plVariantType::Enum expectedType = GetVariantTypeForProperty(pProp);

  if (expectedType == plVariantType::Invalid)
  {
    plLog::Warning("PL Net: Unsupported property type '{}' for property '{}'",
      pProp->GetSpecificType()->GetTypeName(), pProp->GetPropertyName());
    return false;
  }

  plVariant value = ReadVariantEncoded(msg, expectedType, propInfo.m_pAttribute->GetEncoding());
  
  if (!value.IsValid() || msg.HasReadError())
  {
    plLog::Warning("PL Net: Failed to read value for property '{}'", pProp->GetPropertyName());
    return false;
  }

  // Handle enum conversion
  if (pProp->GetFlags().IsSet(plPropertyFlags::IsEnum) && value.IsA<plInt64>())
  {
    // For enums, we need to set the value differently
    plReflectionUtils::SetMemberPropertyValue(pProp, pComponent, value.ConvertTo<plInt64>());
  }
  else
  {
    plReflectionUtils::SetMemberPropertyValue(pProp, pComponent, value);
  }
  
  return true;
}

plVariantType::Enum plNetworkSerializer::GetVariantTypeForProperty(const plAbstractMemberProperty* pProp)
{
  if (pProp == nullptr)
    return plVariantType::Invalid;

  const plRTTI* pPropType = pProp->GetSpecificType();
  if (pPropType == plGetStaticRTTI<bool>())
    return plVariantType::Bool;
  else if (pPropType == plGetStaticRTTI<plInt8>())
    return plVariantType::Int8;
  else if (pPropType == plGetStaticRTTI<plUInt8>())
    return plVariantType::UInt8;
  else if (pPropType == plGetStaticRTTI<plInt16>())
    return plVariantType::Int16;
  else if (pPropType == plGetStaticRTTI<plUInt16>())
    return plVariantType::UInt16;
  else if (pPropType == plGetStaticRTTI<plInt32>())
    return plVariantType::Int32;
  else if (pPropType == plGetStaticRTTI<plUInt32>())
    return plVariantType::UInt32;
  else if (pPropType == plGetStaticRTTI<plInt64>())
    return plVariantType::Int64;
  else if (pPropType == plGetStaticRTTI<plUInt64>())
    return plVariantType::UInt64;
  else if (pPropType == plGetStaticRTTI<float>())
    return plVariantType::Float;
  else if (pPropType == plGetStaticRTTI<double>())
    return plVariantType::Double;
  else if (pPropType == plGetStaticRTTI<plVec2>())
    return plVariantType::Vector2;
  else if (pPropType == plGetStaticRTTI<plVec3>())
    return plVariantType::Vector3;
  else if (pPropType == plGetStaticRTTI<plVec4>())
    return plVariantType::Vector4;
  else if (pPropType == plGetStaticRTTI<plQuat>())
    return plVariantType::Quaternion;
  else if (pPropType == plGetStaticRTTI<plString>())
    return plVariantType::String;
  else if (pPropType == plGetStaticRTTI<plColor>())
    return plVariantType::Color;
  else if (pProp->GetFlags().IsSet(plPropertyFlags::IsEnum))
    return plVariantType::Int64;

  return plVariantType::Invalid;
}

bool plNetworkSerializer::IsVariantTypeSupported(plVariantType::Enum type)
{
  return type == plVariantType::Bool || type == plVariantType::Int8 || type == plVariantType::UInt8 ||
         type == plVariantType::Int16 || type == plVariantType::UInt16 || type == plVariantType::Int32 ||
         type == plVariantType::UInt32 || type == plVariantType::Int64 || type == plVariantType::UInt64 ||
         type == plVariantType::Float || type == plVariantType::Double || type == plVariantType::Vector2 ||
         type == plVariantType::Vector3 || type == plVariantType::Vector4 || type == plVariantType::Quaternion ||
         type == plVariantType::String || type == plVariantType::Color;
}

//////////////////////////////////////////////////////////////////////////
// Encoded Read/Write
//////////////////////////////////////////////////////////////////////////

void plNetworkSerializer::WriteVariantEncoded(plNetworkMessage& msg, const plVariant& value, plNetSerializeEncoding::Enum encoding)
{
  switch (value.GetType())
  {
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
      if (encoding == plNetSerializeEncoding::Half)
        msg.WriteHalf(value.Get<float>());
      else
        msg.WriteFloat(value.Get<float>());
      break;

    case plVariantType::Double:
      msg.WriteDouble(value.Get<double>());
      break;

    case plVariantType::Vector2:
      if (encoding == plNetSerializeEncoding::Half)
      {
        plVec2 v = value.Get<plVec2>();
        msg.WriteHalf(v.x);
        msg.WriteHalf(v.y);
      }
      else
      {
        msg.WriteVec2(value.Get<plVec2>());
      }
      break;

    case plVariantType::Vector3:
      if (encoding == plNetSerializeEncoding::Half)
        msg.WriteVec3Half(value.Get<plVec3>());
      else
        msg.WriteVec3(value.Get<plVec3>());
      break;

    case plVariantType::Vector4:
      if (encoding == plNetSerializeEncoding::Half)
      {
        plVec4 v = value.Get<plVec4>();
        msg.WriteHalf(v.x);
        msg.WriteHalf(v.y);
        msg.WriteHalf(v.z);
        msg.WriteHalf(v.w);
      }
      else
      {
        msg.WriteVec4(value.Get<plVec4>());
      }
      break;

    case plVariantType::Quaternion:
      if (encoding == plNetSerializeEncoding::SmallestThree)
        msg.WriteQuatSmallestThree(value.Get<plQuat>());
      else
        msg.WriteQuat(value.Get<plQuat>());
      break;

    case plVariantType::String:
      msg.WriteString(value.Get<plString>());
      break;

    case plVariantType::Color:
    {
      plColor c = value.Get<plColor>();
      msg.WriteFloat(c.r);
      msg.WriteFloat(c.g);
      msg.WriteFloat(c.b);
      msg.WriteFloat(c.a);
      break;
    }

    default:
      plLog::Warning("PL Net: Unsupported variant type {} for network serialization", static_cast<int>(value.GetType()));
      break;
  }
}

plVariant plNetworkSerializer::ReadVariantEncoded(plNetworkMessage& msg, plVariantType::Enum expectedType, plNetSerializeEncoding::Enum encoding)
{
  switch (expectedType)
  {
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
      if (encoding == plNetSerializeEncoding::Half)
        return plVariant(msg.ReadHalf());
      else
        return plVariant(msg.ReadFloat());

    case plVariantType::Double:
      return plVariant(msg.ReadDouble());

    case plVariantType::Vector2:
      if (encoding == plNetSerializeEncoding::Half)
      {
        float x = msg.ReadHalf();
        float y = msg.ReadHalf();
        return plVariant(plVec2(x, y));
      }
      else
      {
        return plVariant(msg.ReadVec2());
      }

    case plVariantType::Vector3:
      if (encoding == plNetSerializeEncoding::Half)
        return plVariant(msg.ReadVec3Half());
      else
        return plVariant(msg.ReadVec3());

    case plVariantType::Vector4:
      if (encoding == plNetSerializeEncoding::Half)
      {
        float x = msg.ReadHalf();
        float y = msg.ReadHalf();
        float z = msg.ReadHalf();
        float w = msg.ReadHalf();
        return plVariant(plVec4(x, y, z, w));
      }
      else
      {
        return plVariant(msg.ReadVec4());
      }

    case plVariantType::Quaternion:
      if (encoding == plNetSerializeEncoding::SmallestThree)
        return plVariant(msg.ReadQuatSmallestThree());
      else
        return plVariant(msg.ReadQuat());

    case plVariantType::String:
      return plVariant(msg.ReadString());

    case plVariantType::Color:
    {
      float r = msg.ReadFloat();
      float g = msg.ReadFloat();
      float b = msg.ReadFloat();
      float a = msg.ReadFloat();
      return plVariant(plColor(r, g, b, a));
    }

    default:
      plLog::Warning("PL Net: Unsupported variant type {} for network deserialization", static_cast<int>(expectedType));
      return plVariant();
  }
}
