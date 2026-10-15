#include "knx_ip_tunnel_connection.h"
#include <stdlib.h>

uint32_t KnxIpTunnelConnection::QueuedBytes = 0;

KnxIpTunnelConnection::KnxIpTunnelConnection()
{

}

void KnxIpTunnelConnection::TxClear()
{
    for (uint8_t i = 0; i < TxCount; i++)
    {
        const uint8_t idx = (TxFirst + i) % KNX_TUNNEL_QUEUE_DEPTH;
        free(TxData[idx]);
        QueuedBytes -= TxLen[idx];
        TxData[idx] = nullptr;
        TxLen[idx] = 0;
    }
    TxFirst = 0;
    TxCount = 0;
    TxOnWire = false;
    TxRepeats = 0;
}

void KnxIpTunnelConnection::Reset()
{
    print("Close Tunnel-Connection[");
    print("?");
    print("], Channel: 0x");
    println(ChannelId, 16);

    TxClear();
    ChannelId = 0;
    IpAddress = 0;
    PeerAddress = 0;
    PortData = 0;
    PortCtrl = 0;
    lastHeartbeat = 0;
    SequenceCounter_S = 0;
    SequenceCounter_R = 255;
    IndividualAddress = 0;
    IsConfig = false;
    StatRequests = 0;
    StatAcks = 0;
    StatResends = 0;
    StatDrops = 0;
    StatRepeats = 0;
    StatOutOfOrder = 0;
    StatQueuePeak = 0;
}
