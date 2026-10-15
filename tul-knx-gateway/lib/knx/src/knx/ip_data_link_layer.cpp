#include "config.h"
#ifdef USE_IP

#include "ip_data_link_layer.h"

#include "bits.h"
#include "platform.h"
#include "device_object.h"
#include "knx_ip_routing_indication.h"
#include "knx_ip_search_request.h"
#include "knx_ip_search_response.h"
#include "knx_ip_search_request_extended.h"
#include "knx_ip_search_response_extended.h"
#include "knx_facade.h"
#ifdef KNX_TUNNELING
#include "knx_ip_connect_request.h"
#include "knx_ip_connect_response.h"
#include "knx_ip_state_request.h"
#include "knx_ip_state_response.h"
#include "knx_ip_disconnect_request.h"
#include "knx_ip_disconnect_response.h"
#include "knx_ip_tunneling_request.h"
#include "knx_ip_tunneling_ack.h"
#include "knx_ip_description_request.h"
#include "knx_ip_description_response.h"
#include "knx_ip_config_request.h"
#endif

#include <stdio.h>
#include <string.h>

#define KNXIP_HEADER_LEN 0x6
#define KNXIP_PROTOCOL_VERSION 0x10

#define MIN_LEN_CEMI 10

IpDataLinkLayer::IpDataLinkLayer(DeviceObject& devObj, IpParameterObject& ipParam,
    NetworkLayerEntity &netLayerEntity, Platform& platform, BusAccessUnit& busAccessUnit, DataLinkLayerCallbacks* dllcb) : DataLinkLayer(devObj, netLayerEntity, platform, busAccessUnit), _ipParameters(ipParam), _dllcb(dllcb)
{
}

// KNXnet/IP routing (the multicast ROUTING_INDICATION exchange) starts only once
// the device has an individual address other than the factory 15.15.0. Two
// unprogrammed sticks on the same TP line would otherwise pass every telegram of
// main groups 14-31 back and forth between TP and IP until the hop count runs out.
// Tunnelling is not affected: tunnel clients get their frames from the TP side.
bool IpDataLinkLayer::routingActive()
{
    return _deviceObject.individualAddressProgrammed();
}

bool IpDataLinkLayer::sendFrame(CemiFrame& frame)
{
    if (!routingActive())
    {
        // Nothing to route to while unprogrammed. Confirmed as delivered: the IP
        // side has no routing peers. NetworkLayerCoupler::dataConfirm discards the
        // confirmation of a routed frame (foreign source) anyway, so this reports
        // nothing to a TP sender; it only keeps the device's own frames -- the
        // answers it sends to both sides, such as a programming mode response --
        // from being reported as failed.
        dataConReceived(frame, true);
        return true;
    }

    KnxIpRoutingIndication packet(frame);
    // only send 50 packet per second: see KNX 3.2.6 p.6
    if(isSendLimitReached())
    {
        // every other sendFrame path reports via dataConReceived(); the send-limit drop was the
        // only branch returning false WITHOUT one -> emit the (negative) con to keep the contract
        // uniform. Safe: a routed frame's foreign-source con is discarded at NetworkLayerCoupler,
        // no re-queue/retransmit (isSendLimitReached is a pure sliding-window counter).
        _stats.routingLimitDrops++;
        dataConReceived(frame, false);
        return false;
    }
    bool success = sendBytes(packet.data(), packet.totalLength());
    _stats.routingTx++;
    const uint32_t second = millis() / 1000;
    if (second != _routingSecond)
    {
        _routingSecond = second;
        _routingInSecond = 0;
    }
    if (++_routingInSecond > _stats.routingPeakPerSecond)
        _stats.routingPeakPerSecond = _routingInSecond;
#ifdef KNX_ACTIVITYCALLBACK
    if(_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_SEND << KNX_ACTIVITYCALLBACK_DIR));
#endif
    dataConReceived(frame, success);
    return success;
}

#ifdef KNX_TUNNELING
// Route a frame back to the exact tunnel that originated it (by channelId).
// Used for CEMI configuration responses (M_PropRead_con / M_PropWrite_con /
// M_Reset_ind) so they no longer leak to other tunnels. Cherry-pick of
// OpenKNX/knx 1bd8201 (thewhobox, 2026-02-20), adapted to our pre-refactor
// layout (no separate IpTunnelServer class yet).
void IpDataLinkLayer::dataRequestToChannelId(CemiFrame& frame, uint8_t channelId)
{
    // ChannelId 0 marks a FREE slot (see dataRequestToTunnel guard). A request
    // carrying channelId 0 must never resolve to a slot — otherwise the loop
    // below would match the first unallocated tunnel and route a frame into it.
    if (channelId == 0)
        return;

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        if (tunnels[i].ChannelId == channelId)
        {
            tun = &tunnels[i];
            break;
        }
    }

    if (tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Found no Tunnel for ChannelId: ");
        println(channelId, 16);
#endif
        return;
    }

    sendFrameToTunnel(tun, frame);
}

void IpDataLinkLayer::dataRequestToTunnel(CemiFrame& frame)
{
    // L_Data goes to tunnelling connections only: on a device management connection
    // everything after the connect is DEVICE_CONFIGURATION_REQUEST/_ACK (03_08_03
    // 2.3.2 p.7), and a frame there used up a sequence number its client never saw.
    if(frame.addressType() == AddressType::GroupAddress)
    {
        for(int i = 0; i < KNX_TUNNELING; i++)
            if(tunnels[i].ChannelId != 0 && !tunnels[i].IsConfig && tunnels[i].IndividualAddress == frame.sourceAddress())
                sendFrameToTunnel(&tunnels[i], frame);
                //TODO check if source is from tunnel
        return;
    }

    KnxIpTunnelConnection *tun = nullptr;
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        if(tunnels[i].ChannelId == 0 || tunnels[i].IsConfig || tunnels[i].IndividualAddress == frame.sourceAddress())
            continue;

        if(tunnels[i].IndividualAddress == frame.destinationAddress())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if(tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Found no Tunnel for IA: ");
        println(frame.destinationAddress(), 16);
#endif
        return;
    }

    sendFrameToTunnel(tun, frame);
}

void IpDataLinkLayer::dataConfirmationToTunnel(CemiFrame& frame)
{
    if(frame.addressType() == AddressType::GroupAddress)
    {
        for(int i = 0; i < KNX_TUNNELING; i++)
            if(tunnels[i].ChannelId != 0 && !tunnels[i].IsConfig && tunnels[i].IndividualAddress == frame.sourceAddress())
                sendFrameToTunnel(&tunnels[i], frame);
                //TODO check if source is from tunnel
        return;
    }

    // To the open tunnel that sent the frame. Skipping the tunnel whose address is
    // the destination dropped the con of a client probing its own tunnel address
    // (source == destination). (upstream OpenKNX/knx 8010a05)
    KnxIpTunnelConnection *tun = nullptr;
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        if(tunnels[i].ChannelId == 0 || tunnels[i].IsConfig)
            continue;

        if(tunnels[i].IndividualAddress == frame.sourceAddress())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if(tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Found no Tunnel for IA: ");
        println(frame.destinationAddress(), 16);
#endif
        return;
    }

    sendFrameToTunnel(tun, frame);
}

