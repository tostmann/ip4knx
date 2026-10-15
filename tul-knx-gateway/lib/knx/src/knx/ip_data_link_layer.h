#pragma once

#include "config.h"
#ifdef USE_IP

#include <stdint.h>
#include <atomic>
#include "data_link_layer.h"
#include "ip_parameter_object.h"
#include "knx_ip_tunnel_connection.h"
#include "service_families.h"

// Datagrams IpDataLinkLayer::loop() reads per pass, before it runs the timers:
// an ack waiting in the receive buffer is seen before its request is resent.
#ifndef KNX_IP_RX_PER_LOOP
#define KNX_IP_RX_PER_LOOP 8
#endif

// Counted since boot for the status page.
struct KnxIpStats
{
    uint32_t routingTx = 0;            // ROUTING_INDICATIONs sent
    uint32_t routingLimitDrops = 0;    // ROUTING_INDICATIONs the send limit dropped
    uint32_t routingPeakPerSecond = 0; // most sent within one second (calendar seconds)
    uint32_t routingRxLostMessage = 0; // ROUTING_LOST_MESSAGEs received
    uint32_t routingRxBusy = 0;        // ROUTING_BUSYs received
    uint32_t tunnelRequests = 0;       // TUNNELLING_REQUESTs sent to clients
    uint32_t tunnelAcks = 0;           // TUNNELLING_ACKs received from them
    uint32_t configRequests = 0;       // DEVICE_CONFIGURATION_REQUESTs sent
    uint32_t configAcks = 0;           // DEVICE_CONFIGURATION_ACKs received
    uint32_t ackUnknownChannel = 0;    // acks for no open channel
    uint32_t ackForeign = 0;           // acks from a host other than the channel's client
    uint32_t ackStale = 0;             // acks for a request no longer on the wire
    uint32_t ackError = 0;             // acks with a status other than E_NO_ERROR
    uint32_t ackRttMaxMs = 0;          // longest request-to-ack time
    uint32_t tunnelResends = 0;        // requests sent again for a missing ack
    uint32_t tunnelAckTimeouts = 0;    // channels closed because the repeat was not acknowledged either
    uint32_t tunnelQueueDrops = 0;     // frames a full queue gave up
    uint32_t tunnelQueuePeak = 0;      // most frames queued for one channel
    uint32_t tunnelQueueBytesPeak = 0; // most octets queued over all channels
    uint32_t tunnelRxRepeats = 0;      // TUNNELLING_REQUESTs received again
    uint32_t tunnelRxOutOfOrder = 0;   // TUNNELLING_REQUESTs with an unexpected sequence
    uint32_t configRxRepeats = 0;      // DEVICE_CONFIGURATION_REQUESTs received again
    uint32_t configRxOutOfOrder = 0;   // DEVICE_CONFIGURATION_REQUESTs with an unexpected sequence
};

class IpDataLinkLayer : public DataLinkLayer
{
    using DataLinkLayer::_deviceObject;

  public:
    IpDataLinkLayer(DeviceObject& devObj, IpParameterObject& ipParam, NetworkLayerEntity& netLayerEntity,
                    Platform& platform, BusAccessUnit& busAccessUnit, DataLinkLayerCallbacks* dllcb = nullptr);

    void loop();
    void enabled(bool value);
    bool enabled() const;
    // Re-announce the joined group without leaving it (see Platform::refreshMultiCast).
    void refreshMultiCast();
    void knxBusConnected(bool connected);  // Set KNX bus connection status
    bool routingActive();                  // KNXnet/IP routing only once an individual address is set
    DptMedium mediumType() const override;
#ifdef KNX_TUNNELING
    void dataRequestToTunnel(CemiFrame& frame) override;
    void dataRequestToChannelId(CemiFrame& frame, uint8_t channelId) override;
    void dataRequestToAllDevMgmt(CemiFrame& frame) override;
    void dataConfirmationToTunnel(CemiFrame& frame) override;
    void dataIndicationToTunnel(CemiFrame& frame) override;
    bool isTunnelAddress(uint16_t addr) override;
    bool isConfigChannel(uint8_t channelId) override;
    bool isSentToTunnel(uint16_t address, bool isGrpAddr);
    // True for a tunnel address the device acknowledges on TP whether a tunnel
    // holds it or not (see refreshDefendedTunnelAddresses).
    bool isDefendedTunnelAddress(uint16_t address) const;
    uint8_t getActiveTunnelCount() const;
    const KnxIpTunnelConnection* tunnelSlots() const { return tunnels; }
#endif
    const KnxIpStats& stats() const { return _stats; }

  private:
    KnxIpStats _stats;
    uint32_t _routingSecond = 0;       // the second _routingInSecond counts
    uint32_t _routingInSecond = 0;
    bool _enabled = false;
    bool _knxBusConnected = false;  // Track KNX bus connection status
    uint8_t _frameCount[10] = {0,0,0,0,0,0,0,0,0,0};
    uint8_t _frameCountBase = 0;
    uint32_t _frameCountTimeBase = 0;
    uint32_t _unhandledLoggedAt = 0;   // the unknown-service log line, at most one a second
    uint32_t _unhandledSuppressed = 0;
    bool sendFrame(CemiFrame& frame);
#ifdef KNX_TUNNELING
    void sendFrameToTunnel(KnxIpTunnelConnection *tunnel, CemiFrame& frame);
    void loopHandleConnectRequest(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port);
    bool fromTunnelPeer(const KnxIpTunnelConnection* tun, uint32_t src_addr);
    bool tunnelAddressInUse(uint16_t pa);
    void loopHandleConnectionStateRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr, uint16_t src_port);
    void loopHandleDisconnectRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr);
    void loopHandleDescriptionRequest(uint8_t* buffer, uint16_t length);
    void loopHandleDeviceConfigurationRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr);
    void loopHandleTunnelingRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr);
    void loopHandleAck(uint8_t* buffer, uint16_t length, bool config, uint32_t src_addr);
    void sendTunnelHead(KnxIpTunnelConnection* tunnel);
    void resendOrCloseTunnel(KnxIpTunnelConnection* tunnel);
    bool dropQueuedGroupTelegram(KnxIpTunnelConnection* tunnel);
    void sendDisconnectRequest(KnxIpTunnelConnection* tunnel);
    void loopTunnelTimers();
#endif
    bool loopHandleDatagram();
#if KNX_SERVICE_FAMILY_CORE >= 2
    void loopHandleSearchRequestExtended(uint8_t* buffer, uint16_t length);
#endif
    bool sendBytes(uint8_t* buffer, uint16_t length);
    bool isSendLimitReached();

    IpParameterObject& _ipParameters;
    DataLinkLayerCallbacks* _dllcb;
#ifdef KNX_TUNNELING
    KnxIpTunnelConnection tunnels[KNX_TUNNELING];
    uint8_t _lastChannelId = 0;

    void refreshDefendedTunnelAddresses();
    // Two copies, because the TP acknowledge path reads them in the UART task: the
    // refresh fills the one not in use and then switches over.
    uint16_t _defendedAddresses[2][KNX_TUNNELING] = {};
    uint8_t _defendedCount[2] = {0, 0};
    std::atomic<uint8_t> _defendedSet{0};
    uint32_t _defendedRefreshMs = 0;
    bool _defendedRefreshed = false;
#endif
};
#endif