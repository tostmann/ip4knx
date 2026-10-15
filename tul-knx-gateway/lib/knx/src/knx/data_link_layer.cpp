#include "data_link_layer.h"

#include "bits.h"
#include "platform.h"
#include "device_object.h"
#include "cemi_server.h"
#include "cemi_frame.h"


void DataLinkLayerCallbacks::activity(uint8_t info)
{
    if(_activityCallback)
        _activityCallback(info);
}

void DataLinkLayerCallbacks::setActivityCallback(ActivityCallback activityCallback)
{
    _activityCallback = activityCallback;
}

DataLinkLayer::DataLinkLayer(DeviceObject& devObj, NetworkLayerEntity& netLayerEntity, Platform& platform, BusAccessUnit& busAccessUnit) :
    _deviceObject(devObj), _networkLayerEntity(netLayerEntity), _platform(platform), _bau(busAccessUnit)
{
#ifdef KNX_ACTIVITYCALLBACK
    _netIndex = netLayerEntity.getEntityIndex();
#endif
}

#ifdef USE_CEMI_SERVER

void DataLinkLayer::cemiServer(CemiServer& cemiServer)
{
    _cemiServer = &cemiServer;
}

#ifdef KNX_TUNNELING
void DataLinkLayer::dataRequestToTunnel(CemiFrame& frame)
{
    println("default dataRequestToTunnel");
}

void DataLinkLayer::dataRequestToChannelId(CemiFrame& frame, uint8_t channelId)
{
    println("default dataRequestToChannelId");
}

void DataLinkLayer::dataRequestToAllDevMgmt(CemiFrame& frame)
{
    // a medium without tunnels has no device management connection
}

bool DataLinkLayer::isConfigChannel(uint8_t channelId)
{
    return false; // a medium without tunnels has no device management connection
}

void DataLinkLayer::dataConfirmationToTunnel(CemiFrame& frame)
{
    println("default dataConfirmationToTunnel");
}

void DataLinkLayer::dataIndicationToTunnel(CemiFrame& frame)
{
    println("default dataIndicationToTunnel");
}

bool DataLinkLayer::isTunnelAddress(uint16_t addr)
{
    println("default IsTunnelAddress");
    return false;
}
#endif