void IpDataLinkLayer::dataIndicationToTunnel(CemiFrame& frame)
{
    if(frame.addressType() == AddressType::GroupAddress)
    {
        for(int i = 0; i < KNX_TUNNELING; i++)
            if(tunnels[i].ChannelId != 0 && !tunnels[i].IsConfig && tunnels[i].IndividualAddress != frame.sourceAddress())
                sendFrameToTunnel(&tunnels[i], frame);
        return;
    }

    KnxIpTunnelConnection *tun = nullptr;
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        if(tunnels[i].ChannelId == 0 || tunnels[i].IsConfig || tunnels[i].IndividualAddress == frame.sourceAddress())
            continue;

        if(tunnels[i].IndividualAddress == frame.destinationAddress())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if(tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Found no Tunnel for IA: ");
        println(frame.destinationAddress(), 16);
#endif
        return;
    }

    sendFrameToTunnel(tun, frame);
}

void IpDataLinkLayer::sendFrameToTunnel(KnxIpTunnelConnection *tunnel, CemiFrame& frame)
{
#ifdef KNX_LOG_TUNNELING
    print("Send to Channel: ");
    println(tunnel->ChannelId, 16);
#endif
    KnxIpTunnelingRequest req(frame);
    req.connectionHeader().sequenceCounter(tunnel->SequenceCounter_S++);
    req.connectionHeader().length(LEN_CH);
    req.connectionHeader().channelId(tunnel->ChannelId);

    const bool config = frame.messageCode() != L_data_req && frame.messageCode() != L_data_con && frame.messageCode() != L_data_ind;
    if(config)
        req.serviceTypeIdentifier(DeviceConfigurationRequest);

    // Timed until its ack arrives (status page); a newer request replaces one
    // still waiting.
    tunnel->LastSentSeq = req.connectionHeader().sequenceCounter();
    tunnel->LastSentAt = millis();
    tunnel->AckPending = true;
    tunnel->StatRequests++;
    if (config)
        _stats.configRequests++;
    else
        _stats.tunnelRequests++;

    _platform.sendBytesUniCast(tunnel->IpAddress, tunnel->PortData, req.data(), req.totalLength());
}

// Counted for the status page only: nothing is resent yet, so an ack changes no
// state of the channel.
void IpDataLinkLayer::loopHandleAck(uint8_t* buffer, uint16_t length, bool config)
{
    if (length < KNXIP_HEADER_LEN + LEN_CH)
        return;

    // DEVICE_CONFIGURATION_ACK has the same layout as TUNNELLING_ACK.
    KnxIpTunnelingAck ack(buffer, length);
    const uint8_t channelId = ack.connectionHeader().channelId();
    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        if (channelId != 0 && tunnels[i].ChannelId == channelId)
        {
            tun = &tunnels[i];
            break;
        }
    }

    if (tun == nullptr)
    {
        _stats.ackUnknownChannel++;
        return;
    }

    if (ack.connectionHeader().status() != E_NO_ERROR)
        _stats.ackError++;
    if (config)
        _stats.configAcks++;
    else
        _stats.tunnelAcks++;
    tun->StatAcks++;

    if (tun->AckPending && ack.connectionHeader().sequenceCounter() == tun->LastSentSeq)
    {
        const uint32_t rtt = millis() - tun->LastSentAt;
        if (rtt > _stats.ackRttMaxMs)
            _stats.ackRttMaxMs = rtt;
        tun->AckPending = false;
    }
}

bool IpDataLinkLayer::isConfigChannel(uint8_t channelId)
{
    for(int i = 0; i < KNX_TUNNELING; i++)
        if(tunnels[i].ChannelId == channelId)
            return tunnels[i].IsConfig;

    return false;
}

bool IpDataLinkLayer::isTunnelAddress(uint16_t addr)
{
    if(addr == 0)
        return false; // 0.0.0 is not a valid tunnel address and is used as default value
    
    for(int i = 0; i < KNX_TUNNELING; i++)
        if(tunnels[i].IndividualAddress == addr)
            return true;

    return false;
}

bool IpDataLinkLayer::isSentToTunnel(uint16_t address, bool isGrpAddr)
{
    if(isGrpAddr)
    {
        for(int i = 0; i < KNX_TUNNELING; i++)
            if(tunnels[i].ChannelId != 0)
                return true;
        return false;
    } else {
        for(int i = 0; i < KNX_TUNNELING; i++)
            if(tunnels[i].ChannelId != 0 && tunnels[i].IndividualAddress == address)
                return true;
        return false;
    }
}

uint8_t IpDataLinkLayer::getActiveTunnelCount() const
{
    uint8_t count = 0;
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        if(tunnels[i].ChannelId != 0)
            count++;
    }
    return count;
}

// A tunnelling device defends its additional individual addresses and generates
// layer-2 acknowledges for them (03_08_04 2.2.2). An address nobody acknowledges
// is free to an address check (03_05_02 2.22.3), so ETS could give one to a new
// device, which then collides with the tunnel that gets it. Defended are the
// usable addresses the next connect would hand out: the written pool while it is
// current, otherwise the default made from the device address. An unprogrammed
// device (15.15.0) defends none: it has no place in the topology yet, so it
// claims no addresses in it.
// Only the acknowledge is given, nothing is transmitted (upstream OpenKNX/knx
// f745efe). That answers the check by acknowledge (2.22.3), from another interface
// of the line and through this device's own tunnels alike: the device does not
// acknowledge what it sends itself, so the tunnel's L_Data.con counts an address
// defended here as delivered (TpUartDataLinkLayer::processRxFrame). The check over
// a transport connection (03_05_02 2.19) would also need the T_Disconnect that
// 03_08_04 2.2.2 asks for, which is not sent.
void IpDataLinkLayer::refreshDefendedTunnelAddresses()
{
    uint16_t next[KNX_TUNNELING];
    uint8_t count = 0;
    if(_deviceObject.individualAddressProgrammed())
    {
        const uint16_t ownAddress = _deviceObject.individualAddress();
        uint16_t propCount = 0;
        _ipParameters.readPropertyLength(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, propCount);
        const uint8_t* addresses = nullptr;
        if(propCount == KNX_TUNNELING)
            addresses = _ipParameters.propertyData(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES);
        uint8_t made[KNX_TUNNELING * 2];
        if(addresses == nullptr || !IpParameterObject::isCurrentTunnelPool(ownAddress, addresses))
        {
            IpParameterObject::defaultTunnelAddresses(ownAddress, made);
            addresses = made;
        }
        for(int i = 0; i < KNX_TUNNELING; i++)
        {
            uint16_t pa = 0;
            popWord(pa, addresses + i*2);
            if(IpParameterObject::isUsableTunnelAddress(ownAddress, pa))
                next[count++] = pa;
        }
    }

    // Switch only on a change. The copy not in use was switched away from at least
    // a second ago, so the acknowledge path is long done reading it.
    const uint8_t current = _defendedSet.load(std::memory_order_relaxed);
    if(count == _defendedCount[current] &&
       memcmp(next, _defendedAddresses[current], count * sizeof(uint16_t)) == 0)
        return;
    const uint8_t other = current ^ 1;
    memcpy(_defendedAddresses[other], next, count * sizeof(uint16_t));
    _defendedCount[other] = count;
    _defendedSet.store(other, std::memory_order_release);
}

