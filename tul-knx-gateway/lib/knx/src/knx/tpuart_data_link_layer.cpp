#include "config.h"
#ifdef USE_TP
#pragma GCC optimize("O3")

#include "address_table_object.h"
#include "bits.h"
#include "cemi_frame.h"
#include "device_object.h"
#include "platform.h"
#include "tpuart_data_link_layer.h"
#include <stdio.h>

void TpUartDataLinkLayer::setRepetitions(uint8_t nack, uint8_t busy)
{
    _tpuart.setRepetitions(nack, busy);
}

bool TpUartDataLinkLayer::sendFrame(CemiFrame &cemiFrame)
{
    uint8_t *tpData = (uint8_t *)malloc(cemiFrame.telegramLengthtTP());
    if (!tpData) // bail on alloc failure instead of null-deref in fillTelegramTP() under heap pressure
    {
        dataConReceived(cemiFrame, false);
        return false;
    }
    cemiFrame.fillTelegramTP(tpData);

    TPUart::Frame *tpFrame = new TPUart::Frame((char *)tpData, cemiFrame.telegramLengthtTP());
    // Two allocations can fail here: the frame object and the octet buffer it copies
    // into. An empty frame reports data() == nullptr -> bail instead of handing a
    // null pointer to the transmit queue.
    if (!tpFrame || tpFrame->data() == nullptr)
    {
        free(tpData);
        delete tpFrame; // no-op on nullptr; the queue never took ownership
        dataConReceived(cemiFrame, false);
        return false;
    }

    // when the bus is not usable or in monitoring mode, discard the frame -
    // silently. A frame taken while the chip was already silent waited for the next
    // reset to be confirmed negatively, 30 s or more later.
    if (!_tpuart.busOperational() || _tpuart.isMonitoring())
    {
        free(tpData);
        delete tpFrame; // not queued -> free (queue didn't take ownership)
        dataConReceived(cemiFrame, false);
        return false;
    }

    // when frame not enqueued, discard the frame with a message
    if (!_tpuart.pushTransmitQueue(tpFrame))
    {
        free(tpData);
        delete tpFrame; // queue full, not taken -> free (else leak per dropped frame)
        // At most one line a second: under a flood every refused frame logged one.
        static uint32_t queueFullLoggedAt = 0;
        static uint32_t queueFullSuppressed = 0;
        if (millis() - queueFullLoggedAt >= 1000)
        {
            char msg[80];
            snprintf(msg, sizeof msg, "Ignore frame because transmit queue is full! (+%lu more not shown)",
                     (unsigned long)queueFullSuppressed);
            printMessage(queueFullSuppressed ? msg : "Ignore frame because transmit queue is full!", true);
            queueFullLoggedAt = millis();
            queueFullSuppressed = 0;
        }
        else
            queueFullSuppressed++;
        dataConReceived(cemiFrame, false);
        return false;
    }

    free(tpData);
    // success: queue took ownership of tpFrame -> it deletes it after TX (no delete here)
    // printHex("  CEMI>: ", cemiFrame.data(), cemiFrame.dataLength());
    return true;
}

void TpUartDataLinkLayer::connected(bool state /* = true */)
{
    if (state)
        println("TP is connected");
    else
        println("TP is disconnected");

    _connected = state;
}

void TpUartDataLinkLayer::reset()
{
    _tpuart.reset();
}

void TpUartDataLinkLayer::stop(bool state)
{
    if (!_initialized)
        return;

    _tpuart.stopMode(state);
}

void TpUartDataLinkLayer::requestBusy(bool state)
{
    _tpuart.busyMode(state);
}

void TpUartDataLinkLayer::monitor()
{
#if MASK_VERSION == 0x091A
    // A device advertising the ROUTING service family must not offer a bus monitor
    // (03_08_04 2.2.4), and in this stack the ROUTING DIB goes out under mask 091A.
    // Refusing here rather than at the callers keeps a future one from reopening the
    // path — the monitor takes the transceiver away from the coupler.
    return;
#else
    if (!_initialized)
        return;

    _tpuart.startMonitoring();
#endif
}

void TpUartDataLinkLayer::initialize()
{
    if (_initialized)
        return;

    // After an unusual device restart, perform a reset, as the TPUart may still be in an incorrect state.
    if (!_initialized && _platform.interface() != nullptr)
    {
        _tpuart.registerReceivedFrame(std::bind(&TpUartDataLinkLayer::processRxFrame, this, std::placeholders::_1));
        _tpuart.registerMessage(std::bind(&TpUartDataLinkLayer::printMessage, this, std::placeholders::_1, std::placeholders::_2));
        _tpuart.registerCheckAcknowledge(std::bind(&TpUartDataLinkLayer::checkAcknowledge, this, std::placeholders::_1, std::placeholders::_2));
        _tpuart.registerDroppedFrame(std::bind(&TpUartDataLinkLayer::droppedFrame, this, std::placeholders::_1));
#ifdef NCN5120
        _tpuart.begin(TPUart::BcuType::BCU_NCN5120, _platform.interface());
#else
        _tpuart.begin(TPUart::BcuType::BCU_TPUART2, _platform.interface());
#endif
        _initialized = true;
    }
}

void TpUartDataLinkLayer::enabled(bool value)
{
    initialize();

    stop(!value);
}

bool TpUartDataLinkLayer::enabled() const
{
    return _initialized && _tpuart.isConnected();
}

void TpUartDataLinkLayer::loop()
{
    if (!_initialized)
        return;

    const uint16_t individualAddress = _deviceObject.individualAddress();
    if (individualAddress > 0 && _individualAddress != individualAddress)
    {
        _individualAddress = individualAddress;
        _tpuart.setOwnAddress(_individualAddress);
    }

    _tpuart.process();
}

