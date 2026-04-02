#pragma once
#include "TPUart/Frame.h"

#ifndef TPUART_RX_SEARCH_BUFFER_SIZE
#define TPUART_RX_SEARCH_BUFFER_SIZE 300 // 263 + 37 reserv for frame errors errors and control bytes
#endif

namespace TPUart
{
    class SearchBuffer
    {
      private:
        Frame _frame;
        char _buffer[TPUART_RX_SEARCH_BUFFER_SIZE] = {};
        volatile size_t _position = 0;
        volatile size_t _timeout = 0;

      public:
        SearchBuffer();

        bool available();
        bool add(const char value);
        void clear();
        bool move(const size_t size);
        const char *get();
        const size_t position();
        const size_t timeout();
        const char get(size_t position);
        void timeout(size_t position);
        Frame &frame();
        bool empty();
    };
} // namespace TPUart
