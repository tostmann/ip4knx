#pragma GCC optimize("O3")
#include "TPUart/Transmitter.h"
#include "TPUart/DataLinkLayer.h"

namespace TPUart
{
    const size_t MAX_QUEUE_SIZE = 50;

    /**
     * @brief Constructs a new Transmitter object.
     *
     * @param dll Reference to a DataLinkLayer object.
     *
     * This constructor initializes the Transmitter object with the provided
     * DataLinkLayer reference. It also sets the initial values for
     * _pendingAcknowledge, _awaitResponse, and _time to 0.
     */
    Transmitter::Transmitter(DataLinkLayer &dll) : _dll(dll)
    {
        _pendingAcknowledge = 0;
        _state = TX_IDLE;
        _transmitPos = 0;
        _transmitOffset = 0;
        _time = 0;
        _maxQueueSize = MAX_QUEUE_SIZE;
    }

    /**
     * @brief Destructor for the Transmitter class.
     *
     * Frees every frame the transmitter still holds: reset() moves the unconfirmed
     * ones to _dropped, and with nobody left to confirm them they are deleted here.
     */
    Transmitter::~Transmitter()
    {
        reset();
        while (!_dropped.empty())
        {
            delete _dropped.front();
            _dropped.pop();
        }
    }

    /**
     * @brief Finalizes the transmitter by setting the _awaitResponse flag to false.
     *
     * This function is used to indicate that the transmitter should no longer
     * await a response. It is typically called when the transmission process
     * is complete.
     */
    void Transmitter::finalize()
    {
        if (_state == TX_AWAIT) _state = TX_IDLE;
    }

    /**
     * @brief Processes the transmission queue.
     *
     * This function checks several conditions before processing the transmission queue:
     * - If a response is awaited, the function returns immediately.
     * - If the queue is empty, the function returns immediately.
     * - If the receiver is in an invalid state, the function returns immediately.
     *
     * If a frame is currently being processed, it is deleted. The next frame in the queue is then
     * retrieved and the transmission statistics are updated.
     *
     * If the chip type is TPUart2 and the frame size exceeds 64 bytes, the function returns immediately.
     *
     * Finally, the frame is transmitted.
     */
    void Transmitter::processQueue()
    {
        if (_state != TX_IDLE) return;
        if (_dll._resetPending) return; // transceiver restarting after our reset
        if (_dll._receiver._invalid) return;
        if (_queue.empty()) return;

        if (_frame != nullptr)
        {
            delete _frame;
            _frame = nullptr;
            _transmitPos = 0;
            _transmitOffset = 0;
        }

        _frame = _queue.front();
        _queue.pop();
#ifdef TPUART_DROP_TEST_MS
        if (_dll._dropTestOwnResetAt) _dll._dropTestArmedWhilePending++;
#endif
        _dll._statistics.incrementTxFrames();
        _dll._statistics.incrementTxFrameBytes(_frame->size());

        // Fallback if the frame is too big - Filtered on DLL, too
        if (_dll._bcuType == BCU_TPUART2 && _frame->size() > 64)
        {
            delete _frame;
            _frame = nullptr;
            return;
        }

        asm volatile("" ::: "memory");
        _state = TX_TRANSMIT;
    }

    /**
     * @brief Processes the expiration of a waiting response.
     *
     * This function checks if the transmitter is awaiting a response and if the
     * waiting time has exceeded a specified timeout (60 seconds). If the response
     * wait time has expired, reset the BCU.
     */
    void Transmitter::processWatchdog()
    {
        if (_state != TX_AWAIT) return;
        // _last could be updated in parallel, so it must be temporarily stored
        const uint32_t time = _time;
        if (millis() - time < 60000) return;

        _dll.printError("Watchdog: Transmitter did not get confirm.");
        _watchdogResets++;
        _dll.reset();
    }

