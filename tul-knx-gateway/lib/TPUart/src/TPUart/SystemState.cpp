#include "TPUart/SystemState.h"

#define SYSTEM_STATE_V20V 0x80
#define SYSTEM_STATE_VDD2 0x40
#define SYSTEM_STATE_VBUS 0x20
#define SYSTEM_STATE_VFILT 0x10
#define SYSTEM_STATE_XTAL 0x08
#define SYSTEM_STATE_TW 0x04
#define SYSTEM_STATE_MODE 0x03
#define SYSTEM_STATE_MODE_NORMAL 0x33
#define SYSTEM_STATE_MODE_STOP 0x02
#define SYSTEM_STATE_MODE_SYNC 0x01
#define SYSTEM_STATE_MODE_POWERUP 0x00

namespace TPUart
{
    /*
     * update current state
     */
    void SystemState::update(char state)
    {
        if (_state != state) _dirty = true;

        _state = state;
    }

    /*
     * V20V linear voltage regulator is within normal operating range
     */
    bool SystemState::v20v()
    {
        return _state & SYSTEM_STATE_V20V;
    }

    /*
     * DC2 regulator is within normal operating range
     */
    bool SystemState::vdd2()
    {
        return _state & SYSTEM_STATE_VDD2;
    }

    /*
     * KNX bus voltage is within normal operating range
     */
    bool SystemState::vbus()
    {
        return _state & SYSTEM_STATE_VBUS;
    }

    /*
     * voltage on tank capacitor is within normal operating range State Service
     */
    bool SystemState::vfilt()
    {
        return _state & SYSTEM_STATE_VFILT;
    }

    /*
     * crystal oscillator frequency is within normal operating range
     */
    bool SystemState::xtal()
    {
        return _state & SYSTEM_STATE_XTAL;
    }

    /*
     * crystal oscillator frequency is within normal operating range
     */
    bool SystemState::thermalWarning()
    {
        return _state & SYSTEM_STATE_TW;
    }

    /*
     * Operation mode
     *
     * 0b00000000 PowerUP
     * 0b00000001 Sync
     * 0b00000010 Stop
     * 0b00000011 Normal
     */
    char SystemState::mode()
    {
        return _state & SYSTEM_STATE_MODE;
    }

    /*
     * Deliver the current operation mode as string
     */
    const char *SystemState::modeString()
    {
        switch (mode())
        {
            case 0x03:
                return "Normal";
            case 0x02:
                return "Stop";
            case 0x01:
                return "Sync";
            default:
                return "Power-UP";
        }
    }

    std::string SystemState::print()
    {
        std::string message = "SystemState: ";
        message += modeString();
        message += " (";
        if (v20v()) message += " V20V";
        if (vdd2()) message += " VDD2";
        if (vbus()) message += " VBUS";
        if (vfilt()) message += " VFILT";
        if (xtal()) message += " XTAL";
        if (thermalWarning()) message += " TW";
        message += " )";
        return message;
    }

    /*
     * Operation mode: Normal
     */
    bool SystemState::normalMode()
    {
        return mode() == SYSTEM_STATE_MODE_NORMAL;
    }

    bool SystemState::stopMode()
    {
        return mode() == SYSTEM_STATE_MODE_STOP;
    }

    bool SystemState::syncMode()
    {
        return mode() == SYSTEM_STATE_MODE_SYNC;
    }

    bool SystemState::powerupMode()
    {
        return mode() == SYSTEM_STATE_MODE_POWERUP;
    }

    bool SystemState::dirty()
    {
        bool dirty = _dirty;
        _dirty = false;
        return dirty;
    }

} // namespace TPUart
