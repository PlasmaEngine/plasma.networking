#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Components/NetworkPredictedComponent.h>
#include <NetworkPlugin/Core/NetworkMessage.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <NetworkPlugin/Core/NetworkPrediction.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>

namespace
{
  void WriteInput(plNetworkMessage& msg, const plNetworkPredictionInput& input)
  {
    msg.WriteUInt32(input.m_uiTick);
    msg.WriteVec3(input.m_vMovement);
    msg.WriteVec2(input.m_vLook);
    msg.WriteUInt32(input.m_uiButtons);
  }

  plNetworkPredictionInput ReadInput(plNetworkMessage& msg)
  {
    plNetworkPredictionInput input;
    input.m_uiTick = msg.ReadUInt32();
    input.m_vMovement = msg.ReadVec3();
    input.m_vLook = msg.ReadVec2();
    input.m_uiButtons = msg.ReadUInt32();
    return input;
  }

  void WriteState(plNetworkMessage& msg, const plNetworkPredictionState& state)
  {
    msg.WriteUInt32(state.m_uiTick);
    msg.WriteVec3(state.m_vPosition);
    msg.WriteQuat(state.m_qRotation);
    msg.WriteVec3(state.m_vVelocity);
  }

  plNetworkPredictionState ReadState(plNetworkMessage& msg)
  {
    plNetworkPredictionState state;
    state.m_uiTick = msg.ReadUInt32();
    state.m_vPosition = msg.ReadVec3();
    state.m_qRotation = msg.ReadQuat();
    state.m_vVelocity = msg.ReadVec3();
    return state;
  }
}

void plNetworkPredictionManager::Initialize(plNetworkWorldModule* pModule)
{
  m_pModule = pModule;
}

void plNetworkPredictionManager::RegisterComponent(plNetworkPredictedComponent* pComponent)
{
  if (pComponent && !m_Components.Contains(pComponent))
    m_Components.PushBack(pComponent);
}

void plNetworkPredictionManager::UnregisterComponent(plNetworkPredictedComponent* pComponent)
{
  m_Components.RemoveAndSwap(pComponent);
}

void plNetworkPredictionManager::Update(float fDeltaTime)
{
  if (!m_pModule || !m_pModule->GetPeer())
    return;

  m_fAccumulator += plMath::Min(fDeltaTime, 0.25f);
  plUInt32 uiTicksThisFrame = 0;
  while (m_fAccumulator >= GetFixedDeltaTime() && uiTicksThisFrame < 8)
  {
    m_fAccumulator -= GetFixedDeltaTime();
    ++m_uiCurrentTick;
    ++uiTicksThisFrame;
    Tick();
  }
}

void plNetworkPredictionManager::Tick()
{
  for (plNetworkPredictedComponent* pComponent : m_Components)
  {
    if (!pComponent || !pComponent->IsActiveAndInitialized() || pComponent->GetNetID() == 0)
      continue;

    if (pComponent->IsLocallyOwned())
    {
      TickOwner(*pComponent);
    }
    else if (m_pModule->IsServer() || m_pModule->IsHost())
    {
      TickServer(*pComponent);
    }
  }
}

void plNetworkPredictionManager::TickOwner(plNetworkPredictedComponent& component)
{
  plNetworkPredictionInput input;
  input.m_uiTick = m_uiCurrentTick;
  component.GatherInput(input);

  const plVec3 vBefore = component.GetOwner()->GetGlobalPosition();
  component.SimulateTick(input, GetFixedDeltaTime());

  plNetworkPredictedComponent::PredictedFrame& frame = component.m_PredictedFrames.ExpandAndGetRef();
  frame.m_Input = input;
  frame.m_State.m_uiTick = input.m_uiTick;
  component.CapturePredictionState(frame.m_State);
  frame.m_State.m_vVelocity = (frame.m_State.m_vPosition - vBefore) / GetFixedDeltaTime();
  component.m_vPreviousTickPosition = frame.m_State.m_vPosition;

  while (component.m_PredictedFrames.GetCount() > 128)
    component.m_PredictedFrames.RemoveAtAndCopy(0);

  if (m_pModule->IsClient())
    SendInputs(component);
}

void plNetworkPredictionManager::TickServer(plNetworkPredictedComponent& component)
{
  if (component.m_ServerInputBuffer.IsEmpty())
  {
    component.m_uiServerWaitTicks = 0;
    return;
  }

  ++component.m_uiServerWaitTicks;
  if (component.m_ServerInputBuffer.GetCount() < 2 && component.m_uiServerWaitTicks < 2)
    return;

  const plNetworkPredictedComponent::BufferedInput buffered = component.m_ServerInputBuffer[0];
  component.m_ServerInputBuffer.RemoveAtAndCopy(0);
  component.m_uiServerWaitTicks = 0;
  component.m_uiLastServerProcessedTick = buffered.m_Input.m_uiTick;
  component.m_LastServerInput = buffered.m_Input;
  component.m_bHasServerInput = true;

  const plVec3 vBefore = component.GetOwner()->GetGlobalPosition();
  component.SimulateTick(buffered.m_Input, GetFixedDeltaTime());
  component.m_LastServerState.m_uiTick = buffered.m_Input.m_uiTick;
  component.CapturePredictionState(component.m_LastServerState);
  component.m_LastServerState.m_vVelocity = (component.m_LastServerState.m_vPosition - vBefore) / GetFixedDeltaTime();
  SendStateAck(component, buffered.m_Sender, buffered.m_Input.m_uiTick);
}

