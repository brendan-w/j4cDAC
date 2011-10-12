/* j4cDAC DAC driver
 *
 * Copyright 2010 Jacob Potter
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <serial.h>
#include <string.h>
#include <lpc17xx_gpdma.h>
#include <lpc17xx_ssp.h>
#include <lpc17xx_pwm.h>
#include <lpc17xx_clkpwr.h>
#include <lpc17xx_timer.h>
#include <ether.h>
#include <dac.h>
#include <hardware.h>
#include <assert.h>
#include <attrib.h>
#include <transform.h>
#include <lightengine.h>
#include <tables.h>
#include <playback.h>
#include <render.h>

/* Each point is 18 bytes. We buffer 1800 points = 32400 bytes.
 *
 * This gives us up to 60ms at 30k, 45ms at 40k, 36ms at 50k, or 30ms at
 * 60k.
 */
static packed_point_t dac_buffer[DAC_BUFFER_POINTS] AHB0;
static int dac_produce;
static volatile int dac_consume;

/* Buffer for point rate changes.
 */
static uint32_t dac_rate_buffer[DAC_RATE_BUFFER_SIZE];
static int dac_rate_produce;
static volatile int dac_rate_consume;

/* Color channel delay lines.
 */
#define DAC_MAX_COLOR_DELAY	20
struct delay_line {
	uint16_t buffer[DAC_MAX_COLOR_DELAY];
	uint8_t index;
	uint8_t points;
};
static struct delay_line red_delay, green_delay, blue_delay;

/* Internal state. */
int dac_current_pps;
int dac_count;
int dac_flags = 0;

uint8_t dac_shutter_req = 0;
enum dac_state dac_state = DAC_IDLE;

/* Shutter pin config. */
#define DAC_SHUTTER_PIN		6
#define DAC_SHUTTER_EN_PIN	7

/* dac_set_rate()
 *
 * Set the DAC point rate to a new value.
 */
int dac_set_rate(int points_per_second) {
	/* The PWM peripheral is set in dac_init() to use CCLK/4. */
	int ticks_per_point = (SystemCoreClock / 4) / points_per_second;
	LPC_PWM1->MR0 = ticks_per_point;

	/* The LDAC low pulse must be at least 20ns long. At CCLK/4 = 24
	 * MHz, one cycle is 42ns. */
	LPC_PWM1->MR5 = ticks_per_point - 1;

	/* Enable this rate to take effect when the timer next overflows. */
	LPC_PWM1->LER = (1<<0) | (1<<5);

	dac_current_pps = points_per_second;

	return 0;
}

/* dac_start()
 *
 * Unsuspend the timers controlling the DAC, and start it playing at a
 * specified point rate.
 */
int dac_start(void) {
	if (dac_state != DAC_PREPARED) {
		outputf("dac: not starting - not prepared");
		return -1;
	}

	if (!dac_current_pps) {
		outputf("dac: not starting - no pps");
		return -1;
	}

	outputf("dac: starting");

	LPC_PWM1->TCR = PWM_TCR_COUNTER_ENABLE | PWM_TCR_PWM_ENABLE;

	dac_state = DAC_PLAYING;

	led_set_backled(1);

	return 0;
}

/* dac_rate_queue
 *
 * Queue up a point rate change.
 */
int dac_rate_queue(int points_per_second) {
	if (dac_state == DAC_IDLE) {
		outputf("drq rejected: idle");
		return -1;
	}

	int produce = dac_rate_produce;

	int fullness = produce - dac_rate_consume;
	if (fullness < 0)
		fullness += DAC_RATE_BUFFER_SIZE;

	/* The buffer can only ever become one word short of full -
	 * produce = consume means empty.
	 */
	if (fullness >= DAC_RATE_BUFFER_SIZE - 1) {
		outputf("drq rejected: full");
		return -1;
	}

	dac_rate_buffer[produce] = points_per_second;

	dac_rate_produce = (produce + 1) % DAC_RATE_BUFFER_SIZE;

	return 0;
}

/* dac_request
 *
 * "Dear ring buffer: where should I put data and how much should I write?"
 *
 * This returns a number of words (not bytes) to be written to the location
 * give by dac_request_addr().
 */
int dac_request(void) {
	int consume = dac_consume;
	int ret;

	if (dac_state == DAC_IDLE)
		return -1;

/*
	outputf("d_r: p %d, c %d", dac_produce, consume);
*/

	if (dac_produce >= consume) {
		/* The read pointer is behind the write pointer, so we can
		 * go ahead and fill the buffer up to the end. */
		if (consume == 0) {
			/* But not if consume = 0, since the buffer can only
			 * ever become one word short of full. */
			ret = (DAC_BUFFER_POINTS - dac_produce) - 1;
		} else {
			ret = DAC_BUFFER_POINTS - dac_produce;
		}
	} else {
		/* We can only fil up as far as the write pointer. */
		ret = (consume - dac_produce) - 1;
	}

	return ret;
}