    /*
     * Each frame must be initiated with a U_L_DATA_START_REQ and each subsequent byte with another position byte (6 bits).
     * Since the position byte consists of the U_L_DATA_START_REQ + position and we start with 0 anyway, no further
     * distinction is necessary.
     *
     * However, the last byte (checksum) uses the U_L_DATA_END_REQ + position!
     * Additionally, there is another peculiarity with extended frames that can be up to 263 bytes long, where 6 bits are no longer sufficient.
     * Here, a U_L_DATA_OFFSET_REQ + position (3 bits) must be prefixed. Thus, 9 bits are available for the position.
     */

    void Transmitter::processTransmitByte()
    {
        flushAcknowledge();

        if (_state != TX_TRANSMIT) return;
        // if (!_awaitResponse) return;
        if (!_dll.txLock()) return;
        // Double check
        if (_state != TX_TRANSMIT)
        {
            _dll.txUnlock(); // returning with it held stopped every acknowledge and transmission for good
            return;
        }

        const unsigned short size = _frame->size();
        // if (_transmitPos >= size)
        // {
        //     _dll.txUnlock();
        //     return;
        // }

        // _dll.printMessage("Transmitting %u of %u", _transmitPos, size);
        const bool last = _transmitPos == (size - 1);
        const unsigned char offset = (_transmitPos >> 6);
        const unsigned char position = (_transmitPos & 0x3F);

        // The BCU keeps the offset until it is changed, so resend it only when it changes.
        if (offset != _transmitOffset)
        {
            _dll._interface->write(U_L_DATA_OFFSET_REQ | offset);
            _transmitOffset = offset;
        }

        if (last) // Last byte (Checksum) - the transmit
        {
            _dll._interface->write(U_L_DATA_END_REQ | position);
        }
        else
        {
            _dll._interface->write(U_L_DATA_START_REQ | position);
        }

        _dll._interface->write(_frame->data(_transmitPos));
        if (last)
        {
            resetWatchdogTimer();
            _state = TX_AWAIT;
        }

        _transmitPos++;
        _dll.txUnlock();

        flushAcknowledge();
    }

    /**
     * @brief Adds a frame to the transmission queue.
     *
     * This function attempts to add a frame to the transmission queue. If the queue
     * has reached its maximum size, the frame will not be added and the function
     * will return false.
     *
     * @param frame Pointer to the Frame object to be added to the queue.
     * @return true if the frame was successfully added to the queue, false if the queue is full.
     */
    bool Transmitter::pushQueue(Frame *frame)
    {
        if (_queue.size() >= _maxQueueSize) return false;

        _queue.push(frame);
        if (_queue.size() > _queuePeak) _queuePeak = _queue.size();
        return true;
    }

    /**
     * @brief Checks if the transmitter is awaiting a response.
     *
     * This function returns the status of the transmitter's response waiting state.
     *
     * @return true if the transmitter is awaiting a response, false otherwise.
     */
    bool Transmitter::awaitResponse()
    {
        return _state == TX_AWAIT;
    }

    /**
     * @brief Returns the current size of the queue.
     *
     * This function retrieves the number of elements currently stored in the queue.
     *
     * @return size_t The number of elements in the queue.
     */
    size_t Transmitter::queueSize()
    {
        return _queue.size();
    }

    /**
     * @brief Resets the Transmitter by clearing the message queue and resetting the state.
     *
     * Unconfirmed frames - the one in flight and all queued - move to _dropped for
     * DataLinkLayer::reportDroppedFrames(); an already confirmed current frame is
     * deleted. The state returns to TX_IDLE.
     */
    void Transmitter::reset()
    {
        _dll.txLock(true);
        // A frame in TX_TRANSMIT or TX_AWAIT never got its L_DATA_CON, nor did any
        // still queued: keep them for DataLinkLayer::reportDroppedFrames(), which
        // confirms them negatively once no lock is held, instead of leaving their
        // sender to wait out its own timeout. A frame back in TX_IDLE was confirmed
        // already and is not reported a second time. (upstream f1c1237)
        if (_frame != nullptr)
        {
            if (_state == TX_TRANSMIT || _state == TX_AWAIT)
                _dropped.push(_frame);
            else
                delete _frame;
            _frame = nullptr;
            _transmitPos = 0;
            _transmitOffset = 0;
        }
        while (!_queue.empty())
        {
            _dropped.push(_queue.front());
            _queue.pop();
        }

        _state = TX_IDLE;
        resetWatchdogTimer();
        _dll.txUnlock();
    }

