#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Components/NetworkComponent.h>

using plNetworkDebugComponentManager = plComponentManagerSimple<class plNetworkDebugComponent, plComponentUpdateType::Always>;

/// \brief Provides network debugging visualization and information.
/// Shows network state, replication status, and connection info in-game.
class PL_NETWORKPLUGIN_DLL plNetworkDebugComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkDebugComponent, plNetworkComponent, plNetworkDebugComponentManager);

public:
  plNetworkDebugComponent() = default;
  virtual ~plNetworkDebugComponent() = default;

  // plNetworkComponent interface
  virtual void NetworkSerialize(plNetworkMessage& msg) override {}
  virtual void NetworkDeserialize(plNetworkMessage& msg) override {}

  // Debug visualization toggles
  void SetShowNetworkId(bool bShow) { m_bShowNetworkId = bShow; }
  bool GetShowNetworkId() const { return m_bShowNetworkId; }

  void SetShowReplicationState(bool bShow) { m_bShowReplicationState = bShow; }
  bool GetShowReplicationState() const { return m_bShowReplicationState; }

  void SetShowNetworkRole(bool bShow) { m_bShowNetworkRole = bShow; }
  bool GetShowNetworkRole() const { return m_bShowNetworkRole; }

  // Display settings
  void SetTextScale(float fScale) { m_fTextScale = fScale; }
  float GetTextScale() const { return m_fTextScale; }

  void SetTextOffset(const plVec3& vOffset) { m_vTextOffset = vOffset; }
  plVec3 GetTextOffset() const { return m_vTextOffset; }

  // Colors
  void SetServerColor(const plColor& color) { m_ServerColor = color; }
  void SetClientColor(const plColor& color) { m_ClientColor = color; }
  void SetDirtyColor(const plColor& color) { m_DirtyColor = color; }

protected:
  virtual void OnSimulationStarted() override;
  virtual void Update() override;

private:
  void DrawDebugInfo();
  void DrawNetworkId();
  void DrawReplicationState();
  void DrawNetworkRole();

  // Visualization toggles
  bool m_bShowNetworkId = true;
  bool m_bShowReplicationState = true;
  bool m_bShowNetworkRole = true;

  // Display settings
  float m_fTextScale = 1.0f;
  plVec3 m_vTextOffset = plVec3(0, 0, 1.0f);

  // Colors
  plColor m_ServerColor = plColor::LimeGreen;
  plColor m_ClientColor = plColor::CornflowerBlue;
  plColor m_DirtyColor = plColor::Orange;
};
