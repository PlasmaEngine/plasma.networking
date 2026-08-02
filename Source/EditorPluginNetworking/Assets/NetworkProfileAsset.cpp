#include <EditorPluginNetworking/EditorPluginNetworkingPCH.h>

#include <EditorPluginNetworking/Assets/NetworkProfileAsset.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plNetworkProfileAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

plNetworkProfileAssetDocument::plNetworkProfileAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plNetworkReplicationProfileDescriptor>(sDocumentPath, plAssetDocEngineConnection::None)
{
}

plTransformStatus plNetworkProfileAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag,
  const plPlatformProfile* pAssetProfile, const plAssetFileHeader& assetHeader, plBitflags<plTransformFlags> transformFlags)
{
  PL_IGNORE_UNUSED(sOutputTag);
  PL_IGNORE_UNUSED(pAssetProfile);
  PL_IGNORE_UNUSED(assetHeader);
  PL_IGNORE_UNUSED(transformFlags);
  GetProperties()->Save(stream);
  return plTransformStatus(PL_SUCCESS);
}
