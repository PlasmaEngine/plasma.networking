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

        // test framework headers and import library from the SDK
        var unitTestsRoot = Path.Combine(PlasmaPackageSdk.Root, "Code", "UnitTests");
        PublicIncludePaths.Add(unitTestsRoot);
        PublicIncludePaths.Add(Path.Combine(unitTestsRoot, "TestFramework"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "UnitTests"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "UnitTests", "TestFramework"));

        if (context.Platform == TargetPlatform.Windows)
        {
            PublicIncludePaths.Add(Path.Combine(unitTestsRoot, "TestFramework", "Platform", "Win"));
        }

        PlasmaPackageSdk.AddImportLibrary(this, context, "plTestFramework");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/NetworkPlugin", "plNetworkPlugin");
        PlasmaPackageSdk.ConfigureEnetBuiltIn(this, context);
    }
}
