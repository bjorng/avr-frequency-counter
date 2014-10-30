/*
 *    Frequency counter for ATTiny84A
 *    Copyright (C) 2014  Bjorn Gustavsson
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License along
 *    with this program; if not, write to the Free Software Foundation, Inc.,
 *    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Reciprocal Frequency Counter.
 *
 * Count and show the frequency of the square wave on input pins
 * INT0/PB2 and T1/PA4. We use INT0 for low frequencies and T1
 * for high frequencies.
 *
 * We use the reciprocal counter method. Instead of counting
 * the number of events (falling edges) during a fixed time
 * period, we measure the time period for a fixed number of
 * events and calculate the frequency as the number of events
 * divided by the length of the time period. That gives us
 * good resolution even for very low frequencies.
 */

#include <stdio.h>
#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#define DEBUG 0

#define MAX_PERIOD 0xffffffffUL
typedef unsigned long tick_t;

/*
 * API functions for the display. A different kind
 * of display can be used if those functions are
 * reimplemnted.
 */
static void lcd_init(void);
static void lcd_home(void);
static void lcd_putc(char c);

/*
 * Other functions.
 */
#if DEBUG
static void inline debug_show_state(uint8_t n);
#endif
static inline unsigned long cli_ticks(void)
    __attribute__ ((always_inline));
static void init_time_keeping(void);
static void init_event_counting(void);
static void slow_mode(void);
static void inline set_timer_cmp_reg(uint8_t log2ne)
    __attribute__ ((always_inline));
static void display_measurement(uint8_t n, tick_t p);
static void show_line(char* s);
static void display_freq(unsigned long freq);

struct counter {
    /*
     * The current frequency can be calculated as:
     *
     *     2^log2num_events / (64 * F_CPU * period)
     *
     * A tick is 64 cpu cycles.
     */
    tick_t period;	/* Length of last measured period (in ticks) */
    uint8_t log2num_events;	/* log2 of number of events that occurred. */

    /*
     * Internal data for the interrupt routines to keep track of the
     * current measuring period.
     */
    uint8_t first_time;
    uint8_t current_log2num_events; /* For the current counting period.   */
    tick_t prev_ticks;	     /* Number of ticks at start of period. */
};

static volatile struct counter slow_cnt = { MAX_PERIOD, 0, 1, 0, 0 };
static volatile struct counter fast_cnt = { MAX_PERIOD, 0, 1, 1, 0 };
static struct counter volatile *current;

#define WD_TOP 4
static volatile signed char fast_wd = WD_TOP;

int main(void)
{
    _delay_ms(100);		/* Wait for stable power */
    init_time_keeping();
    init_event_counting();
    sei();
    lcd_init();

    for (;;) {
	uint8_t n;
	unsigned long p;

	_delay_ms(100);

	/*
	 * Read out information about the latest completed
	 * measurement period.
	 */
	cli();
	n = current->log2num_events;
	p = current->period;
	sei();

#if DEBUG
	debug_show_state(n);
#endif

	/*
	 * See if we should switch to slow mode.
	 */
	if (fast_wd-- < 0 && current == &fast_cnt) {
	    /*
	     * We have not got any fast mode interrupts for 400 ms.
	     * Switch to slow mode.
	     */
	    fast_wd = WD_TOP;
	    slow_mode();
	}

	/* Now display the result from the last measurement. */
	display_measurement(n, p);
    }
    return 0;
}

#if DEBUG
void debug_show_state(uint8_t n)
{
    int nc;

    DDRA |= _BV(PA5) | _BV(PA6);

    /*
     * Show the 2 logarithm for the number of events as a number of
     * square wave cycles on output port PA5. (Connect an oscilloscope
     * probe to PA5 to see it. Set the triggering mode to Normal or
     * increase the trigger Holdoff time so that the square wave stays
     * on the screen.)
     */

    nc = 2 * n;
    while (nc-- > 0) {
	PINA |= _BV(PA5);
	_delay_us(20);
	if (nc % 10 == 0) {
	    _delay_us(50);
	}
    }

    /*
     * Set PA6 high if we are in slow mode and low otherwise.
     * (Connect a LED or logic probe to PA6 to see the mode.)
     */

    if (current == &slow_cnt) {
	PORTA |= _BV(PA6);
    } else {
	PORTA &= ~_BV(PA6);
    }
}
#endif

