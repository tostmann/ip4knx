#pragma GCC optimize("O3")
#include "TPUart/SearchBuffer.h"
#include <Arduino.h>
#include <cstring>

namespace TPUart
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
    SearchBuffer::SearchBuffer() : _frame(_buffer)
    {
        memset(_buffer, 0, TPUART_RX_SEARCH_BUFFER_SIZE);
    }
#pragma GCC diagnostic pop

    Frame &SearchBuffer::frame()
    {
        return _frame;
    }

    bool SearchBuffer::available()
    {
        return _position < TPUART_RX_SEARCH_BUFFER_SIZE;
    }

    bool SearchBuffer::add(const char value)
    {
        if (!available()) return false;
        _buffer[_position] = value;
        _position = _position + 1; // ++ not allowed on volatile
        return true;
    }

    void SearchBuffer::clear()
    {
        _position = 0;
        _timeout = 0;
    }

    bool SearchBuffer::move(const size_t size)
    {
        if (_position < size) return false;
        if (_position - size > 0) memmove(_buffer, _buffer + size, _position - size);
        _position = _position - size;
        _timeout = (size >= _timeout) ? 0 : _timeout - size;
        return true;
    }

    const char *SearchBuffer::get()
    {
        return _buffer;
    }

    const char SearchBuffer::get(size_t position)
    {
        return _buffer[position];
    }

    const size_t SearchBuffer::position()
    {
        return _position;
    }

    const size_t SearchBuffer::timeout()
    {
        return _timeout;
    }

    bool SearchBuffer::empty()
    {
        return !_position;
    }

    void SearchBuffer::timeout(size_t position)
    {
        _timeout = position;
    }

}; // namespace TPUart
