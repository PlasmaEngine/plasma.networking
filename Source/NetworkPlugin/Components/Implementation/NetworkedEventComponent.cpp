#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Components/NetworkedEventComponent.h>
#include <NetworkPlugin/Core/NetworkRpc.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plNetworkedEventComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(FireEventNoParam, In, "EventName"),
    PL_SCRIPT_FUNCTION_PROPERTY(FireEventBool, In, "EventName", In, "Value"),
    PL_SCRIPT_FUNCTION_PROPERTY(FireEventInt, In, "EventName", In, "Value"),
    PL_SCRIPT_FUNCTION_PROPERTY(FireEventUInt, In, "EventName", In, "Value"),
    PL_SCRIPT_FUNCTION_PROPERTY(FireEventFloat, In, "EventName", In, "Value"),
    PL_SCRIPT_FUNCTION_PROPERTY(FireEventDouble, In, "EventName", In, "Value"),
    PL_SCRIPT_FUNCTION_PROPERTY(FireEventString, In, "EventName", In, "Value"),
    PL_SCRIPT_FUNCTION_PROPERTY(FireEventVec3, In, "EventName", In, "Value"),

    // Reflected so the RPC manager can find and invoke it, but no plNetworkRpcAttribute
    // in reflection to avoid crashes in the editor's phantom type / Visual Script system.
    // RPC registration is done manually in OnSimulationStarted.
    PL_FUNCTION_PROPERTY(AllClients_OnEvent),
  }
  PL_END_FUNCTIONS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Networking"),
  }
  PL_END_ATTRIBUTES;
  PL_BEGIN_MESSAGESENDERS
  {
    PL_MESSAGE_SENDER(m_NetworkedEventSender)
  }
  PL_END_MESSAGESENDERS;
}
PL_END_COMPONENT_TYPE
// clang-format on

void plNetworkedEventComponent::OnSimulationStarted()
{
  plNetworkComponent::OnSimulationStarted();

  // Manually register the RPC (not via attribute to avoid phantom type issues in the editor)
  if (m_pNetworkModule)
  {
    m_pNetworkModule->GetRpcManager()->RegisterRpc(
      GetDynamicRTTI(),
      "AllClients_OnEvent",
      plNetworkRpcTarget::AllClientsAndServer,
      plNetworkRpcReliability::ReliableOrdered);
  }
}

void plNetworkedEventComponent::FireEvent(const plString& sEventName, const plVariant& param1, const plVariant& param2)
{
  plInt32 iType1 = 0;
  plString sParam1Raw;
  plInt32 iType2 = 0;
  plString sParam2Raw;

  EncodeVariant(param1, iType1, sParam1Raw);
  EncodeVariant(param2, iType2, sParam2Raw);

  PL_CALL_RPC(this, AllClients_OnEvent, sEventName, iType1, sParam1Raw, iType2, sParam2Raw);
}

void plNetworkedEventComponent::FireEventNoParam(plStringView sEventName)
{
  FireEvent(plString(sEventName));
}

void plNetworkedEventComponent::FireEventBool(plStringView sEventName, bool bValue)
{
  FireEvent(plString(sEventName), plVariant(bValue));
}

void plNetworkedEventComponent::FireEventInt(plStringView sEventName, plInt32 iValue)
{
  FireEvent(plString(sEventName), plVariant(iValue));
}

void plNetworkedEventComponent::FireEventUInt(plStringView sEventName, plUInt32 uiValue)
{
  FireEvent(plString(sEventName), plVariant(uiValue));
}

void plNetworkedEventComponent::FireEventFloat(plStringView sEventName, float fValue)
{
  FireEvent(plString(sEventName), plVariant(fValue));
}

void plNetworkedEventComponent::FireEventDouble(plStringView sEventName, double fValue)
{
  FireEvent(plString(sEventName), plVariant(fValue));
}

void plNetworkedEventComponent::FireEventString(plStringView sEventName, plStringView sValue)
{
  FireEvent(plString(sEventName), plVariant(plString(sValue)));
}

void plNetworkedEventComponent::FireEventVec3(plStringView sEventName, const plVec3& vValue)
{
  FireEvent(plString(sEventName), plVariant(vValue));
}

