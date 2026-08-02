#pragma once

#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Types/Enum.h>

#include <NetworkPlugin/NetworkPluginDLL.h>

struct plNetworkRoleEnum
{
  using StorageType = plUInt32;

  enum Enum
  {
    Undefined,
    Server,
    Client,
    Host,
    Both,
    Default = Undefined
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkRoleEnum);

struct plNetworkEventTypeEnum
{
  using StorageType = plUInt32;

  enum Enum
  {
    Undefined ,
    Connected,
    Disconnected,
    Timeout,
    Message,
    Default = Undefined
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkEventTypeEnum);

struct plNetworkModeEnum
{
  using StorageType = plUInt32;

  enum Enum
  {
    Offline,
    Client,
    Server,
    Default = Offline
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkModeEnum);

struct plNetworkChannelTypeEnum
{
  using StorageType = plUInt32;

  enum Enum
  {
    None,     /// Invalid channel
    Unreliable, /// Message may be lost or out of order.
    UnreliableOrdered, /// Messages may be lost but will always be in order.
    Reliable, /// Messages will always be received bit may be out of order.
    ReliableOrdered, /// messages will always be received and will be in order
    Default = None
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkChannelTypeEnum);

struct plNetworkPriority
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    Critical = 0,
    High = 1,
    Normal = 2,
    Low = 3,
    VeryLow = 4,
    Default = Normal
  };
};

struct plDriverTypeEnum
{
  using StorageType = plUInt32;

  enum Enum
  {
    ENet,
    Default = ENet
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plDriverTypeEnum);

struct PL_NETWORKPLUGIN_DLL plNetworkConfig : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plNetworkConfig, plReflectedClass);

public:

  plNetworkConfig() = default;
  plNetworkConfig(const plNetworkConfig& other) = default;
  plNetworkConfig(plNetworkConfig&& other) = default;
  plNetworkConfig& operator=(const plNetworkConfig& other) = default;

  /// \brief Define the network driver that in use
  plDriverTypeEnum::Enum m_DriverType = plDriverTypeEnum::ENet;

  /// \brief Maximum number of connections
  plUInt16 m_uiConnectionLimit = 32;

  /// \brief Server address to connect to.
  plString m_sServerAddress = "127.0.0.1";

  /// \brief Server port to connect to.
  plUInt16 m_uiServerPort = 7777;

  /// \brief Maximum message size in bytes
  plUInt16 m_uiMessageSize = 1500;

  /// \brief The size of the message pool. Beyond this will case crashes.
  plUInt16 m_uiMessagePoolSize = 2048;
};

struct plNetworkConnectionState
{
  using StorageType = plUInt32;

  enum Enum
  {
    Offline,
    Connecting,
    Connected,
    Disconnecting,
    Disconnected,
    Default = Offline
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkConnectionState);

struct plNetworkObjectRole
{
  using StorageType = plUInt32;

  enum Enum
  {
    None,
    OwnedAuthoritative,
    Replicated,
    ReplicatedSimulated,
    Default = None
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkObjectRole);

struct PL_NETWORKPLUGIN_DLL plNetworkConnection : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plNetworkConnection, plReflectedClass);

public:
  plNetworkConnection() = default;
  plNetworkConnection(const plNetworkConnection& other) = default;
  plNetworkConnection(plNetworkConnection&& other) = default;
  plNetworkConnection& operator=(const plNetworkConnection& other) = default;
  PL_ALWAYS_INLINE bool operator==(const plNetworkConnection& other) const { return m_uiConnectionID == other.m_uiConnectionID; }

  /// \brief Id of the active connection
  plUInt32 m_uiConnectionID;
};

struct PL_NETWORKPLUGIN_DLL plNetworkStats : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plNetworkStats, plReflectedClass);

public:
  plNetworkStats() = default;
  plNetworkStats(const plNetworkStats& other) = default;
  plNetworkStats(plNetworkStats&& other) = default;
  plNetworkStats& operator=(const plNetworkStats& other) = default;

  float m_fRountTripTime = 0.0f;
  plUInt32 m_uiPacketsLost= 0;
  plUInt64 m_uiTotalPacketsSent = 0;
  plUInt32 m_uiTotalPacketsLost = 0;
  plUInt64 m_uiTotalBytesSent = 0;
  plUInt64 m_uiTotalBytesReceived = 0;
};

struct plNetworkMessageIDs
{
  using StorageType = plUInt8;

  enum Enum
  {
    Undefined = 0,
    
    //////////////////////////////////////////////////////////////////////////
    // Core Protocol Messages (0-63)
    //////////////////////////////////////////////////////////////////////////
    Handshake,
    HandshakeReply,
    ObjectReplicate,
    ObjectReplicatePart,
    ObjectSpawn,
    ObjectSpawnPart,
    ObjectDespawn,
    ObjectRole,
    ObjectRpc,
    
    // Time synchronization
    TimeSync,           ///< Client -> Server: Request time sync
    TimeSyncResponse,   ///< Server -> Client: Time sync response
    
    // Connection management
    Disconnect,         ///< Graceful disconnect with reason
    Reconnect,          ///< Client attempting to reconnect
    ReconnectResponse,  ///< Server response to reconnect attempt
    
    // Lag compensation
    ServerRewind,       ///< Request server to rewind for hit detection

    // Level management
    LevelLoad,          ///< Server -> Clients: Load a level
    LevelLoadComplete,  ///< Client -> Server: Level loading finished
    LevelStart,         ///< Server -> Clients: Gameplay can begin
    
    //////////////////////////////////////////////////////////////////////////
    // Engine Component Messages (64-127)
    // Reserved for built-in engine components (character controller, etc.)
    //////////////////////////////////////////////////////////////////////////
    EngineMessageStart = 64,
    
    // Client prediction
    PredictedInput = 64,      ///< Client -> Server: redundant fixed-tick input commands
    CharacterSetOwner = 65,   ///< Server -> Clients: Set character owner
    PredictedStateAck = 66,   ///< Server -> owning client: authoritative state acknowledgement

    // Transform replication
    TransformState = 67,      ///< Authority -> Peers: Position/rotation/velocity snapshot
    
    EngineMessageEnd = 127,
    
    //////////////////////////////////////////////////////////////////////////
    // User/Game Messages (128-255)
    // Reserved for game-specific messages
    //////////////////////////////////////////////////////////////////////////
    UserMessageStart = 128,  ///< Start of user-defined message range
    
    Default = Undefined,
    Max = 255
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkMessageIDs);

/// @brief Reasons for network disconnection
struct plNetworkDisconnectReason
{
  using StorageType = plUInt8;

  enum Enum
  {
    Unknown = 0,
    ClientQuit,           ///< Client initiated graceful disconnect
    ServerShutdown,       ///< Server is shutting down
    Kicked,               ///< Client was kicked by server
    Timeout,              ///< Connection timed out
    VersionMismatch,      ///< Protocol/game version mismatch
    AuthenticationFailed, ///< Failed to authenticate
    ServerFull,           ///< Server is full
    RateLimited,          ///< Client exceeded rate limits
    Banned,               ///< Client is banned
    Default = Unknown
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plNetworkDisconnectReason);
