#include "esp32_platform.h"

#ifdef ARDUINO_ARCH_ESP32
#include <Arduino.h>
#include <EEPROM.h>

#include "knx/bits.h"
#include "lwip/igmp.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"

// Received KNXnet/IP datagrams wait here until IpDataLinkLayer::loop() reads
// them. A socket queues them in its lwIP receive mailbox instead, and that
// holds six (CONFIG_LWIP_UDP_RECVMBOX_SIZE, fixed in the precompiled core):
// ten disconnect requests sent back to back were all in before loop() ran
// once, four were dropped, and their tunnels stayed open until the 120 s
// heartbeat timeout. Each datagram is copied out here as lwIP delivers it, so
// the pbuf — on WiFi a driver receive buffer — is released at once.
#define UDP_RX_BUFFER_SIZE 4096
// The largest datagram IpDataLinkLayer::loop() reads.
#define UDP_RX_MAX_DATAGRAM 512

struct UdpRxHeader
{
    uint32_t addr;   // sender, network byte order
    uint16_t port;
    uint16_t len;
};

struct UdpSend
{
    struct udp_pcb* pcb;
    struct pbuf* p;
    ip_addr_t dst;
    uint16_t port;
    err_t err;
};

// #ifndef KNX_SERIAL
//     #define KNX_SERIAL Serial1
//     #pragma warn "KNX_SERIAL not defined, using Serial1"
// #endif
 
#if defined(KNX_IP_LAN)
    #include "ETH.h"
    #define KNX_NETIF ETH
#elif defined(W5500_ETH)
    // Optional W5500 add-on (TUL32 FPC header): which interface carries KNX is
    // a property of the board in front of us, not of the build, so the choice
    // has to be made per call. The application owns the answer and defines
    // knxUseEthernet(); this weak default keeps the stack linkable without it.
    #include <WiFi.h>
    #include "ETH.h"
    bool __attribute__((weak)) knxUseEthernet() { return false; }
#else // KNX_IP_WIFI
    #include <WiFi.h>
    #define KNX_NETIF WiFi
#endif

#ifdef W5500_ETH
    #define KNX_IF_LOCALIP()   (knxUseEthernet() ? ETH.localIP()   : WiFi.localIP())
    #define KNX_IF_NETMASK()   (knxUseEthernet() ? ETH.subnetMask() : WiFi.subnetMask())
    #define KNX_IF_GATEWAY()   (knxUseEthernet() ? ETH.gatewayIP()  : WiFi.gatewayIP())
    #define KNX_IF_MAC(a)      (knxUseEthernet() ? (void)ETH.macAddress(a) : (void)WiFi.macAddress(a))
#else
    #define KNX_IF_LOCALIP()   KNX_NETIF.localIP()
    #define KNX_IF_NETMASK()   KNX_NETIF.subnetMask()
    #define KNX_IF_GATEWAY()   KNX_NETIF.gatewayIP()
    #define KNX_IF_MAC(a)      KNX_NETIF.macAddress(a)
#endif

Esp32Platform::Esp32Platform()
{
}

Esp32Platform::Esp32Platform(TPUart::Interface::Abstract* interface) : ArduinoPlatform(interface)
{
}

uint32_t Esp32Platform::currentIpAddress()
{
    return KNX_IF_LOCALIP();
}

uint32_t Esp32Platform::currentSubnetMask()
{
    return KNX_IF_NETMASK();
}

uint32_t Esp32Platform::currentDefaultGateway()
{
    return KNX_IF_GATEWAY();
}

void Esp32Platform::macAddress(uint8_t * addr)
{
    KNX_IF_MAC(addr);
}

uint32_t Esp32Platform::uniqueSerialNumber()
{
    uint64_t chipid = ESP.getEfuseMac();
    uint32_t upperId = (chipid >> 32) & 0xFFFFFFFF;
    uint32_t lowerId = (chipid & 0xFFFFFFFF);
    return (upperId ^ lowerId);
}

void Esp32Platform::restart()
{
    println("restart");
    ESP.restart();
}

