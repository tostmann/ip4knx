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
        volatile char _cachedAcknowledge;
        size_t _transmitPos;
        volatile unsigned long _time;
        unsigned long _maxQueueSize;
        TxState _state;
        Frame *_frame = nullptr;

      public:
        std::queue<Frame *> _queue;
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
        void sendCachedAcknowledge();
        void processQueue();
        void resetWatchdogTimer();
    };

} // namespace TPUart
