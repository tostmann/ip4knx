#pragma GCC optimize("O3")
#include "TPUart/DataLinkLayer.h"
#include <stdarg.h>

namespace TPUart
{
    SystemState &DataLinkLayer::getSystemState()
    {
        return _systemState;
    }

    void DataLinkLayer::begin(BcuType bcuType, Interface::Abstract *interface)
    {
        _bcuType = bcuType;
        _interface = interface;
        if (_interface->hasCallback()) _interface->registerCallback(std::bind(&DataLinkLayer::processReceviedByte, this));
        _initialized = true;
        tryInitialize();
    }

    void DataLinkLayer::tryInitialize()
    {
        if (_bcuState != BCU_UNINITIALIZED) return;
        _lastTryInitialize = millis();
        _initAttempts++;

        uint baudrates[2] = {19200, 38400};
        for (uint baudrate : baudrates)
        {
            if (_bcuType == BCU_TPUART2 && baudrate != 19200) continue;

            if (tryInitialize(baudrate))
            {
                _baudrate = baudrate;
                setBCUState(BCU_CONNECTED, baudrate);
                _uReset = true;
                _bcuState = BCU_CONNECTED;
                // Start the log budget over, so a later loss of the transceiver
                // is reported in full instead of inheriting the quiet mode of a
                // long stretch without a bus.
                _initAttempts = 0;
                return;
            }
        }
    }

    bool DataLinkLayer::tryInitialize(uint baudrate)
    {
        // The handshake now retries forever (see process()), so logging every
        // attempt would bury the rest of the boot output at two lines per 2 s.
        // Keep the first attempts verbose — that is what a user debugging a
        // fresh stick sees — then drop to one report per minute.
        if (_initAttempts <= 3 || (_initAttempts % 30) == 0)
            printMessage("Try Initialize %d", baudrate);

        _interface->end(); // End interface is already initialized
        _interface->begin(baudrate);
        _lastHostRequestAt = millis();
        _interface->write(U_RESET_REQ);
        unsigned long start = millis();
        do
        {
#ifdef ARDUINO_ARCH_ESP32
            vTaskDelay(pdMS_TO_TICKS(1));
#endif
            int value = _interface->read();
            if (value == -1) continue; // Queu is empty
            if (value == 0) continue;  // TPUart send zeros at the beginning
            // Serial.printf("%02X", value);

            // War direkt erfolgreich - Top
            if (value == U_RESET_IND) return true;
            break;
        }
        while (!((millis() - start) >= 50));
        return false;
    }

    void DataLinkLayer::reset()
    {
        // Disarm any pending register read: after a reset the next byte from the
        // device is U_Reset.ind, and claiming that as a register value would
        // skip receivedReset() and with it the reconfiguration that follows.
        _regReadPending = false;
        if (_bcuState == BCU_UNINITIALIZED) return;
        printMessage("Reset BCU");

        rxLock(true);
        _transmitter.reset();
        _uState = 0;
        _uReset = false;
        _receiver.reset();
        _statistics.reset();
        _rxFrameBuffer.clear();
        _rxFrameBufferEntries = 0;
        _repetitionFilter.clear();
        _interface->flush();
        // Use setBCUState so _lastReceivedTime is refreshed; otherwise the
        // 30s disconnect-watchdog will fire again immediately even though
        // we just re-armed the link.
        setBCUState(BCU_CONNECTED);

        // Stamped: this reset request is answered by U_Reset.ind, and the state
        // flips to CONNECTED one line above — so a parked register read could
        // otherwise arm on this very pass and swallow it, skipping
        // receivedReset() and the reconfiguration that follows.
        _lastHostRequestAt = millis();
        _interface->write(U_RESET_REQ);
        _modeExtendedCRC = false;
        _lastDiscardedBytes = 0;
        _receiver._invalid = false;
        rxUnlock();
    }

    const char *DataLinkLayer::getBcuStateInfo()
    {
        switch (_bcuState)
        {
            case BCU_DISCONNECTED:
                return "Disconnected";
            case BCU_CONNECTED:
                return "Connected";
            case BCU_BUSMONITOR:
                return "Busmonitor";
            default:
                return "Uninitialized";
        }
    }

