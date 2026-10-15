#include "ip_parameter_object.h"
#ifdef USE_IP
#include "device_object.h"
#include "platform.h"
#include "bits.h"
#include "data_property.h"
#include "callback_property.h"
#include "knx_ip_supported_service_dib.h"

// 224.0.23.12
#define DEFAULT_MULTICAST_ADDR ((uint32_t)0xE000170C)

IpParameterObject::IpParameterObject(DeviceObject& deviceObject, Platform& platform): _deviceObject(deviceObject),
    _platform(platform)
{
    Property* properties[] =
    {
        new DataProperty(PID_OBJECT_TYPE, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0, (uint16_t)OT_IP_PARAMETER),
        new DataProperty(PID_PROJECT_INSTALLATION_ID, true, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv3),
        new CallbackProperty<IpParameterObject>(this, PID_KNX_INDIVIDUAL_ADDRESS, true, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv3,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            {
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }
                // TODO: get property of deviceobject and use it
                pushWord(io->_deviceObject.individualAddress(), data);
                return 1;
            },
            [](IpParameterObject* io, uint16_t start, uint8_t count, const uint8_t* data) -> uint8_t 
            { 
                io->_deviceObject.individualAddress(getWord(data));
                return 1; 
            }),
#ifdef KNX_TUNNELING
        new DataProperty(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, true, PDT_UNSIGNED_INT, KNX_TUNNELING, ReadLv3 | WriteLv3),
        new DataProperty(PID_CUSTOM_RESERVED_TUNNELS_CTRL, true, PDT_UNSIGNED_CHAR, KNX_TUNNELING, ReadLv3 | WriteLv3), // custom propertiy to control the stacks behaviour for reserverd tunnels, not in Spec (PID >= 200)
        new DataProperty(PID_CUSTOM_RESERVED_TUNNELS_IP, true, PDT_UNSIGNED_LONG, KNX_TUNNELING, ReadLv3 | WriteLv3), // custom propertiy to control the stacks behaviour for reserverd tunnels, not in Spec (PID >= 200)
#endif
        new DataProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, false, PDT_UNSIGNED_CHAR, 0, ReadLv3 | WriteLv3),
        new DataProperty(PID_IP_ASSIGNMENT_METHOD, true, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv3),
        new DataProperty(PID_IP_CAPABILITIES, true, PDT_BITSET8, 0, ReadLv3 | WriteLv1),    // must be set by application due to capabilities of the used ip stack
        new CallbackProperty<IpParameterObject>(this, PID_CURRENT_IP_ADDRESS, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                pushInt(htonl(io->_platform.currentIpAddress()), data);
                return 1;
            }),
        new CallbackProperty<IpParameterObject>(this, PID_CURRENT_SUBNET_MASK, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                pushInt(htonl(io->_platform.currentSubnetMask()), data);
                return 1;
            }),
        new CallbackProperty<IpParameterObject>(this, PID_CURRENT_DEFAULT_GATEWAY, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                pushInt(htonl(io->_platform.currentDefaultGateway()), data);
                return 1;
            }),
        new DataProperty(PID_IP_ADDRESS, true, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv3),
        new DataProperty(PID_SUBNET_MASK, true, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv3),
        new DataProperty(PID_DEFAULT_GATEWAY, true, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv3),
        new CallbackProperty<IpParameterObject>(this, PID_MAC_ADDRESS, false, PDT_GENERIC_06, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                io->_platform.macAddress(data);
                return 1;
            }),
        new CallbackProperty<IpParameterObject>(this, PID_SYSTEM_SETUP_MULTICAST_ADDRESS, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                pushInt(DEFAULT_MULTICAST_ADDR, data);
                return 1;
            }),
        new DataProperty(PID_ROUTING_MULTICAST_ADDRESS, true, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv3, DEFAULT_MULTICAST_ADDR),
        new DataProperty(PID_TTL, true, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv3, (uint8_t)16),
        new CallbackProperty<IpParameterObject>(this, PID_KNXNETIP_DEVICE_CAPABILITIES, false, PDT_BITSET16, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                // Was a fixed 0x0001, which denied tunnelling and routing. Same families and the
                // same routing condition as the service families DIB. (upstream 8a8f2d1)
                pushWord(KnxIpSupportedServiceDIB::deviceCapabilities(io->_deviceObject.individualAddressProgrammed()), data);
                return 1;
            }),
        new DataProperty(PID_FRIENDLY_NAME, true, PDT_UNSIGNED_CHAR, 30, ReadLv3 | WriteLv3),