void plNetworkedEventComponent::AllClients_OnEvent(plString sEventName, plInt32 iType1, plString sParam1Raw, plInt32 iType2, plString sParam2Raw)
{
  plVariant p1 = DecodeVariant(iType1, sParam1Raw);
  plVariant p2 = DecodeVariant(iType2, sParam2Raw);

  plMsgNetworkedEvent msg;
  msg.m_sEventName = sEventName;
  msg.m_Param1 = p1;
  msg.m_Param2 = p2;

  m_NetworkedEventSender.SendEventMessage(msg, this, GetOwner());

  if (m_EventReceivedCallback.IsValid())
  {
    m_EventReceivedCallback(sEventName, p1, p2);
  }
}

//////////////////////////////////////////////////////////////////////////
// Variant encoding: type tag + string for safe RPC transport
// We encode into (Int32 typeTag, String rawValue) so the RPC system
// only needs to transport Int32 and String, which it already supports.
//////////////////////////////////////////////////////////////////////////

void plNetworkedEventComponent::EncodeVariant(const plVariant& value, plInt32& out_iType, plString& out_sRaw)
{
  if (!value.IsValid())
  {
    out_iType = 0; // 0 = invalid / none
    out_sRaw = "";
    return;
  }

  out_iType = static_cast<plInt32>(value.GetType());

  plStringBuilder sb;

  switch (value.GetType())
  {
    case plVariantType::Bool:
      sb.SetFormat("{}", value.Get<bool>() ? 1 : 0);
      break;
    case plVariantType::Int32:
      sb.SetFormat("{}", value.Get<plInt32>());
      break;
    case plVariantType::UInt32:
      sb.SetFormat("{}", value.Get<plUInt32>());
      break;
    case plVariantType::Float:
      sb.SetFormat("{}", value.Get<float>());
      break;
    case plVariantType::Double:
      sb.SetFormat("{}", value.Get<double>());
      break;
    case plVariantType::String:
      sb = value.Get<plString>();
      break;
    case plVariantType::Vector3:
    {
      plVec3 v = value.Get<plVec3>();
      sb.SetFormat("{},{},{}", v.x, v.y, v.z);
      break;
    }
    default:
      plLog::Warning("PL Net EventComponent: Unsupported variant type {} for event parameter.", static_cast<int>(value.GetType()));
      out_iType = 0;
      out_sRaw = "";
      return;
  }

  out_sRaw = sb;
}

plVariant plNetworkedEventComponent::DecodeVariant(plInt32 iType, const plString& sRaw)
{
  if (iType == 0)
    return plVariant(); // Invalid

  plVariantType::Enum type = static_cast<plVariantType::Enum>(iType);

  switch (type)
  {
    case plVariantType::Bool:
    {
      plInt32 val = 0;
      plConversionUtils::StringToInt(sRaw, val).IgnoreResult();
      return plVariant(val != 0);
    }
    case plVariantType::Int32:
    {
      plInt32 val = 0;
      plConversionUtils::StringToInt(sRaw, val).IgnoreResult();
      return plVariant(val);
    }
    case plVariantType::UInt32:
    {
      plUInt32 val = 0;
      plConversionUtils::StringToUInt(sRaw, val).IgnoreResult();
      return plVariant(val);
    }
    case plVariantType::Float:
    {
      double val = 0.0;
      plConversionUtils::StringToFloat(sRaw, val).IgnoreResult();
      return plVariant(static_cast<float>(val));
    }
    case plVariantType::Double:
    {
      double val = 0.0;
      plConversionUtils::StringToFloat(sRaw, val).IgnoreResult();
      return plVariant(val);
    }
    case plVariantType::String:
      return plVariant(sRaw);
    case plVariantType::Vector3:
    {
      // Parse "x,y,z"
      plStringBuilder sb = sRaw;
      plHybridArray<plStringView, 3> parts;
      sb.Split(false, parts, ",");
      if (parts.GetCount() == 3)
      {
        double x = 0, y = 0, z = 0;
        plConversionUtils::StringToFloat(parts[0], x).IgnoreResult();
        plConversionUtils::StringToFloat(parts[1], y).IgnoreResult();
        plConversionUtils::StringToFloat(parts[2], z).IgnoreResult();
        return plVariant(plVec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
      }
      return plVariant();
    }
    default:
      return plVariant();
  }
}
