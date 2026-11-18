#!/usr/bin/env python3
import argparse
import os
import sys
import time
import serial
import serial.tools.list_ports
from tqdm import tqdm

class AxsemFlasher:
    def __init__(self, port, baudrate=38400, timeout=1,
                 open_loop=False, erase_wait=3.0, boot_low=True, line_delay=0.01,
                 timing=False, dtr_off=False, settle=0.1):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.serial = None
        # open_loop: the board has no TX wired back to us, so we never read its
        # replies. The bootloader is synchronous per command (it finishes the
        # erase / each hex write before it reads the next char), so we just wait
        # a safe fixed time after "K" and send hex with a small per-line delay.
        self.open_loop = open_loop
        self.erase_wait = erase_wait
        self.boot_low = boot_low
        self.line_delay = line_delay
        # timing: stamp the boot edges, banner arrival and the real K->OK erase
        # duration, so a TX-wired unit can be used to measure the waits the
        # open-loop unit has to hard-code.
        self.timing = timing
        # dtr_off: don't drive DTR (BOOT/PB3) in the pin dance; pulse reset only.
        # Use when BOOT is forced low externally (a phone on the NFC area drives PB3 low).
        self.dtr_off = dtr_off
        # settle: seconds to let a DTR/RTS change actually reach the pin before the
        # next edge. USB-serial adapters lag pyserial by a few ms; without this the
        # board samples PB3 before DTR has propagated and always boots the app.
        self.settle = settle
        self.t0 = None

    def _stamp(self, label):
        if self.timing and self.t0 is not None:
            print(f"  [{time.monotonic() - self.t0:7.3f}s] {label}", flush=True)
    
    def open(self):
        try:
            self.serial = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=self.timeout,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False
            )
            return True
        except Exception as e:
            print(f"Error opening port {self.port}: {e}")
            return False
    
    def close(self):
        if self.serial and self.serial.is_open:
            self.serial.close()
    
    def set_boot_pin(self, state):
        self.serial.dtr = state
    
    def set_reset_pin(self, state):
        self.serial.rts = state
    
    def enter_boot_mode(self):
        # main.c:784 runs the app only when BOOT (PB3) is HIGH at reset, so hold
        # it low during the reset pulse to force the bootloader. The 2.6" board
        # used a fixed DTR-high here because its DTR-to-PB3 path is inverted;
        # flip --boot-high for a jig that inverts the line the other way.
        print("Entering boot mode...")
        self.t0 = time.monotonic()
        if self.dtr_off:
            # Leave DTR (BOOT/PB3) alone; pulse only reset. BOOT is forced low
            # externally (a phone held on the NFC area drives PB3 low = the boot state).
            # Reset is active-high here (rts True = assert, False = idle); end idle.
            self._stamp("dtr-off: reset pulse only (BOOT forced externally)")
            self.set_reset_pin(False)
            time.sleep(self.settle)
            self.set_reset_pin(True)
            time.sleep(self.settle)
            self.set_reset_pin(False)
            time.sleep(self.settle)
            return
        # Reset is active-high on this rig (rts True = assert, False = idle), so the
        # board is left RUNNING (idle) at the end -- never held in reset. Set BOOT
        # (DTR), let it settle, pulse reset, then release back to idle so the chip
        # samples PB3 in the boot state and boots into the bootloader.
        self._stamp("reset idle")
        self.set_reset_pin(False)
        time.sleep(self.settle)
        self._stamp(f"boot {'low' if self.boot_low else 'high'}")
        self.set_boot_pin(not self.boot_low)
        time.sleep(self.settle)      # let DTR reach PB3 before the reset edge
        self._stamp("reset pulse")
        self.set_reset_pin(True)
        time.sleep(self.settle)      # hold in reset while BOOT is stable
        self._stamp("reset release -> boot")
        self.set_reset_pin(False)    # end idle; chip boots into the bootloader
        time.sleep(0.1)
        self._stamp("boot release")
        self.set_boot_pin(False)
    
    def get_banner(self):
        self.serial.write(b"?")
        time.sleep(0.1)
        response = self.serial.readline().decode('utf-8', errors='ignore').strip()
        return response

    def probe(self):
        # Safe boot-entry check: enter boot mode, read the banner, then stop --
        # no erase, no program. An empty banner means either "not in the
        # bootloader" or "no TX wired back"; only a port with PB4 wired can tell.
        print("Probing bootloader (no flash)...")
        if not self.open():
            return False
        self.enter_boot_mode()
        if self.open_loop:
            print("open-loop: no TX wired back, nothing to read")
            self.close()
            return None
        banner = self.get_banner()
        ok = "Bootloader" in banner
        print(f"Banner: {banner!r}  ->  {'IN bootloader' if ok else 'no banner'}")
        self.close()
        return ok

    def probe_both(self):
        # Find the DTR polarity that forces the bootloader, on a TX-wired port.
        # Tries both and reports which to use. No flashing.
        for label, boot_low in (("default (DTR low)", True),
                                ("--boot-high (DTR high)", False)):
            self.boot_low = boot_low
            print(f"\nTrying {label} ...")
            if not self.open():
                continue
            self.enter_boot_mode()
            banner = self.get_banner()
            ok = "Bootloader" in banner
            print(f"  Banner: {banner!r}  ->  {'IN bootloader' if ok else 'no banner'}")
            self.close()
            if ok:
                print(f"\nUse this polarity: {label}")
                return True
        print("\nNeither polarity gave a banner. Confirm this port has the board's "
              "TX (PB4) wired to the adapter RX -- without it the banner can never "
              "be read, no matter the polarity.")
        return False

    def validate_bootloader(self):
        print("Validating bootloader...")
        if not self.open():
            return False

        self.enter_boot_mode()
        if self.open_loop:
            # no TX to read the banner from; the reset above put it in the
            # bootloader, so give the FRC calibration a moment and trust it.
            print("open-loop: no banner check (no TX wired back)")
            time.sleep(1.0)
            return True

        banner = self.get_banner()
        self._stamp("banner received")
        print(f"Banner received: {banner}")
        
        if "Bootloader" in banner:
            print("Successfully entered bootloader!")
            return True
        
        print("Failed to enter bootloader")
        return False
    
    def erase_application(self):
        print("Erasing application...")
        self.serial.write(b"K")
        if self.open_loop:
            # the bootloader finishes the whole erase before it reads the next
            # char, so a fixed wait is safe; ~0.2-0.4s worst case, use a margin.
            print(f"open-loop: waiting {self.erase_wait}s for erase (no OK to read)")
            time.sleep(self.erase_wait)
            return True
        t_k = time.monotonic()
        response = self.serial.readline().decode('utf-8', errors='ignore').strip()
        if self.timing:
            dur = time.monotonic() - t_k
            # round up to the next 0.5s and add headroom: this is the value the
            # open-loop unit must wait after K before it can safely send hex.
            import math
            rec = math.ceil((dur + 0.5) * 2) / 2
            self._stamp(f"erase OK in {dur:.3f}s -> open-loop --erase-wait {rec}")
        return "OK" in response
    
    def run_application(self):
        print("Running application...")
        self.serial.write(b"R")
        return True
    
    def program_hex_line(self, line):
        self.serial.write((line + '\n').encode())
        time.sleep(self.line_delay)  # the bootloader writes each line before
        return True                   # reading the next; keep a small gap
    
    def program_hex_file(self, hex_file_path):
        if not os.path.exists(hex_file_path):
            print(f"Error: Hex file not found: {hex_file_path}")
            return False
        
        with open(hex_file_path, 'r') as f:
            hex_lines = f.readlines()
        
        print(f"Programming {len(hex_lines)} lines...")
        for line in tqdm(hex_lines, desc="Programming", unit="lines"):
            self.program_hex_line(line.strip())
        
        return True
    
    def flash(self, hex_file_path):
        if not self.validate_bootloader():
            return False
        
        if not self.erase_application():
            print("Failed to erase application")
            self.close()
            return False
        
        print("Application erased")
        
        if not self.program_hex_file(hex_file_path):
            print("Failed to program hex file")
            self.close()
            return False
        
        print("Hex file programmed")
        time.sleep(0.1)
        
        self.run_application()
        time.sleep(0.1)
        
        print("Application running")
        self.close()
        return True