    void DataLinkLayer::processRxFrameBuffer()
    {
        if (!_rxFrameBufferEntries) return;

        char run = 0;
        while (_rxFrameBufferEntries && (TPUART_MAX_RXQUEUE_TIME_PER_LOOP == 0 || run < TPUART_MAX_RXQUEUE_TIME_PER_LOOP))
        {
            rxLock(true);
            // Reassemble the length prefix written by pushRxFrameBuffer() as
            // (frameSize + 3). char is SIGNED on RISC-V (no -funsigned-char), so
            // a low byte >= 0x80 (any frame >= ~125 bytes, e.g. an ETS memory
            // write) would sign-extend and inflate bufferSize to ~0xFFxx. Cast
            // each byte through uint8_t and read them in a fixed order (the two
            // pop() calls had unspecified evaluation order before C++17).
            const uint8_t lenLo = (uint8_t)_rxFrameBuffer.pop();
            const uint8_t lenHi = (uint8_t)_rxFrameBuffer.pop();
            const uint16_t bufferSize = (uint16_t)lenLo | ((uint16_t)lenHi << 8);

            // A valid KNX TP frame is <= TPUART_MAX_KNX_FRAME (bufferSize <= +3).
            // Anything outside [3, max+3] means the stream is desynced/corrupt:
            // we can no longer trust our read position, so drain to resync
            // instead of sizing a VLA off a bogus length (stack smash).
            if (bufferSize < 3 || bufferSize > (TPUART_MAX_KNX_FRAME + 3))
            {
                _rxFrameBuffer.clear();
                _rxFrameBufferEntries = 0;
                asm volatile("" ::: "memory");
                rxUnlock();
                _statistics.incrementRxFrameBufferOverflow();
                break;
            }
            const uint16_t frameSize = bufferSize - 3;

            char frameData[TPUART_MAX_KNX_FRAME] = {};

            for (size_t i = 0; i < frameSize; i++)
                frameData[i] = _rxFrameBuffer.pop();

            Frame frame(frameData, frameSize);

            frame.addFlags(_rxFrameBuffer.pop());
            asm volatile("" ::: "memory");
            _rxFrameBufferEntries = _rxFrameBufferEntries - 1;
            rxUnlock();

            run++;

            bool alreadFound;
            alreadFound = _repetitionFilter.check(frame);
            if (frame.isRepeated() && alreadFound)
            {
                frame.setFiltered();
                _statistics.incrementRxRepetitions();
            }

            // Complete TX Frame
            for (std::function<void(Frame &)> &callback : _callbacksReceivedFrame)
                callback(frame);
        }
    }

    void DataLinkLayer::pushRxFrameBuffer(Frame &frame)
    {
        const unsigned short frameSize = frame.size();
        const unsigned short bufferSize = frameSize + 3;
        if (_rxFrameBuffer.available() <= bufferSize)
        {
            _rxFrameBufferOverflow = true;
            _statistics.incrementRxFrameBufferOverflow();
            return;
        }

        // Size (2 byte)
        _rxFrameBuffer.push(bufferSize & 0xFF);
        _rxFrameBuffer.push(bufferSize >> 8);

        // Frame
        for (size_t i = 0; i < frameSize; i++)
            _rxFrameBuffer.push(frame.data(i));

        // Flags (1 byte)
        _rxFrameBuffer.push(frame.flags());
        asm volatile("" ::: "memory");
        _rxFrameBufferEntries = _rxFrameBufferEntries + 1;
    }

    DataLinkLayer::DataLinkLayer() : _transmitter(*this), _receiver(*this)
    {
#if defined(ARDUINO_ARCH_RP2040)
        mutex_init(&_rxLock);
        mutex_init(&_txLock);
#elif defined(ARDUINO_ARCH_ESP32)
        _rxLock = xSemaphoreCreateMutex();
        _txLock = xSemaphoreCreateMutex();
#endif
    }

    bool DataLinkLayer::rxLock(bool blocking /* = false */)
    {
#if defined(ARDUINO_ARCH_RP2040)
        if (blocking)
        {
            mutex_enter_blocking(&_rxLock);
            return true;
        }

        uint32_t owner;
        return mutex_try_enter(&_rxLock, &owner);
#elif defined(ARDUINO_ARCH_ESP32)
        TickType_t wait = blocking ? 0xFFFFFFFF : 0;
        return xSemaphoreTake(_rxLock, wait) == pdTRUE;
#else
        return true;
#endif
    }

    void DataLinkLayer::rxUnlock()
    {

#if defined(ARDUINO_ARCH_RP2040)
        mutex_exit(&_rxLock);
#elif defined(ARDUINO_ARCH_ESP32)
        xSemaphoreGive(_rxLock);
#endif
    }

