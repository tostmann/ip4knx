#include "knx_ip_supported_service_dib.h"
#include "service_families.h"

#ifdef USE_IP
KnxIpSupportedServiceDIB::KnxIpSupportedServiceDIB(uint8_t* data) : KnxIpDIB(data)
{}


uint8_t KnxIpSupportedServiceDIB::serviceVersion(ServiceFamily family)
{
    uint8_t* start = _data + 2;
    uint8_t* end = _data + length();

    for (uint8_t* it = start; it < end; it += 2)
    {
        if (*it == family)
            return it[1];
    }
    return 0;
}


// Routing is announced only while it runs (IpDataLinkLayer::routingActive());
// only the 091A has the family at all.
uint8_t KnxIpSupportedServiceDIB::lengthFor(bool routing)
{
#if MASK_VERSION == 0x091A
    if (!routing)
        return LEN_SERVICE_DIB - LEN_SERVICE_FAMILIES;
#else
    (void)routing;
#endif
    return LEN_SERVICE_DIB;
}

void KnxIpSupportedServiceDIB::setServiceFamilies(bool routing)
{
    length(lengthFor(routing));
    code(SUPP_SVC_FAMILIES);
    serviceVersion(Core, KNX_SERVICE_FAMILY_CORE);
    serviceVersion(DeviceManagement, KNX_SERVICE_FAMILY_DEVICE_MANAGEMENT);
#ifdef KNX_TUNNELING
    serviceVersion(Tunnelling, KNX_SERVICE_FAMILY_TUNNELING);
#endif
#if MASK_VERSION == 0x091A
    if (routing)
        serviceVersion(Routing, KNX_SERVICE_FAMILY_ROUTING);
#endif
}

void KnxIpSupportedServiceDIB::serviceVersion(ServiceFamily family,  uint8_t version)
{
    uint8_t* start = _data + 2;
    uint8_t* end = _data + length();

    for (uint8_t* it = start; it < end; it += 2)
    {
        if (*it == family)
        {
            it[1] = version;
            break;
        }

        if (*it == 0)
        {
            *it = family;
            it[1] = version;
            break;
        }
    }
}
#endif