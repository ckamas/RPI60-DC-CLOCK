#include <lgpio.h>
#include <signal.h>
#include <stdio.h>

#include <stdint.h>
#include <stdlib.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#define BLANKING 4
#define DOTS 8
#define LOCK 1

// how long each digit pair stays strobed (microseconds)
#define DWELL 5000

#define GPIOCHIP 0 // /dev/gpiochip0 (the 40-pin header on all Pi models)

// 74HC595 chain: Broadcom GPIO pins 10, 11 & 8 for data, clock and latch
#define SR_DATA 10
#define SR_CLOCK 11
#define SR_LATCH 8
#define SR_BITS 32

// Bit positions in the 32-bit shift register image that are toggled directly.
// They must match the bitfield layout of interfaceUnion in main(); main()
// verifies that at startup.
#define SR_BIT_BLANKING 8  // == interface.blanking
#define SR_BIT_SECONDS 9   // == interface.seconds
#define SR_BIT_MINUTES 10  // == interface.minutes
#define SR_BIT_HOURS 11    // == interface.hours
#define SR_BIT_LATCH_U3 12 // == interface.latchU3

// back-panel offset encoder: pin 1 = gnd, pins 2-5 = gpio 21,20,16,12
static const int ENC_PINS[4] = {21, 20, 16, 12};

static int chip = -1;          // lgpio gpiochip handle
static uint32_t srOutput = 0;  // shadow of the shift register contents

// Shift all SR_BITS out MSB first, then rising edge on latch.
// (same behaviour as wiringPi's sr595 driver this replaces)
static void srShift(void) {
  lgGpioWrite(chip, SR_LATCH, 0);
  for (int bit = SR_BITS - 1; bit >= 0; --bit) {
    lgGpioWrite(chip, SR_DATA, (srOutput >> bit) & 1);
    lgGpioWrite(chip, SR_CLOCK, 1);
    lgGpioWrite(chip, SR_CLOCK, 0);
  }
  lgGpioWrite(chip, SR_LATCH, 1);
}

// set/clear one bit of the shift register image and push it out
static void srWriteBit(int bit, int value) {
  if (value)
    srOutput |= (1u << bit);
  else
    srOutput &= ~(1u << bit);
  srShift();
}

// Is the system clock synchronised?  Returns 0 when locked, non-zero otherwise
// (same convention ntpstat used).
//
// chronyd publishes its state to the kernel (it clears STA_UNSYNC once it is
// synchronised; this needs "rtcsync" in chrony.conf, which setup.sh enforces).
// Reading it with ntp_adjtime(modes=0) is a cheap, read-only syscall, so we
// don't have to fork a process and stall the display refresh.
static int clockUnlocked(void) {
  struct timex tx = {0}; // modes == 0: query only, changes nothing
  int state = ntp_adjtime(&tx);
  return (state >= 0 && state != TIME_ERROR) ? 0 : 1;
}

// Read the 4-bit back-panel offset encoder (switches pull to ground).
// Returns `previous` if any pin can't be read, rather than a bogus offset.
static int readOffset(int previous) {
  int v[4];
  for (int i = 0; i < 4; i++) {
    v[i] = lgGpioRead(chip, ENC_PINS[i]);
    if (v[i] < 0)
      return previous;
  }
  // ENC_PINS = {21, 20, 16, 12} -> bits 3, 2, 1, 0
  return ~(v[0] << 3 | v[1] << 2 | v[2] << 1 | v[3] << 0) & 0x0f;
}

static volatile sig_atomic_t stopRequested = 0;
static void onSignal(int sig) {
  (void)sig;
  stopRequested = 1;
}

// Strobe all-zero data into one digit-pair latch, using the same
// blanking-bit sequence as the normal refresh.
static void loadZeros(int strobeBit) {
  srOutput = 1u << strobeBit;
  srWriteBit(SR_BIT_BLANKING, 1);
  lguSleep(DWELL / 1e6);
  srWriteBit(SR_BIT_BLANKING, 0);
}

