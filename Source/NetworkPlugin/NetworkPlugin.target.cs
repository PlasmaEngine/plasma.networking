using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class NetworkPluginTarget : TargetRules
{
    public NetworkPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plNetworkPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        ExtraModules.Add("NetworkPluginModule");
    }
}
