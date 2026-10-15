#pragma once

#include "config.h"
#if MASK_VERSION == 0x091A

#include "bau_systemB_coupler.h"
#include "router_object.h"
#include "ip_parameter_object.h"
#include "ip_data_link_layer.h"
#include "tpuart_data_link_layer.h"
#include "cemi_server_object.h"

class Bau091A : public BauSystemBCoupler, public ITpUartCallBacks, public DataLinkLayerCallbacks
{
  public:
    Bau091A(Platform& platform);
    void loop() override;
    bool enabled() override;
    void enabled(bool value) override;
    bool configured() override;

    IpDataLinkLayer* getPrimaryDataLinkLayer();
    TpUartDataLinkLayer* getSecondaryDataLinkLayer();
    // Whether the IP network can carry KNXnet/IP, for PID_KNXNETIP_DEVICE_STATE.
    void ipNetworkUp(bool up) { _ipNetworkUp = up; }
    // What PID 72 to 75 read.
    const KnxIpCounters& counters() const { return _counters; }
  protected:
    InterfaceObject* getInterfaceObject(uint8_t idx);
    InterfaceObject* getInterfaceObject(ObjectType objectType, uint16_t objectInstance);

    // For TP1 only
    TPAckType isAckRequired(uint16_t address, bool isGrpAddr) override;

    void networkParameterReadIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint16_t objectType,
                                        uint8_t propertyId, const uint8_t* testInfo, uint16_t testInfoLength) override;

    void doMasterReset(EraseCode eraseCode, uint8_t channel) override;
  private:
    KnxIpCounters _counters; // first: the parameter object and both data link layers keep its address
    RouterObject _routerObj;
    IpParameterObject _ipParameters;
    IpDataLinkLayer _dlLayerPrimary;
    TpUartDataLinkLayer _dlLayerSecondary;
#ifdef USE_CEMI_SERVER
    CemiServer _cemiServer;
    CemiServerObject _cemiServerObject;
#endif
    bool _ipNetworkUp = true;
};
#endif
