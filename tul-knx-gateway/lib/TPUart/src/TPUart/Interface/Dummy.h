#pragma once
#ifdef ARDUINO_ARCH_RP2040
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include <Arduino.h>

#include "TPUart/Interface/Abstract.h"

std::function<void(void)> __tpuartDummyCallback;

static bool __time_critical_func(__tpuartDummy)(struct repeating_timer *t)
{
    __tpuartDummyCallback();
    return true;
}

namespace TPUart
{
    namespace Interface
    {
        class Dummy : public Abstract
        {

          private:
            char _data[1024] = {};
            uint8_t _pause[1024] = {};
            volatile size_t _size;
            volatile size_t _pos = 0;
            volatile size_t _test = 0;
            volatile size_t _test2 = 0;
            volatile size_t _last = 0;
            volatile bool _first = true;

            std::function<void(void)> _callback;
            struct repeating_timer _timer;

            uint8_t charPause = 1;

          public:
            Dummy()
            {
                __tpuartDummyCallback = std::bind(&Dummy::interrupt, this);
            }

            void addData(const char *data, size_t size, uint16_t pause)
            {
                for (size_t i = 0; i < size; i++)
                {
                    _data[_size] = data[i];

                    _pause[_size] = charPause;
                    if (i == size - 1) _pause[_size] = pause;

                    _size++;
                }
            }

            void begin(int baud) override
            {
                _pos = 0;
                if (_callback) add_repeating_timer_ms(1, __tpuartDummy, NULL, &_timer);
            }

            void end() override
            {
                if (_callback) cancel_repeating_timer(&_timer);
            }

            bool available() override
            {
                if(_first) return true;
                if (millis() < _last) return false;
                return true;
            }

            bool availableForWrite() override
            {
                return true;
            }

            bool write(char value) override
            {
                return true;
            }

            int read() override
            {
                if(_first) {
                    _first = false;
                    _last = millis() + 50;
                    return U_RESET_IND;
                }
                if (!available()) return -1;
                size_t cur = _pos % _size;
                uint32_t pause = _pause[cur];
                char byte = _data[cur];
                _last = millis() + pause;
                _pos++;
                if( _pos % _size == 0) _last += 5000;
                // Serial.printf("Read %02X    %i\n", byte, pause);
                return byte;
            }

            bool overflow() override
            {
                return false;
            }

            void flush() override
            {
                _pos = 0;
            }

            bool hasCallback() override
            {
                return true;
            }

            void registerCallback(std::function<bool()> callback) override
            {
                _callback = callback;
            }

            void interrupt()
            {
                if (!available()) return;

                if (_callback) _callback();
            }
        };
    } // namespace Interface
} // namespace TPUart

#endif