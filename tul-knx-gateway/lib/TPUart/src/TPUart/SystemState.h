#pragma once
#include <string>

namespace TPUart
{
    class SystemState
    {
      private:
        volatile char _state = 0;
        volatile bool _dirty = false;

      public:
        bool v20v();
        bool vdd2();
        bool vbus();
        bool vfilt();
        bool xtal();
        bool thermalWarning();
        char mode();
        const char * modeString();
        std::string print();
        bool normalMode();
        bool stopMode();
        bool syncMode();
        bool powerupMode();

        void update(char state);
        bool dirty();
    };

} // namespace TPUart