    bool DataLinkLayer::txLock(bool blocking /* = false */)
    {
#if defined(ARDUINO_ARCH_RP2040)
        if (blocking)
        {
            mutex_enter_blocking(&_txLock);
            return true;
        }

        uint32_t owner;
        return mutex_try_enter(&_txLock, &owner);
#elif defined(ARDUINO_ARCH_ESP32)
        TickType_t wait = blocking ? 0xFFFFFFFF : 0;
        return xSemaphoreTake(_txLock, wait) == pdTRUE;
#else
        return true;
#endif
    }

    void DataLinkLayer::txUnlock()
    {
#if defined(ARDUINO_ARCH_RP2040)
        mutex_exit(&_txLock);
#elif defined(ARDUINO_ARCH_ESP32)
        xSemaphoreGive(_txLock);
#endif
    }

    void DataLinkLayer::end(bool deleteUart)
    {
        _initialized = false;
        _bcuState = BCU_UNINITIALIZED;
        if (deleteUart && _interface == nullptr)
        {
            delete _interface;
        }
    }

    /*
     * Callback for the interface to process the received byte.
     * Returns true if there are more bytes to process.
     */
    bool DataLinkLayer::processReceviedByte()
    {
        if (_bcuState == BCU_UNINITIALIZED) return false;

        return _receiver.processReceviedByte();
    }

    void DataLinkLayer::processTransmitByte()
    {
        if (_bcuState == BCU_UNINITIALIZED) return;

        _transmitter.processTransmitByte();
    }

    void DataLinkLayer::process()
    {
        if (!_initialized) return;
        // Free-running re-init instead of the old `&& _interface->available()`
        // gate. The NCN draws its power from the KNX bus: a stick that boots
        // with no bus attached gets no U_RESET_IND, and a dead NCN sends no
        // bytes at all — so available() stayed false forever and the retry that
        // was supposed to recover the link never ran. The device then displayed
        // "Uninitialized" until someone power-cycled it, even after the bus came
        // up. Retrying on a timer heals that case by itself.
        if (_bcuState == BCU_UNINITIALIZED)
        {
            if (millis() - _lastTryInitialize >= TPUART_REINIT_INTERVAL) tryInitialize();
            if (_bcuState == BCU_UNINITIALIZED) return;
        }

        if (_bcuState == BCU_DISCONNECTED && _interface->available()) setBCUState(BCU_CONNECTED);
        // Active recovery: even with empty buffer, periodically poke the NCN
        // with a state request — the response will refill the buffer and the
        // line above will flip us back to CONNECTED.
        if (_bcuState == BCU_DISCONNECTED && (millis() - _requestStateTimer) > 1000)
        {
            _lastHostRequestAt = millis();
            _interface->write(U_STATE_REQ);
            if (_bcuType == BCU_NCN5120) _interface->write(U_SYSTEM_STATE_REQ);
            _requestStateTimer = millis();
        }

        // Disarm an unanswered register read: leaving the receiver armed would
        // cost the next unrelated byte, which is a worse trade than losing the
        // reading.
        if (_regReadPending && millis() - _regReadSentAt > TPUART_REGREAD_TIMEOUT)
        {
            // Under the lock: the receive task must not claim a byte between the
            // decision to give up and the disarm, which would hand back a value
            // for a read that had already been written off. A failed try-lock
            // just defers the disarm to the next pass.
            if (rxLock())
            {
                if (_regReadPending)
                {
                    _regReadPending = false;
                    _regReadTimeouts++;
                }
                rxUnlock();
            }
        }

        exitBusyModeTimer();
        processRequestState();

        handleReset();
        unsigned long start;

        start = millis();
        // if (!_interface->hasCallback())
        while (_interface->available())
            processReceviedByte();

        _receiver.process();
        _transmitter.processQueue();

        start = millis();
        while (_transmitter.isTransmitting())
        {
            processTransmitByte();
#ifdef ARDUINO_ARCH_ESP32
            vTaskDelay(1);
#endif
            if (millis() - start > 20) break;
        }

        processRxFrameBuffer();

        // Show state changes and errors
        showOverflowError();
        showStateError();
        showDiscardedError();
        showSystemState();

        // check nothing received
        const unsigned long last = _receiver._lastReceivedTime;
        asm volatile("" ::: "memory"); // ensures that the last value is not updated again by the parallel task or interrupt after the query of millis()
        const unsigned long current = millis();
        // 30s threshold (was 5s): the receiver only refreshes _lastReceivedTime
        // inside processReceviedByte() under non-blocking rxLock, which can
        // be missed for many seconds when the main task holds rxLock during
        // frame buffer drains. 5s is too tight; 30s avoids spurious flips
        // while still detecting a genuinely dead NCN.
        if (_bcuState == BCU_CONNECTED && current - last > 30000UL)
        {
            setBCUState(BCU_DISCONNECTED);
        }

        processWatchdog();
    }