packed_point_t *dac_request_addr(void) {
	return &dac_buffer[dac_produce];
}

/* dac_advance
 *
 * "Dear ring buffer: I have just added this many points."
 *
 * Call this after writing some number of points to the buffer, as
 * specified by dac_request. It's OK if the invoking code writes *less*
 * than dac_request allowed, but it should not write *more*.
 */
void dac_advance(int count) {
	if (dac_state == DAC_PREPARED || dac_state == DAC_PLAYING) {
		int new_produce = (dac_produce + count) % DAC_BUFFER_POINTS;
		dac_produce = new_produce;
	}
}

/* dac_init
 *
 * Initialize the DAC. This must be called once after reset.
 */
void dac_init() {

	/* Turn on the PWM and timer peripherals. */
	CLKPWR_ConfigPPWR(CLKPWR_PCONP_PCPWM1, ENABLE);
	CLKPWR_SetPCLKDiv(CLKPWR_PCLKSEL_PWM1, CLKPWR_PCLKSEL_CCLK_DIV_4);

	/* Set up the SSP to communicate with the DAC, and initialize to 0 */
	hw_dac_init();

	/* ... and LDAC on the PWM peripheral */
	LPC_PINCON->PINSEL4 |= (1 << 8);

	/* Get the pin set up to produce a low LDAC puslse */
	LPC_GPIO2->FIODIR |= (1 << 4);
	LPC_GPIO2->FIOCLR = (1 << 4);

	/* The PWM peripheral is used to generate LDAC pulses. Set it up,
	 * but hold it in reset until go time. */
	LPC_PWM1->TCR = PWM_TCR_COUNTER_RESET | PWM_TCR_COUNTER_ENABLE;

	/* Reset on match channel 0 */
	LPC_PWM1->MCR = PWM_MCR_RESET_ON_MATCH(0) | PWM_MCR_INT_ON_MATCH(0);

	/* Enable single-edge PWM on channel 5 */
	LPC_PWM1->PCR = PWM_PCR_PWMENAn(5);

	/* The match registers themselves will be set by dac_start(). */

	/* Enable the write-to-DAC interrupt with the highest priority. */
	NVIC_SetPriority(PWM1_IRQn, 0);
	NVIC_EnableIRQ(PWM1_IRQn);

	dac_state = DAC_IDLE;
	dac_count = 0;
	dac_current_pps = 0;

	memset(red_delay.buffer, 0, sizeof(red_delay.buffer));
	memset(green_delay.buffer, 0, sizeof(green_delay.buffer));
	memset(blue_delay.buffer, 0, sizeof(blue_delay.buffer));

	red_delay.points = 2;
	green_delay.points = 2;
	blue_delay.points = 2;
}

/* dac_configure
 *
 * Make the DAC ready to accept points. This is required as an explicit
 * reset call after the DAC stops and before the first time it is started.
 */
int dac_prepare(void) {
	if (dac_state != DAC_IDLE)
		return -1;

	if (le_get_state() != LIGHTENGINE_READY)
		return -1;

	dac_produce = 0;
	dac_consume = 0;
	dac_rate_produce = 0;
	dac_rate_consume = 0;
	dac_flags &= ~DAC_FLAG_STOP_ALL;
	dac_state = DAC_PREPARED;
	dac_shutter_req = 0;

	return 0;
}

/* dac_stop
 *
 * Stop the DAC, set all outputs to zero, and return to the idle state.
 *
 * This is triggered internally when the DAC has a buffer underrun, and may
 * also be called externally at any time.
 */ 
void dac_stop(int flags) {
	/* First things first: shut down the PWM timer. This prevents us
	 * from being interrupted by a PWM interrupt, which could cause
	 * the DAC outputs to be left on. */
	LPC_PWM1->TCR = PWM_TCR_COUNTER_RESET;

	/* Close the shutter. */
	dac_shutter_req = 0;
	dac_flags &= ~DAC_FLAG_SHUTTER;
	LPC_GPIO2->FIOCLR = (1 << DAC_SHUTTER_PIN);

	/* Set LDAC to update immediately */
	LPC_PINCON->PINSEL4 &= ~(3 << 8);

	/* Clear out all the DAC channels. */
	hw_dac_zero_all_channels();

	/* Give LDAC back to the PWM hardware */
	LPC_PINCON->PINSEL4 |= (1 << 8);

	/* Now, reset state */
	dac_state = DAC_IDLE;
	dac_count = 0;
	dac_flags |= flags;

	memset(red_delay.buffer, 0, sizeof(red_delay.buffer));
	memset(green_delay.buffer, 0, sizeof(green_delay.buffer));
	memset(blue_delay.buffer, 0, sizeof(blue_delay.buffer));
}

/* Delay the red, green, and blue lines if needed
 */
