#pragma once
#include "TPUart/Frame.h"
#include "TPUart/RepetitionFilter.h"
#include "TPUart/SearchBuffer.h"
#include "TPUart/Types.h"
#include <Arduino.h>

// Discarded octets kept for the hex line of DataLinkLayer::showDiscardedError();
// later ones are only counted, and the line then shows "N+". (upstream b4addd8)
#ifndef TPUART_DISCARD_LOG_SIZE
#define TPUART_DISCARD_LOG_SIZE 256
#endif
static_assert(TPUART_DISCARD_LOG_SIZE > 0 && TPUART_DISCARD_LOG_SIZE <= 0xFFFF, "TPUART_DISCARD_LOG_SIZE must be in 1..65535");

namespace TPUart
{
    class DataLinkLayer;

    class Receiver
    {
        // bool _frame = false;

        friend class DataLinkLayer;
        friend class Transmitter;

        DataLinkLayer &_dll;
        SearchBuffer _searchBuffer;
        // The first octets discarded since the last dump, written by the receive task
        // and read by showDiscardedError(), both under rxLock. It was a second ring of
        // 6001 octets.
        char _discardedLog[TPUART_DISCARD_LOG_SIZE];
        volatile uint16_t _discardedLogSize = 0;
        void logDiscarded(const char value)
        {
            if (_discardedLogSize < TPUART_DISCARD_LOG_SIZE)
            {
                _discardedLog[_discardedLogSize] = value;
                _discardedLogSize = _discardedLogSize + 1;
            }
        }
        volatile unsigned long _lastDiscarded = 0;

        volatile RxState _state = RX_IDLE;
        // The first octet of a U_SystemStat.ind came while our frame awaited its
        // confirmation; the next octet is its value. (processSearchBufferAcknowledge)
        bool _sysStatValuePending = false;
        bool isSystemStateAnswer(const char value) const;
        // volatile bool _uReset = false;
        // volatile char _uState = 0;

        volatile size_t _awaitBytes = 1;
        volatile unsigned long _lastReceivedTime = 0;

        void processSearchBufferFrame();
        void processSearchBufferInvalid(int x);
        void processSearchBufferAcknowledge();
        void processTimeout();
        void processCompleteFrame(bool acknowledge = false);
        void processSearchBufferTimeout();

        bool sufficientlyBytes();
        bool pushSearchBuffer(const char value);
        void processControlBytes();

      public:
        volatile bool _invalid = false;
        bool test = false;
        // L_DATA_CONs the control-byte decoder took because no echo was waiting for
        // them (each one a confirmation that left the echo path), and of those, the
        // ones that came while no frame was in flight. Since boot, for the status page.
        volatile uint32_t _orphanCons = 0;
        volatile uint32_t _orphanConsIdle = 0;
        // State probes (U_STATE_REQ) written since the last octet the chip sent. A
        // bus-powered chip falls silent with the bus, so this rising is how a cut
        // shows. Raised by requestState() and cleared here, both under rxLock.
        volatile uint8_t _unansweredProbes = 0;
        Receiver(DataLinkLayer &dll);

        void process();
        bool processReceviedByte();
        void processSearchBuffer();
        void reset();

        unsigned short getAwaitBytes();
        unsigned short getSearchBufferPosition();

#ifdef TPUART_CON_DIAG
        // Bench only: the raw bytes ahead of every L_DATA_CON that the control-byte
        // decoder took because no transmitted frame was waiting for it -- each one
        // is a confirmation the stack above never saw.
        static constexpr uint8_t DIAG_HIST = 16;
        static constexpr uint8_t DIAG_SNAPS = 4;
        uint8_t _diagHist[DIAG_HIST] = {};
        uint8_t _diagGap[DIAG_HIST] = {};
        uint8_t _diagHead = 0;
        unsigned long _diagLastAt = 0;
        volatile uint32_t _diagDataCons = 0;
        volatile uint32_t _diagOrphanCons = 0;
        volatile uint32_t _diagAcknOther = 0;
        volatile uint32_t _diagAcknTimeout = 0;
        volatile uint32_t _diagStateTaken = 0;
        uint8_t _diagOtherVal[DIAG_SNAPS] = {};
        uint8_t _diagSnap[DIAG_SNAPS][DIAG_HIST * 2] = {};
        uint8_t _diagSnapTx[DIAG_SNAPS] = {};
        volatile uint32_t _diagSnapCount = 0;
#endif
    };
} // namespace TPUart