    void DataLinkLayer::processWatchdog()
    {
        if (_bcuState == BCU_UNINITIALIZED) return;

        _transmitter.processWatchdog();
    }

    void DataLinkLayer::processRequestState()
    {
        if (_receiver._invalid) return;
        if ((millis() - _requestStateTimer < 1000)) return;

        requestState();

        _requestStateTimer = millis();
    }

    void DataLinkLayer::handleReset()
    {
        if (!_uReset) return;

        printMessage("Reset received");
        _uReset = false;

        // Deferred half of upstream 18d8655: the transceiver reset invalidated
        // whatever we had in flight, so drop it before the next frame is picked.
        // Main-loop context, so this is safe against processQueue().
        _transmitter.reset();

        applyConfiguration();
        requestState();
    }

    void DataLinkLayer::registerMessage(std::function<void(const char *, bool)> callback)
    {
        _callbackMessage = callback;
    }

    void DataLinkLayer::registerReceivedFrame(std::function<void(Frame &)> callback)
    {
        _callbacksReceivedFrame.push_back(callback);
    }

    void DataLinkLayer::registerCheckAcknowledge(std::function<AcknowledgeType(uint16_t, bool)> callback)
    {
        _callbackCheckAcknowledge = callback;
    }

    AcknowledgeType DataLinkLayer::checkAcknowledge(unsigned short destination, bool isGroupAddress)
    {
        if (!_callbackCheckAcknowledge) return ACK_None;

        return _callbackCheckAcknowledge(destination, isGroupAddress);
    }

    Statistics &DataLinkLayer::getStatistics()
    {
        return _statistics;
    }

    void DataLinkLayer::showOverflowError()
    {
        if (!(_rxShowOverflowTime == 0 || (millis() - _rxShowOverflowTime >= 1000))) return;
        _rxShowOverflowTime = millis();

        if (_rxSearchBufferOverflow)
        {
            _rxSearchBufferOverflow = false;
            printError("SearchBuffer Overflow!");
        }

        if (_rxFrameBufferOverflow)
        {
            _rxFrameBufferOverflow = false;
            printError("FrameBuffer Overflow!");
        }

        if (_rxInterfaceOverflow)
        {
            _rxInterfaceOverflow = false;
            printError("Interface Overflow!");
        }
    }

    void DataLinkLayer::setRepetitions(uint8_t nack, uint8_t busy)
    {
        _repetitions = (nack & 0b111) | ((busy & 0b111) << 4);
    }

    void DataLinkLayer::requestState()
    {
        if (_bcuState == BCU_UNINITIALIZED) return;
        if (_bcuState == BCU_BUSMONITOR) return;

        // printMessage("requestState");

        txLock(true);
        // Stamped before the write: a register read armed between here and the
        // answers would swallow them (see TPUART_REGREAD_QUIET_MS).
        _lastHostRequestAt = millis();
        _interface->write(U_STATE_REQ);
        if (_bcuType == BCU_NCN5120) _interface->write(U_SYSTEM_STATE_REQ);
        txUnlock();
    }

    /*
     * Apply the configuration to the BCU depended on the chip type.
     *
     * The configuration is only applied if the chip is initialized and not in monitoring mode.
     */
    void DataLinkLayer::applyConfiguration()
    {
        // These writes are answered with U_Configure.ind. Stamped for the same
        // reason as the state request: a register read armed in the gap would
        // take the first answer for its own. Matters on the setOwnAddress()
        // path, which runs at any time when ETS assigns a new address.
        _lastHostRequestAt = millis();

        if (_bcuState == BCU_UNINITIALIZED) return;
        if (_bcuState == BCU_BUSMONITOR) return;

        // printMessage("applyConfiguration");
        txLock(true);

        // Enable extednded CRC16
        if (_bcuType == BCU_NCN5120)
            _interface->write(U_NCN5120_CONFIGURE_REQ | U_NCN5120_CONFIGURE_CRC_CCITT_REQ);
        else if (_bcuType == BCU_TPUART2)
        {
            _interface->write(U_TPUART2_ACTIVATECRC_REQ);
            _modeExtendedCRC = true;
        }

        // Set Address for AutoACK Unicast
        {
            if (_ownAddress > 0)
            {
                char buffer[10];
                sprintf(buffer, "%u.%u.%u", (_ownAddress >> 12 & 0b1111), (_ownAddress >> 8 & 0b1111), (_ownAddress & 0b11111111));
                if (_bcuType == BCU_NCN5120) _interface->write(U_NCN5120_SET_ADDRESS_REQ);
                if (_bcuType == BCU_TPUART2) _interface->write(U_TPUART2_SET_ADDRESS_REQ);
                _interface->write((_ownAddress >> 8) & 0xFF);
                _interface->write(_ownAddress & 0xFF);
                if (_bcuType == BCU_NCN5120) _interface->write(0xFF); // Dummy Byte needed by NCN only
            }
        }

        // Abweichende Config
        if (_repetitions != 0b00110011)
        {
            if (_bcuType == BCU_NCN5120)
            {
                _interface->write(U_NCN5120_SET_REPETITION_REQ);
                _interface->write(_repetitions);
                _interface->write(0x0); // dummy, see NCN5120 datasheet
                _interface->write(0x0); // dummy, see NCN5120 datasheet
            }
            else if (_bcuType == BCU_TPUART2)
            {
                _interface->write(U_TPUART2_SET_REPETITION_REQ);
                _interface->write(((_repetitions & 0xF0) << 1) || (_repetitions & 0x0F));
            }
        }

        txUnlock();
    }

