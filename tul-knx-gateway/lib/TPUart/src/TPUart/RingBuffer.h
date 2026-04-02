#pragma once
#include <cstddef>
#include <stdexcept>
#include <vector>

#ifndef TPUART_RX_FRAME_BUFFER_SIZE
#define TPUART_RX_FRAME_BUFFER_SIZE 6000
#endif

#define TPUART_RX_FRAME_BUFFER_SIZE1 (TPUART_RX_FRAME_BUFFER_SIZE + 1)

namespace TPUart
{

    class RingBuffer
    {
      public:
        // Konstruktor: Initialisiert head und tail
        RingBuffer();

        // Fügt ein Zeichen in den Puffer ein.
        // Gibt true zurück, wenn das Zeichen eingefügt wurde,
        // false wenn der Puffer voll ist.
        bool push(char data);

        // Liest ein Zeichen aus dem Puffer aus.
        // Das gelesene Zeichen wird in 'data' gespeichert.
        // Gibt true zurück, wenn ein Zeichen vorhanden war,
        // false wenn der Puffer leer ist.
        bool pop(char &data);
        char pop();

        // Überprüft, ob der Puffer leer ist.
        bool isEmpty() const;

        // Überprüft, ob der Puffer voll ist.
        bool isFull() const;

        // Liefert die Anzahl der freien Slots im Puffer.
        size_t available() const;
        size_t size() const;

        // Setzt den Puffer zurück (alle Daten werden verworfen).
        void clear();

      private:
        // Statisch allokiertes Array für die Zeichen.
        // Mit 'volatile' deklariert, um Optimierungen zu vermeiden,
        // falls der Puffer auch in einem Interrupt verändert wird.
        volatile char buffer[TPUART_RX_FRAME_BUFFER_SIZE1];

        // head zeigt auf die nächste freie Position, tail auf die nächste lesbare.
        volatile size_t head;
        volatile size_t tail;
    };
} // namespace TPUart