/*
 * Time keeping. We count "ticks" (1 tick = 64 cpu cycles) and only
 * convert to time when we'll need to show the frequency.
 */

static void init_time_keeping(void)
{
    /*
     * Set prescaler to 64. Using a 20Mhz clock, that will give
     * a timer tick time of appr. 3.2 us, and timer overflow appr.
     * 819 us.
     */

    TCCR0B = _BV(CS01) | _BV(CS00);

    /* Enable the overflow interrupt for time 0 */
    TIMSK0 = _BV(TOIE0);
}

static volatile unsigned long timer0_overflow_count = 0;

ISR(TIM0_OVF_vect)
{
    timer0_overflow_count++;
}

/*
 * Return the number of timer ticks elapsed. Interrupts MUST
 * be disabled when calling this function.
 */
static tick_t cli_ticks(void)
{
    uint8_t t;
    tick_t m;

    m = timer0_overflow_count;
    t = TCNT0;
    if (TIFR0 & _BV(TOV0) && t < 255) {
	m++;
    }
    return (m << 8) | t;
}

/* =====================================================================
 *
 * Count rising edges of the input signal and note their time (in ticks).
 * Output will be the number of rising edges and the time in ticks between
 * them. By dividing the number of edges by the time we get the frequency.
 *
 * We decide beforehand how many events (edges) we want to count. To
 * simplify the calculations, the number of events are the powers of
 * two: 1, 2, 3, 4, ..., 1048576. We adjust the number of events,
 * aiming for a period of at least 10000 ticks. If the period goes below
 * 10000 ticks, we'll adjust the number of events to count upwards.
 * If the number of ticks goes above 30000, we will adjust fewer
 * events next time.
 *
 * We can count events using timer 1 in CTC mode. However, it seems
 * that the minimum number of events that can be counted is 2.
 * Therefore, to react faster while counting low frequencies (less
 * than 20Hz), we use the external interrupt to count individual
 * falling edges.
 *
 * When we switch to slow mode (using the external interrupt) we
 * don't turn off timer 1. That way, we can quickly switch back to
 * fast mode.
 *
 * ====================================================================
 */

static void init_event_counting(void)
{
    /*
     * Our hardware outside the microcontroller has converted the
     * incoming signal to a square wave and inverted it, and is
     * feeding to both input pins PB2 (INT0) and PA4 (T1). We
     * want to count the rising of the original signal, so since
     * our input signal is inverted, we will need react to the
     * FALLING edges.
     */

    /*
     * Generate interrupts on the falling edge of the signal on PB2.
     */

    MCUCR = _BV(ISC01);
    GIMSK = _BV(INT0);

    /*
     * Set up timer 1 in CTC mode, using the input signal on PA4 as
     * the clock for the timer.
     */

    TCCR1A = 0;
    TCCR1B = _BV(WGM12) | _BV(CS12) | _BV(CS11);
    set_timer_cmp_reg(fast_cnt.current_log2num_events);
    TCNT1 = 0;
    TIMSK1 = _BV(OCIE1A);
    TIFR1 |= _BV(OCF1A);

    /*
     * Start up in slow mode. The interrupt handler for slow
     * mode will shift to fast mode if the frequency is too high.
     */

    current = &slow_cnt;
}

/*
 * Interrupt service routine for the slow counting mode.
 */
ISR(EXT_INT0_vect)
{
    tick_t cs = cli_ticks();

    if (slow_cnt.first_time) {
	/* We can't calculate a period because it's the first time. */
	slow_cnt.first_time = 0;
	slow_cnt.prev_ticks = cs;
    } else {
	/*
	 * Calculate the length of period that just ended. We don't need
	 * to update log2num_events since it is always 0 (= one event).
	 */
	tick_t period = cs - slow_cnt.prev_ticks;
	slow_cnt.period = period;
	slow_cnt.prev_ticks = cs;

	/*
	 * If the period is less than 100 ticks (320 us at 20Mhz), do
	 * an emergency switch to fast counter mode. This is normally
	 * handled by the interrupt routine for the fast mode, but if
	 * the frequency rises quickly it might not react sufficiently
	 * quickly. Note that the external pin interrupt has a higher
	 * priority then any of the timer interrupts, so if the
	 * incoming frequency is too high no other interrupt routine
	 * than the external interrupt will ever be called.
	 */
	if (period < 100UL) {
	    GIMSK = 0;
	    fast_cnt.period = MAX_PERIOD;
	    fast_cnt.first_time = 1;
	    fast_cnt.current_log2num_events = 1;
	    fast_cnt.prev_ticks = cs;
	    OCR1A = (1 << fast_cnt.current_log2num_events) - 1;
	    TCNT1 = 0;
	    current = &fast_cnt;
	}
    }
}

