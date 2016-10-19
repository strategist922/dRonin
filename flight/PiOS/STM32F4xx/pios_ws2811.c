/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup PIOS_WS2811 Driver for WS2811 LEDs.
 * @brief Driver for WS2811 LEDs.  Uses one timer (generally TIM1
 * advanced control timer) and two DMAs.  First DMA sets the bit high at
 * the beginning of each cycle.  Next DMA picks one of two phases to lower
 * the bit at.
 * @{
 *
 * @file       pios_ws2811.c
 * @author     dRonin, http://dRonin.org, Copyright (C) 2016.
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Additional note on redistribution: The copyright and license notices above
 * must be maintained in each individual source file that is a derivative work
 * of this source file; otherwise redistribution is prohibited.
 */

#include "pios_config.h"

#if defined(PIOS_INCLUDE_WS2811)

#include "pios.h"
#include "openpilot.h"
#include <pios_stm32.h>
#include "stm32f4xx_tim.h"
#include "pios_tim_priv.h"

#include "pios_ws2811.h"

struct ws2811_pixel_data_s {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

// Each DMA side is 6 LEDs, 24 bits per pixel, 2 bytes per bit, for 288
// bytes.
//
// This corresponds to 300us worth of pixel data on each side, or an
// interrupt rate of 3.33KHz.
#define WS2811_DMA_BUFSIZE (6*24*2)

struct ws2811_dev_s {
#define WS2811_MAGIC 0x31313832		/* '2811' */
	uint32_t magic;

	const struct pios_ws2811_cfg *cfg;

	uint16_t max_leds;

	// This gets clocked out at timer update event to BSRRH
	uint8_t lame_dma_buf[1];

	// These are the buffers.  We clock out a byte to BSRRL at each
	// of the two times that the value can fall.  Every odd value of this
	// is always the gpio bit index.  For a '0', the corresponding even
	// value is the gpio bit index.
	//
	// We fill both buffers initially, and then when one has been clocked
	// out, we fill the other half in the interrupt handler.
	uint8_t dma_buf_0[WS2811_DMA_BUFSIZE];
	uint8_t dma_buf_1[WS2811_DMA_BUFSIZE];

	// These get fixed up to point to the top half of the port halfword
	// if necessary.
	volatile uint8_t *gpio_bsrrh_address;
	volatile uint8_t *gpio_bsrrl_address;

	// And this gets fixed up to be a shifted right image, etc.
	uint8_t gpio_bit;

	bool cur_buf;
	bool eof;

	volatile bool in_progress;

	uint8_t *pixel_data_pos;
	uint8_t *pixel_data_end;

	struct ws2811_pixel_data_s pixel_data[0];
};

int PIOS_WS2811_init(ws2811_dev_t *dev_out, const struct pios_ws2811_cfg *cfg,
		int max_leds) 
{
	PIOS_Assert(max_leds > 0);
	PIOS_Assert(max_leds <= 1024);

	ws2811_dev_t dev = PIOS_malloc(sizeof(*dev) +
			sizeof(struct ws2811_pixel_data_s) * max_leds);

	if (!dev) {
		return -1;
	}

	dev->magic = WS2811_MAGIC;
	dev->cfg = cfg;
	dev->max_leds = max_leds;

	dev->pixel_data_end = (uint8_t *)(&dev->pixel_data[max_leds]);

	dev->gpio_bsrrh_address = (volatile uint8_t *)(&cfg->led_gpio->BSRRH);
	dev->gpio_bsrrl_address = (volatile uint8_t *)(&cfg->led_gpio->BSRRL);

	if (cfg->gpio_pin & 0xff00) {
		// It really should only be one bit set, but no matter
		// what it has to be in only one byte (half of the halfword)
		// to make sense.
		PIOS_Assert(!(cfg->gpio_pin & 0xff));

		// Fixup --- store to a byte later (little endian), an
		// adjusted value.
		dev->gpio_bit = cfg->gpio_pin >> 8;

		dev->gpio_bsrrh_address++;
		dev->gpio_bsrrl_address++;
	} else {
		dev->gpio_bit = cfg->gpio_pin;
	}

	for (int i = 0; i < sizeof(dev->dma_buf_0); i += 2) {
		dev->dma_buf_0[i]     = 0;
		dev->dma_buf_1[i]     = 0;
		dev->dma_buf_0[i + 1] = dev->gpio_bit;
		dev->dma_buf_1[i + 1] = dev->gpio_bit;
	}

	dev->lame_dma_buf[0] = dev->gpio_bit;

	PIOS_WS2811_set_all(dev, 0, 0, 0);

	GPIO_InitTypeDef gpio_cfg = {
		.GPIO_Pin = cfg->gpio_pin,
		.GPIO_Mode = GPIO_Mode_OUT,
		.GPIO_Speed = GPIO_Fast_Speed,
		.GPIO_OType = GPIO_OType_OD,
		.GPIO_PuPd = GPIO_PuPd_NOPULL
	};

	GPIO_Init(cfg->led_gpio, &gpio_cfg);

	*dev_out = dev;

	return 0;
}

DONT_BUILD_IF((WS2811_DMA_BUFSIZE % 16) != 0, DMABufIntegralBytes);

// Updates pixel_data_ptr to where we are.  returns true if we've reached
// the end.
static bool fill_dma_buf(uint8_t * restrict dma_buf, uint8_t **pixel_data_ptr,
		uint8_t *pixel_data_end, uint8_t gpio_val) {
	// Our local shadow of this, for efficient blitting.
	uint8_t * restrict p_d_p = *pixel_data_ptr;

	for (int i = 0; i < WS2811_DMA_BUFSIZE; i += 16) {
		if (p_d_p >= pixel_data_end) break;

		uint8_t p = *(p_d_p++);

		for (int j = 0; j < 8; j++) {
			if (!(p & 0x80)) {
				// It's zero-- therefore the pin has to fall
				// early.
				dma_buf[i + j*2] = gpio_val;
			} else {
				// It's one.  Therefore we let the prefilled
				// bit in the odd position clear it; we do
				// nothing.
				dma_buf[i + j*2] = 0;
			}

			// We clock out most significant bit first.
			p = p << 1;
		}
	}

	*pixel_data_ptr = p_d_p;

	return p_d_p >= pixel_data_end;
}

static void ws2811_cue_dma(ws2811_dev_t dev) {
	// XXX ensure timer stopped

	// XXX clear timer event bits

	DMA_DeInit(dev->cfg->bit_set_dma_stream);
	DMA_DeInit(dev->cfg->bit_clear_dma_stream);

	DMA_InitTypeDef dma_init;

	DMA_StructInit(&dma_init);

	/* First set up the single-buffered repeating of the BSRRH */
	dma_init.DMA_Channel = dev->cfg->bit_set_dma_channel; 
	dma_init.DMA_PeripheralBaseAddr = (uintptr_t) dev->gpio_bsrrh_address;
	dma_init.DMA_Memory0BaseAddr = (uintptr_t) dev->lame_dma_buf;
	dma_init.DMA_DIR = DMA_DIR_MemoryToPeripheral;
	dma_init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
	dma_init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
	dma_init.DMA_MemoryInc = DMA_MemoryInc_Disable;
	dma_init.DMA_BufferSize = dev->max_leds;
	dma_init.DMA_Mode = DMA_Mode_Normal;
	dma_init.DMA_Priority = DMA_Priority_VeryHigh;
	dma_init.DMA_FIFOMode = DMA_FIFOMode_Enable;
	dma_init.DMA_FIFOThreshold = DMA_FIFOThreshold_HalfFull;
	dma_init.DMA_MemoryBurst = DMA_MemoryBurst_Single;
	dma_init.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;

	DMA_Init(dev->cfg->bit_set_dma_stream, &dma_init);

	/* Now the tricky one-- set up the double-buffered DMA, to blit to
	 * BSRRL and clear bits at the appropriate time */
	dma_init.DMA_Channel = dev->cfg->bit_clear_dma_channel;
	dma_init.DMA_PeripheralBaseAddr = (uintptr_t) dev->gpio_bsrrl_address;
	dma_init.DMA_Memory0BaseAddr = (uintptr_t) dev->dma_buf_0;
	dma_init.DMA_BufferSize = WS2811_DMA_BUFSIZE;
	dma_init.DMA_Mode = DMA_Mode_Circular;

	/* TODO: Bus performance can be improved by tuning FIFO, memory burst, 
	 * width, etc behaviors... But let's keep things simple for now. */

	DMA_Init(dev->cfg->bit_clear_dma_stream, &dma_init);
	DMA_DoubleBufferModeConfig(dev->cfg->bit_clear_dma_stream,
			(uintptr_t) dev->dma_buf_1,
			DMA_Memory_0);
	DMA_DoubleBufferModeCmd(dev->cfg->bit_clear_dma_stream, ENABLE);

	/* XXX: NVIC for the DMA */

	DMA_Cmd(dev->cfg->bit_set_dma_stream, ENABLE);
	DMA_Cmd(dev->cfg->bit_clear_dma_stream, ENABLE);

	/* XXX: enable the timer's dma request crap, and the timer. */
}

void PIOS_WS2811_trigger_update(ws2811_dev_t dev)
{
	if (dev->in_progress) {
		return;
	}

	dev->in_progress = true;

	dev->eof = false;

	dev->pixel_data_pos = (uint8_t *) dev->pixel_data;

	// Current one to blit is the first
	dev->cur_buf = false;

	// We always clock out at least 2 bufs to keep the DMA code
	// simple.
	fill_dma_buf((uint8_t *) dev->dma_buf_0, &dev->pixel_data_pos,
			dev->pixel_data_end, dev->gpio_bit);

	fill_dma_buf((uint8_t *) dev->dma_buf_1, &dev->pixel_data_pos,
			dev->pixel_data_end, dev->gpio_bit);

	ws2811_cue_dma(dev);
}

void PIOS_WS2811_set(ws2811_dev_t dev, int idx, uint8_t r, uint8_t g,
		uint8_t b)
{
	PIOS_Assert(dev->magic == WS2811_MAGIC);

	PIOS_Assert(idx < dev->max_leds);

	dev->pixel_data[idx] = (struct ws2811_pixel_data_s)
			{ .r = r, .g = g, .b = b };
}

void PIOS_WS2811_set_all(ws2811_dev_t dev, uint8_t r, uint8_t g,
		uint8_t b)
{
	PIOS_Assert(dev->magic == WS2811_MAGIC);

	for (int i = 0; i < dev->max_leds; i++) {
		PIOS_WS2811_set(dev, i, r, g, b);
	}
}

#endif /* PIOS_INCLUDE_WS2811 */
