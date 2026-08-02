#pragma once

#include <Foundation/Reflection/Reflection.h>

#include <NetworkPlugin/NetworkPluginDLL.h>

#include <type_traits>

struct PL_NETWORKPLUGIN_DLL plNetworkMessage : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plNetworkMessage, plReflectedClass);

public:
  plNetworkMessage() = default;
  plNetworkMessage(plUInt8* buffer, plUInt32 messageID, plUInt32 bufferSize, plUInt32 length, plUInt32 position)
    : m_pBuffer(buffer)
    , m_uiMessageID(messageID)
    , m_uiBufferSize(bufferSize)
    , m_uiLength(length)
    , m_uiPosition(position)
    , m_bPendingRelease(false)
    , m_ChannelType(plNetworkChannelTypeEnum::Default)
    , m_Priority(plNetworkPriority::Normal)
  {

  }

  ~plNetworkMessage() = default;

  PL_FORCE_INLINE void WriteBytes(const plUInt8* data, const plUInt32 numBytes)
  {
    if (m_bWriteError || m_pBuffer == nullptr || numBytes > m_uiBufferSize || m_uiPosition > m_uiBufferSize - numBytes)
    {
      m_bWriteError = true;
      return;
    }

    plMemoryUtils::Copy(m_pBuffer + m_uiPosition, data, numBytes);
    m_uiPosition += numBytes;
    m_uiLength = m_uiPosition;
  }

  PL_FORCE_INLINE void ReadBytes(plUInt8* data, const plUInt32 numBytes)
  {
    if (m_bReadError || m_pBuffer == nullptr || numBytes > m_uiLength || m_uiPosition > m_uiLength - numBytes)
    {
      if (data != nullptr && numBytes > 0)
      {
        plMemoryUtils::ZeroFill(data, numBytes);
      }
      m_bReadError = true;
      return;
    }

    plMemoryUtils::Copy(data, m_pBuffer + m_uiPosition, numBytes);
    m_uiPosition += numBytes;
  }

  PL_FORCE_INLINE void* SkipBytes(const plUInt32 numBytes)
  {
    if (m_bReadError || m_pBuffer == nullptr || numBytes > m_uiLength || m_uiPosition > m_uiLength - numBytes)
    {
      m_bReadError = true;
      return nullptr;
    }

    unsigned char* result = m_pBuffer + m_uiPosition;
    m_uiPosition += numBytes;
    return result;
  }

  #define PL_REGISTER_NETWORK_TYPE(type, name) \
  PL_FORCE_INLINE void Write##name(type value) { WriteBytes(reinterpret_cast<plUInt8*>(&value), sizeof(type)); } \
  PL_FORCE_INLINE type Read##name() { type value = 0; ReadBytes(reinterpret_cast<plUInt8*>(&value), sizeof(type)); return value; }

  PL_REGISTER_NETWORK_TYPE(plInt8, Int8);
  PL_REGISTER_NETWORK_TYPE(plUInt8, UInt8);
  PL_REGISTER_NETWORK_TYPE(plInt16, Int16);
  PL_REGISTER_NETWORK_TYPE(plUInt16, UInt16);
  PL_REGISTER_NETWORK_TYPE(plInt32, Int32);
  PL_REGISTER_NETWORK_TYPE(plUInt32, UInt32);
  PL_REGISTER_NETWORK_TYPE(plInt64, Int64);
  PL_REGISTER_NETWORK_TYPE(plUInt64, UInt64);
  PL_REGISTER_NETWORK_TYPE(float, Float);
  PL_REGISTER_NETWORK_TYPE(double, Double);
  PL_REGISTER_NETWORK_TYPE(bool, Bool);

  template <typename T>
  PL_FORCE_INLINE void WriteStruct(const T& data)
  {
    static_assert(std::is_trivially_copyable_v<T>, "Network structs must not contain pointers or owning containers");
    WriteBytes(reinterpret_cast<const plUInt8*>(&data), sizeof(T));
  }

  template <typename T>
  PL_FORCE_INLINE void ReadStruct(T& data)
  {
    static_assert(std::is_trivially_copyable_v<T>, "Network structs must not contain pointers or owning containers");
    ReadBytes((plUInt8*)&data, sizeof(T));
  }

  PL_FORCE_INLINE void WriteVec2(const plVec2& value)
  {
    WriteFloat(value.x);
    WriteFloat(value.y);
  }

  PL_FORCE_INLINE plVec2 ReadVec2()
  {
    const float x = ReadFloat();
    const float y = ReadFloat();
    return plVec2(x, y);
  }

  PL_FORCE_INLINE void WriteVec2Double(const plVec2d& value)
  {
    WriteDouble(value.x);
    WriteDouble(value.y);
  }

  PL_FORCE_INLINE plVec2d ReadVec2Double()
  {
    const double x = ReadDouble();
    const double y = ReadDouble();
    return plVec2d(x, y);
  }

  PL_FORCE_INLINE void WriteVec3(const plVec3& value)
  {
    WriteFloat(value.x);
    WriteFloat(value.y);
    WriteFloat(value.z);
  }

  PL_FORCE_INLINE plVec3 ReadVec3()
  {
    const float x = ReadFloat();
    const float y = ReadFloat();
    const float z = ReadFloat();
    return plVec3(x, y, z);
  }

  PL_FORCE_INLINE void WriteVec3Double(const plVec3d& value)
  {
    WriteDouble(value.x);
    WriteDouble(value.y);
    WriteDouble(value.z);
  }

  PL_FORCE_INLINE plVec3d ReadVec3Double()
  {
    const double x = ReadDouble();
    const double y = ReadDouble();
    const double z = ReadDouble();
    return plVec3d(x, y, z);
  }

  PL_FORCE_INLINE void WriteVec4(const plVec4& value)
  {
    WriteFloat(value.x);
    WriteFloat(value.y);
    WriteFloat(value.z);
    WriteFloat(value.w);
  }

  PL_FORCE_INLINE plVec4 ReadVec4()
  {
    const float x = ReadFloat();
    const float y = ReadFloat();
    const float z = ReadFloat();
    const float w = ReadFloat();
    return plVec4(x, y, z, w);
  }

  PL_FORCE_INLINE void WriteString(const plString& value)
  {
    if (value.GetElementCount() > plMath::MaxValue<plUInt16>())
    {
      m_bWriteError = true;
      return;
    }
    WriteUInt16(static_cast<plUInt16>(value.GetElementCount()));
    WriteBytes(reinterpret_cast<const plUInt8*>(value.GetData()), value.GetElementCount());
  }

  PL_FORCE_INLINE plString ReadString()
  {
    plUInt16 length = ReadUInt16();
    if (length == 0)
      return plString();
    
    // Validate that we have enough data remaining in the buffer
    plHybridArray<char, 256> buffer;
    buffer.SetCount(length + 1);
    ReadBytes((plUInt8*)buffer.GetData(), length);
    if (m_bReadError)
      return plString();

    buffer[length] = '\0';
    return plString(buffer.GetData());
  }

  PL_FORCE_INLINE void WriteStringView(const plStringView& value)
  {
    if (value.GetElementCount() > plMath::MaxValue<plUInt16>())
    {
      m_bWriteError = true;
      return;
    }
    WriteUInt16(static_cast<plUInt16>(value.GetElementCount()));
    plStringBuilder tmp;
    WriteBytes(reinterpret_cast<const plUInt8*>(value.GetData(tmp)), value.GetElementCount());
  }

  /// Note: Returns plString since plStringView requires stable backing storage
  PL_FORCE_INLINE plString ReadStringView()
  {
    return ReadString();
  }

  PL_FORCE_INLINE void WriteQuat(const plQuat& value)
  {
    WriteFloat(value.x);
    WriteFloat(value.y);
    WriteFloat(value.z);
    WriteFloat(value.w);
  }

  PL_FORCE_INLINE plQuat ReadQuat()
  {
    const float x = ReadFloat();
    const float y = ReadFloat();
    const float z = ReadFloat();
    const float w = ReadFloat();
    return plQuat(x, y, z, w);
  }

  //////////////////////////////////////////////////////////////////////////
  // Quantization Helpers - Reduced precision for bandwidth optimization
  //////////////////////////////////////////////////////////////////////////

  /// @brief Write a half-precision float (16-bit)
  /// Range: ~6.1e-5 to 65504, precision ~3 decimal digits
  /// Saves 2 bytes per float (50% reduction)
  PL_FORCE_INLINE void WriteHalf(float value)
  {
    plUInt16 half = FloatToHalf(value);
    WriteUInt16(half);
  }

  /// @brief Read a half-precision float
  PL_FORCE_INLINE float ReadHalf()
  {
    plUInt16 half = ReadUInt16();
    return HalfToFloat(half);
  }

  /// @brief Write a Vec3 using half-precision floats
  /// 12 bytes -> 6 bytes (50% reduction)
  PL_FORCE_INLINE void WriteVec3Half(const plVec3& value)
  {
    WriteHalf(value.x);
    WriteHalf(value.y);
    WriteHalf(value.z);
  }

  /// @brief Read a Vec3 using half-precision floats
  PL_FORCE_INLINE plVec3 ReadVec3Half()
  {
    float x = ReadHalf();
    float y = ReadHalf();
    float z = ReadHalf();
    return plVec3(x, y, z);
  }

  /// @brief Write a quaternion using "smallest three" encoding
  /// 16 bytes -> 7 bytes (56% reduction)
  /// Drops the largest component (reconstructible from unit constraint)
  PL_FORCE_INLINE void WriteQuatSmallestThree(const plQuat& q)
  {
    // Find the largest absolute component
    float absX = plMath::Abs(q.x);
    float absY = plMath::Abs(q.y);
    float absZ = plMath::Abs(q.z);
    float absW = plMath::Abs(q.w);

    plUInt8 largestIndex = 0;
    float largestValue = absX;

    if (absY > largestValue) { largestIndex = 1; largestValue = absY; }
    if (absZ > largestValue) { largestIndex = 2; largestValue = absZ; }
    if (absW > largestValue) { largestIndex = 3; largestValue = absW; }

    // Ensure the largest component is positive (negate quat if needed)
    float sign = 1.0f;
    switch (largestIndex)
    {
      case 0: sign = (q.x >= 0) ? 1.0f : -1.0f; break;
      case 1: sign = (q.y >= 0) ? 1.0f : -1.0f; break;
      case 2: sign = (q.z >= 0) ? 1.0f : -1.0f; break;
      case 3: sign = (q.w >= 0) ? 1.0f : -1.0f; break;
    }

    // Write the index (2 bits) in a byte for simplicity
    WriteUInt8(largestIndex);

    // Write the three smallest components as 16-bit signed integers
    // Range [-1, 1] mapped to [-32767, 32767]
    const float SCALE = 32767.0f;

    auto writeComponent = [&](float c) {
      plInt16 quantized = static_cast<plInt16>(plMath::Clamp(c * sign * SCALE, -32767.0f, 32767.0f));
      WriteInt16(quantized);
    };

    if (largestIndex != 0) writeComponent(q.x);
    if (largestIndex != 1) writeComponent(q.y);
    if (largestIndex != 2) writeComponent(q.z);
    if (largestIndex != 3) writeComponent(q.w);
  }

  /// @brief Read a quaternion using "smallest three" encoding
  PL_FORCE_INLINE plQuat ReadQuatSmallestThree()
  {
    plUInt8 largestIndex = ReadUInt8();
    if (m_bReadError || largestIndex > 3)
    {
      m_bReadError = true;
      return plQuat::MakeIdentity();
    }

    const float SCALE = 1.0f / 32767.0f;

    float components[4];
    float sumSquares = 0.0f;

    for (plUInt8 i = 0; i < 4; ++i)
    {
      if (i == largestIndex)
      {
        components[i] = 0.0f; // Placeholder, will be reconstructed
      }
      else
      {
        plInt16 quantized = ReadInt16();
        components[i] = static_cast<float>(quantized) * SCALE;
        sumSquares += components[i] * components[i];
      }
    }

    // Reconstruct the largest component from unit constraint
    // x² + y² + z² + w² = 1
    components[largestIndex] = plMath::Sqrt(plMath::Max(0.0f, 1.0f - sumSquares));

    return plQuat(components[0], components[1], components[2], components[3]);
  }

  /// @brief Write a bounded Vec3 with configurable precision
  /// Position is quantized within known world bounds
  /// @param value The position to write
  /// @param vMin Minimum world bounds
  /// @param vMax Maximum world bounds  
  /// @param bitsPerAxis Bits per axis (8-16 typical, 16 = ~1cm precision in 1km world)
  PL_FORCE_INLINE void WriteBoundedVec3(const plVec3& value, const plVec3& vMin, const plVec3& vMax, plUInt8 bitsPerAxis = 16)
  {
    plVec3 range = vMax - vMin;
    float maxVal = static_cast<float>((1 << bitsPerAxis) - 1);

    auto quantizeAxis = [&](float v, float min, float r) -> plUInt16 {
      // Guard against division by zero
      if (r < 0.0001f)
        return 0;
      float normalized = (v - min) / r;
      normalized = plMath::Clamp(normalized, 0.0f, 1.0f);
      return static_cast<plUInt16>(normalized * maxVal);
    };

    WriteUInt16(quantizeAxis(value.x, vMin.x, range.x));
    WriteUInt16(quantizeAxis(value.y, vMin.y, range.y));
    WriteUInt16(quantizeAxis(value.z, vMin.z, range.z));
  }

  /// @brief Read a bounded Vec3
  PL_FORCE_INLINE plVec3 ReadBoundedVec3(const plVec3& vMin, const plVec3& vMax, plUInt8 bitsPerAxis = 16)
  {
    plVec3 range = vMax - vMin;
    float maxVal = static_cast<float>((1 << bitsPerAxis) - 1);

    auto dequantizeAxis = [&](plUInt16 q, float min, float r) -> float {
      return min + (static_cast<float>(q) / maxVal) * r;
    };

    float x = dequantizeAxis(ReadUInt16(), vMin.x, range.x);
    float y = dequantizeAxis(ReadUInt16(), vMin.y, range.y);
    float z = dequantizeAxis(ReadUInt16(), vMin.z, range.z);

    return plVec3(x, y, z);
  }

  /// @brief Write an angle using 16-bit precision
  /// Full 360° range with ~0.0055° precision
  PL_FORCE_INLINE void WriteAngle(plAngle angle)
  {
    // Normalize to [0, 2π]
    float radians = angle.GetRadian();
    radians = plMath::Mod(radians, plMath::Pi<float>() * 2.0f);
    if (radians < 0) radians += plMath::Pi<float>() * 2.0f;
    
    // Quantize to 16 bits
    plUInt16 quantized = static_cast<plUInt16>((radians / (plMath::Pi<float>() * 2.0f)) * 65535.0f);
    WriteUInt16(quantized);
  }

  /// @brief Read an angle using 16-bit precision
  PL_FORCE_INLINE plAngle ReadAngle()
  {
    plUInt16 quantized = ReadUInt16();
    float radians = (static_cast<float>(quantized) / 65535.0f) * plMath::Pi<float>() * 2.0f;
    return plAngle::MakeFromRadian(radians);
  }

  /// @brief Write uniform scale as a single half-float
  /// 12 bytes -> 2 bytes for uniform scale (83% reduction)
  PL_FORCE_INLINE void WriteUniformScale(float scale)
  {
    WriteHalf(scale);
  }

  /// @brief Read uniform scale
  PL_FORCE_INLINE float ReadUniformScale()
  {
    return ReadHalf();
  }

  /// @brief Write a time value as milliseconds (32-bit)
  PL_FORCE_INLINE void WriteTime(plTime time)
  {
    WriteFloat(static_cast<float>(time.GetSeconds()));
  }

  /// @brief Read a time value
  PL_FORCE_INLINE plTime ReadTime()
  {
    return plTime::MakeFromSeconds(ReadFloat());
  }

