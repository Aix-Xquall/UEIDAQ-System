#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uei_udp_receiver.py

Realtime UDP receiver + plotter for UEI CSV-like packets.

This version is tolerant to small schema differences on the wire:
  Format A (older):
    D,1,<seq>,<slot_index>,<group_name>,<samples_per_channel>,<raw...>

  Format B (current project: includes board_name):
    D,1,<seq>,<slot_index>,<board_name>,<group_name>,<samples_per_channel>,<raw...>

Raw layout:
  scan-major interleaved (same as C++): [s0ch0,s0ch1,..., s1ch0,s1ch1,...]
"""

import socket
import json
import threading
import queue
import time
import sys
import argparse
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
from matplotlib.widgets import Button
from collections import deque

# ================= Tunables =================
BUFFER_SIZE = 65536
MAX_FPS = 30
SNAPSHOT_MAX_FPS = 15
PLOT_DISPLAY_LIMIT = 20000
MAX_BUFFER_SEC = 20.0
LAST_PACKET_PREVIEW_CHARS = 240
# ===========================================


def safe_print(msg: str):
    print(msg, flush=True)


class SystemMapper:
    """
    Load UEI_DAQ_Settings.json and build stream mapping.

    Stream key = (slot_index, group_name)

    Rule A:
      - slot.active == true => slot enabled
      - channel_groups[].active == true => group enabled (expected to be streamed)

    MVP host constraint:
      - skip any group where moving_average.active==true or fft.active==true
    """
    def __init__(self, config_path: str):
        self.config_path = config_path
        self.system_name = "UEI_SYSTEM"
        self.udp_target_ip = ""
        self.udp_target_port = 5005

        # New name (preferred): matches PDNA_PARAMS.numSamplesPerChannel intent.
        # Backward compatible with old "packet_interval_ms".
        self.numSamplesPerChannel = 1000

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

            # Prefer new name, fallback to old
            if "numSamplesPerChannel" in cfg:
                self.numSamplesPerChannel = int(cfg.get("numSamplesPerChannel", self.numSamplesPerChannel))
            else:
                self.numSamplesPerChannel = int(cfg.get("packet_interval_ms", self.numSamplesPerChannel))

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
                    if not g.get("active", False):
                        continue

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
            safe_print(f"[System] numSamplesPerChannel: {self.numSamplesPerChannel}")
            safe_print(f"[System] Expect streams (Rule A, MA/FFT off): {len(self.streams)}")
            for s in self.streams:
                safe_print(f"  - {s['title']} ch={s['channels']}")

            if len(self.streams) == 0:
                safe_print("[Warn] No eligible streams found. Check slot/group active flags and MA/FFT settings.")

        except Exception as e:
            safe_print(f"[System] Config load failed: {e}")
            self.system_name = "UEI_SYSTEM(MOCK)"
            self.udp_target_port = 5005
            self.numSamplesPerChannel = 1000
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

    This mapping depends on the configured range on the hardware.
    If unsure, run with --no-volts.
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
        self.buffer_lock = threading.Lock()

        self._last_packet_lock = threading.Lock()
        self._last_packet_text = "Last UDP: (none)"
        self._last_packet_dirty = True

        self.time_window = 5.0
        self._last_snapshot_time = 0.0

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
        self.proc_thread = threading.Thread(target=self.process_worker, daemon=True)
        self.proc_thread.start()

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

        plt.subplots_adjust(bottom=0.22, hspace=0.5)

        for i, ax in enumerate(self.axes):
            ax.set_title(self.mapper.streams[i]["title"])
            ax.grid(True, which="both", linestyle="--", linewidth=0.5)
            ax.set_xlim(-self.time_window, 0)

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

        self.last_packet_artist = self.fig.text(
            0.01, 0.02,
            "Last UDP: (none)",
            ha="left", va="bottom",
            fontsize=9,
            family="monospace",
            color="black"
        )
        self.fig.canvas.mpl_connect("close_event", self._on_close)

    def make_callback(self, label, btn):
        return lambda _event: self.change_window(label, btn)

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
        try:
            preview = raw_bytes.decode("utf-8", errors="ignore").replace("\r", "").replace("\n", "")
        except Exception:
            preview = ""

        if len(preview) > LAST_PACKET_PREVIEW_CHARS:
            preview = preview[:LAST_PACKET_PREVIEW_CHARS] + " ..."

        ts = time.strftime("%H:%M:%S")
       # text = f"Last UDP [{ts}] ({len(raw_bytes)} bytes): {preview}"

        with self._last_packet_lock:
        #    self._last_packet_text = text
            self._last_packet_dirty = True

    def udp_worker(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((self.bind_ip, self.bind_port))
        safe_print(f"[UDP] Listening on {self.bind_ip}:{self.bind_port} (CSV-like text)")

        while self.running:
            try:
                data, _addr = sock.recvfrom(BUFFER_SIZE)
                self._set_last_packet_preview(data)
                self.packet_queue.put(data)
            except Exception:
                break

        sock.close()

    def process_worker(self):
        while self.running:
            try:
                data = self.packet_queue.get(timeout=0.2)
            except queue.Empty:
                continue

            self.process_packet(data)
            cnt = 0
            while not self.packet_queue.empty() and cnt < 100:
                self.process_packet(self.packet_queue.get())
                cnt += 1

    def _decode_packet_fields(self, parts):
        """
        Return a dict with keys:
          seq, slot_index, board_name(optional), group_name, samples_per_channel, raw_list
        Supports:
          A: D,1,seq,slot,group,samples,raw...
          B: D,1,seq,slot,board,group,samples,raw...
        """
        if len(parts) < 7:
            return None
        if parts[0] != "D" or parts[1] != "1":
            return None

        seq = int(parts[2])
        slot_index = int(parts[3])

        # Heuristic: try A first
        group_a = parts[4]
        try:
            spc_a = int(parts[5])
            key_a = (slot_index, group_a)
            if key_a in self.mapper.stream_index:
                return {
                    "seq": seq,
                    "slot_index": slot_index,
                    "board_name": None,
                    "group_name": group_a,
                    "samples_per_channel": spc_a,
                    "raw_list": parts[6:]
                }
        except Exception:
            pass

        # Try B
        if len(parts) < 8:
            return None
        board_b = parts[4]
        group_b = parts[5]
        spc_b = int(parts[6])
        key_b = (slot_index, group_b)
        if key_b not in self.mapper.stream_index:
            return None

        return {
            "seq": seq,
            "slot_index": slot_index,
            "board_name": board_b,
            "group_name": group_b,
            "samples_per_channel": spc_b,
            "raw_list": parts[7:]
        }

    def process_packet(self, raw_bytes: bytes):
        try:
            line = raw_bytes.decode("utf-8", errors="ignore").strip()
            if not line:
                return

            parts = line.split(",")
            pkt = self._decode_packet_fields(parts)
            if pkt is None:
                return

            slot_index = pkt["slot_index"]
            group_name = pkt["group_name"]
            samples_per_channel = pkt["samples_per_channel"]
            raw_list = pkt["raw_list"]

            stream_idx = self.mapper.stream_index.get((slot_index, group_name), None)
            if stream_idx is None:
                return

            stream = self.mapper.streams[stream_idx]
            ch_list = stream["channels"]
            num_ch = len(ch_list)
            if num_ch <= 0 or samples_per_channel <= 0:
                return

            expected = num_ch * samples_per_channel
            if len(raw_list) != expected:
                # Soft warning only (keeps UI alive)
                safe_print(f"[Warn] Size mismatch: got {len(raw_list)} ints, expected {expected} "
                           f"(slot={slot_index} group={group_name} spc={samples_per_channel} ch={num_ch})")
                return

            raw_i32 = np.fromiter((int(x) for x in raw_list), dtype=np.int32, count=expected)
            raw_mat = raw_i32.reshape((samples_per_channel, num_ch))

            if self.convert_volts and stream["board_name"] == "DNA-AI-217":
                raw_u32 = raw_mat.view(np.uint32)
                y_mat = convert_ai217_raw_to_volt(raw_u32)
            else:
                y_mat = raw_mat.astype(np.float64)

            with self.buffer_lock:
                maxlen = self.maxlens[stream_idx]
                for j in range(num_ch):
                    ch_id = ch_list[j]
                    if ch_id not in self.buffers[stream_idx]:
                        self.buffers[stream_idx][ch_id] = deque(maxlen=maxlen)
                    self.buffers[stream_idx][ch_id].extend(y_mat[:, j].tolist())

        except Exception as e:
            safe_print(f"[ParseError] {e}")

    def _update_last_packet_ui(self):
        with self._last_packet_lock:
            if not self._last_packet_dirty:
                return
            text = self._last_packet_text
            self._last_packet_dirty = False
        self.last_packet_artist.set_text(text)

    def _update_once(self, _frame=None):
        if len(self.mapper.streams) == 0:
            safe_print("[FATAL] No streams to plot. Fix JSON active flags / MA/FFT settings.")
            return []

        if not self.running:
            return []

        now = time.time()
        min_interval = 1.0 / max(1, SNAPSHOT_MAX_FPS)
        if now - self._last_snapshot_time < min_interval:
            self._update_last_packet_ui()
            return []
        self._last_snapshot_time = now

        for stream_idx, ax in enumerate(self.axes):
            stream = self.mapper.streams[stream_idx]
            rate = float(stream["rate_hz"]) if stream["rate_hz"] > 0 else 10.0
            points_needed = max(2, int(rate * self.time_window))

            with self.buffer_lock:
                slot_data = self.buffers[stream_idx]
                if not slot_data:
                    continue
                slot_snapshot = {ch_id: list(dq) for ch_id, dq in slot_data.items()}

            has_update = False
            for ch_id, full_data in slot_snapshot.items():
                if len(full_data) < 2:
                    continue
                has_update = True

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

        self._update_last_packet_ui()
        return []

    def start_animation(self):
        interval_ms = max(1, int(1000 / MAX_FPS))
        self.anim = FuncAnimation(
            self.fig,
            self._update_once,
            interval=interval_ms,
            blit=False,
            cache_frame_data=False
        )
        plt.show(block=True)
        while self.running and plt.fignum_exists(self.fig.number):
            plt.pause(0.1)

    def _on_close(self, _event):
        self.running = False

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
        plotter.start_animation()
    except KeyboardInterrupt:
        plotter.close()


if __name__ == "__main__":
    main()