    /**
     * @brief Sends an acknowledge, or parks it when txLock is taken.
     *
     * Runs in the receive task when a frame's destination is decoded. A parked
     * acknowledge is written by flushAcknowledge() while its bus window lasts and
     * dropped after it: before, it waited for the next transmitted octet and could
     * be applied to whatever frame the chip received then. (upstream 61b09e5)
     *
     * @param acknowledge The type of acknowledge message to send.
     */
    void Transmitter::sendAcknowledge(AcknowledgeType acknowledge)
    {
        if (_dll.txLock())
        {
            _dll._interface->write(U_ACK_REQ | acknowledge);
            _dll.txUnlock();
        }
        else
        {
            if (_pendingAcknowledge & 0xFF) _ackParkReplaced = _ackParkReplaced + 1;
            _pendingAcknowledge = (((uint32_t)(micros() >> 6) & 0x00FFFFFFu) << 8) | (uint8_t)(U_ACK_REQ | acknowledge);
            _ackParked = _ackParked + 1;
        }
    }

    /*
     * Writes a parked acknowledge, or drops it once its bus window has passed. The
     * acknowledge starts 15 bit times after the frame; for the shortest standard
     * frame that leaves 41 bit times at 9600 baud after the decision at octet 6,
     * 3698 us less the character that brought it (03_02_02 2.2.7). The NCN data
     * sheet gives no deadline of its own for U_Ackn.req. Called from the main loop:
     * every process() pass, before and after each transmitted octet, after a state
     * probe. (upstream 61b09e5, 9a54040)
     */
    void Transmitter::flushAcknowledge()
    {
        const uint32_t parked = _pendingAcknowledge; // one read: octet and time agree
        const uint8_t pending = parked & 0xFF;
        if (!pending) return;

        // 24 bits of 64 us ticks cover 17 min, so a long stall cannot alias an old
        // park back into the window; the subtraction survives the micros() wrap.
        const uint32_t age = ((((uint32_t)(micros() >> 6)) & 0x00FFFFFFu) - (parked >> 8)) & 0x00FFFFFFu;
        if (age >= (TPUART_ACK_WINDOW_US >> 6) || _dll.isMonitoring())
        {
            if (_pendingAcknowledge == parked)
            {
                _pendingAcknowledge = 0;
                _ackParkDropped = _ackParkDropped + 1;
            }
            return;
        }

        if (_dll.txLock())
        {
            _dll._interface->write(pending);
            if (_pendingAcknowledge == parked) _pendingAcknowledge = 0; // a newer park waits for the next call
            _dll.txUnlock();

            const uint32_t ageUs = age << 6;
            if (ageUs > _ackParkAgeMaxUs) _ackParkAgeMaxUs = ageUs;
            _ackParkWritten = _ackParkWritten + 1;
        }
    }

    /**
     * @brief Sets the maximum size of the queue.
     *
     * This function sets the maximum number of elements that the queue can hold.
     *
     * @param size The maximum number of elements for the queue.
     */
    void Transmitter::setQueueSize(unsigned long size)
    {
        _maxQueueSize = size;
    }

    Frame *Transmitter::currentFrame()
    {
        return _frame;
    }

    bool Transmitter::isTransmitting()
    {
        return _state == TX_TRANSMIT;
    }

    void Transmitter::resetWatchdogTimer()
    {
        _time = millis();
    }

} // namespace TPUart
