/*
The OpenTRV project licenses this file to you
under the Apache Licence, Version 2.0 (the "Licence");
you may not use this file except in compliance
with the Licence. You may obtain a copy of the Licence at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the Licence is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied. See the Licence for the
specific language governing permissions and limitations
under the Licence.

Author(s) / Copyright (s): Deniz Erbilgin 2017
*/

// Uncomment exactly one of the following CONFIG_... lines to select which board is being built for.
#define CONFIG_REV8_2WAY_COMMS // REV8 as CC1 hub.

#define DEBUG // Uncomment for debug output.

#include <Arduino.h>
#include <Wire.h>
#include <OTV0p2Base.h>
#include <OTRadioLink.h>
#include <OTRFM23BLink.h>
#include <OTRadValve.h>
#include <OTProtocolCC.h>
#include <OTV0p2_CONFIG_REV8.h>
#include <OTV0p2_Board_IO_Config.h> // I/O pin allocation and setup: include ahead of I/O module headers.

// Force-enable always-on RX if not already so.
#define ENABLE_RADIO_RX
#define ENABLE_CONTINUOUS_RX


#ifndef DEBUG
#define DEBUG_SERIAL_PRINT(s) // Do nothing.
#define DEBUG_SERIAL_PRINTFMT(s, format) // Do nothing.
#define DEBUG_SERIAL_PRINT_FLASHSTRING(fs) // Do nothing.
#define DEBUG_SERIAL_PRINTLN_FLASHSTRING(fs) // Do nothing.
#define DEBUG_SERIAL_PRINTLN() // Do nothing.
#define DEBUG_SERIAL_TIMESTAMP() // Do nothing.
#else
// Send simple string or numeric to serial port and wait for it to have been sent.
// Make sure that Serial.begin() has been invoked, etc.
#define DEBUG_SERIAL_PRINT(s) { OTV0P2BASE::serialPrintAndFlush(s); }
#define DEBUG_SERIAL_PRINTFMT(s, fmt) { OTV0P2BASE::serialPrintAndFlush((s), (fmt)); }
#define DEBUG_SERIAL_PRINT_FLASHSTRING(fs) { OTV0P2BASE::serialPrintAndFlush(F(fs)); }
#define DEBUG_SERIAL_PRINTLN_FLASHSTRING(fs) { OTV0P2BASE::serialPrintlnAndFlush(F(fs)); }
#define DEBUG_SERIAL_PRINTLN() { OTV0P2BASE::serialPrintlnAndFlush(); }
// Print timestamp with no newline in format: MinutesSinceMidnight:Seconds:SubCycleTime
extern void _debug_serial_timestamp();
#define DEBUG_SERIAL_TIMESTAMP() _debug_serial_timestamp()
#endif // DEBUG

//---------------------

// Indicate that the system is broken in an obvious way (distress flashing of the main UI LED).
// DOES NOT RETURN.
// Tries to turn off most stuff safely that will benefit from doing so, but nothing too complex.
// Tries not to use lots of energy so as to keep the distress beacon running for a while.
void panic();
// Panic with fixed message.
void panic(const __FlashStringHelper *s);

// Version (code/board) information printed as one line to serial (with line-end, and flushed); machine- and human- parseable.
// Format: "board VXXXX REVY; code YYYY/Mmm/DD HH:MM:SS".
void serialPrintlnBuildVersion();

// Call this to do an I/O poll if needed; returns true if something useful happened.
// This call should typically take << 1ms at 1MHz CPU.
// Does not change CPU clock speeds, mess with interrupts (other than possible brief blocking), or sleep.
// Should also do nothing that interacts with Serial.
// Limits actual poll rate to something like once every 8ms, unless force is true.
//   * force if true then force full poll on every call (ie do not internally rate-limit)
// Not thread-safe, eg not to be called from within an ISR.
void pollIO();

// Sends a short 1-line CRLF-terminated status report on the serial connection (at 'standard' baud).
void serialStatusReport();

// Reset CLI active timer to the full whack before it goes inactive again (ie makes CLI active for a while).
// Thread-safe.
void resetCLIActiveTimer();
// Returns true if the CLI is (or should currently be) active, at least intermittently.
// Thread-safe.
bool isCLIActive();
// Used to poll user side for CLI input until specified sub-cycle time.
// A period of less than (say) 500ms will be difficult for direct human response on a raw terminal.
// A period of less than (say) 100ms is not recommended to avoid possibility of overrun on long interactions.
// Times itself out after at least a minute or two of inactivity.
// NOT RENTRANT (eg uses static state for speed and code space).
void pollCLI(uint8_t maxSCT, bool startOfMinute);

constexpr uint8_t nearOverrunThreshold = OTV0P2BASE::GSCT_MAX - 8; // ~64ms/~32 serial TX chars of grace time...

// Create RFM23B. This is the primary radio.
static constexpr uint8_t RFM23B_RX_QUEUE_SIZE = OTRFM23BLink::DEFAULT_RFM23B_RX_QUEUE_CAPACITY;
static constexpr int8_t RFM23B_IRQ_PIN = PIN_RFM_NIRQ;
static constexpr bool RFM23B_allowRX = true;
OTRFM23BLink::OTRFM23BLink<OTV0P2BASE::V0p2_PIN_SPI_nSS, RFM23B_IRQ_PIN, RFM23B_RX_QUEUE_SIZE, RFM23B_allowRX> PrimaryRadio;

