/**
 * @file main.c
 * @brief Minimal AX8052 blink - bring-up / flash validation, no radio/panel/flash.
 *
 * Toggles the white (PB0) and red (PC4) LEDs - the two non-debug LED pins.
 * PB6/PB7 are the DebugLink pins and are left alone. If the board is running
 * this, an LED blinks (~1 Hz). Polarity-agnostic: it blinks either way, so it
 * works before the LED's active-high/low is known.
 */
#include <ax8052f143.h>
#include <libmftypes.h>

/* PB0 = white, PC4 = red (signal-list.md). */
#define LED_ON   do { PORTB |= 0x01; PORTC |= 0x10; } while (0)
#define LED_OFF  do { PORTB &= (uint8_t)~0x01; PORTC &= (uint8_t)~0x10; } while (0)

static void wait_ms(uint16_t ms)
{
    while (ms--)
        delay(1000);      /* libmf: ~1 ms per call */
}

void main(void)
{
    DIRB |= 0x01;         /* PB0 output */
    DIRC |= 0x10;         /* PC4 output */
    for (;;) {
        LED_ON;
        wait_ms(500);
        LED_OFF;
        wait_ms(500);
    }
}
