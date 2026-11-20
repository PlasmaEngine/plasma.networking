#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/Driver/simulated/DriverSimulated.h>
#include <NetworkPlugin/Core/NetworkPeer.h>

plDriverSimulated::plDriverSimulated(plNetworkDriverInterface* pTransport)
  : m_pTransport(pTransport)
{
}

plDriverSimulated::~plDriverSimulated()
{
  if (m_pTransport)
  {
    m_pTransport->Shutdown();
    PL_DEFAULT_DELETE(m_pTransport);
  }
}

plResult plDriverSimulated::Initalize(plNetworkPeer* pNetworkPeer, const plNetworkConfig& config)
{
  m_pPeer = pNetworkPeer;

  if (config.m_Simulation.m_bEnabled)
    m_Simulator.SetSettings(config.m_Simulation);
  else
    m_Simulator.SetFollowCVars(true);

  m_Simulator.Update();
  return PL_SUCCESS;
}

void plDriverSimulated::Shutdown()
{
  // Nothing that was sent is lost because the peer stops: what is still delayed goes out now, in order.
  FlushOutgoingFor(nullptr);
  m_pTransport->Flush();
  m_pTransport->Shutdown();

  m_Outgoing.Clear();
  m_Incoming.Clear();
  m_OutgoingLinks.Clear();
  m_IncomingLinks.Clear();
  m_OpenConnections.Clear();
}

void plDriverSimulated::Disconnect()
{
  FlushOutgoingFor(nullptr);
  m_pTransport->Flush();
  m_pTransport->Disconnect();
  m_OpenConnections.Clear();
}

void plDriverSimulated::Disconnect(const plNetworkConnection& connection)
{
  FlushOutgoingFor(&connection);
  m_pTransport->Flush();
  m_pTransport->Disconnect(connection);
  m_OpenConnections.Remove(connection.m_uiConnectionID);

  for (plUInt32 i = m_Incoming.GetCount(); i > 0; --i)
  {
    if (m_Incoming[i - 1].m_Connection == connection)
      m_Incoming.RemoveAtAndCopy(i - 1);
  }
}

void plDriverSimulated::Enqueue(plDynamicArray<Queued>& ref_queue, Queued&& item)
{
  item.m_uiOrder = m_uiNextOrder++;

  // Insert after everything due at the same time or earlier, so equal delivery times keep the order of arrival.
  plUInt32 uiIndex = ref_queue.GetCount();
  while (uiIndex > 0 && item.m_DeliveryTime < ref_queue[uiIndex - 1].m_DeliveryTime)
    --uiIndex;

  ref_queue.InsertAt(uiIndex, std::move(item));
}

void plDriverSimulated::ScheduleOutgoing(plNetworkChannelTypeEnum::Enum channel, const plNetworkMessage& message, plNetworkConnection target, bool bClientForm)
{
  if (!m_OpenConnections.Contains(target.m_uiConnectionID))
    return;

  LinkState& link = m_OutgoingLinks[target.m_uiConnectionID];
  const plTime now = plTime::Now();
  const plNetworkSimulator::Decision decision = m_Simulator.Schedule(link.m_Link, channel, message.m_uiLength, now);

  const plUInt32 uiSequence = plNetworkSimulator::IsSequenced(channel) ? link.m_uiNextSequence++ : 0;
  if (!decision.m_bDeliver)
    return;

  Queued item;
  item.m_DeliveryTime = decision.m_DeliveryTime;
  item.m_EventType = plNetworkEventTypeEnum::Message;
  item.m_Channel = channel;
  item.m_Connection = target;
  item.m_bClientForm = bClientForm;
  item.m_uiSequence = uiSequence;
  item.m_Data.SetCountUninitialized(message.m_uiLength);
  if (message.m_uiLength > 0)
    plMemoryUtils::Copy(item.m_Data.GetData(), message.m_pBuffer, message.m_uiLength);

  if (decision.m_bDuplicate)
  {
    Queued duplicate;
    duplicate.m_DeliveryTime = decision.m_DuplicateDeliveryTime;
    duplicate.m_EventType = item.m_EventType;
    duplicate.m_Channel = item.m_Channel;
    duplicate.m_Connection = item.m_Connection;
    duplicate.m_bClientForm = item.m_bClientForm;
    duplicate.m_uiSequence = item.m_uiSequence;
    duplicate.m_Data = item.m_Data;
    Enqueue(m_Outgoing, std::move(duplicate));
  }

  Enqueue(m_Outgoing, std::move(item));
}

