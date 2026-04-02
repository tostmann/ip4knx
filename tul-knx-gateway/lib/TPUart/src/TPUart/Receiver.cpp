// #pragma GCC optimize("O3")
#include "TPUart/Receiver.h"
#include "TPUart/DataLinkLayer.h"

#ifndef TPUART_RX_TIMEOUT
#define TPUART_RX_TIMEOUT 5
#endif

namespace TPUart
{
    Receiver::Receiver(DataLinkLayer &dll) : _dll(dll)
    {
    }

    void Receiver::process()
    {
        processTimeout();
    }

    /*
     * Hier wird geprüft ob der SearchBuffer mit einem Timeout-Marker (Positionsangabe) hat.
     * Gibt es einen solche Marker, wird geprüft, ob mehr Bytes erwartet werden als der Marker (Positionsangabe) erlaubt.
     * Sollte das der Fall sein, wird das erste Zeichen verworfen und Marker um ein verschoben (processSearchBufferInvalid).
     * Das wird solange wiederholt bis entweder der Marker nicht mehr vorhanden (0) ist oder die Anzahl der erwarteten Bytes kleiner ist als die Anzahl der Bytes im Buffer.
     * Dadurch wird sicher gestellt, dass ein möglicherweise weiteres Frame im SearchBuffer nicht verloren geht.
     */
    void Receiver::processSearchBufferTimeout()
    {
        if (!_searchBuffer.timeout()) return;
        if (_searchBuffer.timeout() >= _awaitBytes) return;

        if(_state == RX_FRAME_WAIT_ACKN)
        {
                _state = RX_FRAME_COMPLETE;
                processCompleteFrame();
                return;
        }
        processSearchBufferInvalid(4);
    }

    /*
     * Diese Funktion prüft ob im SearchBuffer Daten vorhanden sind aber keine neuen Daten im Empfangspuffer des Interaces.
     * Wenn das der Fall ist, wird die aktuell Position im SearchBuffer markiert.
     */
    void Receiver::processTimeout()
    {
        // Vorprüfung: Damit nicht unnötig ein lock geholt wird (Sortiert nach Rechenaufwand)
        if (_searchBuffer.timeout() == _searchBuffer.position()) return;
        if (_dll._interface->available()) return;
        if (millis() - _lastReceivedTime < TPUART_RX_TIMEOUT) return;

        _dll.rxLock(true);
        size_t timeout = 0;
        // Wiederholung der Vorprüfung
        if (_searchBuffer.timeout() != _searchBuffer.position() && !_dll._interface->available() && millis() - _lastReceivedTime >= TPUART_RX_TIMEOUT)
        {
            timeout = _searchBuffer.position();
            _searchBuffer.timeout(timeout);
        }
        _dll.rxUnlock();

#ifdef TPUART_RX_TIMEOUT_DEBUG
        if (timeout) _dll.printError("TIMEOUT: %u %u", millis() - _lastReceivedTime, timeout);
#endif
    }

    /*
     * Liest die Daten aus dem Interface aus, packt es in den SearchBuffer und startet die Verarbeitung.
     */
    bool Receiver::processReceviedByte()
    {
        if (!_dll.rxLock()) return false;
        if (_dll._interface->overflow())
        {
            _invalid = true;
            _dll._rxInterfaceOverflow = true;
            _dll._statistics.incrementRxUartOverflow();
        }
        const int value = _dll._interface->read();

        if (value != -1)
        {
            _lastReceivedTime = millis();

            const uint start = micros();
            _dll._statistics.incrementRxReceivedBytes();
            pushSearchBuffer(value);
            uint duration = micros() - start;
            _dll._statsDuration += duration;
            _dll._statsDurationCount = _dll._statsDurationCount + 1;
            if (duration > _dll._statsDurationMax) _dll._statsDurationMax = duration;
            if (duration < _dll._statsDurationMin) _dll._statsDurationMin = duration;
        }

        _dll.rxUnlock();
        return true;
    }

    /*
     * Wird bei jedem neuen Byte aufgerufen falls ein Frame noch auf ein Acknowledge wartet.
     * Sollte ein L_DATA_CON oder L_ACKN_IND empfangen werden, wird das Frame als bestätigt markiert.
     * Anschließend wird das Frame abgeschlossen und in den FrameBuffer geschrieben (auch wenn kein Ack gekommen ist).
     */
    void Receiver::processSearchBufferAcknowledge()
    {
        if (_state != RX_FRAME_WAIT_ACKN) return;

        const char value = _searchBuffer.get(_awaitBytes - 1);

        bool acknowledge = false;
        if ((value & L_DATA_CON_MASK) == L_DATA_CON)
        {
            if ((value ^ L_DATA_CON_MASK) >> 7)
            {
                _searchBuffer.frame().setAcknowledge();
            }
            acknowledge = true;
            _dll.getTransmitter().finalize();
        }
        else if ((value & L_ACKN_MASK) == L_ACKN_IND)
        {
            const bool isBusy = !(value & L_ACKN_BUSY_MASK);
            const bool isNack = !(value & L_ACKN_NACK_MASK);
            _searchBuffer.frame().setAcknowledge(isBusy, isNack);
            acknowledge = true;
        }

        processCompleteFrame(acknowledge);
    }

