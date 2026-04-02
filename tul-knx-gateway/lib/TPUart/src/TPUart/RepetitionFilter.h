#include "TPUart/Frame.h"
#include <Arduino.h>
#include <cstdint>
#include <list>
#include <unordered_map>

// #define TPUART_REPETITION_SIMPLE

namespace TPUart
{
    struct RepetitionFilterEntry
    {
        unsigned short checksum;
        unsigned long timestamp;
    };

    class RepetitionFilter
    {
      public:
        bool check(Frame &frame);
        void cleanup();
        void clear();
        std::size_t size();

      private:
        unsigned short crc(Frame &frame);
        unsigned long _avg = 0;
        unsigned long _avgCnt = 0;

#ifdef TPUART_REPETITION_SIMPLE
        // fast lookup: Source -> Checksum
        std::unordered_map<uint16_t, RepetitionFilterEntry> _entries;
#else
        static constexpr std::size_t MAX_SIZE = 50;
        std::list<std::pair<uint16_t, uint16_t>> _entries;
        std::unordered_map<uint16_t, std::list<std::pair<uint16_t, uint16_t>>::iterator> _map;
#endif
    };
} // namespace TPUart