#pragma once
#include <functional>

namespace TPUart
{
    namespace Interface
    {
        class Abstract
        {
          protected:
            bool _running = false;

          public:
            virtual void flush() = 0;
            virtual void begin(int baud) = 0;
            virtual void end() = 0;
            virtual bool available() = 0;
            virtual bool availableForWrite() = 0;
            virtual bool write(char value) = 0;
            virtual int read() = 0;
            virtual bool overflow() { return false; };
            virtual bool hasCallback() { return false; }
            virtual void registerCallback(std::function<bool()> callback) {}
        };
    } // namespace Interface
} // namespace TPUart