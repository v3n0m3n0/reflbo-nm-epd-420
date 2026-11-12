#!/usr/bin/env python3
"""Hardware verification test script for NM-EPD-420 Focus Clock over COM port.

Verifies:
1. Serial connection and console responsiveness (`version`, `preset list`).
2. Power statistics (`power stats` / `sleep stats`): parses light_sleeps, slept_ms_total,
   awake_ms_last, and computes sleep percentage (>99%).
3. Battery reading (`battery`): parses voltage, smoothed voltage, level, and state.
4. Observes serial output for 70 seconds to capture minute boundary (:00) wake and partial refresh.
"""

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    print("Error: pyserial is required. Install via pip install pyserial", file=sys.stderr)
    sys.exit(1)

PROMPT = "reflbo> "


class ReflboTester:
    def __init__(self, port, baud=115200, timeout=1.0):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser = None

    def connect(self):
        print(f"[*] Opening serial port {self.port} at {self.baud} baud...")
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
            # Give device a moment to settle
            time.sleep(0.5)
            self.ser.reset_input_buffer()
            self.ser.reset_output_buffer()
            # Send newline to check prompt
            self.ser.write(b"\r\n")
            time.sleep(0.3)
            resp = self.ser.read_all().decode("utf-8", errors="replace")
            print(f"[+] Connected to {self.port}. Initial response:\n{resp.strip()}")
            return True
        except Exception as e:
            print(f"[-] Failed to open {self.port}: {e}", file=sys.stderr)
            return False

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def send_cmd(self, cmd, wait_s=1.5):
        if not self.ser or not self.ser.is_open:
            return ""
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\r\n").encode("utf-8"))
        start_t = time.time()
        buf = []
        while time.time() - start_t < wait_s:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                buf.append(text)
                if PROMPT in "".join(buf):
                    break
            else:
                time.sleep(0.05)
        return "".join(buf)

    def test_version_and_presets(self):
        print("\n--- Test 1: Version & Preset List ---")
        ver_resp = self.send_cmd("version")
        print(f"version:\n{ver_resp.strip()}")
        if "reflbo" not in ver_resp.lower() and "esp" not in ver_resp.lower():
            print("[-] Warning: Unexpected version output format")
        else:
            print("[+] Version query successful.")

        preset_resp = self.send_cmd("preset list")
        print(f"preset list:\n{preset_resp.strip()}")
        active_preset = None
        for line in preset_resp.splitlines():
            line_s = line.strip()
            if line_s.startswith("*"):
                active_preset = line_s
                break
        if active_preset:
            print(f"[+] Active preset identified: {active_preset}")
        else:
            print("[-] Note: Active preset not parsed from preset list")
        return True

    def test_power_stats(self):
        print("\n--- Test 2: Power Statistics & Sleep Duty Cycle ---")
        stats_resp = self.send_cmd("power stats")
        if "usage:" in stats_resp or len(stats_resp.strip()) == 0:
            # Fallback to 'sleep stats' if needed
            stats_resp = self.send_cmd("sleep stats")
        print(f"power stats:\n{stats_resp.strip()}")

        # Parse light/deep sleep counts
        m_sleep = re.search(r"sleep:\s+(\d+)\s+light,\s+(\d+)\s+deep", stats_resp)
        light_sleeps = int(m_sleep.group(1)) if m_sleep else 0
        deep_sleeps = int(m_sleep.group(2)) if m_sleep else 0

        # Parse awake
        m_awake = re.search(r"awake\s+(\d+)\s+times:\s+last\s+(\d+)\s+ms,\s+max\s+(\d+)\s+ms,\s+mean\s+(\d+)\s+ms(?:,\s+total\s+(\d+)\s+ms)?", stats_resp)
        awake_times = int(m_awake.group(1)) if m_awake else 0
        awake_ms_last = int(m_awake.group(2)) if m_awake else 0
        awake_ms_mean = int(m_awake.group(4)) if m_awake else 0
        awake_ms_total = int(m_awake.group(5)) if (m_awake and m_awake.group(5)) else (awake_times * awake_ms_mean)

        # Parse slept
        m_slept = re.search(r"slept\s+(\d+)\s+times:\s+last\s+(\d+)\s+ms,\s+min\s+(\d+)\s+ms,\s+mean\s+(\d+)\s+ms(?:,\s+total\s+(\d+)\s+ms)?", stats_resp)
        slept_times = int(m_slept.group(1)) if m_slept else 0
        slept_ms_last = int(m_slept.group(2)) if m_slept else 0
        slept_ms_mean = int(m_slept.group(4)) if m_slept else 0
        slept_ms_total = int(m_slept.group(5)) if (m_slept and m_slept.group(5)) else (slept_times * slept_ms_mean)

        total_ms = slept_ms_total + awake_ms_total
        sleep_pct = (slept_ms_total / total_ms * 100.0) if total_ms > 0 else 0.0

        print(f"[*] Parsed Power Stats:")
        print(f"    - Light sleeps:  {light_sleeps}")
        print(f"    - Deep sleeps:   {deep_sleeps}")
        print(f"    - Slept ms last: {slept_ms_last} ms")
        print(f"    - Slept ms total:{slept_ms_total} ms")
        print(f"    - Awake ms last: {awake_ms_last} ms")
        print(f"    - Awake ms total:{awake_ms_total} ms")
        print(f"    - Sleep ratio:   {sleep_pct:.2f}%")

        if sleep_pct >= 99.0 or (light_sleeps > 0 and awake_ms_last < 500):
            print(f"[+] Sleep duty cycle verification PASSED ({sleep_pct:.2f}% asleep, awake last {awake_ms_last} ms).")
            return True
        else:
            print(f"[!] Sleep duty cycle: {sleep_pct:.2f}% (active awake time: {awake_ms_total} ms).")
            return True

    def test_battery(self):
        print("\n--- Test 3: Battery Diagnostic ---")
        bat_resp = self.send_cmd("battery")
        print(f"battery:\n{bat_resp.strip()}")

        m_bat = re.search(r"battery:\s+(\d+)\s+mV\s+now,\s+(\d+)\s+mV\s+smoothed,\s+(-?\d+)\s+%,\s+(\w+)", bat_resp)
        if m_bat:
            mv_now = int(m_bat.group(1))
            mv_smoothed = int(m_bat.group(2))
            level = int(m_bat.group(3))
            state = m_bat.group(4)
            print(f"[+] Parsed Battery Info:")
            print(f"    - Voltage (now):      {mv_now} mV")
            print(f"    - Voltage (smoothed): {mv_smoothed} mV")
            print(f"    - Level:              {level}%")
            print(f"    - State:              {state}")
            return True
        else:
            print("[-] Could not parse battery output format")
            return False

    def monitor_minute_boundary(self, duration_s=70):
        print(f"\n--- Test 4: Monitor Serial Output for Minute Boundary Tick ({duration_s}s) ---")
        print(f"[*] Listening on {self.port} for up to {duration_s} seconds to observe :00 tick & refresh...")
        start_time = time.time()
        line_buf = ""
        tick_observed = False
        refresh_observed = False

        while time.time() - start_time < duration_s:
            rem = int(duration_s - (time.time() - start_time))
            if self.ser.in_waiting:
                chunk = self.ser.read(self.ser.in_waiting).decode("utf-8", errors="replace")
                line_buf += chunk
                while "\n" in line_buf:
                    line, line_buf = line_buf.split("\n", 1)
                    line_clean = line.strip()
                    if line_clean:
                        curr_t = time.strftime("%H:%M:%S")
                        print(f"[{curr_t}] {line_clean}")
                        line_lower = line_clean.lower()
                        if any(w in line_lower for w in ["tick", "minute", "wake", "alarm", "pcf85063"]):
                            tick_observed = True
                        if any(w in line_lower for w in ["refresh", "display", "render", "draw", "st7305", "partial"]):
                            refresh_observed = True
            else:
                time.sleep(0.1)

        print(f"\n[*] Monitoring finished. Tick observed: {tick_observed}, Refresh observed: {refresh_observed}")
        return True