def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found")
        return
    
    print("Available serial ports:")
    for port in ports:
        print(f"  {port.device}: {port.description}")

def main():
    parser = argparse.ArgumentParser(description='Axsem Hex File Flasher')
    parser.add_argument('hex_file', nargs='?', help='Path to the hex file')
    parser.add_argument('-p', '--port', help='Serial port')
    parser.add_argument('-b', '--baudrate', type=int, default=38400,
                        help='Baudrate of the bootloader (default: 38400)')
    parser.add_argument('-l', '--list', action='store_true', help='List available serial ports')
    parser.add_argument('--open-loop', action='store_true',
                        help='board has no TX wired back: never read replies, '
                             'use fixed waits (RX-only flasher)')
    parser.add_argument('--erase-wait', type=float, default=3.0,
                        help='open-loop seconds to wait after K (default: 3.0)')
    parser.add_argument('--boot-high', action='store_true',
                        help='drive BOOT (PB3) high during reset instead of low; '
                             'use if your jig inverts the line')
    parser.add_argument('--line-delay', type=float, default=0.01,
                        help='open-loop seconds between hex lines (default: 0.01)')
    parser.add_argument('--timing', action='store_true',
                        help='stamp boot edges + banner + K->OK erase duration '
                             '(use on a TX-wired unit to size the open-loop waits)')
    parser.add_argument('--probe', action='store_true',
                        help='enter boot mode, read the banner, then stop (no '
                             'erase/flash); diagnostic for a port with TX wired')
    parser.add_argument('--try-both', action='store_true',
                        help='with --probe: test both DTR polarities and report '
                             'which one forces the bootloader (needs TX wired)')
    parser.add_argument('--dtr-off', action='store_true',
                        help="don't drive DTR (BOOT/PB3); pulse reset only. Use "
                             'when BOOT is forced low externally (a phone on the NFC)')
    parser.add_argument('--settle', type=float, default=0.1,
                        help='seconds to wait for a DTR/RTS change to reach the pin '
                             'before the next edge (raise if boot still misses on a slow adapter)')
    parser.add_argument('--set-dtr', type=int, choices=[0, 1],
                        help='pin-test: hold DTR at this level so you can measure '
                             'PB3 with a multimeter (implies pin-test)')
    parser.add_argument('--set-rts', type=int, choices=[0, 1],
                        help='pin-test: hold RTS at this level so you can measure '
                             'the board reset pin (implies pin-test)')
    parser.add_argument('--hold', type=float, default=5.0,
                        help='pin-test: seconds to hold the pins before releasing (default 5.0)')
    parser.add_argument('--listen', type=float, metavar='SECONDS',
                        help='open the port and print every byte for SECONDS, no pin '
                             'activity. Use -b 115200 for the app log, 38400 for the bootloader; '
                             'power-cycle the board while it runs to catch its boot log')

    args = parser.parse_args()

    if args.list:
        list_ports()
        return 0

    if args.set_dtr is not None or args.set_rts is not None:
        # Pin test: drive DTR/RTS to a known level and hold it, so the board's
        # PB3 (DTR) and reset (RTS) can be measured with a multimeter to confirm
        # the adapter actually reaches those pins.
        if not args.port:
            parser.error("pin-test needs -p PORT")
        pins = AxsemFlasher(args.port, args.baudrate)
        if not pins.open():
            return 1
        dtr = args.set_dtr if args.set_dtr is not None else 0
        rts = args.set_rts if args.set_rts is not None else 0
        pins.set_boot_pin(bool(dtr))
        pins.set_reset_pin(bool(rts))
        print(f"Holding DTR={dtr} RTS={rts} for {args.hold:.1f}s -- measure now, "
              f"against BOARD ground: DTR is on PB3 (BOOT), RTS is on the reset pin.")
        print("If PB3 tracks DTR (0 -> ~0 V, 1 -> ~3.3/5 V) the wire works; if it "
              f"doesn't move, DTR isn't reaching PB3.")
        time.sleep(args.hold)
        pins.set_boot_pin(False)
        pins.set_reset_pin(False)
        pins.close()
        return 0

    if args.listen is not None:
        if not args.port:
            parser.error("--listen needs -p PORT")
        lis = AxsemFlasher(args.port, args.baudrate)
        if not lis.open():
            return 1
        print(f"Listening on {args.port} @ {args.baudrate} 8N1 for {args.listen:.0f}s -- "
              f"power-cycle the board now to catch its boot log "
              f"(app logs at 115200, bootloader banner at 38400). Ctrl-C stops early.")
        try:
            end = time.monotonic() + args.listen
            while time.monotonic() < end:
                data = lis.serial.read(256)
                if data:
                    sys.stdout.write(data.decode('utf-8', errors='replace'))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass
        print("\n[done]")
        lis.close()
        return 0

    if args.probe:
        if not args.port:
            parser.error("--probe needs -p PORT")
        probe = AxsemFlasher(args.port, args.baudrate,
                             open_loop=args.open_loop,
                             boot_low=not args.boot_high,
                             line_delay=args.line_delay,
                             timing=args.timing,
                             dtr_off=args.dtr_off,
                             settle=args.settle)
        if args.try_both:
            return 0 if probe.probe_both() else 1
        return 0 if probe.probe() is True else 1

    if not args.hex_file or not args.port:
        parser.print_help()
        return 1

    flasher = AxsemFlasher(args.port, args.baudrate,
                           open_loop=args.open_loop,
                           erase_wait=args.erase_wait,
                           boot_low=not args.boot_high,
                           line_delay=args.line_delay,
                           timing=args.timing,
                           dtr_off=args.dtr_off,
                           settle=args.settle)
    success = flasher.flash(args.hex_file)

    return 0 if success else 1

if __name__ == "__main__":
    sys.exit(main())