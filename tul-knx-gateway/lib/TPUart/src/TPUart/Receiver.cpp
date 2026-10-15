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

    // U_State.ind, the answer to the state request processRequestState() sends
    // once a second. 0xFF fits the pattern but is ignored, not taken as a state,
    // by processControlBytes().
    static inline bool isStateAnswer(const char value)
    {
        return (value & U_STATE_MASK) == U_STATE_IND && (uint8_t)value != 0xFF;
    }

    // U_SystemStat.ind, the first of the two octets the NCN answers the system state
    // request with that processRequestState() sends along.
    bool Receiver::isSystemStateAnswer(const char value) const
    {
        return value == U_SYSTEM_STAT_IND && _dll._bcuType == BCU_NCN5120;
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
                // The byte that ended the pause may be the awaited L_DATA_CON: after the
                // echo of an unacknowledged frame it comes some 3.4 ms later (30 bit
                // times of ack time-out, NCN5130 DS p.36), close to the 4-5 ms that set
                // the marker. Completing the frame here would drop its confirmation;
                // processSearchBufferAcknowledge() takes it instead. 0x0B/0x8B cannot
                // start a frame (an L_Data control field has bit 4 set). The same
                // goes for a U_State.ind ahead of the confirmation.
                if (_searchBuffer.position() >= _awaitBytes)
                {
                    const char value = _searchBuffer.get(_awaitBytes - 1);
                    if (_sysStatValuePending || (value & L_DATA_CON_MASK) == L_DATA_CON ||
                        (_searchBuffer.frame().isTransmitted() && (isStateAnswer(value) || isSystemStateAnswer(value))))
                        return;
                }
                _sysStatValuePending = false; // its value octet did not come
#ifdef TPUART_CON_DIAG
                _diagAcknTimeout++;
#endif
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
            _unansweredProbes = 0;
#ifdef TPUART_CON_DIAG
            {
                const unsigned long now = millis();
                const unsigned long gap = now - _diagLastAt;
                _diagLastAt = now;
                _diagHist[_diagHead] = (uint8_t)value;
                _diagGap[_diagHead] = gap > 255 ? 255 : (uint8_t)gap;
                _diagHead = (_diagHead + 1) % DIAG_HIST;
            }
#endif

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

        // The value octet of a U_SystemStat.ind taken below. Checked first: it can look
        // like anything, a confirmation as well.
        if (_sysStatValuePending)
        {
            _sysStatValuePending = false;
            _dll._systemState.update(value);
            _searchBuffer.dropLast();
            return;
        }

        // The state probe asks the NCN for its system state too, answered with two
        // octets, U_SystemStat.ind and the value. Between the echo of our frame and its
        // confirmation the first one closed the frame without it, and the confirmation
        // went to processControlBytes(), which drops it: the tunnel client got no
        // L_Data.con (orphan_cons in /api/status). Take both and keep waiting.
        // (upstream e73afb8, that part only)
        if (_searchBuffer.frame().isTransmitted() && isSystemStateAnswer(value))
        {
            _sysStatValuePending = true;
            _searchBuffer.dropLast();
            return;
        }

        // A state request that reaches the transceiver while one of our frames is
        // on the bus is answered after the frame and before its confirmation:
        // echo, U_State.ind some 3 ms later, L_DATA_CON 1 ms after that
        // (measured). Closing the frame on the answer left the confirmation to
        // processControlBytes(), which drops it, so the tunnel client never got
        // its L_Data.con. Take the state and keep waiting.
        if (_searchBuffer.frame().isTransmitted() && isStateAnswer(value))
        {
            _dll.receivedState(value);
            _searchBuffer.dropLast();
#ifdef TPUART_CON_DIAG
            _diagStateTaken++;
#endif
            return;
        }

        bool acknowledge = false;
        if ((value & L_DATA_CON_MASK) == L_DATA_CON)
        {
            if ((value ^ L_DATA_CON_MASK) >> 7)
            {
                _searchBuffer.frame().setAcknowledge();
            }
            _searchBuffer.frame().setDataCon();
            acknowledge = true;
            _dll.getTransmitter().finalize();
#ifdef TPUART_CON_DIAG
            _diagDataCons++;
#endif
        }
        else if ((value & L_ACKN_MASK) == L_ACKN_IND)
        {
            const bool isBusy = !(value & L_ACKN_BUSY_MASK);
            const bool isNack = !(value & L_ACKN_NACK_MASK);
            _searchBuffer.frame().setAcknowledge(isBusy, isNack);
            acknowledge = true;
        }
#ifdef TPUART_CON_DIAG
        // Neither confirmation nor the start of the next repetition's echo.
        else if ((value & L_DATA_MASK) != L_DATA_STANDARD_IND && (value & L_DATA_MASK) != L_DATA_EXTENDED_IND)
        {
            _diagOtherVal[_diagAcknOther % DIAG_SNAPS] = (uint8_t)value;
            _diagAcknOther++;
        }
#endif

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
        // Still waiting: a state answer was taken ahead of the confirmation.
        if (_state == RX_FRAME_WAIT_ACKN) return;
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

        // The answer to U_IntRegRd.req is a bare register byte with no service
        // marker (DS p.39 fig.43), so it has to be claimed here, ahead of the
        // frame and control-byte decoders — a value such as 0xB0 would satisfy
        // (v & L_DATA_MASK) == L_DATA_STANDARD_IND and start a phantom frame.
        if (_dll.consumeInternalRegisterByte(value)) return true;

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
        _sysStatValuePending = false;
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
#ifdef TPUART_CON_DIAG
            {
                const uint8_t slot = _diagSnapCount % DIAG_SNAPS;
                for (uint8_t i = 0; i < DIAG_HIST; i++)
                {
                    const uint8_t idx = (_diagHead + i) % DIAG_HIST; // oldest first
                    _diagSnap[slot][2 * i] = _diagHist[idx];
                    _diagSnap[slot][2 * i + 1] = _diagGap[idx];
                }
                _diagSnapTx[slot] = _dll.getTransmitter().awaitResponse();
                _diagOrphanCons++;
                _diagSnapCount++;
            }
#endif
            _orphanCons = _orphanCons + 1;
            if (!_dll.getTransmitter().awaitResponse()) _orphanConsIdle = _orphanConsIdle + 1;
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