/*
 * Force switch to slow mode. Reinitialize the fast mode
 * counter to count two events.
 */
static void slow_mode(void)
{
    cli();
    GIMSK = _BV(INT0);
    slow_cnt.first_time = 1;
    slow_cnt.period = MAX_PERIOD;
    current = &slow_cnt;
    fast_cnt.first_time = 1;
    fast_cnt.current_log2num_events = 1;
    set_timer_cmp_reg(fast_cnt.current_log2num_events);
    TCNT1 = 0;
    sei();
}

static volatile uint8_t counter_high;
static volatile uint8_t cmp_high;

static void inline set_timer_cmp_reg(uint8_t log2ne)
{
    if (log2ne <= 16) {
	/*
	 * Set up the counter from 2 up to 2^16 events.
	 */
	OCR1A = (1UL << log2ne) - 1;
	cmp_high = 0;
    } else {
	/*
	 * Set up our extended counter to count more than
	 * 2^16 events.
	 */
	OCR1A = 0xffff;
	cmp_high = (1 << (log2ne-16)) - 1;
	counter_high = 0;
    }
}

/*
 * Desired minimum number of ticks.
 */
#define MIN_PERIOD 10000UL

/*
 * Interrupt service routine for the fast counting mode.
 */
ISR(TIM1_COMPA_vect)
{
    tick_t ticks = cli_ticks();

    fast_wd = WD_TOP;

    if (counter_high++ != cmp_high) {
	/*
	 * Counter set up to count more than 2^16 events
	 * (for high frequencies).
	 */
	return;
    }
    counter_high = 0;

    if (fast_cnt.first_time) {
	/*
	 * The very first time. We can't calculate a period.
	 */
	fast_cnt.first_time = 0;
	fast_cnt.prev_ticks = ticks;
	return;
    }

    /*
     * Calculate the result for the period that was just finished.
     */
    uint8_t log2ne = fast_cnt.current_log2num_events;
    fast_cnt.log2num_events = log2ne;
    tick_t period = fast_cnt.period = ticks - fast_cnt.prev_ticks;
    fast_cnt.prev_ticks = ticks;

    /*
     * Now see if we should adjust the number of events we are counting
     * for each period.
     */

    if (period < MIN_PERIOD && log2ne < 20) {
	/*
	 * Too short period. Count more events next time.
	 */
	do {
	    log2ne++;
	    period *= 2;
	} while (period < MIN_PERIOD && log2ne < 20);
	set_timer_cmp_reg(log2ne);
	GIMSK = 0;
	current = &fast_cnt;
	fast_cnt.current_log2num_events = log2ne;
    } else if (period > MIN_PERIOD*3 && log2ne > 1) {
	/*
	 * Too long period. Count fewer events next time.
	 */
	do {
	    log2ne--;
	    period /= 2;
	} while (period > MIN_PERIOD*3 && log2ne > 1);
	set_timer_cmp_reg(log2ne);
	fast_cnt.current_log2num_events = log2ne;
    }

    /*
     * Check if we should change mode.
     */

    if (current == &fast_cnt) {
	if (period > MIN_PERIOD*3 && log2ne == 1) {
	    /*
	     * Too long period. Switch to slow mode.
	     */
	    GIMSK = _BV(INT0);
	    slow_cnt.period = period / 2;
	    slow_cnt.prev_ticks = ticks;
	    slow_cnt.first_time = 1;
	    current = &slow_cnt;
	}
    } else if (slow_cnt.period < MIN_PERIOD) {
	/*
	 * Running too fast for slow mode. Switch to fast mode.
	 */
	GIMSK = 0;
	current = &fast_cnt;
    }
}

/* ================================================================
 *
 * Display the frequency from the last measurement.
 *
 * ================================================================
 */

