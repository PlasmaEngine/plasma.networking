#include <EditorPluginNetworking/EditorPluginNetworkingPCH.h>

#include <EditorPluginNetworking/PropertyGrid/ReplicationRowDecorator.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

static plQtNetworkReplicationRowDecorator s_NetworkReplicationDecorator;

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
  plQtPropertyGridWidget::RegisterRowDecorator(&s_NetworkReplicationDecorator);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(NetworkReplicationPropertyMetaState);
}

PL_PLUGIN_ON_UNLOADED()
{
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(NetworkReplicationPropertyMetaState);
  plQtPropertyGridWidget::UnregisterRowDecorator(&s_NetworkReplicationDecorator);
}