// Dumy relative humidity
OTV0P2BASE::DummyHumiditySensorSHT21 RelHumidity;

// Ambient/room temperature sensor, usually on main board.
OTV0P2BASE::RoomTemperatureC16_SHT21 TemperatureC16;


// Use WDT-based timer for xxxPause() routines.
// Tiny low-power sleep to approximately match the PICAXE V0.09 routine of the same name.
static void inline tinyPause() { OTV0P2BASE::nap(WDTO_15MS); } // 15ms vs 18ms nominal for PICAXE V0.09 impl.
// Big low-power sleep to approximately match the PICAXE V0.09 routine of the same name.
// Premature wakeups MAY be allowed to avoid blocking I/O polling for too long.
static void inline bigPause() { OTV0P2BASE::nap(WDTO_120MS); } // 120ms vs 288ms nominal for PICAXE V0.09 impl.

//---------------------

// Returns true if there is time to andle at least one message inbound our outbound.
// Includes time required to encrypt/decrypt/print a message if need be (~0.5s at 1MHz CPU).
static bool timeToHandleMessage()
{
    const uint8_t sct = OTV0P2BASE::getSubCycleTime();
    return(sct < min((OTV0P2BASE::GSCT_MAX/4)*3, nearOverrunThreshold - 1));
}

// Controller's view of Least Significant Digits of the current (local) time, in this case whole seconds.
static constexpr uint8_t TIME_CYCLE_S = 60; // TIME_LSD ranges from 0 to TIME_CYCLE_S-1, also major cycle length.
static uint_fast8_t TIME_LSD; // Controller's notion of seconds within major cycle.

// 'Elapsed minutes' count of minute/major cycles; cheaper than accessing RTC and not tied to real time.
// Starts at or just above zero (within the first 4-minute cycle) to help avoid collisions between units after mass power-up.
// Wraps at its maximum (0xff) value.
static uint8_t minuteCount;

// Indicate that the system is broken in an obvious way (distress flashing the main LED).
// DOES NOT RETURN.
// Tries to turn off most stuff safely that will benefit from doing so, but nothing too complex.
// Tries not to use lots of energy so as to keep distress beacon running for a while.
void panic()
{
    // Reset radio and go into low-power mode.
    PrimaryRadio.panicShutdown();
    // Power down almost everything else...
    OTV0P2BASE::minimisePowerWithoutSleep();
    pinMode(OTV0P2BASE::LED_HEATCALL_L, OUTPUT);
    for( ; ; )
    {
        OTV0P2BASE::LED_HEATCALL_ON();
        tinyPause();
        OTV0P2BASE::LED_HEATCALL_OFF();
        bigPause();
    }
}

// Panic with fixed message.
void panic(const __FlashStringHelper *s)
{
    OTV0P2BASE::serialPrintlnAndFlush(); // Start new line to highlight error.  // May fail.
    OTV0P2BASE::serialPrintAndFlush('!'); // Indicate error with leading '!' // May fail.
    OTV0P2BASE::serialPrintlnAndFlush(s); // Print supplied detail text. // May fail.
    panic();
}

// Rearrange date into sensible most-significant-first order, and make it fully numeric.
// FIXME: would be better to have this in PROGMEM (Flash) rather than RAM, eg as F() constant.
static const char _YYYYMmmDD[] =
{
    __DATE__[7], __DATE__[8], __DATE__[9], __DATE__[10],
    '/',
    __DATE__[0], __DATE__[1], __DATE__[2],
    '/',
    ((' ' == __DATE__[4]) ? '0' : __DATE__[4]), __DATE__[5],
    '\0'
};
// Version (code/board) information printed as one line to serial (with line-end, and flushed); machine- and human- parseable.
// Format: "board VX.X REVY YYYY/Mmm/DD HH:MM:SS".
void serialPrintlnBuildVersion()
{
    OTV0P2BASE::serialPrintAndFlush(F("board V0.2 REV"));
    OTV0P2BASE::serialPrintAndFlush(V0p2_REV);
    OTV0P2BASE::serialPrintAndFlush(' ');
    OTV0P2BASE::serialPrintAndFlush(_YYYYMmmDD);
    OTV0P2BASE::serialPrintlnAndFlush(F(" " __TIME__));
}


void pollIO()
{
    // Poll for inbound frames.
    // If RX is not interrupt-driven then
    // there will usually be little time to do this
    // before getting an RX overrun or dropped frame.
    PrimaryRadio.poll();
}

