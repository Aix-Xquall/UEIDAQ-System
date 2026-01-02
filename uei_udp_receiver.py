import socket
import json
import threading
import queue
import time
import sys
import argparse
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.widgets import Button
from collections import deque

# ================= 可調參數 =================
BUFFER_SIZE = 65536
MAX_FPS = 30
PLOT_DISPLAY_LIMIT = 20000
MAX_BUFFER_SEC = 20.0

# UI bottom status line preview length (avoid huge redraw cost)
LAST_PACKET_PREVIEW_CHARS = 240
# ===========================================


def safe_print(msg: str):
    print(msg, flush=True)


class SystemMapper:
    """
    Load UEI_DAQ_Settings.json (your schema) and build stream mapping.

    Stream key = (slot_index, group_name)

    Rule A:
      - slot.active == true => slot enabled
      - channel_groups[].active == true => group enabled (expected to be streamed)

    MVP host constraint:
      - skip any group where moving_average.active==true or fft.active==true
        (because UEIPAC side is not doing DSP yet; we only plot raw streams)
    """
    def __init__(self, config_path: str):
        self.config_path = config_path
        self.system_name = "UEI_SYSTEM"
        self.udp_target_ip = ""
        self.udp_target_port = 5005
        self.packet_interval_ms = 1000

        self.streams = []
        self.stream_index = {}

        self.load_config(config_path)

    @staticmethod
    def _parse_sample_rate_hz(slot: dict) -> float:
        """sample_rate supports number OR {active,hz} (backward compatible)."""
        sr = slot.get("sample_rate", 0)
        if isinstance(sr, (int, float)):
            return float(sr)
        if isinstance(sr, dict):
            active = bool(sr.get("active", False))
            hz = float(sr.get("hz", 0.0))
            return hz if active else 0.0
        return 0.0

    def load_config(self, path: str):
        try:
            with open(path, "r", encoding="utf-8") as f:
                cfg = json.load(f)

            self.system_name = cfg.get("system_name", self.system_name)
            self.udp_target_ip = cfg.get("udp_target_ip", "")
            self.udp_target_port = int(cfg.get("udp_target_port", self.udp_target_port))
            self.packet_interval_ms = int(cfg.get("packet_interval_ms", 1000))

            streams = []
            for slot in cfg.get("slots", []) or []:
                if not slot.get("active", False):
                    continue

                slot_index = int(slot.get("slot_index", 0))
                board_name = slot.get("board_name", "")

                sr_hz = self._parse_sample_rate_hz(slot)
                if sr_hz <= 0:
                    continue

                for g in slot.get("channel_groups", []) or []:
                    # Rule A: group.active means "will stream"
                    if not g.get("active", False):
                        continue

                    # MVP host: skip any group with MA/FFT enabled
                    ma = g.get("moving_average", {}) or {}
                    fft = g.get("fft", {}) or {}
                    if bool(ma.get("active", False)) or bool(fft.get("active", False)):
                        continue

                    group_name = g.get("group_name", "")
                    channels = g.get("channels", []) or []
                    if not group_name or not channels:
                        continue

                    target_hz = float(g.get("target_hz", 0.0))
                    rate_hz = target_hz if target_hz > 0.0 else sr_hz

                    title = f"Slot {slot_index}: {board_name} / {group_name} ({rate_hz:g} Hz)"
                    streams.append({
                        "slot_index": slot_index,
                        "board_name": board_name,
                        "group_name": group_name,
                        "channels": [int(c) for c in channels],
                        "rate_hz": rate_hz,
                        "title": title
                    })

            self.streams = streams
            self.stream_index = {(s["slot_index"], s["group_name"]): i for i, s in enumerate(self.streams)}

            safe_print(f"[System] Loaded Config: {self.system_name}")
            safe_print(f"[System] UDP target (from config): {self.udp_target_ip}:{self.udp_target_port}")
            safe_print(f"[System] packet_interval_ms: {self.packet_interval_ms}")
            safe_print(f"[System] Expect streams (Rule A, MA/FFT off): {len(self.streams)}")
            for s in self.streams:
                safe_print(f"  - {s['title']} ch={s['channels']}")

            if len(self.streams) == 0:
                safe_print("[Warn] No eligible streams found. Check slot/group active flags and MA/FFT settings.")

        except Exception as e:
            safe_print(f"[System] Config load failed: {e}")
            self.system_name = "UEI_SYSTEM(MOCK)"
            self.udp_target_port = 5005
            self.packet_interval_ms = 1000
            self.streams = [{
                "slot_index": 1,
                "board_name": "DNA-AI-217",
                "group_name": "ai217_10hz",
                "channels": [0, 1, 2, 3, 4, 5],
                "rate_hz": 10.0,
                "title": "Slot 1: DNA-AI-217 / ai217_10hz (10 Hz) [MOCK]"
            }]
            self.stream_index = {(1, "ai217_10hz"): 0}


