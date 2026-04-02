#pragma GCC optimize("O3")
#include "TPUart/RingBuffer.h"

namespace TPUart
{

    // Konstruktor: Initialisiert head und tail
    RingBuffer::RingBuffer() : head(0), tail(0) {}

    // Fügt ein Zeichen in den Puffer ein.
    // Prüft zunächst, ob der Puffer voll ist, indem er vergleicht,
    // ob der nächste Index von head gleich tail ist.
    bool RingBuffer::push(char data)
    {
        size_t next = (head + 1) % TPUART_RX_FRAME_BUFFER_SIZE1;
        if (next == tail)
        {
            // Puffer ist voll (nur TPUART_RX_FRAME_BUFFER_SIZE1 - 1 Elemente können gespeichert werden)
            return false;
        }
        buffer[head] = data;

        asm volatile("" ::: "memory");
        head = next;
        return true;
    }

    // Liest ein Zeichen aus dem Puffer aus.
    // Falls der Puffer leer ist (head == tail), wird false zurückgegeben.
    bool RingBuffer::pop(char &data)
    {
        if (isEmpty())
        {
            return false;
        }
        data = buffer[tail];
        asm volatile("" ::: "memory");
        tail = (tail + 1) % TPUART_RX_FRAME_BUFFER_SIZE1;
        return true;
    }

    char RingBuffer::pop()
    {
        if (isEmpty())
        {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
            throw std::out_of_range("Buffer is empty");
#endif
            return 0;
        }
        char value = buffer[tail];
        asm volatile("" ::: "memory");
        tail = (tail + 1) % TPUART_RX_FRAME_BUFFER_SIZE1;
        return value;
    }

    // Der Puffer ist leer, wenn head und tail gleich sind.
    bool RingBuffer::isEmpty() const
    {
        return head == tail;
    }

    // Der Puffer gilt als voll, wenn der nächste Wert von head gleich tail ist.
    bool RingBuffer::isFull() const
    {
        return ((head + 1) % TPUART_RX_FRAME_BUFFER_SIZE1) == tail;
    }

    // Liefert die Anzahl der freien Slots im Puffer.
    // Mit diesem Ansatz (einen Slot opfern) ist die maximale Anzahl
    // der einfügbaren Elemente (TPUART_RX_FRAME_BUFFER_SIZE1 - 1).
    size_t RingBuffer::available() const
    {
        if (head >= tail)
            return TPUART_RX_FRAME_BUFFER_SIZE1 - (head - tail) - 1;
        else
            return tail - head - 1;
    }

    // Liefert die Anzahl der Elemente im Puffer.
    // Dabei wird ein Slot geopfert, um den Zustand "voll" zu erkennen.
    size_t RingBuffer::size() const
    {
        return TPUART_RX_FRAME_BUFFER_SIZE1 - available() - 1;
    }

    void RingBuffer::clear()
    {
        head = 0;
        tail = 0;
    }

} // namespace TPUart
