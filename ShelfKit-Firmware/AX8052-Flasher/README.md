# AX8052 Flasher

Programs the internal flash of an AX8052 from an ESP32-S3. The ESP32-S3 drives the chip's debug port directly: clock, open-drain data, debug enable, and reset. A serial console on the ESP32-S3 reads, writes, erases, unlocks, sets the debug key, and steps the core. `flash.py` is that console from the PC, including a check that a firmware file reads back equal to what was written.

The firmware lives in the AX8052's own flash. These parts have no external SPI flash.

## Core

The AX8052 runs the 8052 instruction set. Many instructions take one clock cycle per byte. The system clock runs up to 20 MHz. After reset, execution starts at address `0000`.

The core has 64 KB of flash and 8.25 KB of SRAM. Flash is 64 pages of 1 KB, programmed as 16-bit words. A program operation changes a 1 to a 0. Erase restores the erased pages to `FF`. The flash is specified for 100,000 erase cycles, 10,000 minimum.

The debug logic is separate from the instruction stream, so the core can be halted and the flash can be read or written with the CPU stopped. `DBG_EN` has a pull-down. High on that pin removes PB6 and PB7 from GPIO service. PB6 becomes the data line and PB7 becomes the clock. `RESET_N` is active low and has a pull-up.

A 64-bit key locks the debug port. From the factory the key is `FFFFFFFFFFFFFFFF`. Type a key as 16 hex digits with the high byte on the left. On the wire the low byte goes first. Secure erase clears the main flash and then the key, which returns a locked part to the factory key. The `erase` command of this flasher is that operation.

```
                    DBG_EN
                       |
                       v
        PB7 clock --> debug port <-- PB6 data
                       |
                       v
                  AX8052 core
              8052 instruction set
                       |
          +------------+------------+
          |            |            |
       64 KB        8.25 KB     peripherals
       flash         SRAM       SFR, timers,
          |                      UARTs, SPI
          v
   0000  firmware, 63 KB
   FBFF
   FC00  factory calibration, 1 KB
   FFFF
```

`read` can fetch any address in that map, calibration included. `write` stops at `FC00`. `erase` is the secure erase of the main flash and the debug key.

An erased byte is `FF`, which is the one-byte instruction `MOV R7,A`. Opcode `00` is a one-byte `NOP`.

## Parts

The debug port and the 64 KB map above are the same on these devices. Supply is 1.8 V to 3.6 V on every one of them.

| Part | Package | Debug pins |
| --- | --- | --- |
| AX8052F143, and the -1, -2, -3 orderables | QFN40, 7 mm × 5 mm, 0.5 mm pitch | PB6 pin 24, PB7 pin 25, DBG_EN pin 26, RESET_N pin 27 |
| AX8052F131, and the -2, -3 orderables | QFN40 | pins 24 to 27, same as the F143 |
| AX8052F151 | QFN40 | pins 24 to 27, same as the F143 |
| AX8052F100 | QFN28 | PB6 pin 18, PB7 pin 19, DBG_EN pin 20, RESET_N pin 21 |

The F143, F131, and F151 are sub-GHz radio microcontrollers. The F100 is the microcontroller without the radio. The flasher programs the AX8052 flash. It leaves the radio registers, the ADC, and anything off the chip alone.

On the QFN40 parts, pin 28 and the center pad are ground, and pin 29 is `VDD_IO`. The F100 uses the same four signals on the pins in the table. The ESP32-S3 GPIO numbers stay as listed under Hookup.

## QFN40 pinout

Top view. The pin-1 mark is at the upper right. Numbering runs counter-clockwise. The center pad is ground. The debug pins sit together on the left edge.

```
                PA1  PA2  PA3  PA4  PA5  VDD  TST2 TST1 16N  16P
                31   32   33   34   35   36   37   38   39   40
               +--------------------------------------------------+
        PA0 30 |                                                  | 1  VDD_ANA
     VDD_IO 29 |                                                  | 2  GND
        GND 28 |                                                  | 3  ANTP
    RESET_N 27 |                    AX8052F143                    | 4  ANTN
     DBG_EN 26 |                      QFN40                       | 5  ANTP1
    PB7 CLK 25 |                 center pad = GND                 | 6  GND
   PB6 DATA 24 |                                                  | 7  VDD_ANA
        PB5 23 |                                                  | 8  GND
        PB4 22 |                                                  | 9  FILT
        PB3 21 |                                                  | 10 L2
               +--------------------------------------------------+
                20   19   18   17   16   15   14   13   12   11
                PB2  PB1  PB0  PC0  PC1  PC2  PC3  PC4  SYS  L1
```

`SYS` on the bottom edge is SYSCLK. `16N` and `16P` on the top edge are CLK16N and CLK16P.

