#include <EditorPluginNetworking/EditorPluginNetworkingPCH.h>

#include <EditorPluginNetworking/PropertyGrid/ReplicationRowDecorator.h>

#include <GuiFoundation/PropertyGrid/PropertyBaseWidget.moc.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Object/ObjectAccessorBase.h>

#include <QAction>
#include <QMenu>

namespace
{
  bool IsGameObject(const plDocumentObject* pObject)
  {
    return pObject != nullptr && pObject->GetType()->GetTypeName().IsEqual("plGameObject");
  }

  bool IsSupportedVariantType(plVariantType::Enum type)
  {
    switch (type)
    {
      case plVariantType::Bool:
      case plVariantType::Int8:
      case plVariantType::UInt8:
      case plVariantType::Int16:
      case plVariantType::UInt16:
      case plVariantType::Int32:
      case plVariantType::UInt32:
      case plVariantType::Int64:
      case plVariantType::UInt64:
      case plVariantType::Float:
      case plVariantType::Double:
      case plVariantType::Vector2:
      case plVariantType::Vector3:
      case plVariantType::Vector4:
      case plVariantType::Quaternion:
      case plVariantType::String:
      case plVariantType::Color:
        return true;
      default:
        return false;
    }
  }
}

bool plQtNetworkReplicationRowDecorator::IsEligible(plQtPropertyWidget* pWidget)
{
  if (!plQtNetworkReplicationRowDecorator().CanDecorate(pWidget) || pWidget->GetSelection().IsEmpty())
    return false;

  for (const plPropertySelection& selection : pWidget->GetSelection())
  {
    if (IsGameObject(selection.m_pObject))
      continue;
    if (selection.m_pObject == nullptr || selection.m_pObject->GetParent() == nullptr || selection.m_pObject->GetParentProperty() != "Components")
      return false;
  }
  return true;
}

bool plQtNetworkReplicationRowDecorator::CanDecorate(plQtPropertyWidget* pWidget) const
{
  if (pWidget == nullptr || pWidget->GetType() == nullptr || pWidget->GetProperty() == nullptr)
    return false;
  const plAbstractProperty* pProperty = pWidget->GetProperty();
  if (pProperty->GetCategory() == plPropertyCategory::Set && plStringUtils::IsEqual(pProperty->GetPropertyName(), "Tags"))
    return true;
  if (pProperty->GetCategory() != plPropertyCategory::Member || pProperty->GetFlags().IsAnySet(plPropertyFlags::ReadOnly | plPropertyFlags::Pointer))
    return false;
  return pProperty->GetFlags().IsSet(plPropertyFlags::IsEnum) || IsSupportedVariantType(pProperty->GetSpecificType()->GetVariantType());
}

const plDocumentObject* plQtNetworkReplicationRowDecorator::FindReplicationComponent(const plDocumentObject* pComponent)
{
  const plDocumentObject* pGameObject = IsGameObject(pComponent) ? pComponent : (pComponent != nullptr ? pComponent->GetParent() : nullptr);
  if (pGameObject == nullptr)
    return nullptr;

  for (const plDocumentObject* pChild : pGameObject->GetChildren())
  {
    if (pChild->GetParentProperty() == "Components" && pChild->GetType()->GetTypeName() == "plNetworkedPropertiesComponent")
      return pChild;
  }
  return nullptr;
}

plUInt8 plQtNetworkReplicationRowDecorator::GetComponentTypeIndex(const plDocumentObject* pComponent)
{
  if (IsGameObject(pComponent))
    return 0;

  plUInt8 uiIndex = 0;
  for (const plDocumentObject* pSibling : pComponent->GetParent()->GetChildren())
  {
    if (pSibling == pComponent)
      break;
    if (pSibling->GetParentProperty() == "Components" && pSibling->GetType() == pComponent->GetType())
      ++uiIndex;
  }
  return uiIndex;
}

const plDocumentObject* plQtNetworkReplicationRowDecorator::FindEntry(
  plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty)
{
  const plDocumentObject* pReplication = FindReplicationComponent(pComponent);
  if (pReplication == nullptr)
    return nullptr;

  const plStringView sTypeName = pComponent->GetType()->GetTypeName();
  const plUInt8 uiTypeIndex = GetComponentTypeIndex(pComponent);
  for (const plDocumentObject* pEntry : pReplication->GetChildren())
  {
    if (pEntry->GetParentProperty() != "Properties")
      continue;

    plVariant componentType;
    plVariant componentIndex;
    plVariant propertyName;
    if (pAccessor->GetValueByName(pEntry, "ComponentType", componentType).Succeeded() &&
        pAccessor->GetValueByName(pEntry, "ComponentIndex", componentIndex).Succeeded() &&
        pAccessor->GetValueByName(pEntry, "Property", propertyName).Succeeded() &&
        componentType.ConvertTo<plString>().IsEqual(sTypeName) && componentIndex.ConvertTo<plUInt8>() == uiTypeIndex &&
        propertyName.ConvertTo<plString>().IsEqual(pProperty->GetPropertyName()))
    {
      return pEntry;
    }
  }
  return nullptr;
}

