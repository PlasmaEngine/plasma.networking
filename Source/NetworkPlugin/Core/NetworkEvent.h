#pragma once

#include "NetworkMessage.h"


#include <NetworkPlugin/NetworkPluginDLL.h>

#include "Foundation/Communication/Message.h"
#include <NetworkPlugin/Core/NetworkTypes.h>

struct PL_NETWORKPLUGIN_DLL plNetworkEvent
{
public:
  /// \brief Stores the type of event we received
  /// \see plNetworkEventTypeEnum
  plEnum<plNetworkEventTypeEnum> m_EventType;

  /// \brief The message we received
  /// \see plNetworkMessage
  plNetworkMessage m_Message;

  /// \brief This is the connection to the client who produced the event in question.
  plNetworkConnection m_Sender;
};
