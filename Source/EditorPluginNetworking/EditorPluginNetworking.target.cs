using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginNetworkingTarget : TargetRules
{
    public EditorPluginNetworkingTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEditorPluginNetworking";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        TargetDependencies.Add("NetworkPlugin");

        ExtraModules.Add("EditorPluginNetworkingModule");
    }
}
