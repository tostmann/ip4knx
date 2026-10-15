#pragma once
#include "TPUart/Frame.h"
#include "TPUart/Types.h"
#include <queue>

namespace TPUart
{
    class DataLinkLayer;

    class Transmitter
    {
        DataLinkLayer &_dll;
        // An acknowledge the receive task could not write because txLock was taken:
        // the U_ACK_REQ octet in bits 0-7, the time it was parked in 64 us ticks in
        // bits 8-31, in one word so the main loop reads both consistently.
        volatile uint32_t _pendingAcknowledge;
        size_t _transmitPos;
        unsigned char _transmitOffset; // NCN5130 keeps the data-index offset until it is changed (DS p.42)
        volatile unsigned long _time;
        unsigned long _maxQueueSize;
        volatile TxState _state; // written by finalize() (RX path), read by the main-loop TX path -> matches Receiver::_state
        Frame *_frame = nullptr;

      public:
        std::queue<Frame *> _queue;
        // Frames a reset took away before their L_DATA_CON, in sending order, for
        // DataLinkLayer::reportDroppedFrames() to confirm negatively (main loop).
        std::queue<Frame *> _dropped;

        // Counted for the status page since boot; reset() keeps them. An acknowledge
        // is parked when the receive task finds txLock taken; the main loop writes it
        // while its bus window lasts and drops it after (flushAcknowledge()).
        volatile uint32_t _ackParked = 0;        // parks, by the receive task
        volatile uint32_t _ackParkReplaced = 0;  // a park overwritten before its write
        volatile uint32_t _ackParkWritten = 0;   // parks written, by the main loop
        volatile uint32_t _ackParkAgeMaxUs = 0;  // longest park before its write
        volatile uint32_t _ackParkDropped = 0;   // parks past their bus window, not written
        uint32_t _watchdogResets = 0;            // resets for a missing L_DATA_CON
        size_t _queuePeak = 0;                   // most frames queued at once
        Transmitter(DataLinkLayer &dll);
        ~Transmitter();

        bool transmit(const char *data, size_t size);
        void finalize();
        void processWatchdog();

        bool pushQueue(Frame *frame);
        size_t queueSize();
        void reset();
        void sendAcknowledge(AcknowledgeType acknowledge = ACK_None);
        void setQueueSize(unsigned long size);

        void processTransmitByte();
        Frame *currentFrame();
        bool isTransmitting();
        bool awaitResponse();
        void flushAcknowledge();
        void processQueue();
        void resetWatchdogTimer();
    };

} // namespace TPUart