// Decode and handle inbound raw message (msg[-1] contains the count of bytes received).
// A message may contain trailing garbage at the end; the decoder/router should cope.
// The buffer may be reused when this returns,
// so a copy should be taken of anything that needs to be retained.
// If secure is true then this message arrived over an inherently secure channel.
// This will write any output to the supplied Print object,
// typically the Serial output (which must be running if so).
// This routine is NOT allowed to alter in any way the content of the buffer passed.
static void decodeAndHandleRawRXedMessage(Print *p, const uint8_t * const msg)
  {
  const uint8_t msglen = msg[-1];

#if 0 && defined(DEBUG)
  OTRadioLink::printRXMsg(p, msg-1, msglen+1); // Print len+frame.
#endif
   // For non-secure, check that there enough bytes for expected (fixed) frame size.
   if(msglen < 8) { return; } // Too short to be useful, so ignore.
   const uint8_t *cleartextBody = msg;
   const uint8_t cleartextBodyLen = msglen;
  const uint8_t firstByte = msg[0];
  switch(firstByte)
    {
    default: // Reject unrecognised leading type byte.
    case OTRadioLink::FTp2_NONE: // Reject zero-length with leading length byte.
      break;

    // Handle alert message (at hub).
    // Dump onto serial to be seen by the attached host.
    // Non-secure.
    case OTRadioLink::FTp2_CC1Alert:
      {
      OTProtocolCC::CC1Alert a;
      a.OTProtocolCC::CC1Alert::decodeSimple(msg, msglen);
      // After decode instance should be valid and with correct (source) house code.
      if(a.isValid())
        {
        // Pass message to host to deal with as "! hc1 hc2" after prefix indicating relayed (CC1 alert) message.
        p->print(F("+CC1 ! ")); p->print(a.getHC1()); p->print(' '); p->println(a.getHC2());
        }
      return; // OK
      }

    // Handle poll-response message (at hub).
    // Dump onto serial to be seen by the attached host.
    case OTRadioLink::FTp2_CC1PollResponse: // Non-secure.
      {
      OTProtocolCC::CC1PollResponse a;
      a.OTProtocolCC::CC1PollResponse::decodeSimple(cleartextBody, cleartextBodyLen);
      // After decode instance should be valid and with correct (source) house code.
      if(a.isValid())
        {
        // Pass message to host to deal with as:
        //     * hc1 hc2 rh tp tr al s w sy
        // after prefix indicating relayed (CC1) message.
        // (Parameters in same order as make() factory method, see below.)
//   * House code (hc1, hc2) of valve controller that the poll/command is being sent to.
//   * relative-humidity    [0,50] 0-100 in 2% steps (rh)
//   * temperature-ds18b20  [0,199] 0.000-99.999C in 1/2 C steps, pipe temp (tp)
//   * temperature-opentrv  [0,199] 0.000-49.999C in 1/4 C steps, room temp (tr)
//   * ambient-light        [1,62] no units, dark to light (al)
//   * switch               [false,true] activation toggle, helps async poll detect intermittent use (s)
//   * window               [false,true] false=closed,true=open (w)
//   * syncing              [false,true] if true, (re)syncing to FHT8V (sy)
// Returns instance; check isValid().
//            static CC1PollResponse make(uint8_t hc1, uint8_t hc2,
//                                        uint8_t rh,
//                                        uint8_t tp, uint8_t tr,
//                                        uint8_t al,
//                                        bool s, bool w, bool sy);
        p->print(F("+CC1 * "));
            p->print(a.getHC1()); p->print(' '); p->print(a.getHC2()); p->print(' ');
            p->print(a.getRH()); p->print(' ');
            p->print(a.getTP()); p->print(' '); p->print(a.getTR()); p->print(' ');
            p->print(a.getAL()); p->print(' ');
            p->print(a.getS()); p->print(' '); p->print(a.getW()); p->print(' ');
               p->println(a.getSY());
        }
      return;
      }
    }

  // Unparseable frame: drop it; possibly log it as an error.
#if 1 && defined(DEBUG) && !defined(ENABLE_TRIMMED_MEMORY)
  p->print(F("!RX bad msg, len+prefix: ")); OTRadioLink::printRXMsg(p, msg-1, min(msglen+1, 8));
#endif
  return;
  }

// Incrementally process I/O and queued messages, including from the radio link.
// This may mean printing them to Serial (which the passed Print object usually is),
// or adjusting system parameters,
// or relaying them elsewhere, for example.
// This will write any output to the supplied Print object,
// typically the Serial output (which must be running if so).
// This will attempt to process messages in such a way
// as to avoid internal overflows or other resource exhaustion,
// which may mean deferring work at certain times
// such as the end of minor cycle.
// The Print object pointer must not be NULL.
bool handleQueuedMessages(Print *p, bool wakeSerialIfNeeded, OTRadioLink::OTRadioLink *rl)
  {
  // Avoid starting any potentially-slow processing very late in the minor cycle.
  // This is to reduce the risk of loop overruns
  // at the risk of delaying some processing
  // or even dropping some incoming messages if queues fill up.
  if(!timeToHandleMessage()) { return(false); }

  // Deal with any I/O that is queued.
//  bool workDone = pollIO();
  bool workDone = false;
  pollIO();

  // Check for activity on the radio link.
  rl->poll();

  bool neededWaking = false; // Set true once this routine wakes Serial.
  const volatile uint8_t *pb;
  if(NULL != (pb = rl->peekRXMsg()))
    {
    if(!neededWaking && wakeSerialIfNeeded && OTV0P2BASE::powerUpSerialIfDisabled<V0P2_UART_BAUD>()) { neededWaking = true; } // FIXME
    // Don't currently regard anything arriving over the air as 'secure'.
    // FIXME: shouldn't have to cast away volatile to process the message content.
    decodeAndHandleRawRXedMessage(p, (const uint8_t *)pb);
    rl->removeRXMsg();
    // Note that some work has been done.
    workDone = true;
    }

  // Turn off serial at end, if this routine woke it.
  if(neededWaking) { OTV0P2BASE::flushSerialProductive(); OTV0P2BASE::powerDownSerial(); }

  return(workDone);
  }

