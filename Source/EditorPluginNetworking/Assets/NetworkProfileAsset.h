#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <NetworkPlugin/Core/NetworkReplicationProfileResource.h>

class plNetworkProfileAssetDocument : public plSimpleAssetDocument<plNetworkReplicationProfileDescriptor>
{
  PL_ADD_DYNAMIC_REFLECTION(plNetworkProfileAssetDocument, plSimpleAssetDocument<plNetworkReplicationProfileDescriptor>);

public:
  plNetworkProfileAssetDocument(plStringView sDocumentPath);

protected:
  plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& assetHeader, plBitflags<plTransformFlags> transformFlags) override;
};
