#pragma once
#ifdef ARDUINO_ARCH_ESP32
#include "TPUart/Interface/Abstract.h"
#include <Arduino.h>

namespace TPUart
{
    namespace Interface
    {
        class ESP32 : public Abstract
        {
          private:
            int _rx, _tx;
            uart_port_t _uart;
            bool _dma = false;
            volatile bool _overflow = false;
            QueueHandle_t _taskQueue = nullptr;
            TaskHandle_t _taskHandle = nullptr;
            std::function<bool(void)> _callback;

          public:
            ESP32(int rx, int tx, uart_port_t uart);
            ~ESP32();

            static void runTask(void *interface);

            void begin(int baud) override;
            void end() override;
            bool available() override;
            bool availableForWrite() override;
            bool write(char value) override;
            int read() override;
            bool overflow() override;
            void flush() override;
            bool hasCallback() override;
            void registerCallback(std::function<bool()> callback) override;
        };
    } // namespace Interface
} // namespace TPUart

#endif