// Called on the TP acknowledge path, in the UART task: a scan of the copy the
// refresh prepared, no property access.
bool IpDataLinkLayer::isDefendedTunnelAddress(uint16_t address) const
{
    const uint8_t set = _defendedSet.load(std::memory_order_acquire);
    for(uint8_t i = 0; i < _defendedCount[set]; i++)
        if(_defendedAddresses[set][i] == address)
            return true;
    return false;
}
#endif

void IpDataLinkLayer::loop()
{
#ifdef KNX_TUNNELING
    // Ahead of the enabled check: the addresses are defended on TP while the IP
    // side is down as well, since tunnels get them once it is up.
    if(!_defendedRefreshed || millis() - _defendedRefreshMs >= 1000)
    {
        _defendedRefreshMs = millis();
        _defendedRefreshed = true;
        refreshDefendedTunnelAddresses();
    }
#endif

    if (!_enabled)
        return;

#ifdef KNX_TUNNELING
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        if(tunnels[i].ChannelId != 0)
        {
            if(millis() - tunnels[i].lastHeartbeat > 120000)
            {
    #ifdef KNX_LOG_TUNNELING
                print("Closed Tunnel 0x");
                print(tunnels[i].ChannelId, 16);
                println(" due to no heartbeat in 2 minutes");
    #endif
                KnxIpDisconnectRequest discReq;
                discReq.channelId(tunnels[i].ChannelId);
                discReq.hpaiCtrl().length(LEN_IPHPAI);
                discReq.hpaiCtrl().code(IPV4_UDP);
                discReq.hpaiCtrl().ipAddress(tunnels[i].IpAddress);
                discReq.hpaiCtrl().ipPortNumber(tunnels[i].PortCtrl);
                _platform.sendBytesUniCast(tunnels[i].IpAddress, tunnels[i].PortCtrl, discReq.data(), discReq.totalLength());
                tunnels[i].Reset();
            }
        }
    }
#endif


    uint8_t buffer[512];
    uint16_t remotePort = 0;
    uint32_t remoteAddr = 0;
    int len = _platform.readBytesMultiCast(buffer, 512, remoteAddr, remotePort);
    if (len <= 0)
        return;

    if (len < KNXIP_HEADER_LEN)
        return;
    
    if (buffer[0] != KNXIP_HEADER_LEN 
        || buffer[1] != KNXIP_PROTOCOL_VERSION)
        return;

    // KNXnet/IP total length (octets 4-5) must equal the received datagram; a
    // mismatch is malformed -> discard (TSSH 3.2.4/3.2.5, both reference stacks
    // do this). Well-formed short frames (declared == len) still pass.
    uint16_t declaredLen = 0;
    popWord(declaredLen, buffer + 4);
    if (declaredLen != (uint16_t)len)
        return;

#ifdef KNX_ACTIVITYCALLBACK
    if(_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_RECV << KNX_ACTIVITYCALLBACK_DIR));
#endif

    uint16_t code;
    popWord(code, buffer + 2);
    switch ((KnxIpServiceType)code)
    {
        case RoutingIndication:
        {
            if (!routingActive())
                break;   // unprogrammed: no KNXnet/IP routing, see routingActive()

            KnxIpRoutingIndication routingIndication(buffer, len);
            // Routing indications carry an L_Data cEMI; drop malformed frames
            // before the network layer reads ctrl/addr fields off a bogus length.
            // Order load-bearing: totalLenght()!=0 (returns _length = datagram-len minus 6, a
            // trusted transport count, NOT derived from _data[1]) must be evaluated FIRST so
            // valid() never runs on a 0-length cEMI — valid()'s own bounds checks are gated on
            // _length!=0 and would otherwise read stale buffer bytes off the intended frame.
            if (routingIndication.frame().totalLenght() != 0 && routingIndication.frame().valid())
                frameReceived(routingIndication.frame());
            break;
        }
        
        case SearchRequest:
        {
            KnxIpSearchRequest searchRequest(buffer, len);
            KnxIpSearchResponse searchResponse(_ipParameters, _deviceObject);

            auto hpai = searchRequest.hpai();
#ifdef KNX_ACTIVITYCALLBACK
            if(_dllcb)
                _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_SEND << KNX_ACTIVITYCALLBACK_DIR) | (KNX_ACTIVITYCALLBACK_IPUNICAST));
#endif
            _platform.sendBytesUniCast(hpai.ipAddress(), hpai.ipPortNumber(), searchResponse.data(), searchResponse.totalLength());
            break;
        }
        case SearchRequestExt:
        {
            #if KNX_SERVICE_FAMILY_CORE >= 2
            loopHandleSearchRequestExtended(buffer, len);
            #endif
            break;
        }
#ifdef KNX_TUNNELING
        case ConnectRequest:
        {
            loopHandleConnectRequest(buffer, len, remoteAddr, remotePort);
            break;
        }

        case ConnectionStateRequest:
        {
            loopHandleConnectionStateRequest(buffer, len);
            break;
        }

        case DisconnectRequest:
        {
            loopHandleDisconnectRequest(buffer, len, remoteAddr);
            break;
        }

        case DescriptionRequest:
        {
            loopHandleDescriptionRequest(buffer, len);
            break;
        }

        case DeviceConfigurationRequest:
        {
            loopHandleDeviceConfigurationRequest(buffer, len, remoteAddr);
            break;
        }

        case TunnelingRequest:
        {
            loopHandleTunnelingRequest(buffer, len, remoteAddr);
            return;
        }

        case DeviceConfigurationAck:
        {
            loopHandleAck(buffer, len, true);
            break;
        }

        case TunnelingAck:
        {
            loopHandleAck(buffer, len, false);
            break;
        }
#endif
        // Flow control of other routers: counted, not acted on yet, and without
        // the log line every unknown service gets (upstream 3d7ab47).
        case RoutingLostMessage:
        {
            _stats.routingRxLostMessage++;
            break;
        }

        case RoutingBusy:
        {
            _stats.routingRxBusy++;
            break;
        }

        default:
            print("Unhandled service identifier: ");
            println(code, HEX);
            break;
    }
}

