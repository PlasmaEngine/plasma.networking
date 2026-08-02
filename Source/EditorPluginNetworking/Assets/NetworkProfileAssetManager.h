#pragma once

#include <EditorFramework/Assets/AssetDocumentManager.h>

class plNetworkProfileAssetDocumentManager : public plAssetDocumentManager
{
  PL_ADD_DYNAMIC_REFLECTION(plNetworkProfileAssetDocumentManager, plAssetDocumentManager);

public:
  plNetworkProfileAssetDocumentManager();
  ~plNetworkProfileAssetDocumentManager();
  OutputReliability GetAssetTypeOutputReliability() const override { return OutputReliability::Perfect; }

private:
  void OnDocumentManagerEvent(const plDocumentManager::Event& e);
  void InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument,
    const plDocumentObject* pOpenContext) override;
  void InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_documentTypes) const override;
  bool GeneratesProfileSpecificAssets() const override { return false; }

  plAssetDocumentTypeDescriptor m_DocTypeDesc;
};