static void display_measurement(uint8_t n, tick_t ticks)
{
    unsigned long f = 0;

    if (ticks == 0) {
	show_line("---");
    } else {
	/*
	 * Here we want to calculate the frequency in dHz
	 * (tenths of Hz). We use dHz instead of Hz so that
	 * we don't have to use any floating point arithmetics.
	 *
	 * The frequency expressed in ticks is (1 << n) / ticks.
	 * To get the frequency in Hz we must multiply by the
	 * tick frequency, which is F_CPU / 64. To get dHz we
	 * must multiply by 10. That is, the frequency in dHz
	 * is:
	 *
	 *    10 * F_CPU / 64 * (1 << n) / ticks
	 *
	 * or
	 *
	 *    (10 * F_CPU / 64) << n / ticks
	 *
	 * To round up to the nearest dHz, we first add ticks / 2
	 * before the division. Thus the final formula is:
	 *
	 *    (((10 * F_CPU / 64) << n) + ticks / 2) / ticks
	 *
	 */
	if (n < 11) {
	    /*
	     * If F_CPU is 20Mhz, the largest n that will not
	     * overflow a 32 bit unsigned integer is 10:
	     *
	     *  10*20000000/64 << 10 = 0xBEBC2000
	     *
	     * Using 32-bit arithmetic for this formula is
	     * more than 3 times faster than 64-bit arithmetic.
	     */
	    f = (((10UL*F_CPU/64) << n) + ticks/2) / ticks;
	} else {		/* n >= 11 */
	    /*
	     * Do the arithmethic in 64 bits to avoid overflow.
	     */
	    unsigned long long events = ((10ULL * F_CPU/64) << n);
	    f = (events + ticks/2) / ticks;
	}
	display_freq(f);
    }
}

static void show_line(char* s)
{
    int i;
    static char prev_line[9];

    if (strcmp(s, prev_line) == 0) {
	return;			/* No change */
    }

    lcd_home();
    for (i = 0; i < 8 && s[i]; i++) {
	prev_line[i] = s[i];
	lcd_putc(s[i]);
    }
    prev_line[i] = '\0';
    while (i < 8) {
	lcd_putc(' ');
	i++;
    }
}

/*
 * Frequency ranges.
 */
static struct {
    unsigned long min;		/* Lowest value for this range */
    unsigned long max;		/* Highest value for this range */
    signed char point;		/* Position of decimal point */
    signed char lsd;		/* Position of least significant digit */
    unsigned int divisor;	/* Divisor */
    uint8_t prefix;		/* Hz prefix character */
} range[] = {
    /*  01234567         min         max   . lsd   div  prefix */
    {/*  999.9Hz */       0UL,     9999UL, 4, 5,     1, ' '},
    {/* 9.999kHz */    9900UL,    99999UL, 1, 4,    10, 'k'},
    {/* 99.99kHz */   99000UL,   999999UL, 2, 4,   100, 'k'},
    {/* 999.9kHz */  990000UL,  9999999UL, 3, 4,  1000, 'k'},
    {/* 9.999MHz */ 9900000UL, 99999999UL, 1, 4, 10000, 'M'},
};
static unsigned int curr_range = 0;

/* Frequency in dHz (10dHz = 1Hz). */
static void display_freq(unsigned long freq)
{
    char line[9];
    signed char pos;
    signed char point;
    static unsigned long prev_freq = 0xffffffffUL;

    if (freq == prev_freq) {
	return;
    }
    prev_freq = freq;

    if (freq == 0) {
	show_line("---");
	return;
    }

    /*
     * See if we'll need to change range.
     */

    while (freq > range[curr_range].max) {
	curr_range++;
    }
    while (freq < range[curr_range].min) {
	curr_range--;
    }

    /*
     * Now format the frequency within the range.
     *
     * First set up the end of the line (prefix + "Hz").
     */

    line[5] = range[curr_range].prefix;
    line[6] = 'H';
    line[7] = 'z';
    line[8] = '\0';

    pos = range[curr_range].lsd;
    point = range[curr_range].point;
    freq /= range[curr_range].divisor;

    /* Format digits to the right of the decimal point */
    while (pos > point) {
	line[pos--] = freq % 10 + '0';
	freq /= 10;
    }

    /*
     * Fill in the decimal point and one digit to the left
     * of the decimal point.
     */
    line[pos--] = '.';
    line[pos--] = freq % 10 + '0';
    freq /= 10;

    /* Fill in non-zero digits to the left of the decimal point. */
    while (freq) {
	line[pos--] = freq % 10 + '0';
	freq /= 10;
    }

    /* Out of significant digits. Fill in spaces. */
    while (pos >= 0) {
	line[pos--] = ' ';
    }

    show_line(line);
}