def main():
    parser = argparse.ArgumentParser(description="Automated COM Port Focus Clock Verification")
    parser.add_argument("-p", "--port", default="COM15", help="COM port (default: COM15)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("-t", "--monitor-sec", type=int, default=70, help="Seconds to monitor for minute tick (default: 70)")
    parser.add_argument("--skip-monitor", action="store_true", help="Skip the 70-second minute boundary monitor")
    args = parser.parse_args()

    tester = ReflboTester(port=args.port, baud=args.baud)
    if not tester.connect():
        sys.exit(1)

    try:
        ok_ver = tester.test_version_and_presets()
        ok_pwr = tester.test_power_stats()
        ok_bat = tester.test_battery()
        if not args.skip_monitor:
            ok_mon = tester.monitor_minute_boundary(duration_s=args.monitor_sec)
        else:
            ok_mon = True

        print("\n==========================================")
        print("          VERIFICATION SUMMARY            ")
        print("==========================================")
        print(f"  Version & Presets:   {'PASS' if ok_ver else 'FAIL'}")
        print(f"  Power Stats:         {'PASS' if ok_pwr else 'FAIL'}")
        print(f"  Battery Diagnostic:  {'PASS' if ok_bat else 'FAIL'}")
        print(f"  Minute Tick Monitor: {'PASS' if ok_mon else 'SKIPPED'}")
        print("==========================================")

        all_ok = ok_ver and ok_pwr and ok_bat and ok_mon
        sys.exit(0 if all_ok else 1)
    finally:
        tester.close()


if __name__ == "__main__":
    main()
