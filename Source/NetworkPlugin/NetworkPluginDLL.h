#pragma once

#include <Foundation/Basics.h>

// Configure the DLL Import/Export Define
#if PL_ENABLED(PL_COMPILE_ENGINE_AS_DLL)
#  ifdef BUILDSYSTEM_BUILDING_NETWORKPLUGIN_LIB
#    define PL_NETWORKPLUGIN_DLL PL_DECL_EXPORT
#  elif defined(BUILDSYSTEM_FOLDING_PLUGIN_IMPORTS)
#    define PL_NETWORKPLUGIN_DLL
#  else
#    define PL_NETWORKPLUGIN_DLL PL_DECL_IMPORT
#  endif
#else
#  define PL_NETWORKPLUGIN_DLL
#endif

#include <NetworkPlugin/Core/NetworkTypes.h>

#define NETWORK_PROTOCOL_VERSION 2