#if KNX_SERVICE_FAMILY_CORE >= 2
void IpDataLinkLayer::loopHandleSearchRequestExtended(uint8_t* buffer, uint16_t length)
{
    KnxIpSearchRequestExtended searchRequest(buffer, length);

    if(searchRequest.srpByProgMode)
    {
        println("srpByProgMode");
        if(!knx.progMode()) return;
    }

    if(searchRequest.srpByMacAddr)
    {
        println("srpByMacAddr");
        const uint8_t *x = _ipParameters.propertyData(PID_MAC_ADDRESS);
        for(int i = 0; i<6;i++)
            if(searchRequest.srpMacAddr[i] != x[i])
                return;
    }

    //defaults: "Device Information DIB", "Extended Device Information DIB" and "Supported Services DIB".
    const bool routing = routingActive();
    int dibLength = LEN_DEVICE_INFORMATION_DIB + KnxIpSupportedServiceDIB::lengthFor(routing) + LEN_EXTENDED_DEVICE_INFORMATION_DIB;

    if(searchRequest.srpByService)
    {
        println("srpByService");
        uint8_t length = searchRequest.srpServiceFamilies[0];
        uint8_t *currentPos = searchRequest.srpServiceFamilies + 2;
        for(int i = 0; i < (length-2)/2; i++)
        {
            uint8_t serviceFamily = (currentPos + i*2)[0];
            uint8_t version = (currentPos + i*2)[1];
            switch(serviceFamily)
            {
                case Core:
                    if(version > KNX_SERVICE_FAMILY_CORE) return;
                    break;
                case DeviceManagement:
                    if(version > KNX_SERVICE_FAMILY_DEVICE_MANAGEMENT) return;
                    break;
                case Tunnelling:
                    if(version > KNX_SERVICE_FAMILY_TUNNELING) return;
                    break;
                case Routing:
#if MASK_VERSION == 0x091A
                    // not served while routing is inactive, so do not answer a search for it
                    if(!routing || version > KNX_SERVICE_FAMILY_ROUTING) return;
                    break;
#else
                    return; // only a router serves the routing family
#endif
            }
        }
    }

    if(searchRequest.srpRequestDIBs)
    {
        //println("srpRequestDIBs");
        if(searchRequest.requestedDIB(IP_CONFIG))
            dibLength += LEN_IP_CONFIG_DIB; //16

        if(searchRequest.requestedDIB(IP_CUR_CONFIG))
            dibLength += LEN_IP_CURRENT_CONFIG_DIB; //20

        if(searchRequest.requestedDIB(KNX_ADDRESSES))
        {uint16_t length = 0;
            _ipParameters.readPropertyLength(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, length);
            dibLength += 4 + length*2;
        }

        if(searchRequest.requestedDIB(MANUFACTURER_DATA))
            dibLength += 0; //4 + n

        if(searchRequest.requestedDIB(TUNNELING_INFO))
        {
            uint16_t length = 0;
            _ipParameters.readPropertyLength(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, length);
            dibLength += 4 + length*4;
        }
    }

    KnxIpSearchResponseExtended searchResponse(_ipParameters, _deviceObject, dibLength);

    searchResponse.setDeviceInfo(_ipParameters, _deviceObject); //DescriptionTypeCode::DeviceInfo 1
    searchResponse.setSupportedServices(routing); //DescriptionTypeCode::SUPP_SVC_FAMILIES 2
    searchResponse.setExtendedDeviceInfo(); //DescriptionTypeCode::EXTENDED_DEVICE_INFO 8

    if(searchRequest.srpRequestDIBs)
    {
        if(searchRequest.requestedDIB(IP_CONFIG))
            searchResponse.setIpConfig(_ipParameters);

        if(searchRequest.requestedDIB(IP_CUR_CONFIG))
            searchResponse.setIpCurrentConfig(_ipParameters);

        if(searchRequest.requestedDIB(KNX_ADDRESSES))
            searchResponse.setKnxAddresses(_ipParameters, _deviceObject);

        if(searchRequest.requestedDIB(MANUFACTURER_DATA))
        {
            //println("requested MANUFACTURER_DATA but not implemented");
        }

        if(searchRequest.requestedDIB(TUNNELING_INFO))
            searchResponse.setTunnelingInfo(_ipParameters, _deviceObject, tunnels);
    }

    if(searchResponse.totalLength() > 500)
    {
        printf("skipped response length > 500. Length: %d bytes\n", searchResponse.totalLength());
        return;
    }

    _platform.sendBytesUniCast(searchRequest.hpai().ipAddress(), searchRequest.hpai().ipPortNumber(), searchResponse.data(), searchResponse.totalLength());
}
#endif

#ifdef KNX_TUNNELING
// A channel is matched by its id alone, and the ids count up from 1 after every
// boot, so any host on the network could close a tunnel or send frames into it.
// Only the address the CONNECT_REQUEST came from may use the channel. This is the
// UDP source, not the HPAI: behind NAT the HPAI names the client's own address and
// the frames arrive from the translated one. It stops stray and sweeping senders;
// a sender that forges its IP source address on the local network is not stopped.
// (upstream OpenKNX/knx fa6d3a0, adapted: our tunnels keep the control address in
// IpAddress)
bool IpDataLinkLayer::fromTunnelPeer(const KnxIpTunnelConnection* tun, uint32_t src_addr)
{
    return tun->PeerAddress == src_addr;
}

bool IpDataLinkLayer::tunnelAddressInUse(uint16_t pa)
{
    for(int x = 0; x < KNX_TUNNELING; x++)
        if(tunnels[x].ChannelId != 0 && tunnels[x].IndividualAddress == pa)
            return true;
    return false;
}