// Returns true if continuous background RX has been set up.
static bool setUpContinuousRX()
  {
  // Possible paranoia...
  // Periodically (every few hours) force radio off or at least to be not listening.
  if((30 == TIME_LSD) && (128 == minuteCount)) { PrimaryRadio.listen(false); }

  const bool needsToListen = true; // By default listen if always doing RX.

  // Act on eavesdropping need, setting up or clearing down hooks as required.
  PrimaryRadio.listen(needsToListen);

  if(needsToListen)
    {
#if 1 && defined(DEBUG) // && defined(ENABLE_RADIO_RX) && !defined(ENABLE_TRIMMED_MEMORY)
    for(uint8_t lastErr; 0 != (lastErr = PrimaryRadio.getRXErr()); )
      {
      DEBUG_SERIAL_PRINT_FLASHSTRING("!RX err ");
      DEBUG_SERIAL_PRINT(lastErr);
      DEBUG_SERIAL_PRINTLN();
      }
    const uint8_t dropped = PrimaryRadio.getRXMsgsDroppedRecent();
    static uint8_t oldDropped;
    if(dropped != oldDropped)
      {
      DEBUG_SERIAL_PRINT_FLASHSTRING("!RX DROP ");
      DEBUG_SERIAL_PRINT(dropped);
      DEBUG_SERIAL_PRINTLN();
      oldDropped = dropped;
      }
#endif
#if 0 && defined(DEBUG) && !defined(ENABLE_TRIMMED_MEMORY)
    // Filtered out messages are not an error.
    const uint8_t filtered = PrimaryRadio.getRXMsgsFilteredRecent();
    static uint8_t oldFiltered;
    if(filtered != oldFiltered)
      {
      DEBUG_SERIAL_PRINT_FLASHSTRING("RX filtered ");
      DEBUG_SERIAL_PRINT(filtered);
      DEBUG_SERIAL_PRINTLN();
      oldFiltered = filtered;
      }
#endif
    }
  return(needsToListen);
  }


// Mask for Port B input change interrupts.
#define MASK_PB_BASIC 0b00000000 // Nothing.
#if defined(PIN_RFM_NIRQ) && defined(ENABLE_RADIO_RX) // RFM23B IRQ only used for RX.
  #if (PIN_RFM_NIRQ < 8) || (PIN_RFM_NIRQ > 15)
    #error PIN_RFM_NIRQ expected to be on port B
  #endif
  #define RFM23B_INT_MASK (1 << (PIN_RFM_NIRQ&7))
  #define MASK_PB (MASK_PB_BASIC | RFM23B_INT_MASK)
#else
  #define MASK_PB MASK_PB_BASIC
#endif

// Mask for Port C input change interrupts.
#define MASK_PC_BASIC 0b00000000 // Nothing.

// Mask for Port D input change interrupts.
#define MASK_PD_BASIC 0b00000001 // Serial RX by default.
#define MASK_PD1 MASK_PD_BASIC // Just serial RX, no voice.
#define MASK_PD MASK_PD1 // No MODE button interrupt.

#if defined(MASK_PB) && (MASK_PB != 0) // If PB interrupts required.
// Previous state of port B pins to help detect changes.
static volatile uint8_t prevStatePB;
// Interrupt service routine for PB I/O port transition changes.
ISR(PCINT0_vect)
  {
//  ++intCountPB;
  const uint8_t pins = PINB;
  const uint8_t changes = pins ^ prevStatePB;
  prevStatePB = pins;

#if defined(RFM23B_INT_MASK)
  // RFM23B nIRQ falling edge is of interest.
  // Handler routine not required/expected to 'clear' this interrupt.
  // TODO: try to ensure that OTRFM23BLink.handleInterruptSimple() is inlineable to minimise ISR prologue/epilogue time and space.
  if((changes & RFM23B_INT_MASK) && !(pins & RFM23B_INT_MASK))
    { PrimaryRadio.handleInterruptSimple(); }
#endif
  }
#endif

// Previous state of port D pins to help detect changes.
static volatile uint8_t prevStatePD;
// Interrupt service routine for PD I/O port transition changes (including RX).
ISR(PCINT2_vect)
  {
  const uint8_t pins = PIND;
  const uint8_t changes = pins ^ prevStatePD;
  prevStatePD = pins;

  // If an interrupt arrived from no other masked source then wake the CLI.
  // The will ensure that the CLI is active, eg from RX activity,
  // eg it is possible to wake the CLI subsystem with an extra CR or LF.
  // It is OK to trigger this from other things such as button presses.
  // FIXME: ensure that resetCLIActiveTimer() is inlineable to minimise ISR prologue/epilogue time and space.
  if(!(changes & MASK_PD & ~1)) { resetCLIActiveTimer(); }
  }