void DataLinkLayer::dataRequestFromTunnel(CemiFrame& frame)
{
    // No L_Data.con here: dataConReceived() returns it once the line has answered,
    // with its result. Confirmed up front, a frame was reported delivered before it
    // was on the line and whether or not anybody acknowledged it; an address check
    // through the tunnel found every address occupied. A frame that is handled
    // locally below never reaches the line and is confirmed where it returns.
    // (upstream OpenKNX/knx 74097b8)
    frame.messageCode(L_data_ind);

    // 03_06_03 4.1.5.3.3: the system-broadcast flag applies to open media only, and a
    // cEMI server to a closed medium shall ignore it. The flag is the client's, and
    // frameReceived() below dispatches on it: a broadcast service sent with the flag
    // cleared reached the system-broadcast handler, which serves a different set of
    // services and drops the rest. Normalizing here also keeps the local view in step
    // with the line, where fillTelegramTP() forces the same bit. (upstream 9bde6c7)
    if (mediumType() == DptMedium::KNX_TP1 || mediumType() == DptMedium::KNX_IP)
        frame.systemBroadcast(Broadcast);

    // 03_02_02 2.2.5.1: the extended frame shall not be used where the standard frame
    // is sufficient. A client can ask for either, and sendTelegram() already picks the
    // standard frame for a short APDU on the network-layer path, which a tunnelled
    // frame bypasses. "Sufficient" also requires an empty extended frame format field,
    // which the standard frame cannot carry — asking valid() keeps an LTE frame
    // extended instead of dropping its address type. (upstream 7825b8b)
    if (mediumType() == DptMedium::KNX_TP1 && frame.valid() && frame.npdu().octetCount() <= 15)
        frame.frameType(StandardFrame);

#ifdef KNX_TUNNELING
    // TunnelOpti
    // Optimize performance when receiving unicast data over tunnel wich is not meant to be used on the physical KNX-TP line
    // dont send to KNX-TP when
    // frame is individual adressed  AND
    // destionation == PA of Tunnel-Server  OR
    // destination is a routed PA (= not the TP/secondary line/segment but IP/primary) OR (configurable KNX_TUNNELING_STRICT_TOPOLOGY)
    // destination == PA of a Tunnel (configurable KNX_TUNNELING_NO_TUNNEL_PA_ON_TP)

    // Upstream OpenKNX/knx 83afcad (2025-10-27): the two PA-class suppressions
    // are not KNX-standard compliant — gated them behind opt-in defines.

    bool local = false;
    if(frame.addressType() == AddressType::IndividualAddress)
    {
        local = frame.destinationAddress() == _deviceObject.individualAddress();
#ifdef KNX_TUNNELING_STRICT_TOPOLOGY
        local = local || isRoutedPA(frame.destinationAddress());
#endif
#ifdef KNX_TUNNELING_NO_TUNNEL_PA_ON_TP
        local = local || isTunnelingPA(frame.destinationAddress());
#endif
    }
    // A frame handled here is confirmed before the local stack runs: an answer it
    // produces then reaches the client after the L_Data.con of the request, as from a
    // device on the line. Clients wait for that con before they accept the answer.
    // (upstream 1b01e5e)
    if(local)
    {
        frame.confirm(ConfirmNoError);
        _cemiServer->dataConfirmationToTunnel(frame);
    }
#endif

    // Send to local stack ( => cemiServer for potential other tunnel and network layer for routing)
    frameReceived(frame);

#ifdef KNX_TUNNELING
    if(local)
        return;
#endif

    // Send to KNX medium; the L_Data.con follows from dataConReceived()
    sendFrame(frame);
}
#endif

void DataLinkLayer::dataRequest(AckType ack, AddressType addrType, uint16_t destinationAddr, uint16_t sourceAddr, FrameFormat format, Priority priority, NPDU& npdu, bool doNotRepeat)
{
    // Normal data requests and broadcasts will always be transmitted as (domain) broadcast with domain address for open media (e.g. RF medium) 
    // The domain address "simulates" a closed medium (such as TP) on an open medium (such as RF or PL)
    // See 3.2.5 p.22
    sendTelegram(npdu, ack, destinationAddr, addrType, sourceAddr, format, priority, Broadcast, doNotRepeat);
}

void DataLinkLayer::systemBroadcastRequest(AckType ack, FrameFormat format, Priority priority, NPDU& npdu, uint16_t sourceAddr, bool doNotRepeat)
{
    // System Broadcast requests will always be transmitted as broadcast with KNX serial number for open media (e.g. RF medium) 
    // See 3.2.5 p.22
    sendTelegram(npdu, ack, 0, GroupAddress, sourceAddr, format, priority, SysBroadcast, doNotRepeat);
}

