#ifdef ARDUINO_ARCH_ESP32
#include "arduino_platform.h"
#include "TPUart/Interface/Abstract.h"


#include <IPAddress.h>
#include "esp_err.h"
#include "lwip/ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

struct udp_pcb;
struct pbuf;

class Esp32Platform : public ArduinoPlatform
{
public:
    Esp32Platform();
    Esp32Platform(TPUart::Interface::Abstract* interface);

    // ip stuff
    uint32_t currentIpAddress() override;
    uint32_t currentSubnetMask() override;
    uint32_t currentDefaultGateway() override;
    void macAddress(uint8_t* addr) override;

    // unique serial number
    uint32_t uniqueSerialNumber() override;

    // basic stuff
    void restart();

    //multicast
    bool setupMultiCast(uint32_t addr, uint16_t port) override;
    void refreshMultiCast() override;
    void closeMultiCast() override;
    bool sendBytesMultiCast(uint8_t* buffer, uint16_t len) override;
    int readBytesMultiCast(uint8_t* buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port) override;
    
    //unicast
    bool sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len) override;

    // KNXnet/IP datagrams dropped since boot because the receive buffer was
    // full or the datagram was longer than the stack reads.
    uint32_t udpRxDropped() const { return _udpRxDropped; }
    // Most datagrams waiting in the receive buffer at once since boot.
    uint32_t udpRxBacklogPeak() const { return _udpRxBacklogPeak; }

    //memory
    uint8_t* getEepromBuffer(uint32_t size);
    void commitToEeprom();

    protected: IPAddress _remoteIP;     // last UDP sender, for route-back unicast replies
    protected: uint16_t _remotePort;
    protected: IPAddress _multicastIP;  // the joined routing group, for sendBytesMultiCast()
    protected: uint16_t _multicastPort = 0;

private:
    static esp_err_t udpOpen(void* ctx);
    static esp_err_t udpClose(void* ctx);
    static void udpReceive(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, uint16_t port);
    bool udpSend(const IPAddress& ip, uint16_t port, const uint8_t* buffer, uint16_t len);

    struct udp_pcb* _udpPcb = nullptr;
    ip4_addr_t _udpGroup = {};
    RingbufHandle_t _udpRx = nullptr;
    volatile uint32_t _udpRxDropped = 0;   // written by udpReceive() (tcpip thread), read in loop()
    // Datagrams put into and taken out of the receive buffer: each is written on
    // one side only, so their difference is the backlog without a shared counter.
    volatile uint32_t _udpRxQueued = 0;    // udpReceive()
    volatile uint32_t _udpRxTaken = 0;     // readBytesMultiCast(), closeMultiCast()
    volatile uint32_t _udpRxBacklogPeak = 0;
    uint32_t _udpRxDroppedReported = 0;
    uint32_t _udpRxReportedAt = 0;
    // int8_t _rxPin = -1;
    // int8_t _txPin = -1;
};

#endif