void IpDataLinkLayer::loopHandleConnectRequest(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port)
{
    KnxIpConnectRequest connRequest(buffer, length);
#ifdef KNX_LOG_TUNNELING
    println("Got Connect Request!");
    switch(connRequest.cri().type())
    {
        case DEVICE_MGMT_CONNECTION:
            println("Device Management Connection");
            break;
        case TUNNEL_CONNECTION:
            println("Tunnel Connection");
            break;
        case REMLOG_CONNECTION:
            println("RemLog Connection");
            break;
        case REMCONF_CONNECTION:
            println("RemConf Connection");
            break;
        case OBJSVR_CONNECTION:
            println("ObjectServer Connection");
            break;
    }
    
    print("Data Endpoint: ");
    uint32_t ip = connRequest.hpaiData().ipAddress();
    print(ip >> 24);
    print(".");
    print((ip >> 16) & 0xFF);
    print(".");
    print((ip >> 8) & 0xFF);
    print(".");
    print(ip & 0xFF);
    print(":");
    println(connRequest.hpaiData().ipPortNumber());
    print("Ctrl Endpoint: ");
    ip = connRequest.hpaiCtrl().ipAddress();
    print(ip >> 24);
    print(".");
    print((ip >> 16) & 0xFF);
    print(".");
    print((ip >> 8) & 0xFF);
    print(".");
    print(ip & 0xFF);
    print(":");
    println(connRequest.hpaiCtrl().ipPortNumber());
#endif

    //We only support 0x03 and 0x04!
    if(connRequest.cri().type() != TUNNEL_CONNECTION && connRequest.cri().type() != DEVICE_MGMT_CONNECTION)
    {
#ifdef KNX_LOG_TUNNELING
        println("Only Tunnel/DeviceMgmt Connection ist supported!");
#endif
        KnxIpConnectResponse connRes(0x00, E_CONNECTION_TYPE);
        _platform.sendBytesUniCast(connRequest.hpaiCtrl().ipAddress(), connRequest.hpaiCtrl().ipPortNumber(), connRes.data(), connRes.totalLength());
        return;
    }

    if(connRequest.cri().type() == TUNNEL_CONNECTION && connRequest.cri().layer() != 0x02) //LinkLayer
    {
        //We only support 0x02!
#ifdef KNX_LOG_TUNNELING
    println("Only LinkLayer ist supported!");
#endif
        KnxIpConnectResponse connRes(0x00, E_TUNNELING_LAYER);
        _platform.sendBytesUniCast(connRequest.hpaiCtrl().ipAddress(), connRequest.hpaiCtrl().ipPortNumber(), connRes.data(), connRes.totalLength());
        return;
    }

    // data preparation

    uint32_t srcIP = connRequest.hpaiCtrl().ipAddress()? connRequest.hpaiCtrl().ipAddress() : src_addr;
    uint16_t srcPort = connRequest.hpaiCtrl().ipPortNumber()? connRequest.hpaiCtrl().ipPortNumber() : src_port;

    // read current elements in PID_ADDITIONAL_INDIVIDUAL_ADDRESSES 
    uint16_t propCount = 0;
    _ipParameters.readPropertyLength(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, propCount);
    const uint8_t *addresses = nullptr;
    if(propCount == KNX_TUNNELING)
        addresses = _ipParameters.propertyData(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES);

    // Tunnel addresses belong to the line of the device. The pool below is made
    // from the device address when there is none, and since the configuration is
    // stored it survives a later change of that address: after the device moved to
    // another line, clients would keep getting addresses from the old one. A pool
    // outside the device's line is therefore made anew, and so is the .1 to .10
    // pool that firmware up to 1.4.29 made on the device's own line. A pool written
    // through device management that happens to be exactly .1 to .10 is renewed
    // as well. The line is read from the first entry that is not empty (0.0.0), so
    // a written pool with a gap at the start is kept; one without any entry is made
    // anew.
    const uint16_t ownAddress = _deviceObject.individualAddress();
    if(addresses != nullptr && !IpParameterObject::isCurrentTunnelPool(ownAddress, addresses))
    {
#ifdef KNX_LOG_TUNNELING
        println("Tunnel-PAs are outside the device's line or the old default, renewing them");
#endif
        addresses = nullptr;
    }

    if(addresses == nullptr)    // no tunnel PA configured, that means device is unconfigured and has 15.15.0
    {
        uint8_t addrbuffer[KNX_TUNNELING*2];
        IpParameterObject::defaultTunnelAddresses(ownAddress, addrbuffer);
        uint8_t count = KNX_TUNNELING;
        _ipParameters.writeProperty(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, 1, addrbuffer, count);
        // re-point `addresses` at the property's live storage: addrbuffer[] is block-scoped
        // and would dangle when read later (popWord(addresses + tunIdx*2)); propertyData()
        // returns the persisted copy we just wrote (same source the configured branch uses).
        addresses = _ipParameters.propertyData(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES);
#ifdef KNX_LOG_TUNNELING
    	println("no Tunnel-PAs configured, using own subnet");
#endif
    }

    _ipParameters.readPropertyLength(PID_CUSTOM_RESERVED_TUNNELS_CTRL, propCount);
    const uint8_t *tunCtrlBytes = nullptr;
    if(propCount == KNX_TUNNELING)
        tunCtrlBytes = _ipParameters.propertyData(PID_CUSTOM_RESERVED_TUNNELS_CTRL);

    _ipParameters.readPropertyLength(PID_CUSTOM_RESERVED_TUNNELS_IP, propCount);
    const uint8_t *tunCtrlIp = nullptr;
    if(propCount == KNX_TUNNELING)
        tunCtrlIp = _ipParameters.propertyData(PID_CUSTOM_RESERVED_TUNNELS_IP);
    
    bool resTunActive = (tunCtrlBytes && tunCtrlIp);
#ifdef KNX_LOG_TUNNELING
    	if(resTunActive) println("Reserved Tunnel Feature active");

        if(tunCtrlBytes)
            printHex("tunCtrlBytes", tunCtrlBytes, KNX_TUNNELING);
        if(tunCtrlIp)
            printHex("tunCtrlIp", tunCtrlIp, KNX_TUNNELING*4);
#endif

    // check if there is a reserved tunnel for the source
    int firstFreeTunnel = -1;
    int firstResAndFreeTunnel = -1;
    int firstResAndOccTunnel = -1;
    const bool forTunnel = connRequest.cri().type() == TUNNEL_CONNECTION;
    bool reservedAssign = false;    // tunIdx is a reserved slot: it keeps its own address
    bool noUniqueAddress = false;   // a slot was free, but no address for it
    bool tunnelResActive[KNX_TUNNELING];
    uint8_t tunnelResOptions[KNX_TUNNELING];
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        if(resTunActive)
        {
            tunnelResActive[i] = *(tunCtrlBytes+i) & 0x80;
            tunnelResOptions[i] = (*(tunCtrlBytes+i) & 0x60) >> 5;
        }

        if(resTunActive && tunnelResActive[i])   // tunnel reserve feature active for this tunnel
        {
            #ifdef KNX_LOG_TUNNELING
            print("tunnel reserve feature active for this tunnel: ");
            print(tunnelResActive[i]);
            print("  options: ");
            println(tunnelResOptions[i]);
            #endif
            
            uint32_t rIP = 0;
            popInt(rIP, tunCtrlIp+4*i);
            if(srcIP == rIP && connRequest.cri().type() == TUNNEL_CONNECTION)
            {
                // reserved tunnel for this ip found
                if(tunnels[i].ChannelId == 0) // check if it is free
                {
                    if(firstResAndFreeTunnel < 0)
                        firstResAndFreeTunnel = i;
                }
                else
                {
                    if(firstResAndOccTunnel < 0)
                        firstResAndOccTunnel = i;
                }
            }
        }
        else
        {
            if(tunnels[i].ChannelId == 0 && firstFreeTunnel < 0)
                firstFreeTunnel = i;
        }
    }
#ifdef KNX_LOG_TUNNELING
    	print("firstFreeTunnel: ");
        print(firstFreeTunnel);
        print(" firstResAndFreeTunnel: ");
        print(firstResAndFreeTunnel);
        print(" firstResAndOccTunnel: ");
        println(firstResAndOccTunnel);