bool Esp32Platform::setupMultiCast(uint32_t addr, uint16_t port)
{
#if defined(KNX_IP_LAN)
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("ETH_DEF");
#elif defined(W5500_ETH)
    esp_netif_t* check = esp_netif_get_handle_from_ifkey(
                             knxUseEthernet() ? "ETH_DEF" : "WIFI_STA_DEF");
#else
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
#endif
    if (check == nullptr)
    {
        // No endless blink loop here: without an interface there is simply no
        // endpoint yet. Saying so lets the caller retry when one appears, instead
        // of requiring a power cycle.
        println("No network interface initialized");
        return false;
    }
    IPAddress mcastaddr(htonl(addr));
    
    // Kept apart from _remoteIP/_remotePort: readBytesMultiCast() overwrites those
    // with the sender of every datagram, so a routing indication sent to them went
    // by unicast to whoever had sent last -- a search client, a tunnel client, another
    // router -- instead of to the group.
    _multicastIP = mcastaddr;
    _multicastPort = port;

    println("Initializing KNX multicast.");
    print("  Bind ");
    print(mcastaddr.toString().c_str());
    print(":");
    println(port);

    if (_udpRx == nullptr)
        _udpRx = xRingbufferCreate(UDP_RX_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (_udpRx == nullptr)
    {
        println("KNX multicast: no memory for the receive buffer");
        return false;
    }

    closeMultiCast();   // same port: an endpoint still open would refuse the bind
    ip4_addr_set_u32(&_udpGroup, htonl(addr));
    esp_err_t result = esp_netif_tcpip_exec(udpOpen, this);
    if (result != ESP_OK)
        println("KNX multicast join failed");

    return result == ESP_OK;
}

// udpOpen(), udpClose() and udpSendInCore() go through esp_netif_tcpip_exec(),
// which with core locking runs them in the calling task while it holds the
// lwIP core lock. lwIP calls udpReceive() in the tcpip thread with that lock
// held. Keep all four short: while they run, lwIP processes nothing else.

esp_err_t Esp32Platform::udpOpen(void* ctx)
{
    Esp32Platform* self = (Esp32Platform*)ctx;
    struct udp_pcb* pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == nullptr)
        return ESP_ERR_NO_MEM;

    ip_set_option(pcb, SOF_REUSEADDR);
    if (udp_bind(pcb, IP4_ADDR_ANY, self->_multicastPort) != ERR_OK)
    {
        udp_remove(pcb);
        return ESP_FAIL;
    }
    // Any interface, as before: the socket's IP_ADD_MEMBERSHIP with
    // imr_interface = INADDR_ANY came down to this same call.
    if (igmp_joingroup(IP4_ADDR_ANY4, &self->_udpGroup) != ERR_OK)
    {
        // A join can fail part way through the interfaces; undo the rest.
        igmp_leavegroup(IP4_ADDR_ANY4, &self->_udpGroup);
        udp_remove(pcb);
        return ESP_FAIL;
    }
    udp_recv(pcb, udpReceive, self);
    self->_udpPcb = pcb;
    return ESP_OK;
}

esp_err_t Esp32Platform::udpClose(void* ctx)
{
    Esp32Platform* self = (Esp32Platform*)ctx;
    if (self->_udpPcb == nullptr)
        return ESP_OK;

    igmp_leavegroup(IP4_ADDR_ANY4, &self->_udpGroup);
    udp_remove(self->_udpPcb);
    self->_udpPcb = nullptr;
    return ESP_OK;
}

void Esp32Platform::udpReceive(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, uint16_t port)
{
    (void)pcb;
    Esp32Platform* self = (Esp32Platform*)arg;
    // An empty datagram has nothing to read; the socket returned nothing for
    // those either, so it is neither queued nor counted.
    if (p->tot_len > 0)
    {
        void* item = nullptr;
        if (p->tot_len <= UDP_RX_MAX_DATAGRAM &&
            xRingbufferSendAcquire(self->_udpRx, &item, sizeof(UdpRxHeader) + p->tot_len, 0) == pdTRUE)
        {
            UdpRxHeader* hdr = (UdpRxHeader*)item;
            hdr->addr = ip4_addr_get_u32(ip_2_ip4(addr));
            hdr->port = port;
            hdr->len = p->tot_len;
            pbuf_copy_partial(p, (uint8_t*)item + sizeof(UdpRxHeader), p->tot_len, 0);
            xRingbufferSendComplete(self->_udpRx, item);
            self->_udpRxQueued = self->_udpRxQueued + 1;
            const uint32_t backlog = self->_udpRxQueued - self->_udpRxTaken;
            if (backlog > self->_udpRxBacklogPeak)
                self->_udpRxBacklogPeak = backlog;
        }
        else
            self->_udpRxDropped = self->_udpRxDropped + 1;   // reported from readBytesMultiCast(), not from here
    }
    pbuf_free(p);
}

static esp_err_t udpSendInCore(void* ctx)
{
    UdpSend* s = (UdpSend*)ctx;
    s->err = udp_sendto(s->pcb, s->p, &s->dst, s->port);
    return ESP_OK;
}