| Pin | Name | Use |
| --- | --- | --- |
| 1 | VDD_ANA | analog supply output, decouple to pin 2 |
| 2 | GND | ground |
| 3 | ANTP | differential antenna |
| 4 | ANTN | differential antenna |
| 5 | ANTP1 | single-ended antenna output |
| 6 | GND | ground |
| 7 | VDD_ANA | analog supply output, decouple to pin 6 |
| 8 | GND | ground |
| 9 | FILT | synthesizer filter, optional |
| 10 | L2 | synthesizer inductor, optional |
| 11 | L1 | synthesizer inductor, optional |
| 12 | SYSCLK | system clock output, pull-up |
| 13–17 | PC4…PC0 | GPIO, pull-up |
| 18–23 | PB0…PB5 | GPIO, pull-up |
| 21 | PB3 | GPIO, also the deep-sleep wake input. Leave it open |
| 24 | PB6 | data, once DBG_EN is high. Pull-up |
| 25 | PB7 | clock, once DBG_EN is high. Pull-up |
| 26 | DBG_EN | debug enable, pull-down |
| 27 | RESET_N | reset, active low, pull-up. Tie to VDD_IO when unused |
| 28 | GND | ground |
| 29 | VDD_IO | supply, 1.8 V to 3.6 V |
| 30–35 | PA0…PA5 | GPIO and analog, pull-up. Keep these at or below VDD_IO |
| 36 | VDD_IO | supply |
| 37 | TST2 | tie to ground |
| 38 | TST1 | tie to ground |
| 39 | CLK16N | RF reference crystal |
| 40 | CLK16P | RF reference crystal |
| pad | GND | exposed pad, ground |

Port A is the group that must stay at or below `VDD_IO`. The other digital inputs, including the debug pins, are 5 V tolerant. The ESP32-S3 drives 3.3 V. Power the AX8052 from its own 1.8 V to 3.6 V rail. The USB 5 V pin of the ESP32 board is not a supply for the AX8052. Connect the grounds.

## Hookup

The programmer is an ESP32-S3-DevKitC-1. Pin numbers are in `include/ax_pins.h`.

```
   ESP32-S3                         AX8052F143
   GPIO16  DBG_EN  --------------  pin 26  DBG_EN
   GPIO15  RESET   --------------  pin 27  RESET_N
   GPIO4   CLK     --------------  pin 25  PB7
   GPIO6   DATA    --------------  pin 24  PB6     open drain
   GND             --------------  pin 28 and the center pad
```

GPIO6 is the data wire. High releases it to a pull-up. Low drives it to ground. The AX8052 pulls PB6 up on its own.

GPIO5 and GPIO7 are reserved for a 74LVC1G125, if that buffer is fitted between the ESP32 and the data wire. GPIO5 is the buffer input. GPIO7 is its output enable, active low. While programming, the firmware holds the buffer off (GPIO7 high, GPIO5 high) and uses GPIO6. Those two pins are initialized either way, so a fitted buffer stays quiet.

The clock idles low. Each half of the clock is 2 µs. The data bit is set while the clock is low, and the rising edge clocks it into the AX8052. The ESP32 samples the line at the end of the low half.

Leave the ESP32 strap pins 0, 3, 45, and 46 alone, and leave USB (19, 20), the module flash (26–32), octal PSRAM (33–37), and UART0 (43, 44) alone.

## Console

115200 baud, 8 data bits, no parity, one stop bit. Open the port with DTR and RTS off. If either line is asserted, the ESP32-S3 resets and the key stored in its RAM returns to `FFFFFFFFFFFFFFFF`. The key stored in the AX8052 stays as it was.

Addresses, lengths, and step counts are hexadecimal. A command line holds at most 287 characters. Each command that touches the chip opens one session and closes it before the next command.

The firmware attaches like this:

1. `DBG_EN` high.
2. `RESET_N` low for 10 ms, then high, then 10 ms.
3. One `FF` byte, 15000 clocks with data high, sixteen more `FF` bytes.
4. Status. If bit `0x20` is clear, one unlock with the selected key.
5. Halt, if the core is still running.
6. On the way out: buffer off, clock low, `RESET_N` high, `DBG_EN` low.

| Command | What it does |
| --- | --- |
| `help` | prints the command list |
| `pins` | prints the ESP32 GPIO numbers |
| `key <16 hex>` | keeps the unlock key in the ESP32. Writes nothing to the AX8052 |
| `setkey <16 hex>` | programs that key into the chip. The port has to be open |
| `unlock`, `status` | prints the status byte |
| `read <addr> <len>` | reads code, 128 bytes at a time. `dump` is the same command |
| `write <addr> <hex>` | programs words. Even address, even length, at most 128 bytes, below `FC00` |
| `erase` | secure erase, one opcode `54`, then wait for `01` |
| `pc` | prints the program counter |
| `reset` | resets the CPU and leaves it halted, then prints the counter |
| `step` | executes one instruction and prints the counter |
| `steps <n>` | in one session: reset, print the counter, then `n` steps. `n` is hex `1` through `10` |

