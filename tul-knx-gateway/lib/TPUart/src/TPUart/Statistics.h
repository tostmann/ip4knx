#pragma once

namespace TPUart
{
    class DataLinkLayer;

    class Statistics
    {
      private:
        volatile unsigned int _rxControlBytes;
        volatile unsigned int _rxDiscardedBytes;
        volatile unsigned int _rxFrameBufferOverflow;
        volatile unsigned int _rxFrames;
        volatile unsigned int _rxFrameBytes;
        volatile unsigned int _rxLastBusBytes;
        volatile unsigned long _rxLastBusLoadTime;
        volatile unsigned int _rxOverflowFrameBuffer;
        volatile unsigned int _rxOverflowInterface;
        volatile unsigned int _rxOverflowSearchBuffer;
        volatile unsigned int _rxReceivedBytes;
        volatile unsigned int _rxSearchBufferOverflow;
        volatile unsigned int _rxUartOverflow;
        volatile unsigned int _txFrames;
        volatile unsigned int _txOverflowFrameBuffer;
        volatile unsigned int _rxRepetitions;

      public:
        Statistics();

        void reset();

        void incrementRxOverflowFrameBuffer(int increment = 1);
        void incrementRxOverflowInterface(int increment = 1);
        void incrementRxOverflowSearchBuffer(int increment = 1);
        void incrementRxRepetitions(int increment = 1);
        void incrementRxFrames(int increment = 1);
        void incrementRxFrameBytes(int increment = 1);
        // void incrementRxControlBytes(int increment = 1);
        void incrementRxDiscardedBytes(int increment = 1);
        void incrementRxReceivedBytes(int increment = 1);
        void incrementRxSearchBufferOverflow(int increment = 1);
        void incrementRxFrameBufferOverflow(int increment = 1);
        void incrementRxUartOverflow(int increment = 1);
        void incrementTxOverflowFrameBuffer(int increment = 1);
        void incrementTxFrames(int increment = 1);

        unsigned int getRxRepetitions();
        unsigned int getRxFrameBytes();
        unsigned int getTxFrames();
        unsigned int getRxFrames();
        unsigned int getRxBusBytes();
        // unsigned int getRxControlBytes();
        unsigned int getRxDiscardedBytes();
        unsigned int getRxReceivedBytes();
        unsigned int getRxSearchBufferOverflow();
        unsigned int getRxFrameBufferOverflow();
        unsigned int getRxUartOverflow();
        unsigned int getBusLoad();
        unsigned int getTxOverflowFrameBuffer();
        unsigned int getRxOverflowFrameBuffer();
        unsigned int getRxOverflowInterface();
        unsigned int getRxOverflowSearchBuffer();
    };

} // namespace TPUart