#endif
    
    
    uint8_t tunIdx = 0xff;
    if(resTunActive && (firstResAndFreeTunnel >= 0 || firstResAndOccTunnel >= 0))   // tunnel reserve feature active (for this src)
    {
        if(firstResAndFreeTunnel >= 0)
        {
            tunIdx = firstResAndFreeTunnel;
            reservedAssign = true;
        }
        else if(firstResAndOccTunnel >= 0)
        {
            if(tunnelResOptions[firstResAndOccTunnel] == 1) // decline req
            {
                ; // do nothing => decline
            }
            else if(tunnelResOptions[firstResAndOccTunnel] == 2)  // close current tunnel connection on this tunnel and assign to this request
            {
                // Only for an address that can be handed out at all; otherwise the
                // open tunnel would be closed for a request that is then refused.
                uint16_t resPa = 0;
                popWord(resPa, addresses + (firstResAndOccTunnel*2));
                if(!IpParameterObject::isUsableTunnelAddress(ownAddress, resPa))
                {
                    noUniqueAddress = true; // decline, the open tunnel stays
                }
                else
                {
                    KnxIpDisconnectRequest discReq;
                    discReq.channelId(tunnels[firstResAndOccTunnel].ChannelId);
                    discReq.hpaiCtrl().length(LEN_IPHPAI);
                    discReq.hpaiCtrl().code(IPV4_UDP);
                    discReq.hpaiCtrl().ipAddress(tunnels[firstResAndOccTunnel].IpAddress);
                    discReq.hpaiCtrl().ipPortNumber(tunnels[firstResAndOccTunnel].PortCtrl);
                    _platform.sendBytesUniCast(tunnels[firstResAndOccTunnel].IpAddress, tunnels[firstResAndOccTunnel].PortCtrl, discReq.data(), discReq.totalLength());
                    tunnels[firstResAndOccTunnel].Reset();

                    tunIdx = firstResAndOccTunnel;
                    reservedAssign = true;
                }
            }
            else if(tunnelResOptions[firstResAndOccTunnel] == 3)  // use the first unreserved tunnel (if one)
            {
                if(firstFreeTunnel >= 0)
                    tunIdx = firstFreeTunnel;
                else
                    ; // do nothing => decline
            }
            //else
                // should not happen
                // do nothing => decline
        }
        //else
            // should not happen
            // do nothing => decline
    }
    else
    {
        if(firstFreeTunnel >= 0)
            tunIdx = firstFreeTunnel;
        //else
        // do nothing => decline
    }

    KnxIpTunnelConnection *tun = nullptr;
    if(tunIdx != 0xFF)
    {
        tun = &tunnels[tunIdx];

        // A tunnel address is never x.y.0 or the device's own address, and never
        // one an open tunnel already has, whatever a written pool holds (08_TSSH
        // 5.3.1, 5.3.2). A device management connection gets none: its connect
        // response carries no address and its L_Data is dropped
        // (loopHandleDeviceConfigurationRequest, loopHandleTunnelingRequest), so it
        // neither uses up a pool address nor is refused for lack of one, and ETS
        // can reach the device to correct a pool as long as a slot is free.
        uint16_t tunPa = 0;
        if(forTunnel && reservedAssign)
        {
            // a reserved tunnel keeps the address of its slot
            popWord(tunPa, addresses + (tunIdx*2));
            if(!IpParameterObject::isUsableTunnelAddress(ownAddress, tunPa) || tunnelAddressInUse(tunPa))
                tunPa = 0;
        }
        else if(forTunnel)
        {
            // any other tunnel takes the first free address of the pool, wherever it
            // stands, and leaves the reserved slots' addresses alone (upstream
            // OpenKNX/knx ce26d6e)
            for(int a = 0; a < KNX_TUNNELING && tunPa == 0; a++)
            {
                if(resTunActive && (tunCtrlBytes[a] & 0x80))
                    continue;
                uint16_t cand = 0;
                popWord(cand, addresses + a*2);
                if(IpParameterObject::isUsableTunnelAddress(ownAddress, cand) && !tunnelAddressInUse(cand))
                    tunPa = cand;
            }
        }

        if(forTunnel && tunPa == 0)     // 0 is x.y.0, so never a usable address
        {
#ifdef KNX_LOG_TUNNELING
    	    println("no tunnel address left that is not x.y.0, the device's own or in use");
#endif
            tunIdx = 0xFF;
            tun = nullptr;
            noUniqueAddress = true;
        }
        if(tun)
            tun->IndividualAddress = tunPa;

    }

    if(tun == nullptr)
    {
        println("no free tunnel availible");
        KnxIpConnectResponse connRes(0x00, noUniqueAddress ? E_NO_MORE_UNIQUE_CONNECTIONS : E_NO_MORE_CONNECTIONS);
        _platform.sendBytesUniCast(connRequest.hpaiCtrl().ipAddress(), connRequest.hpaiCtrl().ipPortNumber(), connRes.data(), connRes.totalLength());
        return;
    }

    if(connRequest.cri().type() == DEVICE_MGMT_CONNECTION)
        tun->IsConfig = true;

    // the channel ID shall be unique on this tunnel server. catch the rare case of a double channel ID
    bool channelIdInUse;
    do
    {
        _lastChannelId++;
        channelIdInUse = false;
        for(int x = 0; x < KNX_TUNNELING; x++)
            if(tunnels[x].ChannelId == _lastChannelId)
                channelIdInUse = true;
    }
    while(channelIdInUse);

    tun->ChannelId = _lastChannelId;
    tun->lastHeartbeat = millis();
    if(_lastChannelId == 255)
        _lastChannelId = 0;

    tun->IpAddress = srcIP;
    tun->PeerAddress = src_addr;
    tun->PortData = connRequest.hpaiData().ipPortNumber()?connRequest.hpaiData().ipPortNumber():srcPort;
    tun->PortCtrl = connRequest.hpaiCtrl().ipPortNumber()?connRequest.hpaiCtrl().ipPortNumber():srcPort;

    print("New Tunnel-Connection[");
    print(tunIdx);
    print("], Channel: 0x");
    print(tun->ChannelId, 16);
    print(" PA: ");
    print(tun->IndividualAddress >> 12);
    print(".");
    print((tun->IndividualAddress >> 8) & 0xF);
    print(".");
    print(tun->IndividualAddress & 0xFF);

    print(" with ");
    print(tun->IpAddress >> 24);
    print(".");
    print((tun->IpAddress >> 16) & 0xFF);
    print(".");
    print((tun->IpAddress >> 8) & 0xFF);
    print(".");
    print(tun->IpAddress & 0xFF);
    print(":");
    print(tun->PortData);
    if(tun->PortData != tun->PortCtrl)
    {
        print(" (Ctrlport: ");
        print(tun->PortCtrl);
        print(")");
    }
    if(tun->IsConfig)
    {
        print(" (Config-Channel)");
    }
    println();


    KnxIpConnectResponse connRes(_ipParameters, tun->IndividualAddress, 3671, tun->ChannelId, connRequest.cri().type());

    // Route back (03_08_02 8.6.2.2): a client behind NAT or a VPN sends a
    // CONNECT_REQUEST whose data endpoint HPAI is all zeroes, because it cannot know
    // which address the server would have to answer to. The reply must not push an
    // address back at it either — filling in our own address hands such a client one
    // it cannot reach. Only the fully zeroed HPAI counts: an HPAI with just the
    // address or just the port at zero is invalid, not a route-back request.
    if (connRequest.hpaiData().ipAddress() == 0 && connRequest.hpaiData().ipPortNumber() == 0)
    {
        connRes.controlEndpoint().ipAddress(0);
        connRes.controlEndpoint().ipPortNumber(0);
    }

    _platform.sendBytesUniCast(tun->IpAddress, tun->PortCtrl, connRes.data(), connRes.totalLength());
}

void IpDataLinkLayer::loopHandleConnectionStateRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpStateRequest stateRequest(buffer, length);

    KnxIpTunnelConnection *tun = nullptr;
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        // channelId 0 is the free-slot sentinel (KnxIpTunnelConnection::Reset),
        // never a valid assigned id; guard it so a request carrying 0 cannot
        // match a free tunnel. Falls through to the E_CONNECTION_ID path below.
        if(stateRequest.channelId() != 0 && tunnels[i].ChannelId == stateRequest.channelId())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if(tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Channel ID nicht gefunden: ");
        println(stateRequest.channelId());
#endif
        KnxIpStateResponse stateRes(0x00, E_CONNECTION_ID);
        _platform.sendBytesUniCast(stateRequest.hpaiCtrl().ipAddress(), stateRequest.hpaiCtrl().ipPortNumber(), stateRes.data(), stateRes.totalLength());
        return;
    }

    if (!_knxBusConnected)
    {
#ifdef KNX_LOG_TUNNELING
        print("KNX Bus connection lost for channel: ");
        println(tun->ChannelId);
#endif
        KnxIpStateResponse stateRes(tun->ChannelId, E_KNX_CONNECTION);
        _platform.sendBytesUniCast(stateRequest.hpaiCtrl().ipAddress(), stateRequest.hpaiCtrl().ipPortNumber(), stateRes.data(), stateRes.totalLength());
        return;
    }

    //TODO check when to send E_DATA_CONNECTION

    tun->lastHeartbeat = millis();
    KnxIpStateResponse stateRes(tun->ChannelId, E_NO_ERROR);
    _platform.sendBytesUniCast(stateRequest.hpaiCtrl().ipAddress(), stateRequest.hpaiCtrl().ipPortNumber(), stateRes.data(), stateRes.totalLength());
}

