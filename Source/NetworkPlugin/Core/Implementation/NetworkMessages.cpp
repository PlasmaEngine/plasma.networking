#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkMessages.h>

// clang-format off

//////////////////////////////////////////////////////////////////////////
// Session Messages
//////////////////////////////////////////////////////////////////////////

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkPlayerJoined);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkPlayerJoined, 1, plRTTIDefaultAllocator<plMsgNetworkPlayerJoined>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkPlayerLeft);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkPlayerLeft, 1, plRTTIDefaultAllocator<plMsgNetworkPlayerLeft>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkSessionStateChanged);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkSessionStateChanged, 1, plRTTIDefaultAllocator<plMsgNetworkSessionStateChanged>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkAllPlayersReady);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkAllPlayersReady, 1, plRTTIDefaultAllocator<plMsgNetworkAllPlayersReady>)
PL_END_DYNAMIC_REFLECTED_TYPE;

//////////////////////////////////////////////////////////////////////////
// Level Manager Messages
//////////////////////////////////////////////////////////////////////////

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkLevelLoadRequested);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkLevelLoadRequested, 1, plRTTIDefaultAllocator<plMsgNetworkLevelLoadRequested>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkLevelStart);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkLevelStart, 1, plRTTIDefaultAllocator<plMsgNetworkLevelStart>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkAllClientsLoaded);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkAllClientsLoaded, 1, plRTTIDefaultAllocator<plMsgNetworkAllClientsLoaded>)
PL_END_DYNAMIC_REFLECTED_TYPE;

//////////////////////////////////////////////////////////////////////////
// World Module Messages
//////////////////////////////////////////////////////////////////////////

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkClientConnected);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkClientConnected, 1, plRTTIDefaultAllocator<plMsgNetworkClientConnected>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkUserMessage);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkUserMessage, 1, plRTTIDefaultAllocator<plMsgNetworkUserMessage>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkedEvent);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkedEvent, 1, plRTTIDefaultAllocator<plMsgNetworkedEvent>)
{
	PL_BEGIN_PROPERTIES
	{
		PL_MEMBER_PROPERTY("EventName", m_sEventName),
		PL_MEMBER_PROPERTY("Param1", m_Param1),
		PL_MEMBER_PROPERTY("Param2", m_Param2),
	}
	PL_END_PROPERTIES;
	PL_BEGIN_FUNCTIONS
	{
		PL_SCRIPT_FUNCTION_PROPERTY(GetParam1Type),
		PL_SCRIPT_FUNCTION_PROPERTY(GetParam2Type),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam1Bool, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam1Int, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam1UInt, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam1Float, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam1Double, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam1String, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam1Vec3, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam2Bool, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam2Int, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam2UInt, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam2Float, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam2Double, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam2String, Out, "Value"),
		PL_SCRIPT_FUNCTION_PROPERTY(TryGetParam2Vec3, Out, "Value"),
	}
	PL_END_FUNCTIONS;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

//////////////////////////////////////////////////////////////////////////
// Object Manager Messages
//////////////////////////////////////////////////////////////////////////

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkValidateSpawn);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkValidateSpawn, 1, plRTTIDefaultAllocator<plMsgNetworkValidateSpawn>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgNetworkObjectSpawned);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgNetworkObjectSpawned, 1, plRTTIDefaultAllocator<plMsgNetworkObjectSpawned>)
PL_END_DYNAMIC_REFLECTED_TYPE;

// clang-format on

namespace
{
	template <typename T>
	bool TryConvertNetworkEventParam(const plVariant& value, T& out_value)
	{
		plResult conversionStatus = PL_FAILURE;
		out_value = value.ConvertTo<T>(&conversionStatus);
		return conversionStatus.Succeeded();
	}
}

plInt32 plMsgNetworkedEvent::GetParam1Type() const
{
	return static_cast<plInt32>(m_Param1.GetType());
}

plInt32 plMsgNetworkedEvent::GetParam2Type() const
{
	return static_cast<plInt32>(m_Param2.GetType());
}

bool plMsgNetworkedEvent::TryGetParam1Bool(bool& out_bValue) const
{
	return TryConvertNetworkEventParam(m_Param1, out_bValue);
}

bool plMsgNetworkedEvent::TryGetParam1Int(plInt32& out_iValue) const
{
	return TryConvertNetworkEventParam(m_Param1, out_iValue);
}

bool plMsgNetworkedEvent::TryGetParam1UInt(plUInt32& out_uiValue) const
{
	return TryConvertNetworkEventParam(m_Param1, out_uiValue);
}

bool plMsgNetworkedEvent::TryGetParam1Float(float& out_fValue) const
{
	return TryConvertNetworkEventParam(m_Param1, out_fValue);
}

bool plMsgNetworkedEvent::TryGetParam1Double(double& out_fValue) const
{
	return TryConvertNetworkEventParam(m_Param1, out_fValue);
}

bool plMsgNetworkedEvent::TryGetParam1String(plString& out_sValue) const
{
	return TryConvertNetworkEventParam(m_Param1, out_sValue);
}

bool plMsgNetworkedEvent::TryGetParam1Vec3(plVec3& out_vValue) const
{
	return TryConvertNetworkEventParam(m_Param1, out_vValue);
}

bool plMsgNetworkedEvent::TryGetParam2Bool(bool& out_bValue) const
{
	return TryConvertNetworkEventParam(m_Param2, out_bValue);
}

bool plMsgNetworkedEvent::TryGetParam2Int(plInt32& out_iValue) const
{
	return TryConvertNetworkEventParam(m_Param2, out_iValue);
}

bool plMsgNetworkedEvent::TryGetParam2UInt(plUInt32& out_uiValue) const
{
	return TryConvertNetworkEventParam(m_Param2, out_uiValue);
}

bool plMsgNetworkedEvent::TryGetParam2Float(float& out_fValue) const
{
	return TryConvertNetworkEventParam(m_Param2, out_fValue);
}

bool plMsgNetworkedEvent::TryGetParam2Double(double& out_fValue) const
{
	return TryConvertNetworkEventParam(m_Param2, out_fValue);
}

bool plMsgNetworkedEvent::TryGetParam2String(plString& out_sValue) const
{
	return TryConvertNetworkEventParam(m_Param2, out_sValue);
}

bool plMsgNetworkedEvent::TryGetParam2Vec3(plVec3& out_vValue) const
{
	return TryConvertNetworkEventParam(m_Param2, out_vValue);
}

PL_STATICLINK_FILE(NetworkPlugin, NetworkPlugin_Core_Implementation_NetworkMessages);
