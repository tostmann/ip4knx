#pragma once
#include "knx_ip_dib.h"

#ifdef USE_IP
enum ServiceFamily : uint8_t
{
    Core = 2,
    DeviceManagement = 3,
    Tunnelling = 4,
    Routing = 5,
    RemoteLogging = 6,
    RemoteConfigDiag = 7,
    ObjectServer = 8
};

// One entry: family id and version.
#define LEN_SERVICE_FAMILIES 2
// The longest DIB this build sends: Core, Device Management, Tunnelling (with
// KNX_TUNNELING) and Routing (091A only).
#if MASK_VERSION == 0x091A
#ifdef KNX_TUNNELING
#define LEN_SERVICE_DIB (2 + 4 * LEN_SERVICE_FAMILIES)
#else
#define LEN_SERVICE_DIB (2 + 3 * LEN_SERVICE_FAMILIES)
#endif
#else
#ifdef KNX_TUNNELING
#define LEN_SERVICE_DIB (2 + 3 * LEN_SERVICE_FAMILIES)
#else
#define LEN_SERVICE_DIB (2 + 2 * LEN_SERVICE_FAMILIES)
#endif
#endif

class KnxIpSupportedServiceDIB : public KnxIpDIB
{
  public:
    KnxIpSupportedServiceDIB(uint8_t* data);
    uint8_t serviceVersion(ServiceFamily family);
    void serviceVersion(ServiceFamily family, uint8_t version);

    // Length of the DIB with or without the routing family.
    static uint8_t lengthFor(bool routing);
    // Length, code and the families this build serves; routing only if announced.
    // Expects the DIB area zeroed, as KnxIpFrame leaves it.
    void setServiceFamilies(bool routing);
};
#endif