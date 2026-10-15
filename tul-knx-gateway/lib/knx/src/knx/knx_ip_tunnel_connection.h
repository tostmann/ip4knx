#pragma once
#include "config.h"
#include "platform.h"
#include "bits.h"

// Requests to a client wait in a queue per connection, one of them on the wire
// until the client acknowledges it (03_08_04 2.6.1, 03_08_03 2.3.2). Frames per
// connection, the one on the wire included, and octets over all connections.
#ifndef KNX_TUNNEL_QUEUE_DEPTH
#define KNX_TUNNEL_QUEUE_DEPTH 16
#endif
#ifndef KNX_TUNNEL_QUEUE_BYTES
#define KNX_TUNNEL_QUEUE_BYTES 16384
#endif

class KnxIpTunnelConnection
{
  public:
    KnxIpTunnelConnection();
    uint8_t ChannelId = 0;
    uint16_t IndividualAddress = 0;
    uint32_t IpAddress = 0;
    uint32_t PeerAddress = 0;   // UDP source of the CONNECT_REQUEST; only it may use the channel
    uint16_t PortData = 0;
    uint16_t PortCtrl = 0;
    uint8_t SequenceCounter_S = 0;
    uint8_t SequenceCounter_R = 255;
    unsigned long lastHeartbeat = 0;
    bool IsConfig = false;

    // The queue, oldest first. Each entry is the whole datagram, so a repeat
    // sends the same octets; the sequence counter goes in when it is first sent.
    uint8_t* TxData[KNX_TUNNEL_QUEUE_DEPTH] = {};
    uint16_t TxLen[KNX_TUNNEL_QUEUE_DEPTH] = {};
    bool TxDroppable[KNX_TUNNEL_QUEUE_DEPTH] = {}; // a group telegram, given up first when full
    uint8_t TxFirst = 0;
    uint8_t TxCount = 0;
    bool TxOnWire = false;          // the oldest was sent and waits for its ack
    uint8_t TxRepeats = 0;
    unsigned long TxSentAt = 0;     // its last (re)send
    unsigned long TxFirstSentAt = 0;
    static uint32_t QueuedBytes;    // all connections together

    // Counted for the status page.
    uint32_t StatRequests = 0;     // TUNNELLING_ / DEVICE_CONFIGURATION_REQUESTs sent
    uint32_t StatAcks = 0;         // their acks received
    uint32_t StatResends = 0;      // requests sent again for a missing ack
    uint32_t StatDrops = 0;        // frames a full queue gave up
    uint32_t StatRepeats = 0;      // client requests received again (our ack lost or late)
    uint32_t StatOutOfOrder = 0;   // client requests with an unexpected sequence number
    uint8_t StatQueuePeak = 0;

    void Reset();
    // Frees the queue; Reset() does it as well.
    void TxClear();

  private:

};