def convert_ai217_raw_to_volt(raw_u32: np.ndarray) -> np.ndarray:
    """
    Convert AI-217 24-bit offset-binary code (in low 24 bits) to voltage.

    NOTE: This is an assumption mapping. If your board range differs, disable with --no-volts.
      0x000000 -> -10V
      0x800000 -> 0V
      0xFFFFFF -> +10V
      V = ((Code - 0x800000) / 0x800000) * 10.0
    """
    codes = (raw_u32 & 0x00FFFFFF).astype(np.float64)
    return ((codes - 8388608.0) / 8388608.0) * 10.0


class RealTimePlotter:
    def __init__(self, mapper: SystemMapper, bind_ip: str = "0.0.0.0", bind_port: int = None, convert_volts: bool = True):
        self.mapper = mapper
        self.bind_ip = bind_ip
        self.bind_port = int(bind_port) if bind_port is not None else mapper.udp_target_port
        self.convert_volts = convert_volts

        self.running = True
        self.packet_queue = queue.Queue()

        # --- Latest UDP raw packet preview (no parsing) ---
        self._last_packet_lock = threading.Lock()
        self._last_packet_text = "Last UDP: (none)"
        self._last_packet_dirty = True
        # -------------------------------------------------

        # UI time window
        self.time_window = 5.0  # default show 5s for 10Hz looks nicer

        # buffers per stream: list[dict[ch_id] = deque]
        self.buffers = []
        self.maxlens = []
        self.lines = []

        for s in self.mapper.streams:
            rate = float(s["rate_hz"]) if s["rate_hz"] > 0 else 10.0
            maxlen = int(rate * MAX_BUFFER_SEC) + 500
            self.maxlens.append(maxlen)
            self.buffers.append({})
            self.lines.append({})

        self.init_plot()

        self.udp_thread = threading.Thread(target=self.udp_worker, daemon=True)
        self.udp_thread.start()

    def init_plot(self):
        plt.ion()
        n = len(self.mapper.streams)
        self.fig, self.axes = plt.subplots(n, 1, figsize=(12, 3.5 * max(1, n)), sharex=False)
        if n == 1:
            self.axes = [self.axes]
        self.axes = np.array(self.axes).flatten()

        try:
            self.fig.canvas.manager.set_window_title(f"{self.mapper.system_name} (UEI CSV-like UDP)")
        except Exception:
            pass

        # Leave more bottom space: buttons + last-packet status line
        plt.subplots_adjust(bottom=0.22, hspace=0.5)

        for i, ax in enumerate(self.axes):
            ax.set_title(self.mapper.streams[i]["title"])
            ax.grid(True, which="both", linestyle="--", linewidth=0.5)
            ax.set_xlim(-self.time_window, 0)

        # UI buttons
        labels = ["100ms", "500ms", "1S", "5S", "10S", "20S"]
        self.btns = []
        start_x = 0.12
        for i, label in enumerate(labels):
            ax_btn = plt.axes([start_x + i * 0.13, 0.08, 0.12, 0.05])
            btn = Button(ax_btn, label, color="0.9", hovercolor="0.8")
            btn.on_clicked(self.make_callback(label, btn))
            self.btns.append(btn)

            if label == "5S":
                btn.color = "orange"
                ax_btn.set_facecolor("orange")

        # --- Bottom status text (latest UDP packet raw preview) ---
        # Use figure-level text so it spans across all subplots
        self.last_packet_artist = self.fig.text(
            0.01, 0.02,
            "Last UDP: (none)",
            ha="left", va="bottom",
            fontsize=9,
            family="monospace",
            color="black"
        )
        # ----------------------------------------------------------

    def make_callback(self, label, btn):
        return lambda event: self.change_window(label, btn)

    def change_window(self, label, clicked_btn):
        val = 1.0
        if "ms" in label:
            val = float(label.replace("ms", "")) / 1000.0
        elif "S" in label:
            val = float(label.replace("S", ""))
        self.time_window = val

        for b in self.btns:
            c = "orange" if b == clicked_btn else "0.9"
            b.color = c
            b.ax.set_facecolor(c)

        for ax in self.axes:
            ax.set_xlim(-self.time_window, 0)

    def _set_last_packet_preview(self, raw_bytes: bytes):
        """Store latest UDP packet preview string (no parsing)."""
        try:
            preview = raw_bytes.decode("utf-8", errors="ignore").replace("\r", "").replace("\n", "")
        except Exception:
            preview = ""

        if len(preview) > LAST_PACKET_PREVIEW_CHARS:
            preview = preview[:LAST_PACKET_PREVIEW_CHARS] + " ..."

        ts = time.strftime("%H:%M:%S")
        text = f"Last UDP [{ts}] ({len(raw_bytes)} bytes): {preview}"

        with self._last_packet_lock:
            self._last_packet_text = text
            self._last_packet_dirty = True

    def udp_worker(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((self.bind_ip, self.bind_port))
        safe_print(f"[UDP] Listening on {self.bind_ip}:{self.bind_port} (CSV-like text)")

        while self.running:
            try:
                data, _addr = sock.recvfrom(BUFFER_SIZE)
                # update latest-packet preview (no parsing)
                self._set_last_packet_preview(data)
                # keep original pipeline
                self.packet_queue.put(data)
            except Exception:
                break

        sock.close()

    def process_packet(self, raw_bytes: bytes):
        """
        Payload format (Scheme A):
          D,1,<seq>,<slot_index>,<group_name>,<samples_per_channel>,<raw...>
        raw layout: scan-major interleaved
        """
        try:
            line = raw_bytes.decode("utf-8", errors="ignore").strip()
            if not line:
                return

            parts = line.split(",")
            if len(parts) < 7:
                return
            if parts[0] != "D" or parts[1] != "1":
                return

            slot_index = int(parts[3])
            group_name = parts[4]
            samples_per_channel = int(parts[5])

            key = (slot_index, group_name)
            stream_idx = self.mapper.stream_index.get(key, None)
            if stream_idx is None:
                return

            stream = self.mapper.streams[stream_idx]
            ch_list = stream["channels"]
            num_ch = len(ch_list)
            if num_ch <= 0 or samples_per_channel <= 0:
                return

            raw_list = parts[6:]
            expected = num_ch * samples_per_channel
            if len(raw_list) != expected:
                return

            raw_i32 = np.fromiter((int(x) for x in raw_list), dtype=np.int32, count=expected)
            raw_mat = raw_i32.reshape((samples_per_channel, num_ch))

            if self.convert_volts and stream["board_name"] == "DNA-AI-217":
                raw_u32 = raw_mat.view(np.uint32)
                y_mat = convert_ai217_raw_to_volt(raw_u32)
            else:
                y_mat = raw_mat.astype(np.float64)

            maxlen = self.maxlens[stream_idx]
            for j in range(num_ch):
                ch_id = ch_list[j]
                if ch_id not in self.buffers[stream_idx]:
                    self.buffers[stream_idx][ch_id] = deque(maxlen=maxlen)
                self.buffers[stream_idx][ch_id].extend(y_mat[:, j].tolist())

        except Exception as e:
            safe_print(f"[ParseError] {e}")

    def _update_last_packet_ui(self):
        """Update bottom status text if new packet arrived."""
        with self._last_packet_lock:
            if not self._last_packet_dirty:
                return
            text = self._last_packet_text
            self._last_packet_dirty = False

        self.last_packet_artist.set_text(text)

    def update_plot(self):
        if len(self.mapper.streams) == 0:
            safe_print("[FATAL] No streams to plot. Fix JSON active flags / MA/FFT settings.")
            return

        while self.running:
            t_start = time.time()
            cnt = 0

            while not self.packet_queue.empty() and cnt < 200:
                self.process_packet(self.packet_queue.get())
                cnt += 1

            for stream_idx, ax in enumerate(self.axes):
                stream = self.mapper.streams[stream_idx]
                rate = float(stream["rate_hz"]) if stream["rate_hz"] > 0 else 10.0
                points_needed = max(2, int(rate * self.time_window))

                slot_data = self.buffers[stream_idx]
                if not slot_data:
                    continue

                has_update = False
                for ch_id, dq in slot_data.items():
                    if len(dq) < 2:
                        continue
                    has_update = True

                    full_data = list(dq)
                    display_data = full_data[-points_needed:] if len(full_data) > points_needed else full_data

                    if len(display_data) > PLOT_DISPLAY_LIMIT:
                        step = max(1, len(display_data) // PLOT_DISPLAY_LIMIT)
                        display_data = display_data[::step]

                    count = len(display_data)
                    real_duration = min(len(full_data), points_needed) / rate
                    x_data = np.linspace(-real_duration, 0, count)

                    if ch_id not in self.lines[stream_idx]:
                        line_obj, = ax.plot([], [], label=f"Ch{ch_id}", lw=1)
                        self.lines[stream_idx][ch_id] = line_obj
                        ax.legend(loc="upper left", fontsize=8)

                    self.lines[stream_idx][ch_id].set_data(x_data, display_data)

                    if ch_id == stream["channels"][0]:
                        y_min, y_max = float(min(display_data)), float(max(display_data))
                        margin = (y_max - y_min) * 0.1 if y_max != y_min else 1.0
                        ax.set_ylim(y_min - margin, y_max + margin)

                if has_update:
                    ax.set_xlim(-self.time_window, 0)
                    ax.grid(True, which="both", linestyle="--", linewidth=0.5)

            # Update last packet UI line (even if no plots updated)
            self._update_last_packet_ui()

            plt.pause(0.001)

            elapsed = time.time() - t_start
            wait = (1.0 / MAX_FPS) - elapsed
            if wait > 0:
                time.sleep(wait)

    def close(self):
        self.running = False
        plt.close("all")
        sys.exit(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="UEI_DAQ_Settings.json", help="Path to UEI_DAQ_Settings.json")
    ap.add_argument("--bind-ip", default="0.0.0.0", help="UDP bind IP (default 0.0.0.0)")
    ap.add_argument("--bind-port", default=None, help="UDP bind port (default: udp_target_port from config)")
    ap.add_argument("--no-volts", action="store_true", help="Do not convert AI-217 raw to volts")
    args = ap.parse_args()

    mapper = SystemMapper(args.config)
    plotter = RealTimePlotter(
        mapper,
        bind_ip=args.bind_ip,
        bind_port=args.bind_port,
        convert_volts=(not args.no_volts)
    )

    try:
        plotter.update_plot()
    except KeyboardInterrupt:
        plotter.close()


if __name__ == "__main__":
    main()
