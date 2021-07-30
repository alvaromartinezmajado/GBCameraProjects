/*
===============================================================================
 Name        : GBCamcorder
 Author      : furrtek
 Version     : 0.3
 Copyright   : CC Attribution-NonCommercial-ShareAlike 4.0
 Description : GameBoy Camcorder firmware
===============================================================================
*/

#include <string.h>
#include <stdlib.h>
#include "main.h"
#include "colors.h"
#include "views.h"
#include "gbcam.h"
#include "capture.h"
#include "sdcard.h"
#include "diskio.h"
#include "ff.h"
#include "io.h"
#include "lcd.h"
#include "icons.h"

// BUGS:
// Viewer allows selecting files when there are no files :(
// Auto-exposure oscillates

// TODO: Audio feedback, bleeps
// TODO: merge image conversion code between lcd_preview() and save_bmp()
// TODO: lcd_preview doesn't work before going into capture mode
// TODO: File list stops where it shouldn't, see view.c
// TODO: File naming ?

// Doc:
// TMR16B0 is used for delays (1us tick @ 72MHz)
// TMR16B1 is used for Phi generation (GB CPU frequency / 4)
// TMR32B0 is used for recording timing (audio/video)
// TMR32B1 is used for LCD backlight PWM
// The Systick frequency is 100Hz (10ms)
// Recording:
// TMR32B0 match 0 triggers the ADC at 8192Hz (no IRQ, the ADC is able to listen to the timer)
// The current audio buffer is filled up to 512 samples
// When full, the frame_tick flag is set to tell capture_loop() that a frame can be recorded
// This yields a max framerate of 8192/512 = 16fps
// There are MAX_AUDIO_BUFFERS 512-bytes audio buffers (circular), allowing for max. ~300ms SD card write latency
// Whenever capture_loop() has the time, it writes the filled buffers to the file

// RAM usage:
// We have 8kB of RAM only
// FATFs takes up at least 512 bytes
// Largest arrays are picture_buffer (3584 bytes) and audio_fifo (MAX_AUDIO_BUFFERSx 512-byte buffers = 3072 bytes)

void SysTick_Handler(void) {
	if (systick < 255)
		systick++;

	if (check_timer)
		check_timer--;

	rec_timer++;
}

void TIMER32_0_IRQHandler(void) {
	// Simulates recording timing for playback
	// TMR32B0 interrupt not used during record (but the timer runs !)
	if (audio_fifo_ptr == 511) {
		audio_fifo_ptr = 0;
		frame_tick = 1;				// This sets the playback framerate (8192/512 = 16 fps)
	} else
		audio_fifo_ptr++;

	LPC_TMR32B0->IR = 1;			// Ack interrupt
    NVIC->ICPR[1] = (1<<11);		// Ack interrupt
}

void ADC_IRQHandler(void) {
	uint32_t ad_stat = LPC_ADC->STAT;
	(void)ad_stat;

	uint32_t ad_data = (LPC_ADC->DR0 >> 8) & 0xFF;
	audio_fifo[audio_fifo_put][audio_fifo_ptr] = ad_data;

	if (ad_data > audio_max)
		audio_max = ad_data;		// Store peak level

	if (audio_fifo_ptr == 511) {
		audio_fifo_ptr = 0;
		audio_max = 0;

		if (frame_tick)
			skipped++;

		frame_tick = 1;				// This sets the framerate (8192/512 = 16 fps)

		if (audio_fifo_ready < MAX_AUDIO_BUFFERS - 1)
			audio_fifo_ready++;		// This should NEVER go over 5 (SD card too slow or stalled ?)

		if (audio_fifo_put == MAX_AUDIO_BUFFERS - 1)	// Cycle buffers
			audio_fifo_put = 0;
		else
			audio_fifo_put++;
	} else {
		audio_fifo_ptr++;
	}

    NVIC->ICPR[1] |= (1<<17);		// Ack interrupt
}

void print_error(uint8_t x, uint8_t y, uint8_t fr) {
	hex_insert(0, fr);
	lcd_print(x, y, str_buffer, COLOR_RED, 1);
}

// Do NOT use this in interrupt handlers
void systick_wait(const uint32_t duration) {
	systick = 0;
	while (systick < duration);
}

uint8_t hexify(uint8_t d) {
	if (d > 9)
		d += 7;
	return '0' + d;
}

void hex_insert(uint32_t pos, uint8_t d) {
	str_buffer[pos] = hexify(d >> 4);
	str_buffer[pos+1] = hexify(d & 15);
	str_buffer[pos+2] = 0;
}

// 72000000/2/100 = 360000
// MCR3 = 360000/x
// MCR0 = MCR3*vol

// Can't use beep while recording or playing ! TMR32B0 is used to generate the sampling frequency
// Frequency in hertz
// Duration in 10ms
// Volume 0~255
void beep(const uint32_t frequency, const uint32_t duration, const uint32_t volume) {
	LPC_TMR32B0->TCR = 0;			// Disable timer
	LPC_TMR32B0->TC = 0;
	LPC_TMR32B0->PR = 100;
	LPC_TMR32B0->PWMC = 1;			// Enable PWM for CT32B0_MAT0 output (TODO: Do this in init ?)
	LPC_TMR32B0->MCR = 0x0400;		// Reset on match register 3
	LPC_TMR32B0->MR0 = 90;			// Smol duty cycle 0.9
	LPC_TMR32B0->MR3 = 100;			// 720Hz tone ?

	LPC_IOCON->PIO1_6 = 2;			// Func: CT32B0_MAT0 (PWM audio out)
	LPC_TMR32B0->TCR = 1;			// Enable timer

	uint32_t period = 360000UL / frequency;
	LPC_TMR32B0->MR3 = period;								// Frequency
	LPC_TMR32B0->MR0 = period - ((period * volume) >> 8);	// Duty cycle
	LPC_TMR32B0->TC = 0;

	systick_wait(duration);

	LPC_IOCON->PIO1_6 = 0;			// Func: PIO (PWM audio disabled)

	LPC_TMR32B0->TCR = 0;			// Disable timer
	LPC_TMR32B0->TC = 0;
	LPC_TMR32B0->PR = 1099;
	LPC_TMR32B0->MCR = 0x0002;		// Reset on match register 0
	LPC_TMR32B0->MR0 = 3;			// Count 0~3 (/4)
}