// Quickly screen/filter RX traffic to preserve queue space for stuff likely to be of interest.
// If in doubt, accept a frame, ie should not reject incorrectly.
// For a CC1 hub, ignore everything except FTp2_CC1Alert and FTp2_CC1PollResponse messages.
static bool FilterRXISR(const volatile uint8_t *buf, volatile uint8_t &buflen)
  {
  if(buflen < 8) { return(false); }
  const uint8_t t = buf[0];
  if((OTRadioLink::FTp2_CC1Alert != t) && (OTRadioLink::FTp2_CC1PollResponse != t)) { return(false); }
  // TODO: filter for only associated relay address/housecodes.
  return(true); // Accept message.
  }

// COHEAT: REV2/REV9 talking on fast GFSK channel 0, REV9 TX to FHT8V on slow OOK.
static const uint8_t nPrimaryRadioChannels = 2;
static const OTRadioLink::OTRadioChannelConfig RFM23BConfigs[nPrimaryRadioChannels] =
  {
  // GFSK channel 0 full config, RX/TX, not in itself secure.
  OTRadioLink::OTRadioChannelConfig(OTRFM23BLink::StandardRegSettingsGFSK57600, true),
  // FS20/FHT8V compatible channel 1 full config, used for TX only, not secure, unframed.
  OTRadioLink::OTRadioChannelConfig(OTRFM23BLink::StandardRegSettingsOOK5000, true, false, true, false, false, true),
  };

// Sends a short 1-line CRLF-terminated status report on the serial connection (at 'standard' baud).
void serialStatusReport()
  {
  OTV0P2BASE::serialPrintlnAndFlush(F("="));
  }

// Handle CLI extension commands.
// Commands of form:
//   +EXT .....
// where EXT is the name of the extension, usually 3 letters.
//
// It is acceptable for extCLIHandler() to alter the buffer passed,
// eg with strtok_t().
static bool extCLIHandler(char *const buf, const uint8_t n)
  {
//    If CC1 hub then allow +CC1 ? command to poll a remote relay.
//    Full command is:
//        +CC1 ? hc1 hc2 rp lc lt lf
//    i.e. six numeric arguments, see below, with out-of-range values coerced (other than housecodes):
//                Factory method to create instance.
//                Invalid parameters (except house codes) will be coerced into range.
//                  * House code (hc1, hc2) of valve controller that the poll/command is being sent to.
//                  * rad-set-point [15,30] in 1 C steps. The set-point the TRV will attempt to keep the room at. // TODO
//                                  Originally [0,100] 0-100 in 1% steps, percent open approx to set rad valve.
//                  * light-colour  [0,3] bit flags 1==red 2==green (lc) 0 => stop everything. This field is ignored.
//                  * light-on-time [1,15] (0 not allowed) 30-450s in units of 30s (lt). This field is ignored.
//                  * light-flash   [1,3] (0 not allowed) 1==single 2==double 3==on (lf). This field is ignored.
//                Returns instance; check isValid().
//                static CC1PollAndCommand make(uint8_t hc1, uint8_t hc2,
//                                              uint8_t rp,
//                                              uint8_t lc, uint8_t lt, uint8_t lf);
//    e.g. +CC1 ? 70 04 20 1 1 1
//        - Send a poll and command (+CC1 ?) to the valve with house code "70 04".
//        - Set the valve set-point to 20 C.
//        - The last three values are ignored but are preserved to minimise coding changes.
  const uint8_t CC1_Q_PREFIX_LEN = 7;
  const uint8_t CC1_Q_PARAMS = 6;
  // Falling through rather than return(true) indicates failure.
  if((n >= CC1_Q_PREFIX_LEN) && (0 == strncmp("+CC1 ? ", buf, CC1_Q_PREFIX_LEN)))
    {
    char *last; // Used by strtok_r().
    char *tok1;
    // Attempt to parse the parameters.
    if((n-CC1_Q_PREFIX_LEN >= CC1_Q_PARAMS*2-1) && (NULL != (tok1 = strtok_r(buf+CC1_Q_PREFIX_LEN, " ", &last))))
      {
      char *tok2 = strtok_r(NULL, " ", &last);
      char *tok3 = (NULL == tok2) ? NULL : strtok_r(NULL, " ", &last);
      char *tok4 = (NULL == tok3) ? NULL : strtok_r(NULL, " ", &last);
      char *tok5 = (NULL == tok4) ? NULL : strtok_r(NULL, " ", &last);
      char *tok6 = (NULL == tok5) ? NULL : strtok_r(NULL, " ", &last);
      if(NULL != tok6)
        {
        OTProtocolCC::CC1PollAndCommand q = OTProtocolCC::CC1PollAndCommand::make(
            atoi(tok1),
            atoi(tok2),
            atoi(tok3),
            atoi(tok4),
            atoi(tok5),
            atoi(tok6));
        if(q.isValid())
          {
          uint8_t txbuf[OTProtocolCC::CC1PollAndCommand::primary_frame_bytes+1]; // More than large enough for preamble + sync + alert message.
          const uint8_t bodylen = q.encodeSimple(txbuf, sizeof(txbuf), true);
          // Non-secure: send raw frame as-is.
          // TX at normal power since ACKed and can be repeated if necessary.
          if(PrimaryRadio.sendRaw(txbuf, bodylen))
            { return(true); } // Success!
          // Fall-through is failure...
          OTV0P2BASE::serialPrintlnAndFlush(F("!TX fail"));
          }
        }
      }
    return(false); // FAILED if fallen through from above.
    }
  return(false); // FAILED if not otherwise handled.
  }