    /*
     * Print n message by using the registered callback.
     */
    void DataLinkLayer::printMessage(const char *format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        if (_callbackMessage) _callbackMessage(buffer, false);
    }

    /*
     * Print an error message by using the registered callback.
     */
    void DataLinkLayer::printError(const char *format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        if (_callbackMessage) _callbackMessage(buffer, true);
    }

    /*
     * push a new frame to the transmit queue.
     * if the transmit queue is full, the return is false.
     *
     * Important! The frame is not copied, only a pointer is stored.
     * The frame will only delete wenn the frame is send.
     * Is the queue full, the frame must be deleted by the caller.
     */
    bool DataLinkLayer::pushTransmitQueue(Frame *frame)
    {
        if (_bcuState == BCU_UNINITIALIZED) return false;
        if (_bcuState == BCU_BUSMONITOR) return false;

        if (_bcuType == BCU_TPUART2 && frame->size() > 64) return false;

        if (!_transmitter.pushQueue(frame)) return false;

        return true;
    }

    /*
     * Return the receiver object for direct access.
     */
    Receiver &DataLinkLayer::getReceiver()
    {
        return _receiver;
    }

    /*
     * Return the transmitter object for direct access.
     */
    Transmitter &DataLinkLayer::getTransmitter()
    {
        return _transmitter;
    }

    /*
     * This function allows controlling the power supply (VCC2) of the NCN5120.
     * Note that the setting survives a restart of the host system and a reset of the BCU.
     * Anyone using this function should therefore re-enable the power supply on boot of the host system.
     *
     * WORKS, verified on hardware — see the measurement at the end.
     *
     * Measured 2026-08-17 on TUL (ESP32-C3) and TUL2 (ESP32-C6), both NCN5130,
     * both with live bus power: calling this with state=false left VDD2 and
     * V20V high in the system state, which the stack re-reads every second via
     * processRequestState(). That was read as "the ACR0 write is ignored", with
     * an unverified suspicion about bytes bypassing the transmitter queue.
     *
     * Root cause found 2026-08-30, once an ACR0 readback existed: the bit masks
     * in Types.h were shifted one position — DC2EN sat at 0x80 while the device
     * has it at bit 5 (table 16, p.54). So state=false sent 0x30, which still
     * has DC2EN *set*. VDD2 staying up was the correct answer to what was
     * actually asked. The masks are corrected now.
     *
     * Measured 2026-08-31 with writeInternalRegister() + a readback, on a live
     * bus: ACR0 0x74 -> 0x54 makes VDD2 go low within ~2 s and back up when the
     * register is restored; 0x74 -> 0x34 does the same to V20V. Both rails do
     * follow their enable bits, and the write does arrive — so the remaining
     * difference to the 2026-08-17 attempt is this function itself: it writes
     * straight into the UART with no check that the line is idle, so its two
     * bytes can land inside a frame. writeInternalRegister() refuses unless the
     * receiver, the search buffer and the transmitter are all quiet, which is
     * the likely reason it lands where powerControl() did not (plausible, not
     * proven — the losing case was not re-measured).
     *
     * That is what it does now: the write goes through writeInternalRegister()
     * and a readback is queued behind it. Verified 2026-08-31 on the C3 bench
     * stick via the NCN_DC2_DIAG harness — powerControl(false) reads ACR0 back
     * as 0x14 with V20V and VDD2 both low, powerControl(true) restores 0x74 and
     * both rails come back. The KNX stack itself still never touches ACR0
     * (applyConfiguration() writes CRC, address and repetition only).
     *
     * Caveat that outlives the fix: false means the line was busy and nothing
     * was written — retry, do not assume.
     */
    bool DataLinkLayer::powerControl(bool state)
    {
        if (_bcuState == BCU_UNINITIALIZED) return false;
        // if (_bcuState == BCU_BUSMONITOR) return false;
        if (_bcuType != BCU_NCN5120) return false;

        // state=true restores the documented ACR0 reset value (0x74); state=false
        // clears DC2EN *and* V20VEN, so it takes the 20 V regulator down with
        // DC2 and the device cannot drive the bus until it is switched back on.
        const uint8_t value = state
            ? (uint8_t)(ACR0_FLAG_DC2EN | ACR0_FLAG_V20VEN | ACR0_FLAG_XCLKEN | ACR0_V20VCLIMIT_RESET)
            : (uint8_t)(ACR0_FLAG_XCLKEN | ACR0_V20VCLIMIT_RESET);

        // Goes through writeInternalRegister() rather than writing the two bytes
        // straight into the UART: they must not land inside a frame. A refusal
        // means the line was busy and *nothing* was written — call again from
        // loop() until it returns true, do not assume the change took effect.
        if (!writeInternalRegister(U_INT_REG_WR_REQ_ACR0, value)) return false;

        printMessage(state ? "VCC2 power enabled" : "VCC2 power disabled");

        // A register write is never acknowledged, so ask for a readback. Best
        // effort by design: it is refused if the line is not quiet, and the
        // caller is expected to check internalRegisterValid()/Value() — or to
        // ask again — rather than treat the write as confirmed.
        requestInternalRegister(U_INT_REG_RD_REQ_ACR0);

        requestState();
        return true;
    }

