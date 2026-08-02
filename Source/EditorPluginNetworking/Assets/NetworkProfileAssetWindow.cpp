#include <EditorPluginNetworking/EditorPluginNetworkingPCH.h>

#include <EditorFramework/Assets/AssetDocument.h>
#include <EditorFramework/Assets/AssetStatusIndicator.moc.h>
#include <EditorPluginNetworking/Assets/NetworkProfileAssetWindow.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <GuiFoundation/PropertyGrid/PropertyGridWidget.moc.h>

#include <QVBoxLayout>

plQtNetworkProfileAssetDocumentWindow::plQtNetworkProfileAssetDocumentWindow(plDocument* pDocument)
  : plQtDocumentWindow(pDocument)
{
  plQtDocumentPanel* pPropertyPanel = new plQtDocumentPanel(this, pDocument);
  pPropertyPanel->setObjectName("NetworkProfileDockWidget");
  pPropertyPanel->setWindowTitle("Replication Profile");

  QWidget* pContainer = new QWidget();
  pContainer->setLayout(new QVBoxLayout());
  pContainer->layout()->setContentsMargins(0, 0, 0, 0);
  pContainer->layout()->addWidget(new plQtAssetStatusIndicator(static_cast<plAssetDocument*>(pDocument)));
  pContainer->layout()->addWidget(new plQtPropertyGridWidget(pContainer, pDocument));
  pPropertyPanel->setWidget(pContainer, ads::CDockWidget::ForceNoScrollArea);
  m_pDockManager->setCentralWidget(pPropertyPanel);
  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);
  FinishWindowCreation();
}
