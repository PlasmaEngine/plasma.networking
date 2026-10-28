#include <EditorPluginNetworking/EditorPluginNetworkingPCH.h>

#include <EditorPluginNetworking/Assets/NetworkProfileAssetWindow.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

plNetworkProfileAssetDocumentWindow::plNetworkProfileAssetDocumentWindow(plDocument* pDocument)
  : plDocumentWindow(pDocument)
{
  // the property grid is the whole document
  AddPropertiesPanel({"NetworkProfileDockWidget", "Replication Profile"});
  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);
}
