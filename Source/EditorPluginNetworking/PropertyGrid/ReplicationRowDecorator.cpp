#include <EditorPluginNetworking/EditorPluginNetworkingPCH.h>

#include <EditorPluginNetworking/PropertyGrid/ReplicationRowDecorator.h>

#include <EditorUi/MenuBuilder.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Object/ObjectAccessorBase.h>

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
} // namespace

bool plNetworkReplicationRowDecorator::CanDecorate(const plPropertyGridModel::Row& row)
{
  const plAbstractProperty* pProperty = row.m_pProperty;
  if (pProperty == nullptr || row.m_pType == nullptr || row.m_pAccessor == nullptr)
    return false;

  // Element rows of containers are not replicated on their own.
  if (!row.m_Items.IsEmpty() && row.m_Items[0].m_Index.IsValid())
    return false;

  if (pProperty->GetCategory() == plPropertyCategory::Set && plStringUtils::IsEqual(pProperty->GetPropertyName(), "Tags"))
    return true;
  if (pProperty->GetCategory() != plPropertyCategory::Member || pProperty->GetFlags().IsAnySet(plPropertyFlags::ReadOnly | plPropertyFlags::Pointer))
    return false;
  return pProperty->GetFlags().IsSet(plPropertyFlags::IsEnum) || IsSupportedVariantType(pProperty->GetSpecificType()->GetVariantType());
}

bool plNetworkReplicationRowDecorator::IsEligible(const plPropertyGridModel::Row& row)
{
  if (!CanDecorate(row) || row.m_Items.IsEmpty())
    return false;

  for (const plPropertySelection& selection : row.m_Items)
  {
    if (IsGameObject(selection.m_pObject))
      continue;
    if (selection.m_pObject == nullptr || selection.m_pObject->GetParent() == nullptr || selection.m_pObject->GetParentProperty() != "Components")
      return false;
  }
  return true;
}

const plDocumentObject* plNetworkReplicationRowDecorator::FindReplicationComponent(const plDocumentObject* pComponent)
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

plUInt8 plNetworkReplicationRowDecorator::GetComponentTypeIndex(const plDocumentObject* pComponent)
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

const plDocumentObject* plNetworkReplicationRowDecorator::FindEntry(
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

bool plNetworkReplicationRowDecorator::AddEntry(
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

bool plNetworkReplicationRowDecorator::RemoveEntry(
  plObjectAccessorBase* pAccessor, const plDocumentObject* pComponent, const plAbstractProperty* pProperty)
{
  const plDocumentObject* pEntry = FindEntry(pAccessor, pComponent, pProperty);
  return pEntry == nullptr || pAccessor->RemoveObject(pEntry).Succeeded();
}

bool plNetworkReplicationRowDecorator::GetState(
  const plPropertyGridModel::Row& row, bool& out_bActive, bool& out_bMixed, plStringBuilder& out_sTooltip) const
{
  if (!IsEligible(row))
    return false;

  plUInt32 uiReplicated = 0;
  for (const plPropertySelection& selection : row.m_Items)
  {
    if (FindEntry(row.m_pAccessor, selection.m_pObject, row.m_pProperty) != nullptr)
      ++uiReplicated;
  }

  out_bActive = uiReplicated == row.m_Items.GetCount();
  out_bMixed = uiReplicated > 0 && uiReplicated < row.m_Items.GetCount();
  out_sTooltip = out_bMixed ? "Mixed replication state \xE2\x80\x94 click to replicate all" : (out_bActive ? "Replicated \xE2\x80\x94 click to stop replicating" : "Not replicated \xE2\x80\x94 click to replicate");
  return true;
}

void plNetworkReplicationRowDecorator::SetReplicated(const plPropertyGridModel::Row& row, bool bReplicated)
{
  if (!IsEligible(row))
    return;

  plObjectAccessorBase* pAccessor = row.m_pAccessor;
  pAccessor->StartTransaction(bReplicated ? "Replicate Property" : "Stop Replicating Property");
  bool bSuccess = true;
  for (const plPropertySelection& selection : row.m_Items)
  {
    bSuccess &= bReplicated ? AddEntry(pAccessor, selection.m_pObject, row.m_pProperty) : RemoveEntry(pAccessor, selection.m_pObject, row.m_pProperty);
  }

  if (bSuccess)
    pAccessor->FinishTransaction();
  else
    pAccessor->CancelTransaction();
}

void plNetworkReplicationRowDecorator::Toggle(const plPropertyGridModel::Row& row) const
{
  bool bActive = false;
  bool bMixed = false;
  plStringBuilder tooltip;
  if (GetState(row, bActive, bMixed, tooltip))
    SetReplicated(row, !bActive || bMixed);
}

void plNetworkReplicationRowDecorator::ExtendMenu(plEditorUiMenuBuilder& ref_menu, const plPropertyGridModel::Row& row, plDocument* pDocument) const
{
  bool bActive = false;
  bool bMixed = false;
  plStringBuilder tooltip;
  if (!GetState(row, bActive, bMixed, tooltip))
    return;

  // The menu keeps the row alive until its items have run, so the items can refer to it.
  ref_menu.AddSeparator();
  plEditorUiMenuItem& replicate = ref_menu.AddAction("Replicate Property", [&row]()
    { SetReplicated(row, true); });
  replicate.m_bEnabled = !bActive || bMixed;

  plEditorUiMenuItem& stop = ref_menu.AddAction("Stop Replicating", [&row]()
    { SetReplicated(row, false); });
  stop.m_bEnabled = bActive || bMixed;

  if (const plDocumentObject* pEntry = FindEntry(row.m_pAccessor, row.m_Items[0].m_pObject, row.m_pProperty); pEntry != nullptr && pDocument != nullptr)
  {
    ref_menu.AddAction("Select Replication Entry", [pDocument, pEntry]()
      { pDocument->GetSelectionManager()->SetSelection(pEntry); });
  }
}
