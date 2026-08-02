using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class NetworkPluginModule : ModuleRules
{
    public NetworkPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/NetworkPlugin", "NetworkPluginPCH.h",
            "BUILDSYSTEM_BUILDING_NETWORKPLUGIN_LIB");

        // Both enet and Jolt stay in the engine and are read out of the SDK.
        PlasmaPackageSdk.ConfigureEnetBuiltIn(this, context);
        PlasmaPackageSdk.ConfigureJoltBuiltIn(this, context);
    }
}