void DataLinkLayer::dataConReceived(CemiFrame& frame, bool success)
{
    MessageCode backupMsgCode = frame.messageCode();
    frame.messageCode(L_data_con);
    frame.confirm(success ? ConfirmNoError : ConfirmError);
    AckType ack = frame.ack();
    AddressType addrType = frame.addressType();
    uint16_t destination = frame.destinationAddress();
    uint16_t source = frame.sourceAddress();
    FrameFormat type = frame.frameType();
    Priority priority = frame.priority();
    NPDU& npdu = frame.npdu();
    SystemBroadcast systemBroadcast = frame.systemBroadcast();

#ifdef USE_CEMI_SERVER
    // A frame an open tunnel sent (the cEMI client, without tunnelling): its
    // L_Data.con, carrying the result set above, goes back to it and not to the
    // local stack. Only the data link layer the cEMI server sends tunnel requests on
    // answers: the 091A runs this on both of its layers for a frame it also routes,
    // and would otherwise confirm twice. (upstream OpenKNX/knx 74097b8)
    // With tunnels, a frame from the cEMI client address (the device's own + 1 from
    // the start) is confirmed to nobody, as before: no tunnel holds that address, and
    // dataConfirmationToTunnel() would hand the con to a device management connection.
#ifdef KNX_TUNNELING
    const bool toClient = _cemiServer->isTunnelAddress(frame.sourceAddress());
#else
    const bool toClient = frame.sourceAddress() == _cemiServer->clientAddress();
#endif
    if (toClient || frame.sourceAddress() == _cemiServer->clientAddress())
    {
        if (toClient && _cemiServer->dataLinkLayer() == this)
            _cemiServer->dataConfirmationToTunnel(frame);
        return;
    }
#endif

    if (addrType == GroupAddress && destination == 0)
            if (systemBroadcast == SysBroadcast)
                _networkLayerEntity.systemBroadcastConfirm(ack, type, priority, source, npdu, success);
            else
                _networkLayerEntity.broadcastConfirm(ack, type, priority, source, npdu, success);
    else
        _networkLayerEntity.dataConfirm(ack, addrType, destination, type, priority, source, npdu, success);

    frame.messageCode(backupMsgCode);
}

void DataLinkLayer::frameReceived(CemiFrame& frame)
{
    AckType ack = frame.ack();
    AddressType addrType = frame.addressType();
    uint16_t destination = frame.destinationAddress();
    uint16_t source = frame.sourceAddress();
    FrameFormat type = frame.frameType();
    Priority priority = frame.priority();
    NPDU& npdu = frame.npdu();
    uint16_t ownAddr = _deviceObject.individualAddress();
    SystemBroadcast systemBroadcast = frame.systemBroadcast();

#ifdef USE_CEMI_SERVER
    // Do not send our own message back to the tunnel
#ifdef KNX_TUNNELING
    //we dont need to check it here
    // send inbound frames to the tunnel if we are the secondary (TP) interface
    if( _networkLayerEntity.getEntityIndex() == 1)
        _cemiServer->dataIndicationToTunnel(frame);
#else
    if (frame.sourceAddress() != _cemiServer->clientAddress())
    {
        _cemiServer->dataIndicationToTunnel(frame);
    }
#endif
#endif

    // print("Frame received destination: ");
    // print(destination, 16);
    // println();
    // print("frameReceived: frame valid? :");
    // println(npdu.frame().valid() ? "true" : "false");
    if (source == ownAddr)
        _deviceObject.individualAddressDuplication(true);

    if (addrType == GroupAddress && destination == 0)
    {
        if (systemBroadcast == SysBroadcast)
            _networkLayerEntity.systemBroadcastIndication(ack, type, npdu, priority, source);
        else 
            _networkLayerEntity.broadcastIndication(ack, type, npdu, priority, source);
    }
    else
    {
        _networkLayerEntity.dataIndication(ack, addrType, destination, type, npdu, priority, source);
    }
}