    /*
     * Read one internal device-specific register (DS p.39 fig.43: U_IntRegRd.req
     * is 0b001110aa, so ACR0 with aa=01 is 0x39). The device answers with the
     * register content as a plain data byte and marks it in no way at all, which
     * is why the stack never had a read: the byte has to be claimed before the
     * frame decoder sees it, or a value like 0xB0 parses as a telegram start
     * ((0xB0 & L_DATA_MASK) == L_DATA_STANDARD_IND).
     *
     * The request therefore only goes out on a quiet line — nothing being
     * received, nothing in the search buffer, nothing being transmitted. Traffic
     * arriving in the gap between request and answer is still possible, so the
     * caller has to treat a single reading as evidence, not as proof; repeating
     * it costs nothing.
     */
    bool DataLinkLayer::requestInternalRegister(uint8_t readRequestByte)
    {
        if (_bcuType != BCU_NCN5120) return false;
        // CONNECTED only: while DISCONNECTED the stack poll and the watchdog
        // reset are in flight, and an armed read would eat the U_Reset.ind that
        // drives handleReset()/applyConfiguration().
        if (_bcuState != BCU_CONNECTED) return false;
        if (_regReadPending) return false;
        if (!quietForRegisterAccess()) return false;

        if (!rxLock()) return false;
        // Re-check under the lock: the receive task may have taken a byte
        // between the check above and here.
        if (!quietForRegisterAccess())
        {
            rxUnlock();
            return false;
        }
        _regReadValid = false;
        _regReadRequest = readRequestByte;
        _regReadSentAt = millis();
        // Armed before the write, never after: the answer can be back before
        // the next statement runs.
        _regReadPending = true;
        _interface->write(readRequestByte);
        rxUnlock();
        return true;
    }

    /*
     * True when nothing else is on the wire in either direction. A register
     * answer carries no service marker, so whatever arrives first is taken for
     * it — every one of these conditions exists to make sure that "whatever"
     * really is the answer:
     *   - receiver mid-frame or holding bytes    -> a frame byte would be taken
     *   - transmitter busy                       -> our bytes land inside a frame
     *   - bytes already waiting in the driver    -> stale byte claimed as answer
     *   - within the answer window of a state request -> U_State.ind claimed
     */
    bool DataLinkLayer::quietForRegisterAccess()
    {
        if (_receiver._state != RX_IDLE) return false;
        if (_receiver.getSearchBufferPosition() > 0) return false;
        if (_transmitter.isTransmitting() || _transmitter.awaitResponse()) return false;
        if (_interface->available()) return false;
        if (millis() - _lastHostRequestAt < TPUART_REGREAD_QUIET_MS) return false;
        return true;
    }