    void Receiver::processCompleteFrame(bool acknowledge)
    {
        if (_state == RX_FRAME_COMPLETE || _state == RX_FRAME_WAIT_ACKN)
        {
            unsigned short size = _searchBuffer.frame().size();
            if (!_dll.isMonitoring() && _dll._modeExtendedCRC) size += 2;
            if (acknowledge) size++; // Es hängt noch ein ACKN oder DATA_CON dran
            _dll.pushRxFrameBuffer(_searchBuffer.frame());
            _searchBuffer.frame().resetFlags();

            _dll._statistics.incrementRxFrameBytes(_searchBuffer.frame().size());
            // _dll._statistics.incrementRxControlBytes(size - _searchBuffer.frame().size());
            _dll._statistics.incrementRxFrames();

            _searchBuffer.move(size);
            _awaitBytes = 1;
            _state = RX_IDLE;

            // Wenn der buffer leer ist, kann auch das invalid flag zurückgesetzt werden
            //_dll.printError("FV! %u %u %u", _dll._interface->available(), _searchBuffer.position(), _awaitBytes);
            if (!_dll._interface->available())
            {
                if (_searchBuffer.empty())
                {
                    _invalid = false;
                }
            }
        }
        else
        {
            if (_searchBuffer.position()) processSearchBufferInvalid(1);
        }
    }

    void Receiver::processSearchBuffer()
    {
        if (_searchBuffer.empty()) return;
        processSearchBufferTimeout();

        if (!sufficientlyBytes()) return;

        processSearchBufferAcknowledge();
        if (_searchBuffer.empty()) return;

        if (_searchBuffer.frame().isFrame())
            processSearchBufferFrame();

        else if (_invalid)
            processSearchBufferInvalid(2);
        else
            processControlBytes();

        // next
        processSearchBuffer();
    }

    void Receiver::processSearchBufferInvalid(int x)
    {
        while (_searchBuffer.position())
        {
            char value = _searchBuffer.get(0);
            //_dll.printError("IVB1: %02X  - H:%u I:%u P:%u T:%u A:%u", value, x, _invalid, _searchBuffer.position(), _searchBuffer.timeout(), (uint)_awaitBytes);
            _lastDiscarded = millis();
            asm volatile("" ::: "memory");
            _discardedBytes.push(value);
            _searchBuffer.move(1);
            _dll._statistics.incrementRxDiscardedBytes();

            if (_searchBuffer.frame().isFrame()) break;
        }

        _searchBuffer.frame().resetFlags();
        _state = RX_IDLE;
        _invalid = true;
        _awaitBytes = 1;
    }