`step` is its own session, so a second `step` starts from a fresh reset. `steps` keeps the reset and the following instructions in the same session.

Status is one byte from opcode `35`.

| Mask | Meaning |
| --- | --- |
| `01` | reset is held |
| `02` | core is halted |
| `04` | transmit enabled |
| `08` | CPU loading |
| `10` | IRAM programming |
| `20` | port is unlocked |
| `40` | debug requested |
| `80` | debug forced |

The console prints the raw byte, and for a normal command it also prints whether `20` and `02` are set. After `erase` it prints only the raw byte. `read`, `write`, `setkey`, `pc`, `reset`, `step`, and `steps` run only when bit `20` is set. `erase` still goes out when a status byte was received, because secure erase is allowed on a locked port. A broken frame aborts the command before that opcode is sent.

## Programming

A transfer on the wire is 16 clocks, low bit first: six ones, a start bit of 0, eight data bits, a stop bit of 1. A gap with argument 4 is 32 clocks. Argument 6 is 48 clocks. During a gap the data line is released. The long idle after the first `FF` is 15000 clocks and is not scaled the same way. Reply `01` acknowledges a simple command. A program-counter read is different: the two reply bytes are the counter, high byte then low byte.

To read one code byte the flasher first writes `MOV 97,#D0`, then for that address:

1. `5E` and the low address byte
2. `5F` and the high address byte
3. `MOVC A,@A+PC` (`60 83`)
4. `MOV 8F,A` (`61 F5 8F`)

The code byte is the first of the two bytes returned by step 4.

To program a word other than `FFFF`, the chunk is opened with two writes to NVKEY (`96`): `41`, then `78`. A word of `FFFF` is skipped, because those bits are already erased. Each remaining word is:

1. address high into `93`, address low into `92`
2. data high into `95`, data low into `94`
3. `MOV 91,#30`
4. a gap of argument `16`
5. read SFR `91` back through `8F`

The status byte from that read has bit 1 set and bit 0 clear, and the other replies of the word are `01`.

`setkey` uses the same NVKEY prefix and writes four 16-bit halves at indexes `10`, `12`, `14`, and `16`. The trigger there is `MOV 91,#34`. The low byte of index `10` is the low byte of the key. A half whose value is `FFFF` is written too.

`erase` sends `54` once. The chip's reply to that opcode is empty. The ESP32 then clocks with the data line released until a framed byte arrives, for at most 1,600,000 clocks. `01` means the erase has finished. Reset is held high for the whole wait.

`steps` sends `59` (reset held), `5A` (reset released, core stays halted), reads the counter, then repeats `58` (one instruction) and a counter read.

## Host tool

`flash.py` is the console, run from Windows. It uses only the Python standard library and keeps DTR and RTS off. The first argument is the serial port of the ESP32-S3. The second is the command.

```
python flash.py COM5 help
python flash.py COM5 pins
python flash.py COM5 unlock
python flash.py COM5 status
python flash.py COM5 key FFFFFFFFFFFFFFFF
python flash.py COM5 setkey FFFFFFFFFFFFFFFF
python flash.py COM5 read 0 40
python flash.py COM5 read 0 8000 --out firmware.bin
python flash.py COM5 dump FC00 400
python flash.py COM5 write 0 00000000
python flash.py COM5 flash firmware.bin
python flash.py COM5 flash firmware.bin --addr 0x200 --steps 4
python flash.py COM5 erase
python flash.py COM5 pc
python flash.py COM5 reset
python flash.py COM5 step
python flash.py COM5 steps 4
```

`read` and `dump` take a hex address and a hex length, the same way the console does. `read 0 40` fetches 64 bytes. `--out` stores those bytes. `write` programs one chunk: even hex address, even number of hex digits, at most 128 bytes, below `FC00`. `flash` splits a file into those chunks, reads it back, and compares every byte. An odd file length is padded with `FF`. `--addr` on `flash` is decimal or `0x` hex. `--steps` is a decimal count from 1 to 16. The script sends the hex count the console expects.

A file name in the command position is the same as `flash`:

```
python flash.py COM5 firmware.bin --addr 0x200
```

If the chip already holds a 0 where the file needs a 1, the compare fails. Erase, then flash the file again. Erase also returns the debug key to the factory value.

`loop`, `bits`, `bitsod`, `wire5`, `wire6`, `trace`, `sense`, and `scan` are the wire checks from the console.

## Building the ESP32 firmware

One target, in `platformio.ini`: ESP32-S3-DevKitC-1, Arduino core, console at 115200. No extra libraries.

```
pio run -t upload --upload-port COM5
```

Open the console afterwards with DTR and RTS off. The board prints the command list and the GPIO numbers.