// Remaining minutes to keep CLI active; zero implies inactive.
// Starts up with full value to allow easy setting of time, etc, without specially activating CLI.
// Marked volatile for thread-safe lock-free non-read-modify-write access to byte-wide value.
// Compound operations on this value must block interrupts.
#define CLI_DEFAULT_TIMEOUT_M 2
static volatile uint8_t CLITimeoutM = CLI_DEFAULT_TIMEOUT_M;
// Reset CLI active timer to the full whack before it goes inactive again (ie makes CLI active for a while).
// Thread-safe.
void resetCLIActiveTimer() { CLITimeoutM = CLI_DEFAULT_TIMEOUT_M; }
// Returns true if the CLI is active, at least intermittently.
// Thread-safe.
bool isCLIActive() { return(0 != CLITimeoutM); }
#if defined(ENABLE_EXTENDED_CLI)
static const uint8_t MAXIMUM_CLI_RESPONSE_CHARS = 1 + OTV0P2BASE::CLI::MAX_TYPICAL_CLI_BUFFER;
#else
static const uint8_t MAXIMUM_CLI_RESPONSE_CHARS = 1 + OTV0P2BASE::CLI::MIN_TYPICAL_CLI_BUFFER;
#endif
// Used to poll user side for CLI input until specified sub-cycle time.
// Commands should be sent terminated by CR *or* LF; both may prevent 'E' (exit) from working properly.
// A period of less than (say) 500ms will be difficult for direct human response on a raw terminal.
// A period of less than (say) 100ms is not recommended to avoid possibility of overrun on long interactions.
// Times itself out after at least a minute or two of inactivity.
// NOT RENTRANT (eg uses static state for speed and code space).
void pollCLI(const uint8_t maxSCT, const bool startOfMinute)
  {
  // Perform any once-per-minute operations.
  if(startOfMinute)
    {
    ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
      {
      // Run down CLI timer if need be.
      if(CLITimeoutM > 0) { --CLITimeoutM; }
      }
    }

  const bool neededWaking = OTV0P2BASE::powerUpSerialIfDisabled<V0P2_UART_BAUD>();

  // Wait for input command line from the user (received characters may already have been queued)...
  // Read a line up to a terminating CR, either on its own or as part of CRLF.
  // (Note that command content and timing may be useful to fold into PRNG entropy pool.)
  static char buf[MAXIMUM_CLI_RESPONSE_CHARS+1]; // Note: static state, efficient for small command lines.  Space for terminating '\0'.
  const uint8_t n = OTV0P2BASE::CLI::promptAndReadCommandLine(maxSCT, buf, sizeof(buf), NULL);

  if(n > 0)
    {
    // Got plausible input so keep the CLI awake a little longer.
    resetCLIActiveTimer();

    // Process the input received, with action based on the first char...
    bool showStatus = true; // Default to showing status.
    switch(buf[0])
      {
      // Explicit request for help, or unrecognised first character.
      // Avoid showing status as may already be rather a lot of output.
      default: case '?': { /* dumpCLIUsage(maxSCT); */ showStatus = false; break; }

      // Exit/deactivate CLI immediately.
      // This should be followed by JUST CR ('\r') OR LF ('\n')
      // else the second will wake the CLI up again.
      case 'E': { CLITimeoutM = 0; break; }

      // Status line and optional smart/scheduled warming prediction request.
      case 'S':
        {
        Serial.print(F("Resets/overruns: "));
        const uint8_t resetCount = eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_RESET_COUNT);
        Serial.print(resetCount);
        Serial.print(' ');
        const uint8_t overrunCount = (~eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_OVERRUN_COUNTER)) & 0xff;
        Serial.print(overrunCount);
        Serial.println();
        break; // Note that status is by default printed after processing input line.
        }

      // Handle CLI extension commands.
      // Command of form:
      //   +EXT .....
      // where EXT is the name of the extension, usually 3 letters.
      //
      // It is acceptable for extCLIHandler() to alter the buffer passed,
      // eg with strtok_t().
      case '+':
        {
        const bool success = extCLIHandler(buf, n);
        Serial.println(success ? F("OK") : F("FAILED"));
        break;
        }
      }

    // Almost always show status line afterwards as feedback of command received and new state.
    if(showStatus) { serialStatusReport(); }
    // Else show ack of command received.
    else { Serial.println(F("OK")); }
    }
  else { Serial.println(); } // Terminate empty/partial CLI input line after timeout.

  // Force any pending output before return / possible UART power-down.
  OTV0P2BASE::flushSerialSCTSensitive();

  if(neededWaking) { OTV0P2BASE::powerDownSerial(); }
  }