DptMedium TpUartDataLinkLayer::mediumType() const
{
    return DptMedium::KNX_TP1;
}

bool TpUartDataLinkLayer::powerControl(bool state)
{
    // Returns what the layer below reports: false means the line was busy and
    // nothing was written. The old void signature swallowed exactly that, so a
    // refused write was indistinguishable from a successful one.
    return _tpuart.powerControl(state);
}

TpUartDataLinkLayer::TpUartDataLinkLayer(DeviceObject &devObj,
                                         NetworkLayerEntity &netLayerEntity,
                                         Platform &platform,
                                         BusAccessUnit &busAccessUnit,
                                         ITpUartCallBacks &cb,
                                         DataLinkLayerCallbacks *dllcb)
    : DataLinkLayer(devObj, netLayerEntity, platform, busAccessUnit),
      _cb(cb),
      _dllcb(dllcb)
{
}

bool TpUartDataLinkLayer::isConnected()
{
    return _tpuart.isConnected();
}

bool TpUartDataLinkLayer::busOperational()
{
    return _initialized && _tpuart.busOperational();
}

bool TpUartDataLinkLayer::isStopped()
{
    return false;
}

bool TpUartDataLinkLayer::isBusy()
{
    return false;
}

bool TpUartDataLinkLayer::isMonitoring()
{
    return _tpuart.isMonitoring();
}

TPUart::AcknowledgeType TpUartDataLinkLayer::checkAcknowledge(unsigned short destination, bool isGroupAddress)
{
    TPAckType ack = _cb.isAckRequired(destination, isGroupAddress);

    if (ack == TPAckType::AckReqAck)
        return TPUart::AcknowledgeType::ACK_Addressed;

    if (ack == TPAckType::AckReqBusy)
        return TPUart::AcknowledgeType::ACK_Busy;

    if (ack == TPAckType::AckReqNack)
        return TPUart::AcknowledgeType::ACK_Nack;

    return TPUart::AcknowledgeType::ACK_None;
}

void TpUartDataLinkLayer::processRxFrame(TPUart::Frame &tpFrame)
{
    if (isMonitoring())
    {
        printMessage(tpFrame.printFrame().c_str(), false);
    }
    
#if MASK_VERSION != 0x091A
    if (tpFrame.isFiltered())
        return;
#endif

    uint8_t *cemiData = (uint8_t *)tpFrame.cemiData();
    if (cemiData == nullptr) // allocation failed under heap pressure -> drop, do not build a frame on null
        return;

    CemiFrame cemiFrame(cemiData, tpFrame.cemiSize());

    if (tpFrame.isTransmitted()) {
        // One L_Data.con per frame: the transceiver repeats an unacknowledged frame
        // up to three times and answers only the last attempt with its L_DATA_CON
        // (NCN5130 DS p.36). The echoes of the earlier attempts carry no result;
        // confirming each one told a tunnel client "failed" four times for one
        // request.
        if (tpFrame.isDataCon())
        {
            // The device does not acknowledge what it sends itself. An individual
            // address it acknowledges for every other sender on the line -- one it
            // routes, a tunnel's, a defended tunnel address -- has reached it, so it
            // counts as delivered: otherwise a tunnel client is told "failed" for a
            // frame the device took, and an address check through the tunnel reads
            // the device's own tunnel addresses as free. Groups keep the line's answer.
            bool success = tpFrame.isAck();
            if (!success && !tpFrame.isGroupAddress())
                success = _cb.isAckRequired(tpFrame.destination(), false) == TPAckType::AckReqAck;
            // The con mirrors the request (03_06_03 4.1.5.3.4). The echo of a repeated
            // attempt has the repeat flag cleared, and clients such as FHEM's KNXIO
            // drop a confirmation that does not read as the frame they sent.
            cemiFrame.repetition(WasNotRepeated);
            dataConReceived(cemiFrame, success);
        }
        free(cemiData); // Frame::cemiData() returns a malloc()'d buffer -> must be free()'d, not delete'd
        return;
    }

    // printHex("  TP<: ", (const uint8_t *)tpFrame.data(), tpFrame.size());
    // printHex("  CEMI<: ", cemiFrame.data(), cemiFrame.dataLength());

#ifdef KNX_ACTIVITYCALLBACK
    if (_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_RECV << KNX_ACTIVITYCALLBACK_DIR));
#endif

    frameReceived(cemiFrame);
    free(cemiData); // Frame::cemiData() returns a malloc()'d buffer -> must be free()'d, not delete'd
}

// A frame a transceiver reset threw away before its L_DATA_CON: confirm it
// negatively, as sendFrame() does for a frame it cannot queue, rather than leave a
// tunnel client or the local stack to wait out its own timeout. Main loop, no
// TPUart lock held. (upstream OpenKNX/knx 7a6f545)
void TpUartDataLinkLayer::droppedFrame(TPUart::Frame& tpFrame)
{
    uint8_t *cemiData = (uint8_t *)tpFrame.cemiData();
    if (cemiData == nullptr) // allocation failed under heap pressure -> nothing to confirm with
        return;

    CemiFrame cemiFrame(cemiData, tpFrame.cemiSize());
    cemiFrame.repetition(WasNotRepeated);
    dataConReceived(cemiFrame, false);
    free(cemiData); // Frame::cemiData() returns a malloc()'d buffer -> must be free()'d, not delete'd
}

void TpUartDataLinkLayer::printMessage(const char *message, bool error)
{
    if (error)
        print("\e[0;31m");

    print(message);

    if (error)
        print("\e[0m");

    println("");
}

#endif