    /*
     * Write one internal device-specific register (DS p.39 fig.42: U_IntRegWr.req
     * is 0b001010aa followed by the data byte; ACR0 is aa=01, i.e. 0x29).
     *
     * Same quiet-line requirement as the read, and for the same reason: these
     * bytes go straight out via _interface->write(), so they must not be issued
     * while a frame is in flight. There is no acknowledgement of any kind for a
     * register write — the only way to know it landed is to read the register
     * back, which is what makes requestInternalRegister() the other half of this.
     */
    bool DataLinkLayer::writeInternalRegister(uint8_t writeRequestByte, uint8_t value)
    {
        if (_bcuType != BCU_NCN5120) return false;
        if (_bcuState != BCU_CONNECTED) return false;
        if (_regReadPending) return false;
        if (!quietForRegisterAccess()) return false;

        if (!rxLock()) return false;
        if (!quietForRegisterAccess())
        {
            rxUnlock();
            return false;
        }
        // Deliberately NOT stamped: a register write produces no answer, so
        // there is nothing for the quiet window to protect — and stamping here
        // made the readback that callers queue right afterwards impossible.
        _interface->write(writeRequestByte);
        _interface->write(value);
        rxUnlock();
        return true;
    }

    /*
     * Claim the answer byte. Called from the receive path before any decoding.
     */
    bool DataLinkLayer::consumeInternalRegisterByte(char value)
    {
        if (!_regReadPending) return false;

        // Never claim U_Reset.ind. reset() disarms a pending read for the resets
        // we ourselves ask for, but the transceiver can reset on its own — a bus
        // brown-out does it, which is exactly the situation where the fault
        // capture is arming reads. Swallowing that byte skips receivedReset()
        // and with it applyConfiguration(), leaving the device running without
        // CRC-CCITT while this side still expects it: every telegram then fails
        // its checksum and is dropped, and nothing recovers, because the bytes
        // keep arriving so the link never looks disconnected.
        //
        // The cost of the exception is one lost reading in the case where ACR0
        // genuinely holds 0x03 — which would mean XCLKEN and both regulators
        // off, a device that cannot run. A missed reading retries; a deaf bus
        // does not.
        if ((uint8_t)value == U_RESET_IND) return false;

        _regReadValue = (uint8_t)value;
        _regReadAt = millis();
        _regReadValid = true;
        _regReadPending = false;
        return true;
    }

    /*
     * Activate or deactivate the stop mode. In stop mode, the NCN5120 stop sending frames to host controller.
     */
    bool DataLinkLayer::stopMode(bool state)
    {
        if (_bcuState == BCU_UNINITIALIZED) return false;
        if (_bcuType != BCU_NCN5120) return false;

        txLock(true);
        if (state)
        {
            printMessage("Stop mode enabled");
            _interface->write(U_STOP_MODE_REQ);
        }
        else
        {
            printMessage("Stop mode disabled");
            _interface->write(U_EXIT_STOP_MODE_REQ);
        }
        txUnlock();

        return true;
    }

    /*
     * Actiavte the budy mode. In busy mode, all frames will be rejected with BUSY.
     * The mode will be left after 700ms. This is done to prevent the mode from being accidentally activated.
     */
    bool DataLinkLayer::busyMode(bool state)
    {
        if (_bcuState == BCU_UNINITIALIZED) return false;

        txLock(true);
        if (state)
        {
            printMessage("Busy mode enabled for around 700ms");
            if (_bcuType == BCU_NCN5120) _interface->write(U_NCN5120_SET_BUSY_REQ);
            if (_bcuType == BCU_TPUART2) _interface->write(U_TPUART2_SET_BUSY_REQ);
            _busyMode = millis();
        }
        else
        {
            printMessage("Busy mode disabled");
            if (_bcuType == BCU_NCN5120) _interface->write(U_NCN5120_QUIT_BUSY_REQ);
            if (_bcuType == BCU_TPUART2) _interface->write(U_TPUART2_QUIT_BUSY_REQ);
            _busyMode = 0;
        }
        txUnlock();

        return true;
    }

    /*
     * This function disables the busy mode after approximately 700ms.
     * The reason for the 700ms is that TPUART2 also exits the mode in hardware after 700ms, ensuring the same behavior between both chip types.
     */
    void DataLinkLayer::exitBusyModeTimer()
    {
        if (!_busyMode) return;
        if ((millis() - _busyMode) < 700) return;

        busyMode(false);
    }

