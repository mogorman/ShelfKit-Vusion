#ifndef AX_PINS_H
#define AX_PINS_H

// ESP32-S3-DevKitC-1 wiring. Change only this block.
// Strap pins (0, 3, 45, 46), USB (19, 20) and the module flash bus stay free.
// Share GND with the tag. Do not feed the tag from USB 5 V.
// OE is active low: the pin level is high when the data driver is released.

static const int kPinClk = 4;
static const int kPinDataOut = 5;
static const int kPinDataIn = 6;
static const int kPinOe = 7;
static const int kPinResetN = 15;
static const int kPinDbgEn = 16;

// Half-period of DBG_CLK. The link is synchronous. Two microseconds
// per half is enough.
static const int kClockHalfUs = 2;

// Active-low OE: released driver -> pin high, driving -> pin low.
inline int ax_oe_pin_level(int release) { return release ? 1 : 0; }

#endif