// Called when we are asked to stop: put zeros in every latch (seconds,
// minutes, hours, then the aux latch holding BLANKING/LOCK/DOTS) and leave the
// pins at their idle levels (data/clock low, latch high).
static void shutdownDisplay(void) {
  loadZeros(SR_BIT_SECONDS);
  loadZeros(SR_BIT_MINUTES);
  loadZeros(SR_BIT_HOURS);
  srOutput = 0;
  srWriteBit(SR_BIT_LATCH_U3, 1); // aux latch loads on this bit's rising edge
  srWriteBit(SR_BIT_LATCH_U3, 0);
  srShift(); // all-zero image, latch idles high
}

int main(void) {
  int i;

  union {
    struct {
      uint32_t lower : 4;
      uint32_t upper : 4;
      uint32_t blanking : 1;
      uint32_t seconds : 1;
      uint32_t minutes : 1;
      uint32_t hours : 1;
      uint32_t latchU3 : 1;
    } interface;
    uint32_t data;
  } interfaceUnion = {.data = 0}; // zero all 32 bits, not just the 13 used

  interfaceUnion.interface.lower = 0;
  interfaceUnion.interface.upper = 0;
  interfaceUnion.interface.blanking = 0;
  interfaceUnion.interface.seconds = 0;
  interfaceUnion.interface.minutes = 0;
  interfaceUnion.interface.hours = 0;
  interfaceUnion.interface.latchU3 = 0;

  // make sure the bitfield layout matches the SR_BIT_* positions we rely on
#define CHECK_BIT(field, pos)                                                  \
  do {                                                                         \
    interfaceUnion.data = 0;                                                   \
    interfaceUnion.interface.field = 1;                                        \
    if (interfaceUnion.data != (1u << (pos))) {                                \
      printf("ERROR! bitfield layout mismatch (" #field ") %s %i\n",           \
             __FILE__, __LINE__);                                              \
      return -1;                                                               \
    }                                                                          \
  } while (0)
  CHECK_BIT(blanking, SR_BIT_BLANKING);
  CHECK_BIT(seconds, SR_BIT_SECONDS);
  CHECK_BIT(minutes, SR_BIT_MINUTES);
  CHECK_BIT(hours, SR_BIT_HOURS);
  CHECK_BIT(latchU3, SR_BIT_LATCH_U3);
#undef CHECK_BIT
  interfaceUnion.data = 0;

  chip = lgGpiochipOpen(GPIOCHIP);
  if (chip < 0) {
    printf("ERROR! can't open gpiochip%d (%s) %s %i\n", GPIOCHIP,
           lguErrorText(chip), __FILE__, __LINE__);
    return -1;
  }

  // shift register outputs: data/clock low, latch idles high
  if (lgGpioClaimOutput(chip, 0, SR_DATA, 0) < 0 ||
      lgGpioClaimOutput(chip, 0, SR_CLOCK, 0) < 0 ||
      lgGpioClaimOutput(chip, 0, SR_LATCH, 1) < 0) {
    printf("ERROR! can't claim shift register pins %s %i\n", __FILE__,
           __LINE__);
    return -1;
  }

  // setup the encoder on the back (inputs with pull-ups)
  for (i = 0; i < 4; i++) {
    if (lgGpioClaimInput(chip, LG_SET_PULL_UP, ENC_PINS[i]) < 0) {
      printf("ERROR! can't claim encoder gpio %d %s %i\n", ENC_PINS[i],
             __FILE__, __LINE__);
      return -1;
    }
  }

  printf("RTC to derive True Time 60-DC display\n");


  int oldSl = -1; // used to detect a new led update period...
  int locked = 1; // set to zero when the clock is synchronised
  int offset = 0; // hours offset from the back-panel encoder

  // exit cleanly on systemctl stop/restart, reboot or ctrl-c
  struct sigaction sa = {0};
  sa.sa_handler = onSignal; // no SA_RESTART: interrupt sleeps promptly
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  while (!stopRequested) {
    time_t now = time(NULL);

    struct tm utc;
    gmtime_r(&now, &utc);

    int hh = utc.tm_hour / 10, hl = utc.tm_hour % 10, mh = utc.tm_min / 10,
        ml = utc.tm_min % 10, sh = utc.tm_sec / 10, sl = utc.tm_sec % 10;

    //    printf ("%s h=%i%i m=%i%i s=%i%i\n",t,hh,hl,mh,ml,sh,sl);

    // get the offset
    offset = readOffset(offset);
    //    printf ("12=%i 16=%i 20=%i 21=%i
    //    ",digitalRead(12),digitalRead(16),digitalRead(20),digitalRead(21));
    //    printf ("offset=%i\n",offset);
    int hours = hh * 10 + hl - offset;
    //    printf ("hours1=%i\n",hours);
    if (hours >= 24) {
      hours -= 24;
    } else if (hours < 0) {
      hours += 24;
    }
    hours %= 24;
    //    printf ("hours2=%i\n",hours);
    hl = hours % 10;
    hh = hours / 10;

    //    printf ("%s h=%i%i m=%i%i s=%i%i\n",t,hh,hl,mh,ml,sh,sl);
    // Each field is sent as: load the shift register image with this field's
    // strobe bit set, push it with the blanking bit up for DWELL, then push it
    // again with blanking down.  The "interface.<field> = 0" after each push
    // clears OUR copy so the next field's image doesn't carry this strobe
    // (the image already pushed keeps its strobe until the next field loads).
    // Don't remove them: all three strobes would then pile up.

    // send seconds
    interfaceUnion.interface.lower = sl;
    interfaceUnion.interface.upper = sh;
    interfaceUnion.interface.seconds = 1;
    srOutput = interfaceUnion.data;
    srWriteBit(SR_BIT_BLANKING, 1);
    interfaceUnion.interface.seconds = 0;
    lguSleep(DWELL / 1e6);
    srWriteBit(SR_BIT_BLANKING, 0);

    // send minutes
    interfaceUnion.interface.lower = ml;
    interfaceUnion.interface.upper = mh;
    interfaceUnion.interface.minutes = 1;
    srOutput = interfaceUnion.data;
    srWriteBit(SR_BIT_BLANKING, 1);
    interfaceUnion.interface.minutes = 0;
    lguSleep(DWELL / 1e6);
    srWriteBit(SR_BIT_BLANKING, 0);

    // send hours
    interfaceUnion.interface.lower = hl;
    interfaceUnion.interface.upper = hh;
    interfaceUnion.interface.hours = 1;
    srOutput = interfaceUnion.data;
    srWriteBit(SR_BIT_BLANKING, 1);
    interfaceUnion.interface.hours = 0;
    lguSleep(DWELL / 1e6);
    srWriteBit(SR_BIT_BLANKING, 0);


    // only do this once a second....
    if (oldSl!=sl) {
      oldSl=sl;

      // cheap syscall, so it is fine to check every second
      locked = clockUnlocked();

      interfaceUnion.interface.upper = BLANKING;

      if (locked==0) { // anything but zero is unlocked....
        interfaceUnion.interface.upper |= LOCK;
      }

      // set the : to toggle every second
      if (sl & 1) {
        interfaceUnion.interface.upper |= DOTS;
      }

      interfaceUnion.interface.lower = 0;
      srOutput = interfaceUnion.data;
      srWriteBit(SR_BIT_LATCH_U3, 1);
      srWriteBit(SR_BIT_LATCH_U3, 0); // toggle the e bit to load the latch
      }
    }

  shutdownDisplay();
  lgGpiochipClose(chip);
  return 0;
}
