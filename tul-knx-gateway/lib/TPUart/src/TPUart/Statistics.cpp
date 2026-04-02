#include "TPUart/Statistics.h"
#include <Arduino.h>

namespace TPUart
{
    Statistics::Statistics()
    {
        reset();
    }

    void Statistics::reset()
    {
        _rxDiscardedBytes = 0;
        _rxFrameBufferOverflow = 0;
        _rxFrames = 0;
        _rxFrameBytes = 0;
        _rxLastBusBytes = 0;
        _rxLastBusLoadTime = 0;
        _rxOverflowFrameBuffer = 0;
        _rxOverflowInterface = 0;
        _rxOverflowSearchBuffer = 0;
        _rxReceivedBytes = 0;
        _rxSearchBufferOverflow = 0;
        _rxUartOverflow = 0;
        _txFrames = 0;
        _txOverflowFrameBuffer = 0;
        _rxControlBytes = 0;
        _rxRepetitions = 0;
    }

    void Statistics::incrementRxRepetitions(int increment /* = 1 */)
    {
        _rxRepetitions += increment;
    }

    void Statistics::incrementRxFrameBytes(int increment /* = 1 */)
    {
        _rxFrameBytes += increment;
    }

    void Statistics::incrementRxOverflowFrameBuffer(int increment /* = 1 */)
    {
        _rxOverflowFrameBuffer += increment;
    }

    void Statistics::incrementRxOverflowInterface(int increment /* = 1 */)
    {
        _rxOverflowInterface += increment;
    }

    void Statistics::incrementRxOverflowSearchBuffer(int increment /* = 1 */)
    {
        _rxOverflowSearchBuffer += increment;
    }

    void Statistics::incrementTxOverflowFrameBuffer(int increment /* = 1 */)
    {
        _txOverflowFrameBuffer += increment;
    }

    void Statistics::incrementTxFrames(int increment /* = 1 */)
    {
        _txFrames += increment;
    }

    void Statistics::incrementRxFrames(int increment /* = 1 */)
    {
        _rxFrames += increment;
    }

    // void Statistics::incrementRxControlBytes(int increment /* = 1 */)
    // {
    //     _rxControlBytes += increment;
    // }

    void Statistics::incrementRxDiscardedBytes(int increment /* = 1 */)
    {
        _rxDiscardedBytes += increment;
    }

    void Statistics::incrementRxReceivedBytes(int increment /* = 1 */)
    {
        _rxReceivedBytes += increment;
    }

    void Statistics::incrementRxSearchBufferOverflow(int increment /* = 1 */)
    {
        _rxSearchBufferOverflow += increment;
    }

    void Statistics::incrementRxFrameBufferOverflow(int increment /* = 1 */)
    {
        _rxFrameBufferOverflow += increment;
    }

    void Statistics::incrementRxUartOverflow(int increment /* = 1 */)
    {
        _rxUartOverflow += increment;
    }

    unsigned int Statistics::getRxRepetitions()
    {
        return _rxRepetitions;
    }

    unsigned int Statistics::getRxFrameBytes()
    {
        return _rxFrameBytes;
    }

    unsigned int Statistics::getTxOverflowFrameBuffer()
    {
        return _txOverflowFrameBuffer;
    }

    unsigned int Statistics::getRxOverflowFrameBuffer()
    {
        return _rxOverflowFrameBuffer;
    }

    unsigned int Statistics::getRxOverflowInterface()
    {
        return _rxOverflowInterface;
    }

    unsigned int Statistics::getRxOverflowSearchBuffer()
    {
        return _rxOverflowSearchBuffer;
    }

    unsigned int Statistics::getTxFrames()
    {
        return _txFrames;
    }

    unsigned int Statistics::getRxBusBytes()
    {
        return getRxFrameBytes() + getRxDiscardedBytes(); //+ (getRxFrames() * 4);
    }

    // unsigned int Statistics::getRxControlBytes()
    // {
    //     return _rxControlBytes;
    // }

    unsigned int Statistics::getRxFrames()
    {
        return _rxFrames;
    }

    unsigned int Statistics::getRxDiscardedBytes()
    {
        return _rxDiscardedBytes;
    }

    unsigned int Statistics::getRxReceivedBytes()
    {
        return _rxReceivedBytes;
    }

    unsigned int Statistics::getRxSearchBufferOverflow()
    {
        return _rxSearchBufferOverflow;
    }

    unsigned int Statistics::getRxFrameBufferOverflow()
    {
        return _rxFrameBufferOverflow;
    }

    unsigned int Statistics::getRxUartOverflow()
    {
        return _rxUartOverflow;
    }

    unsigned int Statistics::getBusLoad()
    {
        unsigned long currentTime = millis();
        unsigned int timeDiff = currentTime - _rxLastBusLoadTime;
        if (timeDiff == 0) return 0; // Avoid division by zero

        // Calculate bus load as the number of bytes received per second
        const unsigned int currentBytes = getRxBusBytes();
        const unsigned int load = ((currentBytes - _rxLastBusBytes) * 1000 + timeDiff - 1) / timeDiff;
        _rxLastBusLoadTime = currentTime;
        _rxLastBusBytes = currentBytes;

        return load;
    }

} // namespace TPUart
