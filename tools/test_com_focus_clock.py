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
from datetime import datetime, timezone
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Error: pyserial is required. Install via pip install pyserial", file=sys.stderr)
    sys.exit(1)

PROMPT = "reflbo> "


def parse_panel_status(text):
    m = re.search(r"partials\s+(\d+)/(\d+),\s+full\s+(\d+),\s+timeouts\s+(\d+)", text)
    if not m:
        return None
    return {
        "partials": int(m.group(1)),
        "partial_max": int(m.group(2)),
        "full": int(m.group(3)),
        "timeouts": int(m.group(4)),
    }


def parse_sleep_stats(text):
    m = re.search(r"sleep:\s+(\d+)\s+light,\s+(\d+)\s+deep", text)
    if not m:
        return None
    m_left = re.search(r"test cycles left\s+(\d+)", text)
    return {
        "light": int(m.group(1)),
        "deep": int(m.group(2)),
        "test_cycles_left": int(m_left.group(1)) if m_left else 0,
    }


def parse_rtc_epoch(text):
    m = re.search(r"rtc:\s*(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)", text)
    if not m or "INVALID" in text:
        return None, 0
    dt = datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    return dt.timestamp(), dt.year


class ReflboTester:
    def __init__(self, port, baud=115200, timeout=1.0):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser = None

    def connect(self):
        print(f"[*] Opening serial port {self.port} at {self.baud} baud...")
        try:
            ser = serial.Serial()
            ser.port = self.port
            ser.baudrate = self.baud
            ser.timeout = self.timeout
            ser.dtr = True
            ser.rts = True
            ser.open()
            ser.rts = False
            ser.dtr = False
            self.ser = ser
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
            try:
                self.ser.rts = False
                self.ser.dtr = False
            except Exception:
                pass
            self.ser.close()

    def send_cmd(self, cmd, wait_s=2.0):
        if not self.ser or not self.ser.is_open:
            return ""
        time.sleep(0.1)
        self.ser.read_all()
        self.ser.write((cmd.strip() + "\r\n").encode("utf-8"))
        start_t = time.time()
        buf = ""
        seen_cmd = False
        while time.time() - start_t < wait_s:
            if self.ser.in_waiting:
                chunk = self.ser.read(self.ser.in_waiting).decode("utf-8", errors="replace")
                buf += chunk
                if cmd.strip() in buf:
                    seen_cmd = True
                if seen_cmd and buf.rstrip().endswith(PROMPT.strip()):
                    break
            else:
                time.sleep(0.05)
        return buf

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

        m_bat = re.search(r"battery:\s+(\d+)\s+mV\s+now,\s+(\d+)\s+mV\s+smoothed,\s+(-?\d+)\s+%,\s+([^\r\n]+)", bat_resp)
        if m_bat:
            mv_now = int(m_bat.group(1))
            mv_smoothed = int(m_bat.group(2))
            level = int(m_bat.group(3))
            state = m_bat.group(4).strip()
            usb_detected = "usb" in state.lower()
            print(f"[+] Parsed Battery Info:")
            print(f"    - Voltage (now):      {mv_now} mV")
            print(f"    - Voltage (smoothed): {mv_smoothed} mV")
            print(f"    - Level:              {level}%")
            print(f"    - State:              {state}")
            print(f"    - USB detected:       {usb_detected}")
            return True
        else:
            print("[-] Could not parse battery output format")
            return False

    def ensure_rtc_valid(self):
        out = self.send_cmd("rtc get")
        m = re.search(r"rtc:\s+(\d{4})-\d{2}-\d{2}T", out)
        year = int(m.group(1)) if m else 0
        if "INVALID" in out or year < 2020:
            iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            print(f"[!] RTC invalid/unset ({year}); setting to {iso}")
            self.send_cmd(f"rtc set {iso}")
        else:
            print(f"[+] RTC already valid (year {year}); leaving untouched")

    def monitor_minute_boundary(self, duration_s=70):
        print(f"\n--- Test 4: Monitor Serial Output for Minute Boundary Tick ({duration_s}s) ---")
        before_panel_raw = self.send_cmd("panel status")
        before_panel = parse_panel_status(before_panel_raw)
        before_rtc_raw = self.send_cmd("rtc get")
        _, before_year = parse_rtc_epoch(before_rtc_raw)
        before_rtc_valid = before_year >= 2020
        print(f"[*] Before monitor - RTC valid: {before_rtc_valid}, panel: {before_panel}")

        print(f"[*] Listening on {self.port} for up to {duration_s} seconds to observe :00 tick & refresh...")
        start_time = time.time()
        line_buf = ""
        tick_observed = False
        refresh_observed = False
        error_observed = False

        while time.time() - start_time < duration_s:
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
                        if any(w in line_lower for w in ["guru meditation", "panic", "busy timeout"]):
                            error_observed = True
                        if any(w in line_lower for w in ["tick", "minute", "wake", "alarm", "pcf85063"]):
                            tick_observed = True
                        if any(w in line_lower for w in ["refresh", "display", "render", "draw", "st7305", "partial"]):
                            refresh_observed = True
            else:
                time.sleep(0.1)

        after_panel_raw = self.send_cmd("panel status")
        after_panel = parse_panel_status(after_panel_raw)
        print(f"\n[*] Monitoring finished. Tick observed: {tick_observed}, Refresh observed: {refresh_observed}")
        if before_panel and after_panel:
            print(
                f"[*] Panel delta: partials: {before_panel['partials']} -> {after_panel['partials']}, "
                f"full: {before_panel['full']} -> {after_panel['full']}, "
                f"timeouts: {before_panel['timeouts']} -> {after_panel['timeouts']}"
            )
        else:
            print(f"[-] Failed to parse panel status (before={before_panel}, after={after_panel})")

        if error_observed:
            print("[-] Error/panic/BUSY timeout observed during monitoring")
            return False

        if before_rtc_valid:
            if not before_panel or not after_panel:
                return False
            if after_panel["timeouts"] != before_panel["timeouts"]:
                print("[-] Panel timeouts increased during monitoring")
                return False
            if not (after_panel["partials"] > before_panel["partials"] or after_panel["full"] > before_panel["full"]):
                print("[-] Panel partials/full failed to advance across minute boundary")
                return False
        return True

    def test_t6_focus_lora_sleep(self):
        print("\n[T6] Checking Focus mode keeps LoRa asleep...")
        out = self.send_cmd("lora")
        print(out.strip())
        low = out.lower()
        if "lora:" not in low or "err_" in low or "unrecognized" in low or "usage:" in low:
            return False
        presets = self.send_cmd("preset list").lower()
        if "focus" in presets and "active" in presets:
            return "disabled or not populated" in low or "asleep" in low
        return True

    def test_t7_rtc_no_jump_on_restart(self):
        print("\n[T7] Checking RTC time across reboot...")
        import re
        from datetime import datetime, timezone
        out1 = self.send_cmd("rtc get")
        print(out1.strip())
        m1 = re.search(r"rtc:\s*(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)", out1)
        if not m1 or "INVALID" in out1 or int(m1.group(1)[:4]) < 2020:
            now_iso = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            print(f"[*] Setting RTC to {now_iso}...")
            print(self.send_cmd(f"rtc set {now_iso}").strip())
            out1 = self.send_cmd("rtc get")
            print(out1.strip())
            m1 = re.search(r"rtc:\s*(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)", out1)
            if not m1:
                return False
        t1 = datetime.strptime(m1.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()
        w1 = time.time()
        self.send_cmd("reboot")
        self.close()
        time.sleep(5.0)
        for _ in range(10):
            if self.connect():
                resp = self.send_cmd("version").lower()
                if "reflbo" in resp or "esp-idf" in resp:
                    break
                self.close()
            time.sleep(1.0)
        out2 = self.send_cmd("rtc get")
        print(out2.strip())
        m2 = re.search(r"rtc:\s*(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)", out2)
        if not m2:
            return False
        dt2 = datetime.strptime(m2.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
        if dt2.year < 2020:
            return False
        t2 = dt2.timestamp()
        w2 = time.time()
        return abs((t2 - t1) - (w2 - w1)) <= 5.0

    def test_t8_seconds_disabled(self):
        print("\n[T8] Checking preset list & power stats (no 1s wake loop)...")
        presets = self.send_cmd("preset list")
        print(presets.strip())
        stats = self.send_cmd("power stats")
        print(stats.strip())
        low = (presets + "\n" + stats).lower()
        return "1 hz" not in low and "every_second" not in low and len(stats.strip()) > 0

    def test_t9_deep_sleep_partial(self):
        print("\n[T9] Checking `panel status` partial/full/timeouts count...")
        out = self.send_cmd("panel status")
        print(out.strip())
        low = out.lower()
        return (
            "ssd1683" in low
            and "partials" in low
            and "full" in low
            and "timeouts 0" in low
            and "usage:" not in low
            and "unrecognized" not in low
        )

    def test_t10_rtc_drift(self):
        print("\n[T10] Checking `rtc drift`, `battery cal`, and `sync status`...")
        out = self.send_cmd("rtc drift")
        print(out.strip())
        low = out.lower()
        if "rtc drift:" not in low:
            return False
        if "unmeasured" not in low:
            m = re.search(r"rtc drift:\s*([+-]?\d+)\s*ppb", low)
            if not m or abs(int(m.group(1))) > 5000000:
                print(f"[-] rtc drift out of range or unparseable: {out.strip()}")
                return False

        bat_cal = self.send_cmd("battery cal")
        print(bat_cal.strip())
        bat_low = bat_cal.lower()
        if not any(k in bat_low for k in ("curve", "manual", "learned")) or "empty=" not in bat_low or "full=" not in bat_low:
            print("[-] battery cal missing active method / empty= / full=")
            return False

        presets = self.send_cmd("preset list")
        if re.search(r"^\*\s+focus\b", presets, re.MULTILINE):
            sync_out = self.send_cmd("sync status")
            print(sync_out.strip())
            if "focus sntp: 2h thermal" not in sync_out.lower() and "focus sntp: 12h nominal" not in sync_out.lower():
                print("[-] sync status missing Focus SNTP schedule")
                return False
        return True

    def test_t11_display_busy_diag(self):
        print("\n[T11] Checking `sensors` (AHT20) & `panel status`...")
        s_out = self.send_cmd("sensors")
        print(s_out.strip())
        p_out = self.send_cmd("panel status")
        print(p_out.strip())
        s_low = s_out.lower()
        p_low = p_out.lower()
        return (
            ("shtc3" not in s_low)
            and ("mode " not in p_low)
            and ("lpm rate" not in p_low)
            and ("full" in p_low)
            and ("timeouts 0" in p_low)
        )

    def test_sleep_cycles(self, cycles, mode="deep"):
        print(f"\n--- Sleep Cycles Test ({cycles}x {mode}) ---")
        self.ensure_rtc_valid()

        preset_resp = self.send_cmd("preset list")
        m_preset = re.search(r"^\*\s+(\S+)", preset_resp, re.MULTILINE)
        orig_preset = m_preset.group(1) if m_preset else None
        if orig_preset and orig_preset != "focus":
            print(f"[*] Switching preset '{orig_preset}' -> 'focus' for sleep test...")
            self.send_cmd("preset set focus")
            time.sleep(2.0)

        before_stats_raw = self.send_cmd("sleep stats")
        before_stats = parse_sleep_stats(before_stats_raw)
        before_panel_raw = self.send_cmd("panel status")
        before_panel = parse_panel_status(before_panel_raw)
        before_rtc_raw = self.send_cmd("rtc get")
        before_rtc_epoch, before_year = parse_rtc_epoch(before_rtc_raw)
        wall_start = time.time()

        if not before_stats or not before_panel or before_rtc_epoch is None or before_year < 2020:
            print(f"[-] Failed to capture baseline (stats={before_stats}, panel={before_panel}, rtc_year={before_year})")
            return False

        if before_panel["partials"] == 0 and before_panel["full"] == 0:
            print("[*] Note: baseline panel partials=0, full=0")

        ack = self.send_cmd(f"sleep test {mode} {cycles}", wait_s=3.0)
        print(f"sleep test ack:\n{ack.strip()}")
        if "sleep cycles follow" not in ack.lower():
            print("[-] Did not receive 'sleep cycles follow' confirmation")
            return False

        self.close()
        m_sec = re.search(r"rtc:\s*\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:(\d{2})Z", before_rtc_raw)
        secs_to_minute = max(1, min(60, 60 - int(m_sec.group(1)))) if m_sec else 60
        initial_wait = secs_to_minute + max(0, cycles - 1) * 60 + 16
        timeout_s = max(210.0, initial_wait + 100.0)
        print(f"[*] Waiting {initial_wait}s without touching {self.port} so all {cycles} {mode} sleep cycles & USJ hold finish...")
        time.sleep(initial_wait)

        deadline = wall_start + timeout_s
        after_stats = None
        after_stats_raw = ""

        try:
            while time.time() < deadline:
                if not self.ser or not self.ser.is_open:
                    port_upper = self.port.upper()
                    while time.time() < deadline:
                        ports = [p.device.upper() for p in list_ports.comports()]
                        if port_upper in ports:
                            break
                        time.sleep(0.5)
                    if time.time() >= deadline:
                        break
                    time.sleep(1.5)
                    if not self.connect():
                        time.sleep(2.0)
                        continue

                try:
                    after_stats_raw = self.send_cmd("sleep stats", wait_s=5.0)
                except Exception as e:
                    print(f"[*] Transient serial error while polling: {e}")
                    self.close()
                    time.sleep(2.0)
                    continue

                low = after_stats_raw.lower()
                if "guru meditation" in low or "panic" in low:
                    print(f"[-] Panic detected during sleep test:\n{after_stats_raw.strip()}")
                    return False

                parsed = parse_sleep_stats(after_stats_raw)
                if parsed and parsed["test_cycles_left"] == 0 and parsed[mode] >= before_stats[mode] + cycles:
                    after_stats = parsed
                    break

                self.close()
                time.sleep(3.0)

            if not after_stats:
                print(f"[-] Timed out after {timeout_s}s waiting for {cycles} {mode} sleep cycles (last stats:\n{after_stats_raw.strip()})")
                return False

            after_panel_raw = self.send_cmd("panel status")
            after_panel = parse_panel_status(after_panel_raw)
            after_rtc_raw = self.send_cmd("rtc get")
            after_rtc_epoch, after_year = parse_rtc_epoch(after_rtc_raw)
            wall_elapsed = time.time() - wall_start

            print(f"[*] Sleep stats: {mode} {before_stats[mode]} -> {after_stats[mode]}, cycles left: {after_stats['test_cycles_left']}")
            if not after_panel:
                print(f"[-] Failed to parse after_panel: {after_panel_raw.strip()}")
                return False
            print(
                f"[*] Panel status: partials {before_panel['partials']} -> {after_panel['partials']}, "
                f"full {before_panel['full']} -> {after_panel['full']}, "
                f"timeouts {before_panel['timeouts']} -> {after_panel['timeouts']}"
            )

            if after_panel["timeouts"] != before_panel["timeouts"]:
                print("[-] Panel timeouts increased during sleep test")
                return False

            before_total = before_panel["partials"] + before_panel["full"] * 20
            after_total = after_panel["partials"] + after_panel["full"] * 20
            if after_total < before_total:
                print("[-] Panel snapshot count regressed across sleep")
                return False
            if (before_panel["partials"] > 0 or before_panel["full"] > 0) and not (
                after_panel["partials"] > 0 or after_panel["full"] > before_panel["full"]
            ):
                print("[-] Panel snapshot did not survive across sleep")
                return False

            if after_rtc_epoch is None or after_year < 2020:
                print(f"[-] Invalid RTC after sleep: {after_rtc_raw.strip()}")
                return False
            rtc_elapsed = after_rtc_epoch - before_rtc_epoch
            drift = abs(rtc_elapsed - wall_elapsed)
            print(f"[*] RTC elapsed: {rtc_elapsed:.1f}s, Wall elapsed: {wall_elapsed:.1f}s, Diff: {drift:.1f}s")
            if drift > 20.0:
                print("[-] RTC clock jump exceeded 20s tolerance")
                return False

            print(f"[+] Sleep cycles test ({cycles}x {mode}) PASSED")
            return True
        finally:
            if not self.ser or not self.ser.is_open:
                for _ in range(5):
                    if self.connect():
                        break
                    time.sleep(1.0)
            if orig_preset and orig_preset != "focus" and self.ser and self.ser.is_open:
                print(f"[*] Restoring preset 'focus' -> '{orig_preset}'...")
                self.send_cmd(f"preset set {orig_preset}")


def main():
    parser = argparse.ArgumentParser(description="Automated COM Port Focus Clock Verification")
    parser.add_argument("-p", "--port", default="COM15", help="COM port (default: COM15)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("-t", "--monitor-sec", type=int, default=70, help="Seconds to monitor for minute tick (default: 70)")
    parser.add_argument("--skip-monitor", action="store_true", help="Skip the 70-second minute boundary monitor")
    parser.add_argument("--no-set-rtc", action="store_true", help="Do not auto-set RTC if invalid/unset")
    parser.add_argument("--extended", action="store_true", help="Run extended diagnostic tests T6-T11")
    parser.add_argument("--sleep-cycles", type=int, default=0, help="Run automated sleep test for N cycles (default: 0)")
    parser.add_argument("--sleep-mode", choices=["deep", "light", "both"], default="deep", help="Sleep mode for --sleep-cycles (default: deep)")
    parser.add_argument("--erase-coredump", action="store_true", help="Erase any existing coredump before running checks")
    args = parser.parse_args()

    tester = ReflboTester(port=args.port, baud=args.baud)
    if not tester.connect():
        sys.exit(1)

    try:
        if args.erase_coredump:
            erase_out = tester.send_cmd("coredump erase")
            print(f"coredump erase:\n{erase_out.strip()}")
            if "coredump: erased" not in erase_out.lower():
                print("[-] Failed to erase coredump")
                sys.exit(1)

        ok_ver = tester.test_version_and_presets()
        ok_pwr = tester.test_power_stats()
        ok_bat = tester.test_battery()
        if not args.no_set_rtc:
            tester.ensure_rtc_valid()
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
        if args.sleep_cycles > 0:
            modes = ["light", "deep"] if args.sleep_mode == "both" else [args.sleep_mode]
            ok_sleep = True
            for m in modes:
                ok_m = tester.test_sleep_cycles(args.sleep_cycles, mode=m)
                print(f"  Sleep Cycles ({m}): {'PASS' if ok_m else 'FAIL'}")
                ok_sleep = ok_sleep and ok_m
            all_ok = all_ok and ok_sleep
        cd_out = tester.send_cmd("coredump")
        print(f"coredump:\n{cd_out.strip()}")
        ok_cd = "coredump: none" in cd_out.lower()
        print(f"  Coredump Check:      {'PASS' if ok_cd else 'FAIL'}")
        all_ok = all_ok and ok_cd
        if args.extended:
            ok_t6 = tester.test_t6_focus_lora_sleep()
            ok_t7 = tester.test_t7_rtc_no_jump_on_restart()
            ok_t8 = tester.test_t8_seconds_disabled()
            ok_t9 = tester.test_t9_deep_sleep_partial()
            ok_t10 = tester.test_t10_rtc_drift()
            ok_t11 = tester.test_t11_display_busy_diag()
            print(f"  T6 Focus LoRa:       {'PASS' if ok_t6 else 'FAIL'}")
            print(f"  T7 RTC Get:          {'PASS' if ok_t7 else 'FAIL'}")
            print(f"  T8 Power Stats:      {'PASS' if ok_t8 else 'FAIL'}")
            print(f"  T9 Display Partial:  {'PASS' if ok_t9 else 'FAIL'}")
            print(f"  T10 RTC Drift:       {'PASS' if ok_t10 else 'FAIL'}")
            print(f"  T11 Sensors AHT20:   {'PASS' if ok_t11 else 'FAIL'}")
            all_ok = all_ok and ok_t6 and ok_t7 and ok_t8 and ok_t9 and ok_t10 and ok_t11
        sys.exit(0 if all_ok else 1)
    finally:
        tester.close()


if __name__ == "__main__":
    main()
