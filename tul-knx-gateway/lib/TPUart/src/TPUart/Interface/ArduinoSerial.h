#pragma once
#include <Arduino.h>

#include "TPUart/Interface/Abstract.h"

namespace TPUart
{
    namespace Interface
    {
        template <class T>
        class ArduinoSerial : public Abstract
        {
          private:
            T &_serial;

          public:
            ArduinoSerial(T &serial) : _serial(serial) {};
            ~ArduinoSerial()
            {
                end();
            }

            void begin(int baud) override
            {
                if (_running) end();
                _serial.begin(baud, SERIAL_8E1);
                _running = true;
            };

            void end() override
            {
                if (!_running) return;
                _running = false;
                _serial.end();
            };

            bool available() override
            {
                if (!_running) return false;
                return _serial.available();
            };

            bool availableForWrite() override
            {
                if (!_running) return false;
                return _serial.availableForWrite();
            };

            bool write(char value) override
            {
                if (!_running) return false;
                return !_serial.write(value);
            };

            int read() override
            {
                if (!available()) return -1;
                return _serial.read();
            };

            bool overflow() override
            {
                if (!_running) return false;

#ifdef ARDUINO_ARCH_RP2040
                return _serial.overflow();
#endif

                return false;
            };

            void flush() override
            {
                if (!_running) return;

                _serial.flush();
            };
        };

    } // namespace Interface
} // namespace TPUart