void IpDataLinkLayer::loopHandleDisconnectRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr)
{
    KnxIpDisconnectRequest discReq(buffer, length);
            
#ifdef KNX_LOG_TUNNELING
    print(">>> Disconnect Channel ID: ");
    println(discReq.channelId());
#endif
    
    KnxIpTunnelConnection *tun = nullptr;
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        // channelId 0 is the free-slot sentinel; guard it so a request carrying
        // 0 cannot match a free tunnel. Falls through to E_CONNECTION_ID below.
        if(discReq.channelId() != 0 && tunnels[i].ChannelId == discReq.channelId())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if(tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Channel ID nicht gefunden: ");
        println(discReq.channelId());
#endif
        KnxIpDisconnectResponse discRes(0x00, E_CONNECTION_ID);
        _platform.sendBytesUniCast(discReq.hpaiCtrl().ipAddress(), discReq.hpaiCtrl().ipPortNumber(), discRes.data(), discRes.totalLength());
        return;
    }

    if (!fromTunnelPeer(tun, src_addr))
    {
        println("disconnect request from a foreign source address -> ignored");
        return;
    }


    KnxIpDisconnectResponse discRes(tun->ChannelId, E_NO_ERROR);
    _platform.sendBytesUniCast(discReq.hpaiCtrl().ipAddress(), discReq.hpaiCtrl().ipPortNumber(), discRes.data(), discRes.totalLength());
    tun->Reset();
}

void IpDataLinkLayer::loopHandleDescriptionRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpDescriptionRequest descReq(buffer, length);
    KnxIpDescriptionResponse descRes(_ipParameters, _deviceObject);
    _platform.sendBytesUniCast(descReq.hpaiCtrl().ipAddress(), descReq.hpaiCtrl().ipPortNumber(), descRes.data(), descRes.totalLength());
}

void IpDataLinkLayer::loopHandleDeviceConfigurationRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr)
{
    // Reject runt frames before constructing the request: the ctor computes the
    // cEMI length as (length - LEN_KNXIP_HEADER - LEN_CH), which underflows
    // uint16 for short frames -> a ~64KB bogus cEMI length that downstream
    // memcpy/fillTelegram would trust. Need the 6-byte KNXnet/IP header +
    // 4-byte connection header + at least a 1-byte cEMI message code.
    if (length < KNXIP_HEADER_LEN + 4 /*connection header*/ + 1)
        return;

    KnxIpConfigRequest confReq(buffer, length);
    
    KnxIpTunnelConnection *tun = nullptr;
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        // channelId 0 is the free-slot sentinel; guard it so a request carrying
        // 0 cannot match a free tunnel. Falls through to E_CONNECTION_ID below.
        if(confReq.connectionHeader().channelId() != 0 && tunnels[i].ChannelId == confReq.connectionHeader().channelId())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if(tun == nullptr)
    {
        print("Channel ID nicht gefunden: ");
        println(confReq.connectionHeader().channelId());
        KnxIpStateResponse stateRes(0x00, E_CONNECTION_ID);
        _platform.sendBytesUniCast(0, 0, stateRes.data(), stateRes.totalLength());
        return;
    }

    if (!fromTunnelPeer(tun, src_addr))
    {
        println("device configuration request from a foreign source address -> ignored");
        return;
    }

    // Counted only: the sequence is not checked yet, a repeat runs again.
    const uint8_t confSequence = confReq.connectionHeader().sequenceCounter();
    if (tun->SequenceCounter_DC >= 0)
    {
        if (confSequence == (uint8_t)tun->SequenceCounter_DC)
        {
            tun->StatRepeats++;
            _stats.configRxRepeats++;
        }
        else if (confSequence != (uint8_t)(tun->SequenceCounter_DC + 1))
        {
            tun->StatOutOfOrder++;
            _stats.configRxOutOfOrder++;
        }
    }
    tun->SequenceCounter_DC = confSequence;

    KnxIpTunnelingAck tunnAck;
    tunnAck.serviceTypeIdentifier(DeviceConfigurationAck);
    tunnAck.connectionHeader().length(4);
    tunnAck.connectionHeader().channelId(tun->ChannelId);
    tunnAck.connectionHeader().sequenceCounter(confReq.connectionHeader().sequenceCounter());
    tunnAck.connectionHeader().status(E_NO_ERROR);
    _platform.sendBytesUniCast(tun->IpAddress, tun->PortData, tunnAck.data(), tunnAck.totalLength());

    tun->lastHeartbeat = millis();

    // A device configuration request carries local device management services
    // only (03_08_03 4.2.5: M_PropRead/Write, M_Reset, M_FuncProp*). An L_Data.req
    // taken from here would reach the bus without the source check that
    // loopHandleTunnelingRequest applies, so a client could send under any address.
    if (confReq.frame().messageCode() == L_data_req)
    {
        println("L_Data.req in a device configuration request -> ignored");
        return;
    }

    _cemiServer->frameReceived(confReq.frame(), tun->ChannelId);
}

