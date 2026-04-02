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

        uint baudrates[2] = {19200, 38400};
        for (uint baudrate : baudrates)
        {
            if (_bcuType == BCU_TPUART2 && baudrate != 19200) continue;

            if (tryInitialize(baudrate))
            {
                setBCUState(BCU_CONNECTED, baudrate);
                _uReset = true;
                _bcuState = BCU_CONNECTED;
                return;
            }
        }
    }

    bool DataLinkLayer::tryInitialize(uint baudrate)
    {
        printMessage("Try Initialize %d", baudrate);

        _interface->end(); // End interface is already initialized
        _interface->begin(baudrate);
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
        _bcuState = BCU_CONNECTED;

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
            const uint16_t bufferSize = _rxFrameBuffer.pop() + (_rxFrameBuffer.pop() << 8);
            const uint16_t frameSize = bufferSize - 3;

            char frameData[frameSize] = {};

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
        if (_bcuState == BCU_UNINITIALIZED && _interface->available())
        {
            if (millis() - _lastTryInitialize > 1000) tryInitialize();
        }

        if (_bcuState == BCU_UNINITIALIZED) return;

        if (_bcuState == BCU_DISCONNECTED && _interface->available()) setBCUState(BCU_CONNECTED);

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
        if (_bcuState == BCU_CONNECTED && current - last > 5000UL)
        {
            // printMessage("Time: %li - %li = %li", current, _receiver._lastReceivedTime, current - last);
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
     */
    bool DataLinkLayer::powerControl(bool state)
    {
        if (_bcuState == BCU_UNINITIALIZED) return false;
        // if (_bcuState == BCU_BUSMONITOR) return false;
        if (_bcuType != BCU_NCN5120) return false;

        rxLock(true); // Prevent sending ACKs during sending multi bytes multibytes
        _interface->write(U_INT_REG_WR_REQ_ACR0);
        if (state)
        {
            printMessage("VCC2 power enabled");
            _interface->write(ACR0_FLAG_DC2EN | ACR0_FLAG_V20VEN | ACR0_FLAG_XCLKEN | ACR0_FLAG_V20VCLIMIT);
        }
        else
        {
            printMessage("VCC2 power disabled");
            _interface->write(ACR0_FLAG_XCLKEN | ACR0_FLAG_V20VCLIMIT);
        }
        rxUnlock();

        requestState();
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
        setBCUState(BCU_CONNECTED);
    }

    void DataLinkLayer::receivedConfiguration(char config)
    {
        // printMessage("U_CONFIGURE_IND %02X", value);
        _modeAutoAcknowlage = config & AUTO_ACKNOWLEDGE;
        _modeExtendedCRC = config & CRC_CCITT;
    }

} // namespace TPUart