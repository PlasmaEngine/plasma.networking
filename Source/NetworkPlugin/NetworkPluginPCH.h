#pragma once
#include <JoltPlugin/JoltPluginPCH.h>
#include <NetworkPlugin/NetworkPluginDLL.h>

#include <Foundation/Basics.h>
#include <Foundation/Logging/Log.h>

// Include core network types (message IDs, enums, etc.) everywhere
#include <NetworkPlugin/Core/NetworkTypes.h>

// Include Windows.h the proper way before enet can include it improperly
#ifdef PL_PLATFORM_WINDOWS
#  include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#endif