/* ================================================================
 * DOG display support.
 * ================================================================
 */

/*
 * Define the different display models.
 *
 * M081 1 line by 8 chars.
 * M162 2 lines by 16 chars.
 * M163 3 lines by 16 chars.
 */

#define DOG_LCD_M081 81
#define DOG_LCD_M162 82
#define DOG_LCD_M163 83
#define DOG_MODEL DOG_LCD_M081

#define DOG_LCD_CONTRAST 0x28

/*
 * All pins for the DOG display must be connected to the same port.
 */
#define DOG_DDR DDRA
#define DOG_PIN PINA
#define DOG_PORT PORTA

#define DOG_SI_BIT  PA0
#define DOG_CLK_BIT PA1
#define DOG_CSB_BIT PA2
#define DOG_RS_BIT  PA3

#define DOG_ALL_BITS (_BV(DOG_SI_BIT) | _BV(DOG_CLK_BIT) | _BV(DOG_CSB_BIT) | _BV(DOG_RS_BIT))

static void spi_transfer(uint8_t value);
static void set_instruction_set(uint8_t is);
static inline void write_command(uint8_t value, unsigned execution_time)
    __attribute__ ((always_inline));
static void set_instruction_set(uint8_t is);
static inline void execute(uint8_t value, unsigned execution_time)
    __attribute__ ((always_inline));

static void lcd_init(void)
{
    DOG_DDR |= DOG_ALL_BITS;
    DOG_PORT |= DOG_ALL_BITS;

    /* The commands that follow are in instruction set 1. */
    set_instruction_set(1);

    /* Bias 1/4. */
    write_command(0x1D, 30);

    /* Set up contrast. (For 5V.) */
    write_command(0x50 | (DOG_LCD_CONTRAST>>4), 30);
    write_command(0x70 | (DOG_LCD_CONTRAST & 0x0F), 30);

    /* Set amplification ratio for the follower control. */
    write_command(0x69, 30);

    /* Get back to the default instruction set. */
    set_instruction_set(0);

    /* Clear the display */
    write_command(0x01, 1100);

    /* Move cursor left to right; no autoscroll. */
    write_command(0x04 | 0x02, 30);

    /* Display on; no cursor; no blink. */
    write_command(0x08 | 0x04, 30);
}

static void lcd_home(void)
{
    write_command(0x80, 30);
}

static void lcd_putc(char c)
{
    DOG_PORT |= _BV(DOG_RS_BIT);
    execute((uint8_t) c, 30);
}

/*
 * Set instruction set. 'is' is in the range 1..3.
 */
static void set_instruction_set(uint8_t is)
{
#if DOG_MODEL == DOG_LCD_M081
    const uint8_t template = 0x30;
#else
    const uint8_t template = 0x38;
#endif
    write_command(template | is, 30);
}

static void write_command(uint8_t value, unsigned execution_time)
{
    DOG_PORT &= ~_BV(DOG_RS_BIT);
    execute(value, execution_time);
}

static void inline execute(uint8_t value, unsigned execution_time)
{
    spi_transfer(value);
    _delay_us(execution_time);
}

static void spi_transfer(uint8_t value)
{
    int i;

    DOG_PORT |= _BV(DOG_CLK_BIT);
    DOG_PORT &= ~_BV(DOG_CSB_BIT);
    for (i = 7; i >= 0; i--) {
	if (value & _BV(i)) {
	    DOG_PORT |= _BV(DOG_SI_BIT);
	} else {
	    DOG_PORT &= ~_BV(DOG_SI_BIT);
	}
	DOG_PIN |= _BV(DOG_CLK_BIT);
	_delay_us(1);
	DOG_PIN |= _BV(DOG_CLK_BIT);
    }
    DOG_PORT |= _BV(DOG_CSB_BIT);
}
