using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginNetworkingModule : ModuleRules
{
    public EditorPluginNetworkingModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginNetworking",
            "EditorPluginNetworkingPCH.h", "BUILDSYSTEM_BUILDING_EDITORPLUGINNETWORKING_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/NetworkPlugin", "plNetworkPlugin");

        PlasmaPackageQt.ConfigureQtModule(this, context, "EditorPluginNetworkingModule",
            "Source/EditorPluginNetworking", "Core", "Gui", "Widgets");
    }
}
