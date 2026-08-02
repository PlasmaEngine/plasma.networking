#include "NetworkPluginPCH.h"

#include <Foundation/Configuration/Startup.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <NetworkPlugin/Core/NetworkReplicationProfileResource.h>


// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(Network, NetworkPluginStartup)

  // list all the subsystems that we want to be initialized first
  BEGIN_SUBSYSTEM_DEPENDENCIES
    "Foundation", // all subsystems from the 'Foundation' group (this is redundant, because `Core` already depends on `Foundation`)
    "Core"        // and all subsystems from the 'Core' group
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
    plResourceManager::RegisterResourceForAssetType("NetworkReplicationProfile", plGetStaticRTTI<plNetworkReplicationProfileResource>());
    const plNetworkReplicationProfileResourceHandle hFallback = plResourceManager::CreateResource<plNetworkReplicationProfileResource>(
      "NetworkReplicationProfileFallback", plNetworkReplicationProfileDescriptor(), "Default network replication profile");
    plResourceManager::SetResourceTypeMissingFallback<plNetworkReplicationProfileResource>(hFallback);
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
    plResourceManager::SetResourceTypeMissingFallback<plNetworkReplicationProfileResource>(plNetworkReplicationProfileResourceHandle());
    plNetworkReplicationProfileResource::CleanupDynamicPluginReferences();
  }

  ON_HIGHLEVELSYSTEMS_STARTUP
  {
  }

  ON_HIGHLEVELSYSTEMS_SHUTDOWN
  {
  }

PL_END_SUBSYSTEM_DECLARATION;
