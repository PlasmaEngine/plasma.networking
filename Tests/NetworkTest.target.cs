using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class NetworkTestTarget : TargetRules
{
    public NetworkTestTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputName = "NetworkTest";
        OutputDirectory = Path.Combine(context.ProjectRoot, "Bin", "Tests", $"{context.Platform}_x64_{context.Configuration}");
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        GenerateDebugInfo = true;
        TargetDependencies.Add("NetworkPlugin");
        ExtraModules.Add("NetworkTestModule");
    }
}
