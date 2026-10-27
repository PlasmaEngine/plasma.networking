#include <EditorPluginNetworking/EditorPluginNetworkingPCH.h>

#include <EditorPluginNetworking/PropertyGrid/ReplicationRowDecorator.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Configuration/Plugin.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

static plNetworkReplicationRowDecorator s_NetworkReplicationDecorator;
static plString s_sIconFolder;

/// The package's editor icons (":/AssetIcons/...", ":/TypeIcons/..." are files under EditorIcons): the plugin
/// binary sits in <package>/Bin/<config>, both in the source tree and in an installed copy.
static plString GetIconFolder()
{
  plStringBuilder sBinary, sCopy;
  plPlugin::GetPluginPaths("plEditorPluginNetworking", sBinary, sCopy, 0);

  plStringBuilder sFolder = sBinary.GetFileDirectory();
  sFolder.PathParentDirectory(2);
  sFolder.AppendPath("EditorIcons");
  return sFolder;
}

static void NetworkReplicationPropertyMetaState(plPropertyMetaStateEvent& e)
{
  if (e.m_pObject == nullptr || !e.m_pObject->GetType()->GetTypeName().IsEqual("plNetworkedPropertyEntry"))
    return;

  const plVariant profile = e.m_pObject->GetTypeAccessor().GetValue("Profile");
  if (!profile.IsA<plString>() || profile.Get<plString>().IsEmpty())
    return;

  auto& states = *e.m_pPropertyStates;
  states["Condition"].m_Visibility = plPropertyUiState::Disabled;
  states["Target"].m_Visibility = plPropertyUiState::Disabled;
  states["Encoding"].m_Visibility = plPropertyUiState::Disabled;
  states["Priority"].m_Visibility = plPropertyUiState::Disabled;
}

PL_PLUGIN_ON_LOADED()
{
  s_sIconFolder = GetIconFolder();
  plEditorUi::AddIconFolder(s_sIconFolder);

  plPropertyGridRowDecorator::Register(&s_NetworkReplicationDecorator);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(NetworkReplicationPropertyMetaState);
}

PL_PLUGIN_ON_UNLOADED()
{
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(NetworkReplicationPropertyMetaState);
  plPropertyGridRowDecorator::Unregister(&s_NetworkReplicationDecorator);

  plEditorUi::RemoveIconFolder(s_sIconFolder);
  s_sIconFolder.Clear();
}
