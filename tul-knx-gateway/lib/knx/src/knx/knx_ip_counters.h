#pragma once

#include <stdint.h>

// The routing statistics of 03_08_03 2.5.23-2.5.26, PID 72 to 75, kept by Bau091A
// and counted by both data link layers in the main loop. The two overflow counts do
// not wrap at their maximum (2.5.23, 2.5.24); none of the four does here.
class KnxIpCounters
{
  public:
    // A telegram for the IP network that did not get there: the routing queue was
    // full, the IP side was down, or the IP stack refused the datagram.
    void lostToIp() { add16(_overflowToIp, 1); }
    // A telegram for the KNX side that did not get there: the IP receive buffer or
    // the TP transmit queue had no room, the bus was not usable, or a transceiver
    // reset took it.
    void lostToKnx(uint32_t n = 1)
    {
        add16(_overflowToKnx, n);
        _lostToKnx += n;
    }
    // A ROUTING_INDICATION the IP stack took.
    void sentToIp() { add32(_transmitToIp); }
    // A telegram the KNX side acknowledged.
    void sentToKnx() { add32(_transmitToKnx); }

    uint16_t overflowToIp() const { return _overflowToIp; }   // PID 72
    uint16_t overflowToKnx() const { return _overflowToKnx; } // PID 73
    uint32_t transmitToIp() const { return _transmitToIp; }   // PID 74
    uint32_t transmitToKnx() const { return _transmitToKnx; } // PID 75
    // Every loss towards KNX since start-up, wrapping, for ROUTING_LOST_MESSAGE.
    uint32_t lostToKnxTotal() const { return _lostToKnx; }

  private:
    static void add16(uint16_t& value, uint32_t n)
    {
        const uint32_t sum = value + n;
        value = sum > 0xFFFF ? 0xFFFF : (uint16_t)sum;
    }
    static void add32(uint32_t& value)
    {
        if (value != 0xFFFFFFFF)
            value++;
    }

    uint16_t _overflowToIp = 0;
    uint16_t _overflowToKnx = 0;
    uint32_t _transmitToIp = 0;
    uint32_t _transmitToKnx = 0;
    uint32_t _lostToKnx = 0;
};
