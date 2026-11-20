#include "NetworkTestPCH.h"

#include "NetworkTestHarness.h"

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plNetworkRpcProbeComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_FUNCTIONS
  {
    PL_FUNCTION_PROPERTY(RpcServer)->AddAttributes(new plNetworkRpcAttribute(plNetworkRpcTarget::Server)),
    PL_FUNCTION_PROPERTY(RpcOwner)->AddAttributes(new plNetworkRpcAttribute(plNetworkRpcTarget::Owner)),
    PL_FUNCTION_PROPERTY(RpcAllClients)->AddAttributes(new plNetworkRpcAttribute(plNetworkRpcTarget::AllClients)),
    PL_FUNCTION_PROPERTY(RpcAll)->AddAttributes(new plNetworkRpcAttribute(plNetworkRpcTarget::AllClientsAndServer)),
    PL_FUNCTION_PROPERTY(RpcMulticast)->AddAttributes(new plNetworkRpcAttribute(plNetworkRpcTarget::Multicast)),
  }
  PL_END_FUNCTIONS;
}
PL_END_COMPONENT_TYPE

PL_BEGIN_COMPONENT_TYPE(plNetworkRpcTwinComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_FUNCTIONS
  {
    PL_FUNCTION_PROPERTY(RpcServer)->AddAttributes(new plNetworkRpcAttribute(plNetworkRpcTarget::Server)),
  }
  PL_END_FUNCTIONS;
}
PL_END_COMPONENT_TYPE

PL_BEGIN_COMPONENT_TYPE(plNetworkCounterComponent, 1, plComponentMode::Dynamic)
PL_END_COMPONENT_TYPE
// clang-format on
