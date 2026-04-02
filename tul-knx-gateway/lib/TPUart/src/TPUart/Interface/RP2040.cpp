#include "TPUart/Interface/RP2040.h"
#ifdef ARDUINO_ARCH_RP2040

std::function<void(void)> __tpuartInterrupt0Callback;
std::function<void(void)> __tpuartInterrupt1Callback;

void __time_critical_func(__tpuartInterrupt0)()
{
    __tpuartInterrupt0Callback();
}

void __time_critical_func(__tpuartInterrupt1)()
{
    __tpuartInterrupt1Callback();
}

namespace TPUart
{
    namespace Interface
    {
        RP2040::RP2040(pin_size_t rx, pin_size_t tx, uart_inst_t *uart, bool irq, bool dma) : _rx(rx), _tx(tx), _uart(uart), _irq(irq), _dma(dma)
        {
            if (_dma)
            {
                _dmaChannel = dma_claim_unused_channel(true); // get free channel for dma
                _dmaConfig = dma_channel_get_default_config(_dmaChannel);
                channel_config_set_transfer_data_size(&_dmaConfig, DMA_SIZE_8);
                channel_config_set_read_increment(&_dmaConfig, false);
                channel_config_set_write_increment(&_dmaConfig, true);
                channel_config_set_high_priority(&_dmaConfig, true);
                channel_config_set_ring(&_dmaConfig, true, TPUART_RP2040_BUFFER_EXP);
                if (_uart == uart0) channel_config_set_dreq(&_dmaConfig, DREQ_UART0_RX);
                if (_uart == uart1) channel_config_set_dreq(&_dmaConfig, DREQ_UART1_RX);
                dma_channel_set_read_addr(_dmaChannel, &uart_get_hw(_uart)->dr, false);
                dma_channel_set_trans_count(_dmaChannel, TPUART_RP2040_TRANSFER_COUNT, false);
                dma_channel_set_config(_dmaChannel, &_dmaConfig, false);
            }

            _txRestore = gpio_get_function(_tx);
            _rxRestore = gpio_get_function(_rx);

            gpio_set_function(_rx, GPIO_FUNC_UART);
            gpio_set_function(_tx, GPIO_FUNC_UART);
        }

        RP2040::~RP2040()
        {
            end();

            if (_dma)
            {
                dma_channel_abort(_dmaChannel);
                dma_channel_cleanup(_dmaChannel);
                dma_channel_unclaim(_dmaChannel);
            }

            gpio_set_function(_rx, _rxRestore);
            gpio_set_function(_tx, _txRestore);
        }

        void RP2040::begin(int baud)
        {
            if (_running) end();
            uart_init(_uart, baud);
            uart_set_format(_uart, 8, 1, UART_PARITY_EVEN);
            uart_set_hw_flow(_uart, false, false);
            uart_set_fifo_enabled(_uart, !_dma); // enabled if not using DMA

            if (_irq)
            {
                if (_uart == uart0)
                {
                    __tpuartInterrupt0Callback = std::bind(&RP2040::interrupt, this);
                    irq_set_exclusive_handler(UART0_IRQ, __tpuartInterrupt0);
                    irq_set_enabled(UART0_IRQ, true);
                }

                if (_uart == uart1)
                {
                    __tpuartInterrupt1Callback = std::bind(&RP2040::interrupt, this);
                    irq_set_exclusive_handler(UART1_IRQ, __tpuartInterrupt1);
                    irq_set_enabled(UART1_IRQ, true);
                };

                uart_set_irqs_enabled(_uart, true, false);
            }

            if (_dma)
            {
                _dmaReaderCount = 0;
                dma_channel_set_write_addr(_dmaChannel, _dmaBuffer, true);
            }
            _running = true;
        }

        void RP2040::end()
        {
            if (!_running) return;
            _running = false;

            if (_dma)
            {
                dma_channel_abort(_dmaChannel);
            }

            if (_irq)
            {
                uart_set_irqs_enabled(_uart, false, false);
            }

            uart_deinit(_uart);
        }

        uint RP2040::dmaTransferCount()
        {
            return TPUART_RP2040_TRANSFER_COUNT - dma_channel_hw_addr(_dmaChannel)->transfer_count;
        }

        uint RP2040::dmaReaderCount()
        {
            return _dmaReaderCount;
        }

        bool RP2040::available()
        {
            if (!_running) return false;
            if (!_dma) return uart_is_readable(_uart);

            return (dmaTransferCount() - _dmaReaderCount) > 0;
        }

        bool RP2040::availableForWrite()
        {
            if (!_running) return false;
            return uart_is_writable(_uart);
        }

        bool RP2040::write(char value)
        {
            if (!_running) return false;
            uart_tx_wait_blocking(_uart);
            uart_putc_raw(_uart, value);
            return true;
        }

        int RP2040::read()
        {
            // if (!_running) return false; // not necessary, because available() is already checking this
            if (!available()) return -1;
            if (!_dma) return uart_getc(_uart);

            // when transfer count and read counter too far apart, then reset the read counter to current transfer count - 1
            if (overflow()) _dmaReaderCount = dmaTransferCount() - 1;

            int ret = _dmaBuffer[_dmaReaderCount++ % TPUART_RP2040_BUFFER_SIZE];

            // is the dma channel expired and last char, then restart the dma channel.
            if (!available() && !dma_channel_is_busy(_dmaChannel))
            {
                _dmaReaderCount = 0;
                dma_channel_abort(_dmaChannel);
                dma_channel_set_write_addr(_dmaChannel, _dmaBuffer, true);
            }

            return ret;
        }

        bool RP2040::overflow()
        {
            if (!_running) return false;
            if (!_dma) return (uart_get_hw(_uart)->fr & 0x00000040);

            return dmaTransferCount() - dmaReaderCount() > TPUART_RP2040_BUFFER_SIZE;
        };

        bool RP2040::hasCallback()
        {
            return _irq;
        }

        void RP2040::registerCallback(std::function<bool()> callback)
        {
            _callback = callback;
        }

        void RP2040::flush()
        {
            if (!_running) return;

            while (available())
                read();
        }

        void RP2040::interrupt()
        {

            // if a callback is registered
            if (_callback)
            {
                // Call the callback function
                _callback();
                // call a second time, as it is not always guaranteed that the callback is called for each character, especially when using DMA
                _callback();
            }

            // reset the interrupt, except when DMA is used as it is automatically reset
            if (!_dma) uart_get_hw(_uart)->icr = UART_UARTICR_RTIC_BITS | UART_UARTICR_RXIC_BITS; // clear interrupt
        }
    } // namespace Interface
} // namespace TPUart

#endif