static void delay_line_write(struct delay_line *dl, uint16_t in) {
	int points = dl->points;

	if (points) {
		int index = dl->index;

		if (index > points)
			index = 0;

		LPC_SSP1->DR = dl->buffer[index];
		dl->buffer[index] = in;
		dl->index = index + 1;
	} else {
		LPC_SSP1->DR = in;
	}
}

static void __attribute__((always_inline)) dac_write_point(dac_point_t *p) {

	uint32_t xi = p->x;
	uint32_t yi = p->y;

	int32_t x = translate_x(xi, yi);
	int32_t y = translate_y(xi, yi);

	#define MASK_XY(v)	((((v) >> 4) + 0x800) & 0xFFF)
	LPC_SSP1->DR = MASK_XY(x) | 0x6000;
	LPC_SSP1->DR = MASK_XY(y) | 0x7000;

	delay_line_write(&red_delay, (p->r >> 4) | 0x4000);
	delay_line_write(&green_delay, (p->g >> 4)| 0x3000);
	delay_line_write(&blue_delay, (p->b >> 4) | 0x2000);

	LPC_SSP1->DR = (p->i >> 4) | 0x5000;
	LPC_SSP1->DR = (p->u1 >> 4) | 0x1000;
	LPC_SSP1->DR = (p->u2 >> 4);

	if (dac_shutter_req) {
		LPC_GPIO2->FIOSET = (1 << DAC_SHUTTER_PIN);
		dac_flags |= DAC_FLAG_SHUTTER;
	} else {
		LPC_GPIO2->FIOCLR = (1 << DAC_SHUTTER_PIN);
		dac_flags &= ~DAC_FLAG_SHUTTER;
	}

	/* Change the point rate? */
	if (p->control & DAC_CTRL_RATE_CHANGE) {
		int rate_consume = dac_rate_consume;
		if (rate_consume != dac_rate_produce) {
			dac_set_rate(dac_rate_buffer[rate_consume]);
			rate_consume++;
			if (rate_consume >= DAC_RATE_BUFFER_SIZE)
				rate_consume = 0;
			dac_rate_consume = rate_consume;
		}
	}

	dac_count++;
}

void PWM1_IRQHandler(void) {
	/* Tell the interrupt handler we've handled it */
	if (LPC_PWM1->IR & PWM_IR_PWMMRn(0)) {
		LPC_PWM1->IR = PWM_IR_PWMMRn(0);
	} else {
		panic("Unexpected PWM IRQ");
	}
 
	switch (playback_src) {
	case SRC_NETWORK:
	case SRC_ILDAPLAYER: {
		ASSERT_EQUAL(dac_state, DAC_PLAYING);

		int consume = dac_consume;

		/* Are we out of buffer space? If so, shut the lasers down. */
		if (consume == dac_produce) {
			dac_stop(DAC_FLAG_STOP_UNDERFLOW);
			return;
		}

		packed_point_t *p = &dac_buffer[consume];
		dac_point_t dp = {
			.x = UNPACK_X(p) << 4,
			.y = UNPACK_Y(p) << 4,
			.i = UNPACK_I(p) << 4,
			.r = UNPACK_R(p) << 4,
			.g = UNPACK_G(p) << 4,
			.b = UNPACK_B(p) << 4,
			.u1 = UNPACK_U1(p) << 4,
			.u2 = UNPACK_U2(p) << 4,
			.control = p->control
		};

		dac_write_point(&dp);

		consume++;

		if (consume >= DAC_BUFFER_POINTS)
			consume = 0;

		dac_consume = consume;
	}
	break;

	case SRC_SYNTH: {
		dac_point_t dp;
		get_next_point(&dp);
		dac_write_point(&dp);
	}
	break;

	}
}

enum dac_state dac_get_state(void) {
	return dac_state;
}

/* dac_fullness
 *
 * Returns the number of points currently in the buffer.
 */
int dac_fullness(void) {
	int fullness = dac_produce - dac_consume;
	if (fullness < 0)
		fullness += DAC_BUFFER_POINTS;
	return fullness;
}

/* shutter_set
 *
 * Set the state of the shutter - nonzero for open, zero for closed.
 *
 * This does not perform the set immediately, since it would then race
 * against the DAC interrupt (setting the shutter open after a DAC stop
 * condition, causing the shutter to be open when it should not be).
 * Instead, it sets a flag asking the DAC interrupt to update the shutter.
 */
void shutter_set(int state) {
	dac_shutter_req = state;
}

/* impl_dac_pack_point
 *
 * The actual dac_pack_point function is declared 'static inline' in dac.h,
 * so it will always be inlined. This wrapper forces a copy of the code to
 * be emitted standalone too for inspection purposes.
 */
void impl_dac_pack_point(packed_point_t *dest, dac_point_t *src) __attribute__((used));
void impl_dac_pack_point(packed_point_t *dest, dac_point_t *src) {
	dac_pack_point(dest, src);
}

INITIALIZER(hardware, dac_init);
