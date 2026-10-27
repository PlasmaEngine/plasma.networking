#include <NetworkPlugin/NetworkPluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <NetworkPlugin/Core/NetworkVariant.h>

namespace
{
  bool ValidateNetworkString(plNetworkMessage& msg)
  {
    const plUInt32 uiLength = msg.ReadUInt32();
    return !msg.HasReadError() && msg.SkipBytes(uiLength) != nullptr;
  }

  bool ValidateNetworkVariant(plNetworkMessage& msg, plUInt32 uiDepth, plUInt32& ref_uiBudget)
  {
    if (uiDepth > 16 || ref_uiBudget == 0)
      return false;
    --ref_uiBudget;

    const plUInt8 uiVersion = msg.ReadUInt8();
    const auto type = static_cast<plVariantType::Enum>(msg.ReadUInt8());
    if (msg.HasReadError() || uiVersion != plGetStaticRTTI<plVariant>()->GetTypeVersion())
      return false;

    plUInt32 uiBytes = 0;
    switch (type)
    {
      case plVariantType::Invalid: return true;
      case plVariantType::Bool:
      case plVariantType::Int8:
      case plVariantType::UInt8: uiBytes = 1; break;
      case plVariantType::Int16:
      case plVariantType::UInt16: uiBytes = 2; break;
      case plVariantType::Int32:
      case plVariantType::UInt32:
      case plVariantType::Float:
      case plVariantType::Angle:
      case plVariantType::ColorGamma: uiBytes = 4; break;
      case plVariantType::Int64:
      case plVariantType::UInt64:
      case plVariantType::Double:
      case plVariantType::Time:
      case plVariantType::TempHashedString:
      case plVariantType::Vector2:
      case plVariantType::Vector2I:
      case plVariantType::Vector2U: uiBytes = 8; break;
      case plVariantType::Vector3:
      case plVariantType::Vector3I:
      case plVariantType::Vector3U: uiBytes = 12; break;
      case plVariantType::Color:
      case plVariantType::Vector4:
      case plVariantType::Vector4I:
      case plVariantType::Vector4U:
      case plVariantType::Quaternion:
      case plVariantType::Uuid: uiBytes = 16; break;
      case plVariantType::Matrix3: uiBytes = 36; break;
      case plVariantType::Matrix4: uiBytes = 64; break;
      case plVariantType::Transform: uiBytes = 40; break;
      case plVariantType::String:
      case plVariantType::HashedString:
      case plVariantType::DataBuffer:
        return ValidateNetworkString(msg);
      case plVariantType::VariantArray:
      case plVariantType::VariantDictionary:
      {
        const plUInt32 uiCount = msg.ReadUInt32();
        if (msg.HasReadError() || uiCount > ref_uiBudget || uiCount > msg.GetRemainingBytes() / 2)
          return false;
        for (plUInt32 i = 0; i < uiCount; ++i)
        {
          if (type == plVariantType::VariantDictionary && !ValidateNetworkString(msg))
            return false;
          if (!ValidateNetworkVariant(msg, uiDepth + 1, ref_uiBudget))
            return false;
        }
        return true;
      }
      default:
        return false;
    }
    return msg.SkipBytes(uiBytes) != nullptr;
  }
}

bool plReadNetworkVariant(plNetworkMessage& msg, plVariant& out_value)
{
  out_value = plVariant();
  const plUInt32 uiSize = msg.ReadUInt32();
  if (msg.HasReadError() || uiSize < 2 || uiSize > 64 * 1024 || uiSize > msg.GetRemainingBytes())
  {
    msg.MarkReadError();
    return false;
  }

  auto* pBytes = static_cast<plUInt8*>(msg.SkipBytes(uiSize));
  plNetworkMessage validation(pBytes, 0, uiSize, uiSize, 0);
  plUInt32 uiBudget = 1024;
  if (!ValidateNetworkVariant(validation, 0, uiBudget) || validation.HasReadError() || validation.GetRemainingBytes() != 0)
  {
    msg.MarkReadError();
    return false;
  }

  plRawMemoryStreamReader reader(pBytes, uiSize);
  reader >> out_value;
  return true;
}
