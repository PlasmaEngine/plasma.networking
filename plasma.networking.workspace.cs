// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaNetworkingWorkspace : WorkspaceRules
{
    public PlasmaNetworkingWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("NetworkPlugin");
        TargetNames.Add("EditorPluginNetworking");
        TargetNames.Add("NetworkTest");
    }
}
