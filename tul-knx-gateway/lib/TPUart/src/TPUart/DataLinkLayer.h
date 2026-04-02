#pragma once
#include <Arduino.h>
#include <deque>
#include <functional>
#include <vector>

#include "TPUart/Frame.h"
#include "TPUart/Interface/Abstract.h"
#include "TPUart/Receiver.h"
#include "TPUart/RingBuffer.h"
#include "TPUart/SearchBuffer.h"
#include "TPUart/Statistics.h"
#include "TPUart/SystemState.h"
#include "TPUart/Transmitter.h"
#include "TPUart/Types.h"

#ifdef ARDUINO_ARCH_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#ifndef TPUART_MAX_PROCESS_TIME_PER_LOOP
#define TPUART_MAX_PROCESS_TIME_PER_LOOP 30
#endif

#ifndef TPUART_MAX_RXQUEUE_TIME_PER_LOOP
#define TPUART_MAX_RXQUEUE_TIME_PER_LOOP 20
#endif

namespace TPUart
{
    class DataLinkLayer
    {
        friend class Receiver;
        friend class Transmitter;

      private:
        bool _initialized = false;
        char _repetitions = 0b00110011; // 0-3 Nack (Default 3) // 5-7 Busy (Default 3)
        short _ownAddress = 0;
        volatile bool _uReset = false;
        volatile char _uState = 0;
        volatile bool _modeAutoAcknowlage = false;
        volatile bool _modeExtendedCRC = false;

        unsigned long _busyMode = 0;
        unsigned long _requestStateTimer = 0;
        unsigned long _lastTryInitialize = 0;
        unsigned long _lastDiscardedMessage = 0;
        unsigned long _lastDiscardedBytes = 0;
        volatile size_t _rxFrameBufferEntries = 0;
        volatile BcuType _bcuType;
        volatile BcuState _bcuState = BCU_UNINITIALIZED;

        // Overflow
        volatile bool _rxSearchBufferOverflow = false;
        volatile bool _rxFrameBufferOverflow = false;
        volatile bool _rxInterfaceOverflow = false;
        volatile unsigned long _rxShowOverflowTime = 0;
        
        Interface::Abstract *_interface = nullptr;
        

#if defined(ARDUINO_ARCH_RP2040)
        mutex_t _rxLock;
        mutex_t _txLock;
        mutex_t _test;
#elif defined(ARDUINO_ARCH_ESP32)
        SemaphoreHandle_t _rxLock;
        SemaphoreHandle_t _txLock;
#endif

        Transmitter _transmitter;
        Receiver _receiver;
        RingBuffer _rxFrameBuffer;
        RepetitionFilter _repetitionFilter;
        Statistics _statistics;
        SystemState _systemState;

        std::vector<std::function<void(Frame &)>> _callbacksReceivedFrame;
        std::function<AcknowledgeType(unsigned short, bool)> _callbackCheckAcknowledge;
        std::function<void(const char *, bool error)> _callbackMessage;

        void connected(bool connected);
        void pushRxFrameBuffer(Frame &frame);
        // void processIncompleteFrame();
        // void processWaitForAcknTimer();
        void processRxFrameBuffer();
        AcknowledgeType checkAcknowledge(unsigned short destination, bool isGroupAddress);
        bool rxLock(bool blocking = false);
        bool txLock(bool blocking = false);
        void rxUnlock();
        void txUnlock();
        void showOverflowError();
        void showStateError();
        void showDiscardedError();
        void showSystemState();
        void processRequestState();

        void tryInitialize();
        bool tryInitialize(uint baudrate);

        void exitBusyModeTimer();
        void setBCUState(BcuState state, int baudrate = 0);

        void receivedConfiguration(char config);
        void receivedState(char state);
        void receivedReset();
        void processWatchdog();

      public:
        DataLinkLayer();

        void begin(BcuType bcuType, Interface::Abstract *Interface);
        void end(bool deleteUart = true);
        void reset();
        void process();

        void registerMessage(std::function<void(const char *, bool)> callback);
        void registerReceivedFrame(std::function<void(Frame &)> callback);
        void registerCheckAcknowledge(std::function<AcknowledgeType(unsigned short, bool)> callback);

        bool processReceviedByte();
        void processTransmitByte();

        void setRepetitions(uint8_t nack, uint8_t busy);
        void requestState();
        void applyConfiguration();
        void handleReset();
        void printMessage(const char *message, ...);
        void printError(const char *message, ...);
        bool pushTransmitQueue(Frame *frame);

        Statistics &getStatistics();
        SystemState &getSystemState();
        Receiver &getReceiver();
        Transmitter &getTransmitter();

        bool powerControl(bool state);
        bool stopMode(bool state);
        bool busyMode(bool state);
        void setOwnAddress(short address);
        bool startMonitoring();
        BcuState getBcuState();
        const char *getBcuStateInfo();
        bool isMonitoring() const;
        bool isConnected() const;

        volatile uint _statsDurationMax = 0;
        volatile uint _statsDurationMin = 0;
        volatile uint _statsDuration = 0;
        volatile uint _statsDurationCount = 0;
    };

} // namespace TPUart