    void Receiver::processSearchBufferFrame()
    {
        Frame &frame = _searchBuffer.frame();

        if (_state == RX_IDLE) _state = RX_FRAME;

        if (_state == RX_FRAME)
        {
            _awaitBytes = frame.awaitDestination();

            if (!sufficientlyBytes()) return;

            // Adresse - SET ACKn
            // _dll.printMessage("    DST ADDRESS %s", frame.humanDestination().c_str());
            if (_dll.getTransmitter().awaitResponse())
            {
                // _dll.printMessage("      awaitResponse %i", _searchBuffer.position());
                if (!((frame.data(0) ^ _dll.getTransmitter().currentFrame()->data(0)) & ~0x20) && frame.destination() == _dll.getTransmitter().currentFrame()->destination() && frame.source() == _dll.getTransmitter().currentFrame()->source())
                {
                    frame.setTransmitted();
                    // _dll.getTransmitter().resetWatchdogTimer();
                }
            }

            if (!_dll.isMonitoring())
            {
                AcknowledgeType acknowledge = frame.isTransmitted() ? ACK_None : _dll.checkAcknowledge(frame.destination(), frame.isGroupAddress());
                if (acknowledge != ACK_None)
                {
                    _dll.getTransmitter().sendAcknowledge(acknowledge);
                    frame.setAcknowledge(acknowledge);
                }
            }

            _state = RX_FRAME_DESTINATION;
        }

        if (_state == RX_FRAME_DESTINATION)
        {
            // Frame size
            _awaitBytes = frame.awaitSize();
            if (!sufficientlyBytes()) return;

            _awaitBytes = frame.size();

            if (_dll._modeExtendedCRC) _awaitBytes += 2;

            _state = RX_FRAME_SIZE;
        }

        if (_state == RX_FRAME_SIZE)
        {
            if (!sufficientlyBytes()) return;

            bool valid = frame.isValid();

            // don't check CRC if the frame is not valid
            if (valid && _dll._modeExtendedCRC)
            {
                if (_dll._bcuType == BCU_NCN5120 && !frame.checkCRC16CCITT()) valid = false;
                if (_dll._bcuType == BCU_TPUART2 && !frame.checkCRC16SPI()) valid = false;
            }

            if (valid)
            {
                // _dll.printMessage("    COMPLETED VALID %i %i", frame.size(), _awaitBytes);

                // Wait for a DATA_CON or ACKN
                if (_dll.isMonitoring() || frame.isTransmitted())
                {
                    _awaitBytes = _awaitBytes + 1; // warte auf noch ein byte welches hoffentlich ein ACKN oder DATA_CON ist
                    _state = RX_FRAME_WAIT_ACKN;
                    return;
                }

                _state = RX_FRAME_COMPLETE;
                processCompleteFrame();
            }
            else
            {
                // _dll.printMessage("INVALID FRAME");
                // String tmp;
                // tmp.reserve(1024);
                // for (size_t i = 0; i < _searchBuffer.position(); i++)
                // {
                //     char t2[3];
                //     snprintf(t2, 3, "%02X ", _searchBuffer.get(i));
                //     tmp += String(t2) + " ";
                // }
                // _dll.printMessage("      %s", tmp.c_str());
                processSearchBufferInvalid(3);
            }
        }
    }

    bool Receiver::pushSearchBuffer(const char value)
    {
        // _dll.printMessage("pushSearchBuffer: %02X", value);
        if (!_searchBuffer.add(value))
        {
            _dll._rxSearchBufferOverflow = true;
            _dll._statistics.incrementRxSearchBufferOverflow();
            return false;
        }

        processSearchBuffer();

        return true;
    }

    void Receiver::reset()
    {
        _searchBuffer.clear();
        _state = RX_IDLE;
        _awaitBytes = 1;
        _invalid = false;
    }

    unsigned short Receiver::getAwaitBytes()
    {
        return _awaitBytes;
    }

    unsigned short Receiver::getSearchBufferPosition()
    {
        return _searchBuffer.position();
    }

    inline bool Receiver::sufficientlyBytes()
    {
        return _searchBuffer.position() >= _awaitBytes;
    }

    void Receiver::processControlBytes()
    {
        const char value = _searchBuffer.get(0);
        uint8_t count = 1;
        // _dll.printMessage("processControlBytes %02X", value);

        if (value == U_RESET_IND)
        {
            _dll.receivedReset();
        }
        else if (value == 0xFF || value == 0xFE || value == 0xFD || value == 0xFC)
        {
        }
        else if (value == U_STOP_MODE_IND && _dll._bcuType == BCU_NCN5120)
        {
            // Maybe trigger an requestSystemState to get SystemState faster
        }
        else if ((value & U_STATE_MASK) == U_STATE_IND)
        {
            _dll.receivedState(value);
        }
        else if (value == U_SYSTEM_STAT_IND && _dll._bcuType == BCU_NCN5120)
        {
            if (_searchBuffer.position() < 2)
            {
                _awaitBytes = 2;
                return;
            }
            // _dll.printMessage("U_SYSTEM_STAT_IND %02X", _searchBuffer.get(1));
            _dll._systemState.update(_searchBuffer.get(1));
            count = 2;
        }
        else if (_dll.isMonitoring() && (value == 0xFF || value == 0xFD))
        {
        }
        else if ((value & U_CONFIGURE_MASK) == U_CONFIGURE_IND)
        {
            _dll.receivedConfiguration(value);
        }
        else if ((value & L_DATA_CON_MASK) == L_DATA_CON)
        {
            _dll.getTransmitter().finalize();
        }

        else if ((value & L_ACKN_MASK) == L_ACKN_IND)
        {
        }

        else
        {
            // Unexpected
            _lastDiscarded = millis();
            asm volatile("" ::: "memory");
            _discardedBytes.push(value);
            _dll._statistics.incrementRxDiscardedBytes();
            // _dll.printMessage("IVB2: %02X", value);
            _invalid = true;
        }

        // if (!_invalid) _dll._statistics.incrementRxControlBytes(count);
        _searchBuffer.move(count);
        _awaitBytes = 1;
    }

} // namespace TPUart