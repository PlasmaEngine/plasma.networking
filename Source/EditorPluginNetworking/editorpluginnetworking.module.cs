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

        // Context menu items and the icon folder are EditorUi's.
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorUi");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plGameUiPlugin");
    }
}