private:
  /// @brief Convert float to half-precision (IEEE 754 half-float)
  static PL_FORCE_INLINE plUInt16 FloatToHalf(float value)
  {
    // Handle special cases
    if (value == 0.0f) return 0;
    
    plUInt32 bits;
    plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&bits), reinterpret_cast<const plUInt8*>(&value), sizeof(float));

    plUInt32 sign = (bits >> 31) & 0x1;
    plInt32 exp = ((bits >> 23) & 0xFF) - 127;
    plUInt32 mant = bits & 0x7FFFFF;

    plUInt16 result;

    if (exp > 15)
    {
      // Overflow - clamp to max half
      result = static_cast<plUInt16>((sign << 15) | (31 << 10) | 0x3FF);
    }
    else if (exp < -14)
    {
      // Underflow - denormalized or zero
      plUInt32 shift = -14 - exp;
      if (shift > 24)
      {
        result = static_cast<plUInt16>(sign << 15);
      }
      else
      {
        mant = (mant | 0x800000) >> (shift + 1);
        result = static_cast<plUInt16>((sign << 15) | (mant >> 13));
      }
    }
    else
    {
      // Normal case
      result = static_cast<plUInt16>((sign << 15) | ((exp + 15) << 10) | (mant >> 13));
    }

    return result;
  }

  /// @brief Convert half-precision to float
  static PL_FORCE_INLINE float HalfToFloat(plUInt16 half)
  {
    plUInt32 sign = (half >> 15) & 0x1;
    plUInt32 exp = (half >> 10) & 0x1F;
    plUInt32 mant = half & 0x3FF;

    plUInt32 result;

    if (exp == 0)
    {
      if (mant == 0)
      {
        // Zero
        result = sign << 31;
      }
      else
      {
        // Denormalized
        exp = 1;
        // Guard against infinite loop if mant is somehow 0 (shouldn't happen but defensive)
        plUInt32 safetyCounter = 0;
        while ((mant & 0x400) == 0 && safetyCounter < 11)
        {
          mant <<= 1;
          exp--;
          safetyCounter++;
        }
        mant &= 0x3FF;
        result = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
      }
    }
    else if (exp == 31)
    {
      // Inf/NaN
      result = (sign << 31) | (0xFF << 23) | (mant << 13);
    }
    else
    {
      // Normal
      result = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }

    float f;
    plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&f), reinterpret_cast<const plUInt8*>(&result), sizeof(float));
    return f;
  }

