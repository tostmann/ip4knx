#pragma once
#include "config.h"
#include "platform.h"
#include "bits.h"

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

    // Counted for the status page; nothing here changes how the channel works.
    uint32_t StatRequests = 0;     // TUNNELLING_ / DEVICE_CONFIGURATION_REQUESTs sent
    uint32_t StatAcks = 0;         // their acks received
    uint32_t StatRepeats = 0;      // client requests received again (our ack lost or late)
    uint32_t StatOutOfOrder = 0;   // client requests with an unexpected sequence number
    uint8_t LastSentSeq = 0;       // the newest request sent, timed until its ack
    unsigned long LastSentAt = 0;
    bool AckPending = false;
    int16_t SequenceCounter_DC = -1; // last DEVICE_CONFIGURATION_REQUEST, -1 = none yet

    void Reset();

  private:

};