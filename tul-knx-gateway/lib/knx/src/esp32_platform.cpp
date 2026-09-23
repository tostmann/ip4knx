#include "esp32_platform.h"

#ifdef ARDUINO_ARCH_ESP32
#include <Arduino.h>
#include <EEPROM.h>

#include "knx/bits.h"
#include "lwip/igmp.h"
#include "esp_netif_net_stack.h"

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
    uint8_t result = _udp.beginMulticast(mcastaddr, port);
    if (result == 0)
        println("KNX multicast join failed");

    return result != 0;
    // KNX_DEBUG_SERIAL.printf("result %d\n", result);
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
    _udp.stop();
}

bool Esp32Platform::sendBytesMultiCast(uint8_t * buffer, uint16_t len)
{
    //printHex("<- ",buffer, len);
    _udp.beginPacket(_multicastIP, _multicastPort);
    _udp.write(buffer, len);
    _udp.endPacket();
    return true;
}

int Esp32Platform::readBytesMultiCast(uint8_t * buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port)
{
    int len = _udp.parsePacket();
    if (len == 0)
        return 0;

    if (len > maxLen)
    {
        println("Unexpected UDP data packet length - drop packet");
        for (size_t i = 0; i < len; i++)
            _udp.read();
        return 0;
    }

    _udp.read(buffer, len);
    _remoteIP = _udp.remoteIP();
    _remotePort = _udp.remotePort();
    src_addr = htonl(_remoteIP);
    src_port = _remotePort;

    // print("Remote IP: ");
    // print(_udp.remoteIP().toString().c_str());
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

    if(_udp.beginPacket(ucastaddr, port) == 1)
    {
        _udp.write(buffer, len);
        if(_udp.endPacket() == 0) println("sendBytesUniCast endPacket fail");
    }
    else
        println("sendBytesUniCast beginPacket fail");
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