    /*
     * Show the current state of the TPUart.
     */
    void DataLinkLayer::showStateError()
    {
        if (!_uState) return;

        std::string errorMessage = "TP Error:";
        if (_uState & SLAVE_COLLISION) errorMessage += " SC";
        if (_uState & RECEIVE_ERROR) errorMessage += " RE";
        if (_uState & TRANSMIT_ERROR) errorMessage += " TE";
        if (_uState & PROTOCOL_ERROR) errorMessage += " PE";
        if (_uState & TEMPERATURE_WARNING) errorMessage += " TW";
        printError(errorMessage.c_str());
        _uState = 0;
    }

    /*
     * Show the number of Discard bytes.
     */
    void DataLinkLayer::showDiscardedError()
    {
        if (!_receiver._discardedBytes.size()) return;
        if ((millis() - _lastDiscardedMessage) < 1000) return;
        if (!(_receiver._invalid || millis() - _receiver._lastDiscarded > 100 || _receiver._discardedBytes.size() > 100)) return;
        if (!rxLock()) return;

        std::string buffer;
        size_t size = _receiver._discardedBytes.size();
        while (_receiver._discardedBytes.size())
        {
            char hexBuffer[4];
            sprintf(hexBuffer, " %02X", _receiver._discardedBytes.pop());
            buffer += hexBuffer;
        }

        printError(" %u Bytes are discarded! (%s )", size, buffer.c_str());

        _lastDiscardedBytes = _statistics.getRxDiscardedBytes();
        _lastDiscardedMessage = millis();

        rxUnlock();
    }

    /*
     * Show the current system state of the NCN5120.
     */
    void DataLinkLayer::showSystemState()
    {
        if (_bcuType != BCU_NCN5120) return;
        if (!_systemState.dirty()) return;

        printMessage(_systemState.print().c_str());
    }

    /*
     * Set the own address of the BCU.
     * This is needed so that the BCU independently acknowledges frames when they are addressed to its own address.
     */
    void DataLinkLayer::setOwnAddress(short address)
    {
        _ownAddress = address;
        applyConfiguration(); // apply new address, if datalinklayer is already initialized
    };

    void DataLinkLayer::setBCUState(BcuState state, int baudrate)
    {
        if (_bcuState == state) return;

        if (state == BCU_CONNECTED)
        {
            if (baudrate == 0)
                printMessage("BCU connected");
            else
                printMessage("BCU connected (Baudrate: %d)", baudrate);

            _receiver._lastReceivedTime = millis();
        }
        else if (state == BCU_DISCONNECTED)
        {
            printMessage("BCU disconnected");
        }
        else if (state == BCU_BUSMONITOR)
        {
            printMessage("BCU in monitor mode");
        }

        _bcuState = state;
    }

    BcuState DataLinkLayer::getBcuState()
    {
        return _bcuState;
    }

    bool DataLinkLayer::startMonitoring()
    {
        if (_bcuState == BCU_UNINITIALIZED) return false;
        if (_bcuState == BCU_BUSMONITOR) return true;

        txLock(true);
        _interface->write(U_BUSMON_REQ);
        txUnlock();
        _modeExtendedCRC = false;
        setBCUState(BCU_BUSMONITOR);
        return true;
    }

    bool DataLinkLayer::isMonitoring() const
    {
        return _bcuState == BCU_BUSMONITOR;
    }

    bool DataLinkLayer::isConnected() const
    {
        return _bcuState == BCU_CONNECTED;
    }

    void DataLinkLayer::receivedState(char state)
    {
        if (state == U_STATE_IND) return;

        // printMessage("U_STATE_IND %02X", state);
        _uState |= state ^ U_STATE_MASK;
    }

    void DataLinkLayer::receivedReset()
    {
        // printMessage("U_RESET_IND");
        _uReset = true;
        _modeExtendedCRC = false;
        _modeAutoAcknowlage = false;
        // Upstream 18d8655 resets the transmitter right here. On ESP32 this
        // function runs in the UART RX task (Interface/ESP32.cpp registers
        // processReceviedByte as a callback), while processQueue() mutates
        // _queue/_frame from the main loop WITHOUT taking txLock — clearing the
        // queue from here would race it into a use-after-free. The reset is
        // therefore deferred to handleReset(), which the main loop runs from
        // process() before the next processQueue().
        setBCUState(BCU_CONNECTED);
    }

    void DataLinkLayer::receivedConfiguration(char config)
    {
        // printMessage("U_CONFIGURE_IND %02X", value);
        _modeAutoAcknowlage = config & AUTO_ACKNOWLEDGE;
        _modeExtendedCRC = config & CRC_CCITT;
    }

} // namespace TPUart