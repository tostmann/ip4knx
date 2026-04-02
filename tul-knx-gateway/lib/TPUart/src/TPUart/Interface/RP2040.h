#pragma once
#ifdef ARDUINO_ARCH_RP2040
#include <Arduino.h>
#include <hardware/gpio.h>
#include <hardware/uart.h>

#include "TPUart/Interface/Abstract.h"

constexpr uint32_t TPUART_RP2040_TRANSFER_COUNT = UINT32_MAX >> 1; // aus irgendeinem Grund funktioniert UINT32_MAX nicht und ich musste den wert halbieren
// constexpr unsigned int TPUART_RP2040_TRANSFER_COUNT = 5000;
// constexpr unsigned int TPUART_RP2040_TRANSFER_COUNT = 256;
constexpr unsigned long TPUART_RP2040_BUFFER_EXP = 8; // 2**BufferExp
constexpr unsigned long TPUART_RP2040_BUFFER_SIZE = (1u << TPUART_RP2040_BUFFER_EXP);

namespace TPUart
{
    namespace Interface
    {
        class RP2040 : public Abstract
        {
          private:
            int _dmaChannel;
            dma_channel_config _dmaConfig;

            volatile uint8_t __attribute__((aligned(TPUART_RP2040_BUFFER_SIZE))) _dmaBuffer[TPUART_RP2040_BUFFER_SIZE] = {};
            volatile uint _dmaReaderCount = 0;
            volatile uint _dmaRestartDiff = 0;

            pin_size_t _rx, _tx;
            gpio_function_t _rxRestore, _txRestore;
            uart_inst_t *_uart;
            bool _irq;
            bool _dma;

            std::function<bool(void)> _callback;

          public:
            RP2040(pin_size_t rx, pin_size_t tx, uart_inst_t *uart, bool irq = false, bool dma = false);
            ~RP2040();
            void begin(int baud);
            void end();
            uint dmaTransferCount();
            uint dmaReaderCount();
            bool available() override;
            bool availableForWrite() override;
            bool write(char value) override;
            int read() override;
            bool overflow() override;
            bool hasCallback() override;
            void registerCallback(std::function<bool()> callback) override;
            void flush() override;
            void interrupt();
        };
    } // namespace Interface
} // namespace TPUart

#endif