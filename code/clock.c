#include <sr595.h>
#include <stdio.h>
#include <wiringPi.h>

#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define BLANKING 4
#define DOTS 8
#define LOCK 1

int main(void) {
  int i, bit;

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
  } interfaceUnion;

  interfaceUnion.interface.lower = 0;
  interfaceUnion.interface.upper = 0;
  interfaceUnion.interface.blanking = 0;
  interfaceUnion.interface.seconds = 0;
  interfaceUnion.interface.minutes = 0;
  interfaceUnion.interface.hours = 0;
  interfaceUnion.interface.latchU3 = 0;

  wiringPiSetupGpio();

  // Pin base 100 for 32 pins.
  //    Use Broadcom GPIO pins 10, 11 & 8 for data, clock and latch
  sr595Setup(100, 32, 10, 11, 8);

  // setup the encoder on the back
  // pin 1 = gnd
  // pins 2-5 = gpio 21,20,16,12

  pinMode(21, INPUT);
  pullUpDnControl(21, PUD_UP);
  pinMode(20, INPUT);
  pullUpDnControl(20, PUD_UP);
  pinMode(16, INPUT);
  pullUpDnControl(16, PUD_UP);
  pinMode(12, INPUT);
  pullUpDnControl(12, PUD_UP);

  printf("RTC to derive True Time 60-DC display\n");
  struct wiringPiNodeStruct *node =
      wiringPiFindNode(100); // find our node so we can access our data!
  if (node == NULL) {
    printf("ERROR! no node found %s %i\n", __FILE__, __LINE__);
    return -1;
  }

  for (;;) {
    time_t now = time(NULL);

    char t[26];
    asctime_r(gmtime(&now), t);

    int hh = t[11] - '0', hl = t[12] - '0', mh = t[14] - '0', ml = t[15] - '0',
        sh = t[17] - '0', sl = t[18] - '0';

    //    printf ("%s h=%i%i m=%i%i s=%i%i\n",t,hh,hl,mh,ml,sh,sl);

    // get the offset
    int offset = ~(digitalRead(21) << 3 | digitalRead(20) << 2 |
                   digitalRead(16) << 1 | digitalRead(12) << 0) &
                 0x0f;
    //    printf ("12=%i 16=%i 20=%i 21=%i
    //    ",digitalRead(12),digitalRead(16),digitalRead(20),digitalRead(21));
    //    printf ("offset=%i\n",offset);
    int hours = hh * 10 + hl - offset;
    //    printf ("hours1=%i\n",hours);
    if (hours > 24) {
      hours -= 24;
    } else if (hours < 0) {
      hours += 24;
    }
    hours %= 24;
    //    printf ("hours2=%i\n",hours);
    hl = hours % 10;
    hh = hours / 10;

    //    printf ("%s h=%i%i m=%i%i s=%i%i\n",t,hh,hl,mh,ml,sh,sl);
#define DWELL 8000

    // send seconds
    interfaceUnion.interface.lower = sl;
    interfaceUnion.interface.upper = sh;
    interfaceUnion.interface.seconds = 1;
    node->data3 = interfaceUnion.data;
    digitalWrite(100 + 8, 1);
    interfaceUnion.interface.seconds = 0;
    delayMicroseconds(DWELL);
    digitalWrite(100 + 8, 0);

    // send minutes
    interfaceUnion.interface.lower = ml;
    interfaceUnion.interface.upper = mh;
    interfaceUnion.interface.minutes = 1;
    node->data3 = interfaceUnion.data;
    digitalWrite(100 + 8, 1);
    interfaceUnion.interface.minutes = 0;
    delayMicroseconds(DWELL);
    digitalWrite(100 + 8, 0);

    // send hours
    interfaceUnion.interface.lower = hl;
    interfaceUnion.interface.upper = hh;
    interfaceUnion.interface.hours = 1;
    node->data3 = interfaceUnion.data;
    digitalWrite(100 + 8, 1);
    interfaceUnion.interface.hours = 0;
    delayMicroseconds(DWELL);
    digitalWrite(100 + 8, 0);

    // get the status of NTP for the lock LED
    interfaceUnion.interface.upper = BLANKING;

    int locked=system("ntpstat 2>&1 > /dev/null");
    if (locked==0) { // anything but zero is unlocked....
      interfaceUnion.interface.upper |= LOCK;
    }

    // set the : to toggle every second
    if (sl & 1) {
      interfaceUnion.interface.upper |= DOTS;
    }

    interfaceUnion.interface.lower = 0;
    node->data3 = interfaceUnion.data;
    digitalWrite(100 + 12, 1);
    digitalWrite(100 + 12, 0); // toggle the e bit to load the latch
  }
  return 0;
}