void noop(void) {
	return;
}

int main(void) {
	// Should already be set correctly in SystemInit()...
	// Enable clocks for: SYS (always on), ROM, RAM, FLASH, GPIO, all 4 timers, SSP, ADC, IOCON
    LPC_SYSCON->SYSAHBCLKCTRL = (1<<0) | (1<<1) | (1<<2) | (1<<3) | (1<<4) | (1<<6) |
    							(1<<7) | (1<<8) | (1<<9) | (1<<10) | (1<<11) | (1<<13) | (1<<16);

    SysTick->LOAD = (72000000/100)-1;	// 10ms tick
    SysTick->VAL = 0;
    SysTick->CTRL = 7;					// Use system clock (72MHz), enable interrupt, enable counter

	backlight = 0;
    check_timer = 0;
    sd_ok = 0;
    gbcam_ok = 0;
    sd_ok_prev = 2;					// Force update
    gbcam_ok_prev = 2;
	slot_func = noop;

	systick_wait(10);				// 100ms

    init_io();

	// TMR16B0 is used for delays (1us tick @ 72MHz, do NOT modify !)
	LPC_TMR16B0->PR = 72;
	LPC_TMR16B0->TCR = 1;			// Enable

	// TMR16B1 is used for Phi generation (GB CPU frequency / 4)
	LPC_TMR16B1->PR = 0;
    LPC_TMR16B1->MCR = 0x0400;		// Reset on match register 3
    LPC_TMR16B1->MR0 = 17;			// Could be anything < 34 ?
	LPC_TMR16B1->MR3 = 34;			// ~1048576Hz (can go faster if needed)
	LPC_TMR16B1->EMR = 0x30;		// Toggle pin on match register 0
	LPC_TMR16B1->TCR = 1;			// Enable

	// TMR32B0 is used for recording timing (audio/video)
	// 72MHz/8192Hz = 8789 with match at 4 (why not ?) and /2 = 1099
	LPC_TMR32B0->PR = 1099;
	LPC_TMR32B0->MCR = 0x0002;		// Reset on match register 0
	LPC_TMR32B0->MR0 = 3;			// Count 0~3 (/4)
	LPC_TMR32B0->TCR = 0;			// Don't enable yet

	// TMR32B1 is used for LCD backlight PWM
    LPC_TMR32B1->PR = 10;			// Prescaler
    LPC_TMR32B1->MCR = 0x0400;		// Reset on match register 3
    LPC_TMR32B1->MR3 = 7200;
    LPC_TMR32B1->MR0 = 7200;		// Inverted brightness (72-x)/72
	LPC_TMR32B1->EMR = 0x30;		// Toggle pin on match register 0
	LPC_TMR32B1->PWMC = 1;			// PWM mode, MR3=Max
    LPC_TMR32B1->TCR = 1;			// Enable

    LPC_SYSCON->PDRUNCFG &= ~(1<<4);	// Power to ADC

	spi_init();

    lcd_init();
	FCLK_FAST();
    lcd_clear();

    lcd_fill(0, 0, 240, 32, 0b0110011100000000);
    lcd_paint(1, 1, logo, 0);
	lcd_hline(0, 32, 240, 0b0100111100000000);	// Useless gradient
	lcd_hline(0, 33, 240, 0b0011010101000000);
	lcd_hline(0, 34, 240, 0b0001001110000000);

    gbcam_reset();

	menu_view();

	fade_in();

	beep(900, 10, 40);
	beep(1200, 10, 40);

	while (1) {
		loop_func();

		if (!check_timer) {
			check_timer = 100;		// 2s TODO: This depends on the time loop_func() takes :(

			if (!sd_ok) {
				FCLK_SLOW();
				if ((fr = f_mount(&FatFs, "", 1)) == FR_OK)
					sd_ok = 1;
				//else
				//	print_error(0, 0, fr);	// DEBUG
			}

			if (!gbcam_ok) {
			    if (!gbcam_detect())
			    	gbcam_ok = 1;
			    else
			    	gbcam_ok = 0;
			}

			if ((sd_ok != sd_ok_prev) || (gbcam_ok != gbcam_ok_prev))
				slot_func();

			if (sd_ok != sd_ok_prev) {
				FCLK_FAST();
				if (sd_ok) {
					lcd_paint(218, 0, icon_sdok, 1);
				} else {
					lcd_paint(218, 0, icon_sdnok, 1);
				}
			}

			if (gbcam_ok != gbcam_ok_prev) {
				FCLK_FAST();
				if (gbcam_ok) {
			    	lcd_paint(184, 0, icon_camok, 1);
				} else {
			    	lcd_paint(184, 0, icon_camnok, 1);
				}
			}

			sd_ok_prev = sd_ok;
			gbcam_ok_prev = gbcam_ok;
		}
	}

    return 0 ;
}