void plDriverSimulated::ScheduleIncoming(const plNetworkEvent& event)
{
  LinkState& link = m_IncomingLinks[event.m_Sender.m_uiConnectionID];
  const plTime now = plTime::Now();

  Queued item;
  item.m_EventType = event.m_EventType;
  item.m_Connection = event.m_Sender;
  item.m_DeliveryTime = now;

  if (event.m_EventType == plNetworkEventTypeEnum::Message)
  {
    // A transport that does not know how a message was sent reports None; treat that as reliable, which never loses it.
    plNetworkChannelTypeEnum::Enum channel = event.m_Message.m_ChannelType;
    if (channel == plNetworkChannelTypeEnum::None)
      channel = plNetworkChannelTypeEnum::ReliableOrdered;

    const plNetworkSimulator::Decision decision = m_Simulator.Schedule(link.m_Link, channel, event.m_Message.m_uiLength, now);
    const plUInt32 uiSequence = plNetworkSimulator::IsSequenced(channel) ? link.m_uiNextSequence++ : 0;

    plNetworkMessage pooled = event.m_Message;
    if (decision.m_bDeliver)
    {
      item.m_DeliveryTime = decision.m_DeliveryTime;
      item.m_Channel = channel;
      item.m_uiSequence = uiSequence;
      item.m_Data.SetCountUninitialized(pooled.m_uiLength);
      if (pooled.m_uiLength > 0)
        plMemoryUtils::Copy(item.m_Data.GetData(), pooled.m_pBuffer, pooled.m_uiLength);

      if (decision.m_bDuplicate)
      {
        Queued duplicate;
        duplicate.m_DeliveryTime = decision.m_DuplicateDeliveryTime;
        duplicate.m_EventType = item.m_EventType;
        duplicate.m_Channel = item.m_Channel;
        duplicate.m_Connection = item.m_Connection;
        duplicate.m_uiSequence = item.m_uiSequence;
        duplicate.m_Data = item.m_Data;
        Enqueue(m_Incoming, std::move(duplicate));
      }
    }

    // The pool message is released at once; the copy takes a pool message again only when it is due.
    m_pPeer->DestroyMessage(pooled);
    if (!decision.m_bDeliver)
      return;
  }
  else if (event.m_EventType == plNetworkEventTypeEnum::Disconnected || event.m_EventType == plNetworkEventTypeEnum::Timeout)
  {
    // A connection ends after everything it delivered before it ended.
    item.m_DeliveryTime = plMath::Max(now, link.m_Link.m_LastReliableDelivery);
    for (const Queued& queued : m_Incoming)
    {
      if (queued.m_Connection == event.m_Sender)
        item.m_DeliveryTime = plMath::Max(item.m_DeliveryTime, queued.m_DeliveryTime);
    }
  }

  Enqueue(m_Incoming, std::move(item));
}

bool plDriverSimulated::IsStaleSequenced(plHashTable<plUInt32, LinkState>& ref_links, const Queued& item)
{
  if (item.m_EventType != plNetworkEventTypeEnum::Message || !plNetworkSimulator::IsSequenced(item.m_Channel))
    return false;

  LinkState& link = ref_links[item.m_Connection.m_uiConnectionID];
  if (item.m_uiSequence <= link.m_uiLastDeliveredSequence)
    return true;

  link.m_uiLastDeliveredSequence = item.m_uiSequence;
  return false;
}

