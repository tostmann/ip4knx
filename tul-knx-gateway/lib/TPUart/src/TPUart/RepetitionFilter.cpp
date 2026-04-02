#include "RepetitionFilter.h"
#include <Arduino.h>

namespace TPUart
{
    unsigned short RepetitionFilter::crc(Frame &frame)
    {
        // CRC-16/SPI-FUJITSU
        unsigned short crc = 0x1D0F;
        unsigned short polynomial = 0x1021;

        const size_t s = frame.size() - 1; // without checksum
        for (size_t i = 0; i < s; i++)
        {
            char data = frame.data(i);
            if (i == 0) data = data | 0b100000; // retry bit

            crc ^= (data << 8);
            for (size_t j = 0; j < 8; j++)
                if (crc & 0x8000)
                    crc = (crc << 1) ^ polynomial;
                else
                    crc <<= 1;
        }
        return crc;
    }

    bool RepetitionFilter::check(Frame &frame)
    {
        const unsigned short source = frame.source();
        const unsigned short checksum = crc(frame);

        const unsigned long start = micros();
        auto it = _map.find(source);
        if (it != _map.end())
        {
            unsigned short ref = it->second->second;
            // Key existiert bereits -> aktualisieren & Eintrag nach vorne schieben
            // 1) Alten Eintrag aus Liste entfernen
            _entries.erase(it->second);
            // 2) Neuer Eintrag wird am Kopf (Front) eingefügt
            _entries.push_front({source, checksum});
            // 3) Iterator in der Map updaten
            _map[source] = _entries.begin();

            if (ref == checksum) return true;
        }
        else
        {
            // Key existiert nicht -> neuer Eintrag
            if (_entries.size() == MAX_SIZE)
            {
                // Falls wir bereits 50 Einträge haben, entfernen wir den ältesten (hinten)
                auto lastIt = --(_entries.end());
                uint16_t oldKey = lastIt->first;
                // Aus der Map löschen
                _map.erase(oldKey);
                // Aus der Liste löschen
                _entries.pop_back();
            }
            // Neuen Eintrag vorne einfügen
            _entries.push_front({source, checksum});
            // Und in Map speichern
            _map[source] = _entries.begin();
        }

        unsigned long duration = micros() - start;
        _avg += duration;
        _avgCnt++;

        // Serial.printf("duration %lu\n", duration);
        // Serial.printf("duration AVG %lu\n", _avg / _avgCnt);
        return false;
    }

    void RepetitionFilter::clear()
    {
        _entries.clear();
        _map.clear();
    }

    std::size_t RepetitionFilter::size()
    {
        return _entries.size();
    }

} // namespace TPUart