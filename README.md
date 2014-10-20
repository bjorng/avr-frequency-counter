avr-frequency-counter
=====================

In my home electronics lab, I thought that a frequency counter would
be a nice thing to have, but not essential. Therefore I decided to
build a frequency counter myself instead of buying one. In the end, I
have probably spent more money than if I would have bought a
frequency counter, but I have learned a lot while building it.

Here is the software for the frequency counter. I don't have a CAD
program yet and I also think that my hardware needs some more work,
so I will not provide detailed schematics for the construction, but
I will provide an overview of the hardware setup below.

My Requirements And How To Meet Them
------------------------------------

I wanted to be able measure even low frequencies accurately (down to
0.1 Hz) and I didn't want any manual setup for different frequency
ranges.

The obvious way to count frequency is to count the number of events
during a fixed time period (for example one second) and then divide
the number of events by the length of the time period. But for low
frequencies the time need to be long to achive good resolution. For
example, if the time period is one second while we are measuring a
signal with a frequency of about 1Hz, the measured frequency could be
either 0, 1, or 2, depending on how the events fall in our measurement
period. So for low frequencies, we would need long measurement periods
(several minutes).

The way around that problem is to use reciprocal counting. We measure
the time for a fixed number of events and calculate the frequency as
the number of events divided by the measured time period.  The error
in the measurement is independent of the frequency being measured, and
only depends on the frequency of the clock we use for measuring. As
long as the frequency being measured is less than the frequency of the
clock being used for measurement reciprocal counting is always better
than direct counting.

The clock we use for measuring the time is the CPU clock divided by
64, or 312.5kHz. We continue to use reciprocal counting also above
312.5kHz. To ensure that we get 4 significant digits, we choose the
number of events sufficiently high so that the length of the time
period will be at least 10000 clock ticks.


Hardware Setup
--------------

The software is written for the Atmel ATtiny84A. It should work
without modifications on an ATtiny44A.

The ATtiny84A should run at 20MHz (which implies that it must be
powered by a 5V supply). If you choose run at a lower clock frequency,
it will **probably** work if you change the value of **CLOCK** in the
Makefile (but is not tested by me).

The signal to be measured should be a square wave from 0V to 5V, and
be connected to both pin 5 (INT0/PB2) and pin 9 (PA4/T1). While you
could connect external signals directly to those pins, I recommend
some sort of input protection and signal conditioning, as a minimum a
74HC14 (an inverting buffer with a schmitt-trigger).

The display is an [DOGM 081][3]. It has one line by 8 characters. The code
for controling the display is at the end of main.c. If you want to use
some other display, you'll just need to provide alternative
implementations for the functions **lcd\_init()**, **ldc\_home()**, and
**lcd\_putc()**.

The display should be wired up for SPI mode (see the [data sheet][3]). The
pins should be connected like this:

* PA0 to SI
* PA1 to CLK
* PA2 to CSB
* PA3 to RS

References
----------

It seems that Hewlett-Packard pioneered the concept of reciprocal
counting.

* [Fundamentals of Electronic Counters][2]. An App Note from
  Hewlett-Packard about frequency counters.

* [Hewlett-Packard Journal from May 1969][1]. The entire issue is
  about frequency counters.

  [1]: http://www.hpl.hp.com/hpjournal/pdfs/IssuePDFs/1969-05.pdf
  [2]: http://cp.literature.agilent.com/litweb/pdf/5965-7660E.pdf
  [3]: http://www.lcd-module.com/eng/pdf/doma/dog-me.pdf
