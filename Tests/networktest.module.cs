using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class NetworkTestModule : ModuleRules
{
    public NetworkTestModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Tests", "NetworkTestPCH.h",
            "BUILDSYSTEM_BUILDING_NETWORKTEST");
        PublicIncludePaths.Add(Path.Combine(context.ProjectRoot, "Tests"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code", "UnitTests"));
        PlasmaPackageSdk.AddImportLibrary(this, context, "plTestFramework");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/NetworkPlugin", "plNetworkPlugin");
        PlasmaPackageSdk.ConfigureEnetBuiltIn(this, context);
    }
}