void plNetworkPredictionManager::SendInputs(plNetworkPredictedComponent& component)
{
  plNetworkMessage msg = m_pModule->GetPeer()->BeginSendMessage();
  msg.WriteUInt8(plNetworkMessageIDs::PredictedInput);
  msg.WriteUInt32(component.GetNetID());

  const plUInt32 uiCount = plMath::Min<plUInt32>(4, component.m_PredictedFrames.GetCount());
  msg.WriteUInt8(static_cast<plUInt8>(uiCount));
  const plUInt32 uiFirst = component.m_PredictedFrames.GetCount() - uiCount;
  for (plUInt32 i = uiFirst; i < component.m_PredictedFrames.GetCount(); ++i)
    WriteInput(msg, component.m_PredictedFrames[i].m_Input);

  msg.SetChannelType(plNetworkChannelTypeEnum::Unreliable);
  m_pModule->GetPeer()->SendMessages(msg);
}

void plNetworkPredictionManager::SendStateAck(plNetworkPredictedComponent& component, const plNetworkConnection& target, plUInt32 uiTick)
{
  component.m_LastServerState.m_uiTick = uiTick;
  plNetworkMessage msg = m_pModule->GetPeer()->BeginSendMessage();
  msg.WriteUInt8(plNetworkMessageIDs::PredictedStateAck);
  msg.WriteUInt32(component.GetNetID());
  WriteState(msg, component.m_LastServerState);
  msg.SetChannelType(plNetworkChannelTypeEnum::Unreliable);
  m_pModule->GetPeer()->SendMessages(msg, target);
}

void plNetworkPredictionManager::HandlePredictedInput(plNetworkMessage& msg, const plNetworkConnection& sender)
{
  const plUInt32 uiNetID = msg.ReadUInt32();
  const plUInt8 uiCount = msg.ReadUInt8();
  if (uiCount == 0 || uiCount > 4)
  {
    msg.MarkReadError();
    return;
  }

  plNetworkPredictedComponent* pComponent = FindComponent(uiNetID);
  plNetworkClient* pClient = m_pModule->GetClient(sender);
  if (!pComponent || !pClient || (!m_pModule->IsServer() && !m_pModule->IsHost()))
    return;

  plUInt32 uiOwner = m_pModule->GetObjectManager()->GetOwner(uiNetID);
  if (uiOwner == 0)
  {
    const plUInt32 uiObjectNetID = m_pModule->GetObjectManager()->GetNetworkID(pComponent->GetOwner()->GetHandle());
    uiOwner = m_pModule->GetObjectManager()->GetOwner(uiObjectNetID);
  }
  if (uiOwner != pClient->m_uiClientID)
    return;

  for (plUInt8 i = 0; i < uiCount; ++i)
  {
    const plNetworkPredictionInput input = ReadInput(msg);
    if (msg.HasReadError() || input.m_uiTick <= pComponent->m_uiLastServerProcessedTick)
      continue;

    bool bExists = false;
    plUInt32 uiInsertAt = pComponent->m_ServerInputBuffer.GetCount();
    for (plUInt32 j = 0; j < pComponent->m_ServerInputBuffer.GetCount(); ++j)
    {
      const plUInt32 uiBufferedTick = pComponent->m_ServerInputBuffer[j].m_Input.m_uiTick;
      if (uiBufferedTick == input.m_uiTick)
      {
        bExists = true;
        break;
      }
      if (uiBufferedTick > input.m_uiTick)
      {
        uiInsertAt = j;
        break;
      }
    }

    if (!bExists && pComponent->m_ServerInputBuffer.GetCount() < 32)
    {
      plNetworkPredictedComponent::BufferedInput buffered;
      buffered.m_Input = input;
      buffered.m_Sender = sender;
      pComponent->m_ServerInputBuffer.InsertAt(uiInsertAt, buffered);
    }
  }
}

void plNetworkPredictionManager::HandlePredictedStateAck(plNetworkMessage& msg)
{
  const plUInt32 uiNetID = msg.ReadUInt32();
  const plNetworkPredictionState state = ReadState(msg);
  if (msg.HasReadError() || !m_pModule->IsClient())
    return;

  if (plNetworkPredictedComponent* pComponent = FindComponent(uiNetID))
  {
    if (pComponent->IsLocallyOwned())
      pComponent->Reconcile(state);
  }
}

plNetworkPredictedComponent* plNetworkPredictionManager::FindComponent(plUInt32 uiNetID) const
{
  for (plNetworkPredictedComponent* pComponent : m_Components)
  {
    if (pComponent && pComponent->GetNetID() == uiNetID)
      return pComponent;
  }
  return nullptr;
}

PL_STATICLINK_FILE(NetworkPlugin, NetworkPlugin_Core_Implementation_NetworkPrediction);
