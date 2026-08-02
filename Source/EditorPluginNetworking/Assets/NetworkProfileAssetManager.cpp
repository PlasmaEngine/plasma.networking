#include <EditorPluginNetworking/EditorPluginNetworkingPCH.h>

#include <EditorPluginNetworking/Assets/NetworkProfileAsset.h>
#include <EditorPluginNetworking/Assets/NetworkProfileAssetManager.h>
#include <EditorPluginNetworking/Assets/NetworkProfileAssetWindow.moc.h>
#include <GuiFoundation/UIServices/ImageCache.moc.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plNetworkProfileAssetDocumentManager, 1, plRTTIDefaultAllocator<plNetworkProfileAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plNetworkProfileAssetDocumentManager::plNetworkProfileAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plNetworkProfileAssetDocumentManager::OnDocumentManagerEvent, this));
  m_DocTypeDesc.m_sDocumentTypeName = "NetworkReplicationProfile";
  m_DocTypeDesc.m_sFileExtension = "plNetworkProfileAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/NetworkProfile.svg";
  m_DocTypeDesc.m_sAssetCategory = "Networking";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plNetworkProfileAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_NetworkReplicationProfile");
  m_DocTypeDesc.m_sResourceFileExtension = "plNetworkProfile";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave;
  plQtImageCache::GetSingleton()->RegisterTypeImage("NetworkReplicationProfile", QPixmap(m_DocTypeDesc.m_sIcon.GetData()));
}

plNetworkProfileAssetDocumentManager::~plNetworkProfileAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plNetworkProfileAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plNetworkProfileAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  if (e.m_Type == plDocumentManager::Event::Type::DocumentWindowRequested &&
      e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plNetworkProfileAssetDocument>())
    new plQtNetworkProfileAssetDocumentWindow(e.m_pDocument);
}

void plNetworkProfileAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath,
  bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  PL_IGNORE_UNUSED(sDocumentTypeName);
  PL_IGNORE_UNUSED(bCreateNewDocument);
  PL_IGNORE_UNUSED(pOpenContext);
  out_pDocument = new plNetworkProfileAssetDocument(sPath);
}

void plNetworkProfileAssetDocumentManager::InternalGetSupportedDocumentTypes(
  plDynamicArray<const plDocumentTypeDescriptor*>& inout_documentTypes) const
{
  inout_documentTypes.PushBack(&m_DocTypeDesc);
}
