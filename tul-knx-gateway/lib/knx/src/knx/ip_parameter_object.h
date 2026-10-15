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
    // True for a written pool that is kept as it is: its first entry that is not
    // empty lies on the device's line, and it is not the legacy pool. Any other
    // pool is made anew from the device address.
    static bool isCurrentTunnelPool(uint16_t ownAddress, const uint8_t* addresses);
    // False for an address that is never handed out for tunnelling, whatever a
    // written pool says: x.y.0, and the device's own address (08_TSSH 5.3.1).
    static bool isUsableTunnelAddress(uint16_t ownAddress, uint16_t address);
#endif

    // PID_KNXNETIP_DEVICE_STATE (03_08_03 2.5.20): bit 0 KNX fault, bit 1 IP fault,
    // each set once its side has been unusable for 5 s and cleared when it is back
    // (3.5.2, 3.5.3). Call once per loop pass; true when the value changed and an
    // M_PropInfo.ind is due. The first call only sets it.
    bool updateDeviceState(bool knxOperational, bool ipOperational);
    uint8_t deviceState() const { return _deviceState; }

  private:
    DeviceObject& _deviceObject;
    Platform& _platform;
    uint8_t _deviceState = 0;
    uint32_t _knxFaultSince = 0;    // millis() the KNX side was first seen unusable, 0 = it is not
    uint32_t _ipFaultSince = 0;
    bool _deviceStateSeeded = false;
};
#endif