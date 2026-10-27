#pragma once

#include <GuiFoundation/PropertyGrid/PropertyGridExtension.h>

class plDocumentObject;
class plObjectAccessorBase;

/// \brief A network button on component property rows: on while the property is replicated
/// (listed in the game object's plNetworkedPropertiesComponent), mixed when only some selected
/// objects replicate it. Clicking adds or removes the entries in one undo step, adding the
/// replication component when there is none. The row's context menu offers the same and selects
/// the replication entry.
class plNetworkReplicationRowDecorator final : public plPropertyGridRowDecorator
{
public:
  bool GetState(const plPropertyGridModel::Row& row, bool& out_bActive, bool& out_bMixed, plStringBuilder& out_sTooltip) const override;
  void Toggle(const plPropertyGridModel::Row& row) const override;
  plStringView GetIconResource() const override { return ":/EditorPluginNetworking/Icons/NetworkSignal.svg"; }
  void ExtendMenu(plEditorUiMenuBuilder& ref_menu, const plPropertyGridModel::Row& row, plDocument* pDocument) const override;

private:
  static bool CanDecorate(const plPropertyGridModel::Row& row);
  static bool IsEligible(const plPropertyGridModel::Row& row);
  static const plDocumentObject* FindReplicationComponent(const plDocumentObject* pComponent);
  static const plDocumentObject* FindEntry(plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty);
  static plUInt8 GetComponentTypeIndex(const plDocumentObject* pComponent);
  static bool AddEntry(plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty);
  static bool RemoveEntry(plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty);
  static void SetReplicated(const plPropertyGridModel::Row& row, bool bReplicated);
};
