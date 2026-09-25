#pragma once

#include "config.h"
#ifdef USE_IP
#include "interface_object.h"
#include "device_object.h"
#include "platform.h"

#define KNXIP_MULTICAST_PORT 3671

class IpParameterObject : public InterfaceObject
{
  public:
    IpParameterObject(DeviceObject& deviceObject, Platform& platform);

#ifdef KNX_TUNNELING
    // Tunnel addresses used while none were written through device management
    // (PID_ADDITIONAL_INDIVIDUAL_ADDRESSES): KNX_TUNNELING entries, 2 bytes each.
    static void defaultTunnelAddresses(uint16_t ownAddress, uint8_t* out);
    // True for the pool firmware up to 1.4.29 made instead: .1 to .10 of the
    // device's own line, in that order.
    static bool isLegacyTunnelAddresses(uint16_t ownAddress, const uint8_t* addresses);
#endif

  private:
    DeviceObject& _deviceObject;
    Platform& _platform;
};
#endif