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

// Retry interval for the initial NCN handshake while BCU_UNINITIALIZED. The
// transceiver is powered from the KNX bus, so a stick that boots before its
// bus is live cannot answer U_RESET_REQ; retrying on this timer lets the link
// come up on its own once the bus is connected, without a reboot.
#ifndef TPUART_REINIT_INTERVAL
#define TPUART_REINIT_INTERVAL 2000
#endif

#ifndef TPUART_REGREAD_QUIET_MS
// A register read may only be armed once the device has had time to answer
// everything else that was asked of it. The state request pair goes out once a
// second from processRequestState(); its answers arrive within well under a
// millisecond, but they carry no marker that distinguishes them from a register
// answer — so arming inside that window would claim U_State.ind (0x07) as the
// register value, which decodes as "DC2EN off": exactly the false reading this
// whole mechanism exists to prevent.
#define TPUART_REGREAD_QUIET_MS 25
#endif

#ifndef TPUART_REGREAD_TIMEOUT
// An unanswered register read leaves the receiver armed to swallow the next
// byte, so the window is kept short: at 38400 baud the answer is back within
// a fraction of a millisecond.
#define TPUART_REGREAD_TIMEOUT 50
#endif

// Max KNX TP frame length (extended frame). RX reassembly bounds the per-frame
// stack buffer against this so a corrupt/desynced length prefix cannot size a
// huge VLA (stack smash). 263 = 9-byte L_Data_Extended header + 254-byte APDU.
#ifndef TPUART_MAX_KNX_FRAME
#define TPUART_MAX_KNX_FRAME 263
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
        unsigned long _initAttempts = 0;
        unsigned long _lastDiscardedMessage = 0;
        unsigned long _lastDiscardedBytes = 0;
        volatile size_t _rxFrameBufferEntries = 0;
        volatile BcuType _bcuType;
        volatile BcuState _bcuState = BCU_UNINITIALIZED;
        volatile uint _baudrate = 0;

        // Internal register read (NCN5130 U_IntRegRd.req, DS p.39 fig.43). The
        // answer is a bare data byte carrying no service marker of its own, so
        // a request arms the receiver to hand the *next* byte over here instead
        // of decoding it. Single-slot on purpose: one outstanding read at a time.
        volatile bool _regReadPending = false;
        volatile bool _regReadValid = false;
        volatile uint8_t _regReadRequest = 0;
        volatile uint8_t _regReadValue = 0;
        volatile unsigned long _regReadSentAt = 0;
        volatile unsigned long _regReadAt = 0;
        volatile unsigned int _regReadTimeouts = 0;
        // When the host last sent something the device will answer unprompted.
        volatile unsigned long _lastHostRequestAt = 0;

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
        bool quietForRegisterAccess();

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

        // Switches DC2 and the V20V regulator via ACR0. Two things to know
        // before calling: state=false takes the 20 V regulator down together
        // with DC2, so the device cannot drive the bus until it is switched back
        // on; and false is returned when the line was busy, in which case
        // nothing was written at all — retry from loop(), do not assume.
        // Verified on hardware 2026-08-31 (C3 + NCN5130): 0x74 -> 0x14 pulls
        // VDD2 and V20V down, and back up on the way home.
        bool powerControl(bool state);
        // Read one internal NCN5130 register (pass U_INT_REG_RD_REQ_ACR0 etc.).
        // Returns false when the request was not issued — wrong BCU type, link
        // down, a read already outstanding, or the line not idle. The answer is
        // unmarked, so it is only issued while nothing else is in flight; poll
        // internalRegisterValid() afterwards.
        bool requestInternalRegister(uint8_t readRequestByte);
        bool internalRegisterPending() const { return _regReadPending; }
        bool internalRegisterValid() const { return _regReadValid; }
        uint8_t internalRegisterRequest() const { return _regReadRequest; }
        uint8_t internalRegisterValue() const { return _regReadValue; }
        unsigned long internalRegisterReadAt() const { return _regReadAt; }
        unsigned int internalRegisterTimeouts() const { return _regReadTimeouts; }
        // Called from the receive path before any decoding; true = byte consumed.
        bool consumeInternalRegisterByte(char value);

        // Write one internal register (pass U_INT_REG_WR_REQ_ACR0 etc.). Same
        // idle-line rule as the read: the two bytes must not land inside a frame
        // that is going out. Returns false when it was not issued. Always read
        // the register back afterwards — a write that never arrives is silent.
        bool writeInternalRegister(uint8_t writeRequestByte, uint8_t value);

        bool stopMode(bool state);
        bool busyMode(bool state);
        void setOwnAddress(short address);
        bool startMonitoring();
        BcuState getBcuState();
        const char *getBcuStateInfo();
        uint getBaudrate() const { return _baudrate; }
        bool isMonitoring() const;
        bool isConnected() const;

        volatile uint _statsDurationMax = 0;
        volatile uint _statsDurationMin = 0;
        volatile uint _statsDuration = 0;
        volatile uint _statsDurationCount = 0;
    };

} // namespace TPUart