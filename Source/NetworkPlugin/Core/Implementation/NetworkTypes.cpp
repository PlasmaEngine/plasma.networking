#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkTypes.h>

PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkRoleEnum, 1)
  PL_ENUM_CONSTANT(plNetworkRoleEnum::Undefined),
  PL_ENUM_CONSTANT(plNetworkRoleEnum::Server),
  PL_ENUM_CONSTANT(plNetworkRoleEnum::Client),
  PL_ENUM_CONSTANT(plNetworkRoleEnum::Host),
  PL_ENUM_CONSTANT(plNetworkRoleEnum::Both)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkEventTypeEnum, 1)
  PL_ENUM_CONSTANT(plNetworkEventTypeEnum::Undefined),
  PL_ENUM_CONSTANT(plNetworkEventTypeEnum::Connected),
  PL_ENUM_CONSTANT(plNetworkEventTypeEnum::Disconnected),
  PL_ENUM_CONSTANT(plNetworkEventTypeEnum::Timeout),
  PL_ENUM_CONSTANT(plNetworkEventTypeEnum::Message)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkModeEnum, 1)
  PL_ENUM_CONSTANT(plNetworkModeEnum::Offline),
  PL_ENUM_CONSTANT(plNetworkModeEnum::Client),
  PL_ENUM_CONSTANT(plNetworkModeEnum::Server)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkChannelTypeEnum, 1)
  PL_ENUM_CONSTANT(plNetworkChannelTypeEnum::None),
  PL_ENUM_CONSTANTS(plNetworkChannelTypeEnum::Unreliable, plNetworkChannelTypeEnum::UnreliableOrdered, plNetworkChannelTypeEnum::Reliable, plNetworkChannelTypeEnum::ReliableOrdered)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plDriverTypeEnum, 1)
  PL_ENUM_CONSTANT(plDriverTypeEnum::ENet),
  PL_ENUM_CONSTANT(plDriverTypeEnum::Platform)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plNetworkConfig, 1, plRTTIDefaultAllocator<plNetworkConfig>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("ConnectionLimit", m_uiConnectionLimit)->AddAttributes(new plDefaultValueAttribute(32)),
    PL_MEMBER_PROPERTY("ServerAddress", m_sServerAddress)->AddAttributes(new plDefaultValueAttribute("127.0.0.1")),
    PL_MEMBER_PROPERTY("ServerPort", m_uiServerPort)->AddAttributes(new plDefaultValueAttribute(7777)),
    PL_MEMBER_PROPERTY("MessageSize", m_uiMessageSize)->AddAttributes(new plDefaultValueAttribute(1500)),
    PL_MEMBER_PROPERTY("MessagePoolSize", m_uiMessagePoolSize)->AddAttributes(new plDefaultValueAttribute(2048)),
    PL_MEMBER_PROPERTY("LobbyId", m_sLobbyId),
    PL_MEMBER_PROPERTY("CreatePlatformLobby", m_bCreatePlatformLobby)->AddAttributes(new plDefaultValueAttribute(false)),
    PL_MEMBER_PROPERTY("LobbyMaxMembers", m_uiLobbyMaxMembers)->AddAttributes(new plDefaultValueAttribute(0)),
    PL_MEMBER_PROPERTY("LobbyPublic", m_bLobbyPublic)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkConnectionState, 1)
  PL_ENUM_CONSTANT(plNetworkConnectionState::Offline),
  PL_ENUM_CONSTANTS(plNetworkConnectionState::Connecting, plNetworkConnectionState::Connected, plNetworkConnectionState::Disconnecting, plNetworkConnectionState::Disconnected)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkObjectRole, 1)
  PL_ENUM_CONSTANTS(plNetworkObjectRole::None, plNetworkObjectRole::OwnedAuthoritative, plNetworkObjectRole::Replicated, plNetworkObjectRole::ReplicatedSimulated)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plNetworkConnection, 1, plRTTIDefaultAllocator<plNetworkConnection>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plNetworkStats, 1, plRTTIDefaultAllocator<plNetworkConnection>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_ENUM(plNetworkMessageIDs, 1)
  PL_ENUM_CONSTANTS(plNetworkMessageIDs::Undefined, plNetworkMessageIDs::Handshake, plNetworkMessageIDs::HandshakeReply, plNetworkMessageIDs::ObjectReplicate)
  PL_ENUM_CONSTANTS(plNetworkMessageIDs::ObjectReplicatePart, plNetworkMessageIDs::ObjectSpawn, plNetworkMessageIDs::ObjectSpawnPart, plNetworkMessageIDs::ObjectDespawn, plNetworkMessageIDs::ObjectRole, plNetworkMessageIDs::ObjectRpc)
  PL_ENUM_CONSTANTS(plNetworkMessageIDs::PredictedInput, plNetworkMessageIDs::PredictedStateAck, plNetworkMessageIDs::TransformState)
PL_END_STATIC_REFLECTED_ENUM;