public:

  #undef PL_REGISTER_NETWORK_TYPE

  bool IsValid() const
  {
    return m_pBuffer != nullptr && m_uiBufferSize > 0 && !m_bPendingRelease;
  }

  /// A read failure is sticky. Failed reads zero-fill their destination and do not advance the cursor.
  bool HasReadError() const { return m_bReadError; }
  void MarkReadError() { m_bReadError = true; }

  /// A write failure is sticky. Failed writes do not advance the cursor.
  bool HasWriteError() const { return m_bWriteError; }

  plUInt32 GetRemainingBytes() const
  {
    return m_uiPosition <= m_uiLength ? m_uiLength - m_uiPosition : 0;
  }

  void SetChannelType(plEnum<plNetworkChannelTypeEnum> channel)
  {
    m_ChannelType = channel;
  }

  plEnum<plNetworkChannelTypeEnum> GetChannelType() const
  {
    return m_ChannelType;
  }

  void SetPriority(plNetworkPriority::Enum priority)
  {
    m_Priority = priority;
  }

  plNetworkPriority::Enum GetPriority() const
  {
    return m_Priority;
  }

  plUInt8* m_pBuffer = nullptr;
  plUInt32 m_uiMessageID = 0;
  plUInt32 m_uiBufferSize = 0;
  plUInt32 m_uiLength = 0;
  plUInt32 m_uiPosition = 0;
  bool m_bPendingRelease = false;
  bool m_bReadError = false;
  bool m_bWriteError = false;
  plEnum<plNetworkChannelTypeEnum> m_ChannelType;
  plNetworkPriority::Enum m_Priority = plNetworkPriority::Normal;

};