#if MASK_VERSION == 0x091A
        // Router only (upstream 8a8f2d1). Appended so the index of every existing property stays,
        // and read-only so neither enters the persisted image or InterfaceObject::layoutTag().
        // 03_08_03 2.5.21 p.14, "shall be implemented by devices providing KNXnet/IP Routing":
        // 0x00 - none of the optional features is implemented here (no PID 72/73 queue overflow
        // or PID 74/75 transmit counters, no priority/FIFO). Upstream reports bit 0, it counts
        // queue overflows.
        new DataProperty(PID_KNXNETIP_ROUTING_CAPABILITIES, false, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv0, (uint8_t)0x00),
        // 03_08_03 2.5.28 p.16, mandatory for any KNXnet/IP or KNX IP device (built here for the router
        // only), default 100 ms (range 20-100). A parameter, not a claim: ROUTING_BUSY is never sent.
        new DataProperty(PID_ROUTING_BUSY_WAIT_TIME, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0, (uint16_t)100),
#endif
        // 03_08_03 2.5.20 p.14, "shall be implemented by any KNXnet/IP Server", evented:
        // Bau091A::loop() sends a change as M_PropInfo.ind. Appended so the index of
        // every other property stays, and read-only so it enters neither the persisted
        // image nor InterfaceObject::layoutTag(). (upstream e902864)
        new CallbackProperty<IpParameterObject>(this, PID_KNXNETIP_DEVICE_STATE, false, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }
                *data = io->_deviceState;
                return 1;
            }),
    };
    initializeProperties(sizeof(properties), properties);

    uint8_t defaultFriendlyName[30] = {0};
    strcpy((char*)defaultFriendlyName, "busware.de TUL");
    property(PID_FRIENDLY_NAME)->write(1, 30, defaultFriendlyName);
}

bool IpParameterObject::updateDeviceState(bool knxOperational, bool ipOperational)
{
    const uint32_t now = millis();
    // 0 marks "no fault pending", so it is never stored as a time.
    if (knxOperational)
        _knxFaultSince = 0;
    else if (_knxFaultSince == 0)
        _knxFaultSince = now ? now : 1;
    if (ipOperational)
        _ipFaultSince = 0;
    else if (_ipFaultSince == 0)
        _ipFaultSince = now ? now : 1;

    uint8_t state = 0;
    if (_knxFaultSince != 0 && now - _knxFaultSince >= 5000)
        state |= 0x01;   // KNX fault
    if (_ipFaultSince != 0 && now - _ipFaultSince >= 5000)
        state |= 0x02;   // IP fault

    const bool changed = state != _deviceState;
    _deviceState = state;
    if (!_deviceStateSeeded)
    {
        // Nobody to tell at start-up, and the bus is briefly not usable until the
        // transceiver is up.
        _deviceStateSeeded = true;
        return false;
    }
    return changed;
}

#ifdef KNX_TUNNELING
// .241 upwards on the device's own line. The bottom of a line is where devices get
// their addresses, and a tunnel address that a device also uses cuts that device
// off from the tunnel client and collides on the bus. The device's own address is
// skipped, since a router does not hand it out for tunnelling (08_TSSH 5.3.1); the
// range then ends one higher. x.y.0 and x.y.255 are never produced.
static_assert(KNX_TUNNELING <= 13, "tunnel addresses from .241 would reach .255");

void IpParameterObject::defaultTunnelAddresses(uint16_t ownAddress, uint8_t* out)
{
    const uint8_t line = ownAddress >> 8;
    uint8_t device = 241;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        if (device == (ownAddress & 0xFF))
            device++;
        out[i * 2] = line;
        out[i * 2 + 1] = device++;
    }
}

bool IpParameterObject::isLegacyTunnelAddresses(uint16_t ownAddress, const uint8_t* addresses)
{
    const uint8_t line = ownAddress >> 8;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        if (addresses[i * 2] != line || addresses[i * 2 + 1] != i + 1)
            return false;
    }
    return true;
}

bool IpParameterObject::isCurrentTunnelPool(uint16_t ownAddress, const uint8_t* addresses)
{
    uint16_t firstEntry = 0;
    for (int i = 0; i < KNX_TUNNELING && firstEntry == 0; i++)
        popWord(firstEntry, addresses + i * 2);
    return firstEntry != 0 && (firstEntry >> 8) == (ownAddress >> 8) &&
           !isLegacyTunnelAddresses(ownAddress, addresses);
}

bool IpParameterObject::isUsableTunnelAddress(uint16_t ownAddress, uint16_t address)
{
    return (address & 0xFF) != 0 && address != ownAddress;
}
#endif

#endif
