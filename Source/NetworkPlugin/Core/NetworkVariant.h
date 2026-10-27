#pragma once

#include <NetworkPlugin/Core/NetworkMessage.h>

/// Reads a length-prefixed Foundation variant after validating every nested allocation.
/// Network blobs are limited to 64 KiB, 1024 values and 16 levels. Custom typed objects
/// and pointers are not accepted from the wire.
PL_NETWORKPLUGIN_DLL bool plReadNetworkVariant(plNetworkMessage& msg, plVariant& out_value);