// One-off setup.
void setup()
  {
  // Set appropriate low-power states, interrupts, etc, ASAP.
  OTV0P2BASE::powerSetup();
  // IO setup for safety, and to avoid pins floating.
  OTV0P2BASE::IOSetup();
  // Restore previous RTC state if available.
  OTV0P2BASE::restoreRTC();
  OTV0P2BASE::serialPrintAndFlush(F("\r\nOpenTRV: ")); // Leading CRLF to clear leading junk, eg from bootloader.
    serialPrintlnBuildVersion();

  // Count resets to detect unexpected crashes/restarts.
  const uint8_t oldResetCount = eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_RESET_COUNT);
  eeprom_write_byte((uint8_t *)V0P2BASE_EE_START_RESET_COUNT, 1 + oldResetCount);

#if defined(DEBUG) && !defined(ENABLE_MIN_ENERGY_BOOT)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("DEBUG");
#endif

  DEBUG_SERIAL_PRINT_FLASHSTRING("Resets: ");
  DEBUG_SERIAL_PRINT(oldResetCount);
  DEBUG_SERIAL_PRINTLN();

  const uint8_t overruns = (~eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_OVERRUN_COUNTER)) & 0xff;
  if(0 != overruns)
    {
    DEBUG_SERIAL_PRINT_FLASHSTRING("Overruns: ");
    DEBUG_SERIAL_PRINT(overruns);
    DEBUG_SERIAL_PRINTLN();
    }

  // Have 32678Hz clock at least running before going any further.
  // Check that the slow clock is running reasonably OK, and tune the fast one to it.
  if(!::OTV0P2BASE::HWTEST::calibrateInternalOscWithExtOsc()) { panic(F("xtal")); } // Async clock not running correctly.

//  // Signal that xtal is running AND give it time to settle.
//  posPOST(0 /*, F("about to test radio module") */);

  // Initialise the radio, if configured, ASAP because it can suck a lot of power until properly initialised.
  PrimaryRadio.preinit(NULL);
  // Check that the radio is correctly connected; panic if not...
  if(!PrimaryRadio.configure(nPrimaryRadioChannels, RFM23BConfigs) || !PrimaryRadio.begin()) { panic(F("r1")); }
  // Apply filtering, if any, while we're having fun...
  PrimaryRadio.setFilterRXISR(FilterRXISR);

//  posPOST(1, F("Radio OK, checking buttons/sensors and xtal"));

  // Seed RNGs, after having gathered some sensor values in RAM...
  OTV0P2BASE::seedPRNGs();

  // Ensure that the unique node ID is set up (mainly on first use).
  // Have one attempt (don't want to stress an already failing EEPROM) to force-reset if not good, then panic.
  // Needs to have had entropy gathered, etc.
  if(!OTV0P2BASE::ensureIDCreated())
    {
    if(!OTV0P2BASE::ensureIDCreated(true)) // Force reset.
      { panic(F("ID")); }
    }

  // Initialised: turn main/heatcall UI LED off.
  OTV0P2BASE::LED_HEATCALL_OFF();

  // Report initial status.
  serialStatusReport();

  // Radio not listening to start with.
  // Ignore any initial spurious RX interrupts for example.
  PrimaryRadio.listen(false);

  // Set up async edge interrupts.
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    //PCMSK0 = PB; PCINT  0--7    (Radio)
    //PCMSK1 = PC; PCINT  8--15
    //PCMSK2 = PD; PCINT 16--24   (Serial RX)

    PCICR =
#if defined(MASK_PB) && (MASK_PB != 0) // If PB interrupts required.
        1 | // 0x1 enables PB/PCMSK0.
#endif
#if defined(MASK_PC) && (MASK_PC != 0) // If PC interrupts required.
        2 | // 0x2 enables PC/PCMSK1.
#endif
#if defined(MASK_PD) && (MASK_PD != 0) // If PD interrupts required.
        4 | // 0x4 enables PD/PCMSK2.
#endif
        0;

#if defined(MASK_PB) && (MASK_PB != 0) // If PB interrupts required.
    PCMSK0 = MASK_PB;
#endif
#if defined(MASK_PC) && (MASK_PC != 0) // If PC interrupts required.
    PCMSK1 = MASK_PC;
#endif
#if defined(MASK_PD) && (MASK_PD != 0) // If PD interrupts required.
    PCMSK2 = MASK_PD;
#endif
    }

  // Start local counters in randomised positions to help avoid inter-unit collisions,
  // eg for mains-powered units starting up together after a power cut,
  // but without (eg) breaking any of the logic about what order things will be run first time through.
  // Uses some decent noise to try to start the units separated.
  const uint8_t b = OTV0P2BASE::getSecureRandomByte(); // randRNG8();
  // Start within bottom half of minute (or close to); sensor readings happen in second half.
  OTV0P2BASE::setSeconds(b >> 2);
  // Start anywhere in first 4 minute cycle.
  minuteCount = b & 3;

  // Start listening.
  PrimaryRadio.listen(true);

  // Set appropriate loop() values just before entering it.
  TIME_LSD = OTV0P2BASE::getSecondsLT();
  }