bool plQtNetworkReplicationRowDecorator::AddEntry(
  plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty)
{
  if (FindEntry(pAccessor, pComponent, pProperty) != nullptr)
    return true;

  const plDocumentObject* pReplication = FindReplicationComponent(pComponent);
  if (pReplication == nullptr)
  {
    const plDocumentObject* pGameObject = IsGameObject(pComponent) ? pComponent : pComponent->GetParent();
    plUuid componentGuid = plUuid::MakeUuid();
    const plRTTI* pReplicationType = plRTTI::FindTypeByName("plNetworkedPropertiesComponent");
    if (pReplicationType == nullptr)
    {
      plLog::Error("Networking editor: plNetworkedPropertiesComponent RTTI is unavailable.");
      return false;
    }
    if (pAccessor->AddObjectByName(pGameObject, "Components", -1, pReplicationType, componentGuid).Failed())
      return false;
    pReplication = pAccessor->GetObject(componentGuid);
  }

  const plRTTI* pEntryType = plRTTI::FindTypeByName("plNetworkedPropertyEntry");
  if (pEntryType == nullptr)
  {
    plLog::Error("Networking editor: plNetworkedPropertyEntry RTTI is unavailable.");
    return false;
  }

  plUuid entryGuid = plUuid::MakeUuid();
  if (pAccessor->AddObjectByName(pReplication, "Properties", -1, pEntryType, entryGuid).Failed())
    return false;

  const plDocumentObject* pEntry = pAccessor->GetObject(entryGuid);
  return pEntry != nullptr &&
         pAccessor->SetValueByName(pEntry, "ComponentType", plString(pComponent->GetType()->GetTypeName())).Succeeded() &&
         pAccessor->SetValueByName(pEntry, "ComponentIndex", GetComponentTypeIndex(pComponent)).Succeeded() &&
         pAccessor->SetValueByName(pEntry, "Property", plString(pProperty->GetPropertyName())).Succeeded();
}

bool plQtNetworkReplicationRowDecorator::RemoveEntry(
  plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty)
{
  const plDocumentObject* pEntry = FindEntry(pAccessor, pComponent, pProperty);
  return pEntry == nullptr || pAccessor->RemoveObject(pEntry).Succeeded();
}

bool plQtNetworkReplicationRowDecorator::GetState(
  plQtPropertyWidget* pWidget, bool& out_bActive, bool& out_bMixed, plStringBuilder& out_sTooltip) const
{
  if (!IsEligible(pWidget))
    return false;

  plUInt32 uiReplicated = 0;
  for (const plPropertySelection& selection : pWidget->GetSelection())
  {
    if (FindEntry(pWidget->GetObjectAccessor(), selection.m_pObject, pWidget->GetProperty()) != nullptr)
      ++uiReplicated;
  }

  out_bActive = uiReplicated == pWidget->GetSelection().GetCount();
  out_bMixed = uiReplicated > 0 && uiReplicated < pWidget->GetSelection().GetCount();
  out_sTooltip = out_bMixed ? "Mixed replication state — click to replicate all" :
    (out_bActive ? "Replicated — click to stop replicating" : "Not replicated — click to replicate");
  return true;
}

void plQtNetworkReplicationRowDecorator::SetReplicated(plQtPropertyWidget* pWidget, bool bReplicated)
{
  if (!IsEligible(pWidget))
    return;

  plObjectAccessorBase* pAccessor = pWidget->GetObjectAccessor();
  pAccessor->StartTransaction(bReplicated ? "Replicate Property" : "Stop Replicating Property");
  bool bSuccess = true;
  for (const plPropertySelection& selection : pWidget->GetSelection())
  {
    bSuccess &= bReplicated ? AddEntry(pAccessor, selection.m_pObject, pWidget->GetProperty()) :
                              RemoveEntry(pAccessor, selection.m_pObject, pWidget->GetProperty());
  }

  if (bSuccess)
    pAccessor->FinishTransaction();
  else
    pAccessor->CancelTransaction();
}

void plQtNetworkReplicationRowDecorator::Toggle(plQtPropertyWidget* pWidget)
{
  bool bActive = false;
  bool bMixed = false;
  plStringBuilder tooltip;
  if (GetState(pWidget, bActive, bMixed, tooltip))
    SetReplicated(pWidget, !bActive || bMixed);
}

void plQtNetworkReplicationRowDecorator::ExtendContextMenu(QMenu& ref_menu, plQtPropertyWidget* pWidget)
{
  bool bActive = false;
  bool bMixed = false;
  plStringBuilder tooltip;
  if (!GetState(pWidget, bActive, bMixed, tooltip))
    return;

  ref_menu.addSeparator();
  QAction* pReplicate = ref_menu.addAction("Replicate Property");
  pReplicate->setEnabled(!bActive || bMixed);
  QObject::connect(pReplicate, &QAction::triggered, pReplicate, [pWidget]() { SetReplicated(pWidget, true); });

  QAction* pStop = ref_menu.addAction("Stop Replicating");
  pStop->setEnabled(bActive || bMixed);
  QObject::connect(pStop, &QAction::triggered, pStop, [pWidget]() { SetReplicated(pWidget, false); });

  if (const plDocumentObject* pEntry = FindEntry(pWidget->GetObjectAccessor(), pWidget->GetSelection()[0].m_pObject, pWidget->GetProperty()))
  {
    QAction* pSelect = ref_menu.addAction("Select Replication Entry");
    QObject::connect(pSelect, &QAction::triggered, pSelect, [pWidget, pEntry]()
      {
        if (plDocument* pDocument = const_cast<plDocument*>(pWidget->GetPropertyGrid()->GetDocument()))
          pDocument->GetSelectionManager()->SetSelection(pEntry);
      });
  }
}
