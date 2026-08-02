#pragma once

#include <GuiFoundation/PropertyGrid/PropertyGridWidget.moc.h>

class plDocumentObject;
class plObjectAccessorBase;

class plQtNetworkReplicationRowDecorator final : public plQtPropertyRowDecorator
{
public:
  bool CanDecorate(plQtPropertyWidget* pWidget) const override;
  bool GetState(plQtPropertyWidget* pWidget, bool& out_bActive, bool& out_bMixed, plStringBuilder& out_sTooltip) const override;
  plStringView GetIconResource() const override { return ":/EditorPluginNetworking/Icons/NetworkSignal.svg"; }
  void Toggle(plQtPropertyWidget* pWidget) override;
  void ExtendContextMenu(QMenu& ref_menu, plQtPropertyWidget* pWidget) override;

private:
  static bool IsEligible(plQtPropertyWidget* pWidget);
  static const plDocumentObject* FindReplicationComponent(const plDocumentObject* pComponent);
  static const plDocumentObject* FindEntry(plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty);
  static plUInt8 GetComponentTypeIndex(const plDocumentObject* pComponent);
  static bool AddEntry(plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty);
  static bool RemoveEntry(plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty);
  static void SetReplicated(plQtPropertyWidget* pWidget, bool bReplicated);
};