bool DataLinkLayer::sendTelegram(NPDU & npdu, AckType ack, uint16_t destinationAddr, AddressType addrType, uint16_t sourceAddr, FrameFormat format, Priority priority, SystemBroadcast systemBroadcast, bool doNotRepeat)
{
    CemiFrame& frame = npdu.frame();
    // print("Send telegram frame valid ?: ");
    // println(frame.valid()?"true":"false");

    // Before touching any field: an oversized frame's _data/_ctrl1 members may have
    // been overwritten by the builder, and the writers below would dereference them.
    // The valid() check further down cannot move up here — it also checks ctrl1 and
    // the frame type, which the lines below are what set. (upstream e5b2903)
    if (frame.oversized())
    {
        println("oversized frame dropped");
        return false;
    }

    frame.messageCode(L_data_ind);
    frame.destinationAddress(destinationAddr);
    frame.sourceAddress(sourceAddr);
    frame.addressType(addrType);
    frame.priority(priority);
    
    if(mediumType() == DptMedium::KNX_TP1)
        frame.repetition(doNotRepeat?NoRepitiion:RepetitionAllowed);
    else
        frame.repetition(RepetitionAllowed);

    frame.systemBroadcast(systemBroadcast);

    if (npdu.octetCount() <= 15)
        frame.frameType(StandardFrame);
    else
        frame.frameType(format);


    if (!frame.valid())
    {
        println("invalid frame");
        return false;
    }

//    if (frame.npdu().octetCount() > 0)
//    {
//        _print("<- DLL ");
//        frame.apdu().printPDU();
//    }

    bool sendTheFrame = true;
    bool success = true;

#ifdef KNX_TUNNELING
    // TunnelOpti
    // Optimize performance when sending unicast data over tunnel wich is not meant to be used on the physical KNX-TP line
    // dont send to KNX-TP when
    // this interface is the secondary interface (e.g. KNX-TP) AND
    // destination == PA of a Tunnel (configurable KNX_TUNNELING_NO_TUNNEL_PA_ON_TP)
#ifdef KNX_TUNNELING_NO_TUNNEL_PA_ON_TP
    if(_networkLayerEntity.getEntityIndex() == 1 && addrType == AddressType::IndividualAddress)    // don't send to tp if we are the secondary (TP) interface AND the destination is a tunnel-PA
    {
        if(isTunnelingPA(destinationAddr))
            sendTheFrame = false;
    }
#endif
#endif

    // The data link layer might be an open media link layer
    // and will setup rfSerialOrDoA, rfInfo and rfLfn that we also 
    // have to send through the cEMI server tunnel
    // Thus, reuse the modified cEMI frame as "frame" is only passed by reference here!
    if(sendTheFrame)
        success = sendFrame(frame);

#ifdef USE_CEMI_SERVER
    CemiFrame tmpFrame(frame.data(), frame.totalLenght());
    // We can just copy the pointer for rfSerialOrDoA as sendFrame() sets
    // a pointer to const uint8_t data in either device object (serial) or
    // RF medium object (domain address)

#ifdef USE_RF
    tmpFrame.rfSerialOrDoA(frame.rfSerialOrDoA()); 
    tmpFrame.rfInfo(frame.rfInfo());
    tmpFrame.rfLfn(frame.rfLfn());
#endif
    tmpFrame.confirm(ConfirmNoError);

    if(_networkLayerEntity.getEntityIndex() == 1)    // only send to tunnel if we are the secondary (TP) interface
        _cemiServer->dataIndicationToTunnel(tmpFrame);
#endif

    return success;
}

uint8_t* DataLinkLayer::frameData(CemiFrame& frame)
{
    return frame._data;
}

#ifdef KNX_TUNNELING
bool DataLinkLayer::isTunnelingPA(uint16_t pa)
{
    uint8_t num = KNX_TUNNELING;
    uint32_t len = 0;
    uint8_t* data = nullptr;
    _bau.propertyValueRead(OT_IP_PARAMETER, 0, PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, num, 1, &data, len);
    //printHex("isTunnelingPA, PID_ADDITIONAL_INDIVIDUAL_ADDRESSES: ", data, len);
    if(num != KNX_TUNNELING)
    {
        println("Tunnel PAs unkwnown");
        if(data != nullptr)
            delete[] data;
        return false;
    }
    for(uint8_t i = 0; i < KNX_TUNNELING; i++)
    {
        uint16_t tunnelpa;
        popWord(tunnelpa, (data)+i*2);
        if(pa == tunnelpa)
        {
            if(data != nullptr)
                delete[] data;
            return true;
        }   
    }
    if(data != nullptr)
        delete[] data;
    return false;
}

bool DataLinkLayer::isRoutedPA(uint16_t pa)
{
    uint16_t ownpa = _deviceObject.individualAddress();
    uint16_t own_sm;

    if ((ownpa & 0x0F00) == 0x0)
        own_sm = 0xF000;
    else
        own_sm = 0xFF00;

    return (pa & own_sm) != ownpa;
}
#endif

