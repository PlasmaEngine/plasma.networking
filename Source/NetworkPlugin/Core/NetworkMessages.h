#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkTypes.h>

#include <Core/Messages/EventMessage.h>
#include <Foundation/Communication/Message.h>
#include <Foundation/Types/Variant.h>

class plGameObject;
class plNetworkMessage;

//////////////////////////////////////////////////////////////////////////
// Session Messages
//////////////////////////////////////////////////////////////////////////

/// @brief Sent when a player joins the network session
struct PL_NETWORKPLUGIN_DLL plMsgNetworkPlayerJoined : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkPlayerJoined, plMessage);

  plUInt32 m_uiClientID = 0;
};

/// @brief Sent when a player leaves the network session
struct PL_NETWORKPLUGIN_DLL plMsgNetworkPlayerLeft : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkPlayerLeft, plMessage);

  plUInt32 m_uiClientID = 0;
};

/// @brief Sent when the session state changes (Lobby, Loading, InGame, PostGame, etc.)
struct PL_NETWORKPLUGIN_DLL plMsgNetworkSessionStateChanged : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkSessionStateChanged, plMessage);

  plUInt8 m_NewState = 0; ///< plNetworkSessionState::Enum
};

/// @brief Sent when all players in the session are ready
struct PL_NETWORKPLUGIN_DLL plMsgNetworkAllPlayersReady : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkAllPlayersReady, plMessage);
};

//////////////////////////////////////////////////////////////////////////
// Level Manager Messages
//////////////////////////////////////////////////////////////////////////

/// @brief Sent when the server requests a level load
struct PL_NETWORKPLUGIN_DLL plMsgNetworkLevelLoadRequested : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkLevelLoadRequested, plMessage);

  plString m_sLevelPath;
};

/// @brief Sent when the server signals gameplay can begin (all clients loaded)
struct PL_NETWORKPLUGIN_DLL plMsgNetworkLevelStart : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkLevelStart, plMessage);
};

/// @brief Sent on the server when all clients finish loading a level
struct PL_NETWORKPLUGIN_DLL plMsgNetworkAllClientsLoaded : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkAllClientsLoaded, plMessage);
};

//////////////////////////////////////////////////////////////////////////
// World Module Messages
//////////////////////////////////////////////////////////////////////////

/// @brief Sent when a client connects to the server
struct PL_NETWORKPLUGIN_DLL plMsgNetworkClientConnected : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkClientConnected, plMessage);

  plUInt32 m_uiClientID = 0;
};

/// @brief Sent when a user-defined network message (ID 128-255) is received
struct PL_NETWORKPLUGIN_DLL plMsgNetworkUserMessage : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkUserMessage, plMessage);

  plUInt8 m_uiMessageID = 0;
  plNetworkConnection m_Sender;
  plNetworkMessage* m_pNetworkMessage = nullptr; ///< Pointer to the raw network message data (valid only during handler)
};

/// @brief Sent locally by plNetworkedEventComponent when a named network event is received.
struct PL_NETWORKPLUGIN_DLL plMsgNetworkedEvent : public plEventMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkedEvent, plEventMessage);

  plString m_sEventName;
  plVariant m_Param1;
  plVariant m_Param2;

  plInt32 GetParam1Type() const;
  plInt32 GetParam2Type() const;

  bool TryGetParam1Bool(bool& out_bValue) const;
  bool TryGetParam1Int(plInt32& out_iValue) const;
  bool TryGetParam1UInt(plUInt32& out_uiValue) const;
  bool TryGetParam1Float(float& out_fValue) const;
  bool TryGetParam1Double(double& out_fValue) const;
  bool TryGetParam1String(plString& out_sValue) const;
  bool TryGetParam1Vec3(plVec3& out_vValue) const;

  bool TryGetParam2Bool(bool& out_bValue) const;
  bool TryGetParam2Int(plInt32& out_iValue) const;
  bool TryGetParam2UInt(plUInt32& out_uiValue) const;
  bool TryGetParam2Float(float& out_fValue) const;
  bool TryGetParam2Double(double& out_fValue) const;
  bool TryGetParam2String(plString& out_sValue) const;
  bool TryGetParam2Vec3(plVec3& out_vValue) const;
};

//////////////////////////////////////////////////////////////////////////
// Object Manager Messages
//////////////////////////////////////////////////////////////////////////

/// @brief Broadcast before a network object is spawned (server-side).
/// Handlers can set m_bAllowSpawn to false to reject the spawn.
/// Because SendMessage is synchronous, the result is available immediately.
struct PL_NETWORKPLUGIN_DLL plMsgNetworkValidateSpawn : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkValidateSpawn, plMessage);

  plUInt32 m_uiClientID = 0;
  plHashedString m_sPrefabGuid;
  bool m_bAllowSpawn = true; ///< Set to false in a handler to reject the spawn
};

/// @brief Broadcast after a network object is spawned (server and client).
struct PL_NETWORKPLUGIN_DLL plMsgNetworkObjectSpawned : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgNetworkObjectSpawned, plMessage);

  plGameObject* m_pSpawnedObject = nullptr;
  plUInt32 m_uiNetworkID = 0;
  plUInt32 m_uiOwnerClientID = 0;
};