bool Esp32Platform::udpSend(const IPAddress& ip, uint16_t port, const uint8_t* buffer, uint16_t len)
{
    if (_udpPcb == nullptr)
        return false;

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (p == nullptr)
        return false;
    memcpy(p->payload, buffer, len);

    UdpSend s = {};
    s.pcb = _udpPcb;
    s.p = p;
    IP_ADDR4(&s.dst, ip[0], ip[1], ip[2], ip[3]);
    s.port = port;
    s.err = ERR_OK;
    esp_netif_tcpip_exec(udpSendInCore, &s);
    pbuf_free(p);
    return s.err == ERR_OK;
}

// Re-send the IGMP membership reports for this interface. Deliberately not a
// leave-and-join: the leave prunes the group at the switch for the moment it takes
// to come back, and routing telegrams in that window are lost. lwIP's
// igmp_joingroup on a group already joined only raises its use count and sends
// nothing, so the report has to be asked for directly. It runs in the TCP/IP task,
// which is where the lwIP core lock lives.
static esp_err_t knxRefreshIgmpReports(void* ctx)
{
    igmp_report_groups((struct netif*)ctx);
    return ESP_OK;
}

void Esp32Platform::refreshMultiCast()
{
#if defined(KNX_IP_LAN)
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
#elif defined(W5500_ETH)
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey(
                             knxUseEthernet() ? "ETH_DEF" : "WIFI_STA_DEF");
#else
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
#endif
    if (netif == nullptr)
        return;

    struct netif* stack_netif = (struct netif*)esp_netif_get_netif_impl(netif);
    if (stack_netif == nullptr)
        return;

    esp_netif_tcpip_exec(knxRefreshIgmpReports, stack_netif);
}

void Esp32Platform::closeMultiCast()
{
    esp_netif_tcpip_exec(udpClose, this);

    // A closed socket discarded what it had queued; so does this endpoint.
    if (_udpRx != nullptr)
    {
        size_t size = 0;
        void* item;
        while ((item = xRingbufferReceive(_udpRx, &size, 0)) != nullptr)
        {
            vRingbufferReturnItem(_udpRx, item);
            _udpRxTaken = _udpRxTaken + 1;
        }
    }
}

bool Esp32Platform::sendBytesMultiCast(uint8_t * buffer, uint16_t len)
{
    //printHex("<- ",buffer, len);
    udpSend(_multicastIP, _multicastPort, buffer, len);
    return true;
}

int Esp32Platform::readBytesMultiCast(uint8_t * buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port)
{
    // Reported here and at most once a second: udpReceive() runs in the tcpip
    // thread with the core lock held, and printing there under a flood would
    // hold up all networking.
    const uint32_t dropped = _udpRxDropped;
    if (dropped != _udpRxDroppedReported && millis() - _udpRxReportedAt >= 1000)
    {
        print("KNX/IP receive: ");
        print(dropped - _udpRxDroppedReported);
        println(" datagram(s) dropped (buffer full or too long)");
        _udpRxDroppedReported = dropped;
        _udpRxReportedAt = millis();
    }

    if (_udpRx == nullptr)
        return 0;

    size_t size = 0;
    uint8_t* item = (uint8_t*)xRingbufferReceive(_udpRx, &size, 0);
    if (item == nullptr)
        return 0;

    UdpRxHeader hdr;
    memcpy(&hdr, item, sizeof(hdr));
    int len = 0;
    if (hdr.len > maxLen)
        println("Unexpected UDP data packet length - drop packet");
    else
    {
        memcpy(buffer, item + sizeof(hdr), hdr.len);
        len = hdr.len;
        _remoteIP = IPAddress(hdr.addr);
        _remotePort = hdr.port;
        src_addr = ntohl(hdr.addr);
        src_port = hdr.port;
    }
    vRingbufferReturnItem(_udpRx, item);
    _udpRxTaken = _udpRxTaken + 1;

    // print("Remote IP: ");
    // print(_remoteIP.toString().c_str());
    // printHex("-> ", buffer, len);

    return len;
}

bool Esp32Platform::sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len)
{
    IPAddress ucastaddr(htonl(addr));

    if(!addr)
        ucastaddr = _remoteIP;
    
    if(!port)
        port = _remotePort;

    if (!udpSend(ucastaddr, port, buffer, len))
        println("sendBytesUniCast fail");
    return true;
}

uint8_t * Esp32Platform::getEepromBuffer(uint32_t size)
{
    uint8_t * eepromptr = EEPROM.getDataPtr();
    if(eepromptr == nullptr) {
        EEPROM.begin(size);
        eepromptr = EEPROM.getDataPtr();
    }
    return eepromptr;
}

void Esp32Platform::commitToEeprom()
{
    EEPROM.getDataPtr(); // trigger dirty flag in EEPROM lib to make sure data will be written to flash
    EEPROM.commit();
}

#endif