// Main code here, loops every 2s.
void loop()
  {
  setUpContinuousRX();
  OTV0P2BASE::powerDownSerial(); // Ensure that serial I/O is off.
  // Power down most stuff (except radio for hub RX).
  OTV0P2BASE::minimisePowerWithoutSleep();
  uint_fast8_t newTLSD;
  while(TIME_LSD == (newTLSD = OTV0P2BASE::getSecondsLT()))
    {
    // Poll I/O and process message incrementally (in this otherwise idle time).
    // Come back and have another go immediately until no work remaining.
    if(handleQueuedMessages(&Serial, true, &PrimaryRadio)) { continue; }

    // Normal long minimal-power sleep until wake-up interrupt.
    // Rely on interrupt to force quick loop round to I/O poll.
    OTV0P2BASE::sleepUntilInt();
    }
  TIME_LSD = newTLSD;

  // Reset and immediately re-prime the RTC-based watchdog.
  OTV0P2BASE::resetRTCWatchDog();
  OTV0P2BASE::enableRTCWatchdog(true);

#if 1  // Force TX once every minute for dev work.
    // Use the zeroth second in each minute to force extra deep device sleeps/resets, etc.
    const bool second0 = (0 == TIME_LSD);
    if (second0) {
    // If CC1 hub then allow +CC1 ? command to poll a remote relay.
    // Full command is:
    //    +CC1 ? hc1 hc2 rp lc lt lf
    // e.g. +CC1 ? 70 04 10 1 1 1
    // ie six numeric arguments, see below, with out-of-range values coerced (other than housecodes):
    //            // Factory method to create instance.
    //            // Invalid parameters (except house codes) will be coerced into range.
    //            //   * House code (hc1, hc2) of valve controller that the poll/command is being sent to.
    //            //   * rad-open-percent     [0,100] 0-100 in 1% steps, percent open approx to set rad valve (rp)
    //            //   * light-colour         [0,3] bit flags 1==red 2==green (lc) 0 => stop everything
    //            //   * light-on-time        [1,15] (0 not allowed) 30-450s in units of 30s (lt) ???
    //            //   * light-flash          [1,3] (0 not allowed) 1==single 2==double 3==on (lf)
    //            // Returns instance; check isValid().
    //            static CC1PollAndCommand make(uint8_t hc1, uint8_t hc2,
    //                                          uint8_t rp,
    //                                          uint8_t lc, uint8_t lt, uint8_t lf);
        OTProtocolCC::CC1PollAndCommand q = OTProtocolCC::CC1PollAndCommand::make(
                70, 04,
                10,
                1, 1, 1);
        if(q.isValid())
        {
            uint8_t txbuf[OTProtocolCC::CC1PollAndCommand::primary_frame_bytes+1]; // More than large enough for preamble + sync + alert message.
            const uint8_t bodylen = q.encodeSimple(txbuf, sizeof(txbuf), true);
            // Non-secure: send raw frame as-is.
            // TX at normal power since ACKed and can be repeated if necessary.
            if(PrimaryRadio.sendRaw(txbuf, bodylen)) {
                OTV0P2BASE::serialPrintlnAndFlush(F("!TX success"));
                return(true);
            } // Success!
            // Fall-through is failure...
            OTV0P2BASE::serialPrintlnAndFlush(F("!TX fail"));
        }
        return(false); // FAILED if fallen through from above.
    }
#endif // 0
  // If time to do some trailing processing, CLI, etc...
  while(timeToHandleMessage())
    {
    if(handleQueuedMessages(&Serial, true, &PrimaryRadio)) { continue; }
    // Done queued work...
    // Command-Line Interface (CLI) polling, if still active.
    if(isCLIActive())
      {
      // Don't wait too late to start listening for a command
      // to give the user a decent chance to enter a command string
      // and/or that may involve encryption.
      const uint8_t stopBy = min((OTV0P2BASE::GSCT_MAX/4)*3, nearOverrunThreshold - 1);
      pollCLI(stopBy, 0 == TIME_LSD);
      }
    break;
    }

  // Detect and handle (actual or near) overrun, if it happens, though it should not.
  if(TIME_LSD != OTV0P2BASE::getSecondsLT())
    {
    // Increment the overrun counter (stored inverted, so 0xff initialised => 0 overruns).
    const uint8_t orc = 1 + ~eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_OVERRUN_COUNTER);
    OTV0P2BASE::eeprom_smart_update_byte((uint8_t *)V0P2BASE_EE_START_OVERRUN_COUNTER, ~orc);
#if 1 && defined(DEBUG)
    DEBUG_SERIAL_PRINTLN_FLASHSTRING("!loop overrun");
#endif
    TIME_LSD = OTV0P2BASE::getSecondsLT(); // Prepare to sleep until start of next full minor cycle.
    }
  }
