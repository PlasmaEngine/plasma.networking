#include "NetworkTestPCH.h"

#include <Core/World/GameObject.h>
#include <NetworkPlugin/Core/NetworkMessage.h>
#include <NetworkPlugin/Core/NetworkPrediction.h>
#include <NetworkPlugin/Core/NetworkSerialize.h>

PL_CREATE_SIMPLE_TEST_GROUP(Network);

PL_CREATE_SIMPLE_TEST(Network, MessageBoundsAndQuantization)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Truncated reads fail safely")
  {
    plUInt8 buffer[8] = {};
    plNetworkMessage msg(buffer, 0, PL_ARRAY_SIZE(buffer), 2, 0);
    const plUInt32 uiPositionBefore = msg.m_uiPosition;
    PL_TEST_INT(msg.ReadUInt32(), 0);
    PL_TEST_BOOL(msg.HasReadError());
    PL_TEST_INT(msg.m_uiPosition, uiPositionBefore);

    plUInt8 destination[4] = {1, 2, 3, 4};
    msg.ReadBytes(destination, PL_ARRAY_SIZE(destination));
    PL_TEST_INT(destination[0], 0);
    PL_TEST_INT(destination[3], 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Writes cannot overflow")
  {
    plUInt8 buffer[4] = {};
    plNetworkMessage msg(buffer, 0, PL_ARRAY_SIZE(buffer), 0, 0);
    msg.WriteUInt64(42);
    PL_TEST_BOOL(msg.HasWriteError());
    PL_TEST_INT(msg.m_uiPosition, 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Quantized values round trip")
  {
    plUInt8 buffer[64] = {};
    plNetworkMessage writer(buffer, 0, PL_ARRAY_SIZE(buffer), 0, 0);
    const plVec3 sourcePosition(12.25f, -3.5f, 99.0f);
    const plQuat sourceRotation = plQuat::MakeFromAxisAndAngle(plVec3::MakeAxisZ(), plAngle::MakeFromDegree(37.0f));
    writer.WriteVec3Half(sourcePosition);
    writer.WriteQuatSmallestThree(sourceRotation);
    writer.WriteBoundedVec3(sourcePosition, plVec3(-100.0f), plVec3(100.0f));
    PL_TEST_BOOL(!writer.HasWriteError());

    plNetworkMessage reader(buffer, 0, PL_ARRAY_SIZE(buffer), writer.m_uiLength, 0);
    const plVec3 halfPosition = reader.ReadVec3Half();
    const plQuat rotation = reader.ReadQuatSmallestThree();
    const plVec3 boundedPosition = reader.ReadBoundedVec3(plVec3(-100.0f), plVec3(100.0f));
    PL_TEST_BOOL(!reader.HasReadError());
    PL_TEST_BOOL(halfPosition.IsEqual(sourcePosition, 0.05f));
    PL_TEST_BOOL(rotation.IsEqualRotation(sourceRotation, 0.001f));
    PL_TEST_BOOL(boundedPosition.IsEqual(sourcePosition, 0.01f));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Prediction input redundancy is bounded")
  {
    plUInt8 buffer[8] = {};
    plNetworkMessage writer(buffer, 0, PL_ARRAY_SIZE(buffer), 0, 0);
    writer.WriteUInt32(17);
    writer.WriteUInt8(5); // The wire protocol permits at most the current command plus three redundant commands.

    plNetworkMessage reader(buffer, 0, PL_ARRAY_SIZE(buffer), writer.m_uiLength, 0);
    plNetworkPredictionManager prediction;
    prediction.HandlePredictedInput(reader, plNetworkConnection());
    PL_TEST_BOOL(reader.HasReadError());
    PL_TEST_FLOAT(prediction.GetFixedDeltaTime(), 1.0f / 60.0f, 0.000001f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "GameObject transform and tags are eligible")
  {
    const plRTTI* pGameObjectType = plGetStaticRTTI<plGameObject>();
    const plAbstractProperty* pPosition = pGameObjectType->FindPropertyByName("LocalPosition");
    const plAbstractProperty* pRotation = pGameObjectType->FindPropertyByName("LocalRotation");
    const plAbstractProperty* pTags = pGameObjectType->FindPropertyByName("Tags");

    PL_TEST_BOOL(pPosition != nullptr && pPosition->GetCategory() == plPropertyCategory::Member);
    PL_TEST_BOOL(pRotation != nullptr && pRotation->GetCategory() == plPropertyCategory::Member);
    PL_TEST_BOOL(pTags != nullptr && pTags->GetCategory() == plPropertyCategory::Set);
    if (pPosition != nullptr && pRotation != nullptr)
    {
      PL_TEST_BOOL(plNetworkSerializer::GetVariantTypeForProperty(static_cast<const plAbstractMemberProperty*>(pPosition)) == plVariantType::Vector3);
      PL_TEST_BOOL(plNetworkSerializer::GetVariantTypeForProperty(static_cast<const plAbstractMemberProperty*>(pRotation)) == plVariantType::Quaternion);
    }
  }
}