void IpDataLinkLayer::loopHandleTunnelingRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr)
{
    // Reject runt frames before constructing the request: the ctor computes the
    // cEMI length as (length - LEN_CH - headerLength()), which underflows uint16
    // for short frames -> a ~64KB bogus cEMI length that downstream memcpy /
    // fillTelegram would trust. Need the 6-byte KNXnet/IP header + 4-byte
    // connection header + at least a 1-byte cEMI message code.
    if (length < KNXIP_HEADER_LEN + 4 /*connection header*/ + 1)
        return;

    KnxIpTunnelingRequest tunnReq(buffer, length);

    KnxIpTunnelConnection *tun = nullptr;
    for(int i = 0; i < KNX_TUNNELING; i++)
    {
        // channelId 0 is the free-slot sentinel; guard it so a request carrying
        // 0 cannot match a free tunnel. Falls through to E_CONNECTION_ID below.
        if(tunnReq.connectionHeader().channelId() != 0 && tunnels[i].ChannelId == tunnReq.connectionHeader().channelId())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if(tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Channel ID nicht gefunden: ");
        println(tunnReq.connectionHeader().channelId());
#endif
        KnxIpStateResponse stateRes(0x00, E_CONNECTION_ID);
        _platform.sendBytesUniCast(0, 0, stateRes.data(), stateRes.totalLength());
        return;
    }

    if (!fromTunnelPeer(tun, src_addr))
    {
        println("tunnel frame from a foreign source address -> ignored");
        return;
    }

    uint8_t sequence = tunnReq.connectionHeader().sequenceCounter();
    if(sequence == tun->SequenceCounter_R)
    {
        tun->StatRepeats++;
        _stats.tunnelRxRepeats++;
#ifdef KNX_LOG_TUNNELING
        print("Received SequenceCounter again: ");
        println(tunnReq.connectionHeader().sequenceCounter());
#endif
        //we already got this one
        //so just ack it
        KnxIpTunnelingAck tunnAck;
        tunnAck.connectionHeader().length(4);
        tunnAck.connectionHeader().channelId(tun->ChannelId);
        tunnAck.connectionHeader().sequenceCounter(tunnReq.connectionHeader().sequenceCounter());
        tunnAck.connectionHeader().status(E_NO_ERROR);
        _platform.sendBytesUniCast(tun->IpAddress, tun->PortData, tunnAck.data(), tunnAck.totalLength());
        return;
    } else if((uint8_t)(sequence - 1) != tun->SequenceCounter_R) {
        tun->StatOutOfOrder++;
        _stats.tunnelRxOutOfOrder++;
#ifdef KNX_LOG_TUNNELING
        print("Wrong SequenceCounter: got ");
        print(tunnReq.connectionHeader().sequenceCounter());
        print(" expected ");
        println((uint8_t)(tun->SequenceCounter_R + 1));
#endif
        //Dont handle it
        return;
    }
    
    KnxIpTunnelingAck tunnAck;
    tunnAck.connectionHeader().length(4);
    tunnAck.connectionHeader().channelId(tun->ChannelId);
    tunnAck.connectionHeader().sequenceCounter(tunnReq.connectionHeader().sequenceCounter());
    tunnAck.connectionHeader().status(E_NO_ERROR);
    _platform.sendBytesUniCast(tun->IpAddress, tun->PortData, tunnAck.data(), tunnAck.totalLength());

    tun->SequenceCounter_R = tunnReq.connectionHeader().sequenceCounter();

    // A device management connection has no tunnel address to send under: its
    // connect response carries none.
    if (tun->IsConfig && tunnReq.frame().messageCode() == L_data_req)
    {
        println("L_Data.req on a device management connection -> ignored");
        return;
    }

    // Tunnel source: 03_08_04 (Tunnelling v01.07.01, p.7) has the server fill in
    // the assigned IA for source 0x0000 and send any other source unchanged. We
    // deliberately rewrite any other source too: a client must not send under
    // another tunnel's or a bus device's address, and a tool that puts a non-zero
    // source in its frames still works.
    //
    // GUARD: only apply to L_Data_req. CemiFrame::sourceAddress() unconditionally
    // reads/writes bytes at _ctrl1+2 (= addl_info_len+4 from cEMI start), which
    // for L_Data are the src IA bytes — but for M_PropRead_req / M_PropWrite_req
    // those same bytes are propertyId / noE+startIdx, so calling sourceAddress()
    // on a non-L_Data frame corrupts the request.
    if (tunnReq.frame().messageCode() == L_data_req)
    {
        uint16_t src = tunnReq.frame().sourceAddress();
        if (src == 0)
        {
            tunnReq.frame().sourceAddress(tun->IndividualAddress);
        }
        else if (src != tun->IndividualAddress)
        {
#ifdef KNX_LOG_TUNNELING
            print("Tunnel 0x");
            print(tun->ChannelId, 16);
            print(": source ");
            print((src >> 12) & 0xF); print("."); print((src >> 8) & 0xF); print("."); print(src & 0xFF);
            print(" rewritten to assigned ");
            print((tun->IndividualAddress >> 12) & 0xF); print(".");
            print((tun->IndividualAddress >> 8) & 0xF); print(".");
            println(tun->IndividualAddress & 0xFF);
#endif
            tunnReq.frame().sourceAddress(tun->IndividualAddress);
        }
    }

    _cemiServer->frameReceived(tunnReq.frame(), tun->ChannelId);
}
#endif

void IpDataLinkLayer::refreshMultiCast()
{
    if (_enabled)
        _platform.refreshMultiCast();
}

void IpDataLinkLayer::enabled(bool value)
{
//    _print("own address: ");
//    _println(_deviceObject.individualAddress());
    if (value && !_enabled)
    {
        // A configuration tool may write PID_ROUTING_MULTICAST_ADDRESS, and the value
        // is persisted. An address outside 224.0.0.0/4, or inside the local control
        // block 224.0.0.0/24 which lwIP refuses, cannot be joined — and because a
        // failed join leaves the whole IP path disabled below, one bad value would
        // take tunnelling with it and survive every reboot. Fall back to the fixed
        // KNXnet/IP group (03_08_02 8.5.2.1) instead of going dark.
        uint32_t group = _ipParameters.propertyValue<uint32_t>(PID_ROUTING_MULTICAST_ADDRESS);
        const uint8_t firstOctet = (uint8_t)(group >> 24);
        const bool linkLocal = (group >> 8) == 0xE00000; // 224.0.0.0/24
        if (firstOctet < 224 || firstOctet > 239 || linkLocal)
        {
            println("routing multicast address unusable, falling back to 224.0.23.12");
            group = 0xE000170C;
        }

        // enabled() must not claim an endpoint the join did not produce: a device
        // that keeps link and address but never joined answers no SEARCH_REQUEST,
        // while status page and LED report a healthy gateway.
        _enabled = _platform.setupMultiCast(group, KNXIP_MULTICAST_PORT);
        return;
    }

    if(!value && _enabled)
    {
        _platform.closeMultiCast();
        _enabled = false;
        return;
    }
}

bool IpDataLinkLayer::enabled() const
{
    return _enabled;
}

void IpDataLinkLayer::knxBusConnected(bool connected)
{
    _knxBusConnected = connected;
}

DptMedium IpDataLinkLayer::mediumType() const
{
    return DptMedium::KNX_IP;
}

bool IpDataLinkLayer::sendBytes(uint8_t* bytes, uint16_t length)
{
    if (!_enabled)
        return false;

    return _platform.sendBytesMultiCast(bytes, length);
}

bool IpDataLinkLayer::isSendLimitReached()
{
    uint32_t curTime = millis() / 100;

    // check if the countbuffer must be adjusted
    if(_frameCountTimeBase >= curTime)
    {
        uint32_t timeBaseDiff = _frameCountTimeBase - curTime;
        if(timeBaseDiff > 10)
            timeBaseDiff = 10;
        for(int i = 0; i < timeBaseDiff ; i++)
        {
            _frameCountBase++;
            _frameCountBase = _frameCountBase % 10;
            _frameCount[_frameCountBase] = 0;
        }
        _frameCountTimeBase = curTime;
    }
    else // _frameCountTimeBase < curTime => millis overflow, reset
    {
        for(int i = 0; i < 10 ; i++)
            _frameCount[i] = 0;
        _frameCountBase = 0;
        _frameCountTimeBase = curTime;
    }

    //check if we are over the limit
    uint16_t sum = 0;
    for(int i = 0; i < 10 ; i++)
        sum += _frameCount[i];
    if(sum > 50)
    {
        println("Dropping packet due to 50p/s limit");
        return true;   // drop packet
    }
    else
    {
        _frameCount[_frameCountBase]++;
        //print("sent packages in last 1000ms: ");
        //print(sum);
        //print(" curTime: ");
        //println(curTime);
        return false;
    }
}
#endif