plResult plDriverSimulated::PopEvent(plNetworkEvent& event)
{
  plNetworkEvent transportEvent;
  while (m_pTransport->PopEvent(transportEvent).Succeeded())
  {
    if (transportEvent.m_EventType == plNetworkEventTypeEnum::Connected)
      m_OpenConnections.Insert(transportEvent.m_Sender.m_uiConnectionID);
    else if (transportEvent.m_EventType == plNetworkEventTypeEnum::Disconnected || transportEvent.m_EventType == plNetworkEventTypeEnum::Timeout)
      m_OpenConnections.Remove(transportEvent.m_Sender.m_uiConnectionID);

    if (!m_Simulator.IsEnabled() && m_Incoming.IsEmpty())
    {
      event = transportEvent;
      return PL_SUCCESS;
    }

    ScheduleIncoming(transportEvent);
  }

  const plTime now = plTime::Now();
  while (!m_Incoming.IsEmpty() && m_Incoming[0].m_DeliveryTime <= now)
  {
    Queued& item = m_Incoming[0];
    if (IsStaleSequenced(m_IncomingLinks, item))
    {
      m_Incoming.RemoveAtAndCopy(0);
      continue;
    }

    event.m_EventType = item.m_EventType;
    event.m_Sender = item.m_Connection;
    event.m_Message = plNetworkMessage();

    if (item.m_EventType == plNetworkEventTypeEnum::Message)
    {
      plNetworkMessage message = m_pPeer->CreateMessage();
      if (!message.IsValid())
        return PL_FAILURE; // Stays queued until a pool message is free again.

      if (item.m_Data.GetCount() > message.m_uiBufferSize)
      {
        m_pPeer->DestroyMessage(message);
        m_Incoming.RemoveAtAndCopy(0);
        continue;
      }

      message.m_uiLength = item.m_Data.GetCount();
      if (message.m_uiLength > 0)
        plMemoryUtils::Copy(message.m_pBuffer, item.m_Data.GetData(), message.m_uiLength);
      message.SetChannelType(item.m_Channel);
      event.m_Message = message;
    }

    m_Incoming.RemoveAtAndCopy(0);
    return PL_SUCCESS;
  }

  return PL_FAILURE;
}

void plDriverSimulated::DispatchOutgoing(Queued& item)
{
  const plUInt32 uiLength = item.m_Data.GetCount();
  plNetworkMessage message(item.m_Data.GetData(), 0, uiLength, uiLength, 0);
  message.SetChannelType(item.m_Channel);

  if (item.m_bClientForm)
    m_pTransport->SendMessages(item.m_Channel, message);
  else
    m_pTransport->SendMessages(item.m_Channel, message, item.m_Connection);
}

void plDriverSimulated::FlushOutgoingFor(const plNetworkConnection* pConnection)
{
  for (plUInt32 i = 0; i < m_Outgoing.GetCount();)
  {
    Queued& item = m_Outgoing[i];
    if (pConnection != nullptr && !(item.m_Connection == *pConnection))
    {
      ++i;
      continue;
    }

    if (!IsStaleSequenced(m_OutgoingLinks, item))
      DispatchOutgoing(item);

    m_Outgoing.RemoveAtAndCopy(i);
  }
}

void plDriverSimulated::Flush()
{
  m_Simulator.Update();

  const plTime now = plTime::Now();
  plUInt32 uiDue = 0;
  while (uiDue < m_Outgoing.GetCount() && m_Outgoing[uiDue].m_DeliveryTime <= now)
  {
    if (!IsStaleSequenced(m_OutgoingLinks, m_Outgoing[uiDue]))
      DispatchOutgoing(m_Outgoing[uiDue]);
    ++uiDue;
  }

  if (uiDue > 0)
    m_Outgoing.RemoveAtAndCopy(0, uiDue);

  m_pTransport->Flush();
}

void plDriverSimulated::SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message)
{
  if (!m_Simulator.IsEnabled() && m_Outgoing.IsEmpty())
  {
    m_pTransport->SendMessages(channelType, message);
    return;
  }

  ScheduleOutgoing(channelType, message, plNetworkConnection(), true);
}

void plDriverSimulated::SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plNetworkConnection target)
{
  if (!m_Simulator.IsEnabled() && m_Outgoing.IsEmpty())
  {
    m_pTransport->SendMessages(channelType, message, target);
    return;
  }

  ScheduleOutgoing(channelType, message, target, false);
}

void plDriverSimulated::SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plDynamicArray<plNetworkConnection> target)
{
  if (!m_Simulator.IsEnabled() && m_Outgoing.IsEmpty())
  {
    m_pTransport->SendMessages(channelType, message, target);
    return;
  }

  for (const plNetworkConnection& connection : target)
    ScheduleOutgoing(channelType, message, connection, false);
}
