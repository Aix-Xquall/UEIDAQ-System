#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uei_udp_receiver.py

Realtime UDP receiver + plotter for UEI CSV-like packets.

Format A:
  D,1,<seq>,<slot_index>,<group_name>,<samples_per_channel>,<raw...>

Raw layout:
  scan-major interleaved (same as C++): [s0ch0,s0ch1,..., s1ch0,s1ch1,...]

Format F:
  F,1,<seq>,<slot_index>,<group_name>,<fft_size>,<bins>,<sample_rate_hz>,<window_type>,<overlap>,<num_channels>,<mag_db...>

FFT layout:
  channel-major, one-sided magnitude (dBFS): [ch0_bin0..binN, ch1_bin0..binN, ...]
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

    Note:
      - Receiver plots whatever the sender transmits; MA/FFT flags do not filter streams here.
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

                    group_name = g.get("group_name", "")
                    channels = g.get("channels", []) or []
                    if not group_name or not channels:
                        continue
                    fft = g.get("fft", {}) or {}
                    fft_active = bool(fft.get("active", False))
                    fft_size = int(fft.get("size", 1024))
                    fft_window = str(fft.get("window_type", "hann"))
                    fft_overlap = float(fft.get("overlap", 0.5))

                    target_hz = float(g.get("target_hz", 0.0))
                    base_rate_hz = sr_hz if sr_hz > 0.0 else target_hz
                    ma = g.get("moving_average", {}) or {}
                    ma_active = bool(ma.get("active", False))
                    ma_decim = int(ma.get("decimation", 1)) if ma_active else 1
                    if ma_decim < 1:
                        ma_decim = 1

                    output_rate_hz = base_rate_hz / ma_decim if ma_decim > 0 else base_rate_hz
                    ma_suffix = f", MAx{ma_decim}" if ma_active and ma_decim > 1 else ""
                    out_suffix = f", out={output_rate_hz:g} Hz" if ma_active and ma_decim > 1 else ""
                    target_suffix = f", target={target_hz:g} Hz" if target_hz > 0.0 else ""
                    if fft_active:
                        title = (f"Slot {slot_index}: {board_name} / {group_name} "
                                 f"(FFT N={fft_size}, Fs={output_rate_hz:g} Hz{ma_suffix}{target_suffix})")
                    else:
                        title = f"Slot {slot_index}: {board_name} / {group_name} ({output_rate_hz:g} Hz{ma_suffix}{out_suffix}{target_suffix})"
                    streams.append({
                        "slot_index": slot_index,
                        "board_name": board_name,
                        "group_name": group_name,
                        "channels": [int(c) for c in channels],
                        "rate_hz": output_rate_hz,
                        "base_rate_hz": base_rate_hz,
                        "ma_decimation": ma_decim,
                        "title": title,
                        "is_fft": fft_active,
                        "fft_size": fft_size,
                        "fft_window": fft_window,
                        "fft_overlap": fft_overlap,
                        "sample_rate_hz": output_rate_hz
                    })

            self.streams = streams
            self.stream_index = {(s["slot_index"], s["group_name"]): i for i, s in enumerate(self.streams)}

            safe_print(f"[System] Loaded Config: {self.system_name}")
            safe_print(f"[System] UDP target (from config): {self.udp_target_ip}:{self.udp_target_port}")
            safe_print(f"[System] numSamplesPerChannel: {self.numSamplesPerChannel}")
            safe_print(f"[System] Expect streams (Rule A): {len(self.streams)}")
            for s in self.streams:
                safe_print(f"  - {s['title']} ch={s['channels']}")

            if len(self.streams) == 0:
                safe_print("[Warn] No eligible streams found. Check slot/group active flags.")

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

        self._rx_lock = threading.Lock()
        self._rx_text = "RX: -"
        self._rx_dirty = True
        self._rx_bytes = 0
        self._rx_packets = 0
        self._rx_last_ts = time.time()

        self.time_window = 1.0
        self._last_snapshot_time = 0.0

        self.buffers = []
        self.maxlens = []
        self.lines = []
        self.fft_meta = []
        self.fig = None
        self.axes = []
        self.time_buttons = []
        self.slot_buttons = {}
        self.status_artist = None
        self.stream_axis = {}
        self.slot_groups = {}
        self.slot_ids = []
        self.active_slot = None
        self.active_streams = set()
        self.grid_rows = 0
        self.grid_cols = 0
        self.fixed_figsize = None
        self.slot_boards = {}

        for s in self.mapper.streams:
            if s.get("is_fft", False):
                maxlen = 0
            else:
                rate = float(s["rate_hz"]) if s["rate_hz"] > 0 else 10.0
                maxlen = int(rate * MAX_BUFFER_SEC) + 500
            self.maxlens.append(maxlen)
            self.buffers.append({})
            self.lines.append({})
            if s.get("is_fft", False):
                fft_size = int(s.get("fft_size", 0))
                bins = (fft_size // 2 + 1) if fft_size > 0 else 0
                self.fft_meta.append({
                    "fft_size": fft_size,
                    "bins": bins,
                    "sample_rate_hz": float(s.get("sample_rate_hz", 0.0) or 0.0),
                    "window_type": str(s.get("fft_window", "")),
                    "overlap": float(s.get("fft_overlap", 0.0) or 0.0)
                })
            else:
                self.fft_meta.append({})

        for i, s in enumerate(self.mapper.streams):
            self.slot_groups.setdefault(s["slot_index"], []).append(i)
            if s["slot_index"] not in self.slot_boards:
                self.slot_boards[s["slot_index"]] = s.get("board_name", "")
        self.slot_ids = sorted(self.slot_groups.keys())
        if self.slot_ids:
            self.active_slot = self.slot_ids[0]
            max_groups = max(len(v) for v in self.slot_groups.values())
            max_rows, max_cols = self._select_grid(max_groups)
            self.fixed_figsize = self._grid_figsize(max_rows, max_cols)

        self.init_plot()

        self.udp_thread = threading.Thread(target=self.udp_worker, daemon=True)
        self.udp_thread.start()
        self.proc_thread = threading.Thread(target=self.process_worker, daemon=True)
        self.proc_thread.start()

    def init_plot(self):
        plt.ion()
        labels = ["100ms", "200ms", "500ms", "1S", "2S", "5S"]
        total_streams = len(self.mapper.streams)
        if total_streams <= 0:
            return

        self.fig = plt.figure(figsize=self.fixed_figsize or (12.0, 6.0))
        stream_idxs = self.slot_groups.get(self.active_slot, [])
        self._ensure_grid(len(stream_idxs))

        self._update_window_title()

        self.time_buttons = []
        gap = 0.02
        btn_w = 0.105
        total_w = btn_w * len(labels) + gap * (len(labels) - 1)
        start_x = max(0.1, (1.0 - total_w) / 2.0)
        for i, label in enumerate(labels):
            ax_btn = self.fig.add_axes([start_x + i * (btn_w + gap), 0.03, btn_w, 0.04])
            btn = Button(ax_btn, label, color="0.9", hovercolor="0.8")
            btn.on_clicked(self.make_callback(label))
            self.time_buttons.append(btn)

        self.slot_buttons = {}
        slot_count = len(self.slot_ids)
        if slot_count > 0:
            slot_area = 0.88
            gap = 0.02
            slot_btn_width = (slot_area - gap * (slot_count - 1)) / slot_count
            slot_btn_width = min(0.16, max(0.07, slot_btn_width * 0.9))
            total_width = slot_btn_width * slot_count + gap * (slot_count - 1)
            start_x = max(0.1, (1.0 - total_width) / 2.0)
            for i, slot_id in enumerate(self.slot_ids):
                ax_btn = self.fig.add_axes([start_x + i * (slot_btn_width + gap), 0.945, slot_btn_width, 0.035])
                label = self._slot_label(slot_id)
                btn = Button(ax_btn, label, color="0.9", hovercolor="0.8")
                btn.label.set_fontsize(btn.label.get_fontsize() + 2)
                btn.on_clicked(self.make_slot_callback(slot_id))
                self.slot_buttons[slot_id] = btn

        self.status_artist = self.fig.text(
            0.01, 0.02,
            "RX: -",
            ha="left", va="bottom",
            fontsize=9,
            family="monospace",
            color="black"
        )
        self.fig.canvas.mpl_connect("close_event", self._on_close)

        self._render_active_slot()
        self._sync_buttons("1S")
        self._sync_slot_buttons()

    def _select_grid(self, group_count: int):
        if group_count <= 4:
            cols = 1
        elif group_count <= 9:
            cols = 2
        else:
            cols = 3
        rows = max(1, int(np.ceil(group_count / cols))) if group_count > 0 else 1
        return rows, cols

    def _grid_figsize(self, rows: int, cols: int):
        width = 12.0 + max(0, cols - 1) * 3.0
        height = 3.6 * rows + 1.8
        return (width, height)

    def _ensure_grid(self, group_count: int):
        rows, cols = self._select_grid(group_count)
        if self.grid_rows == rows and self.grid_cols == cols and len(self.axes) > 0:
            return

        self.grid_rows = rows
        self.grid_cols = cols

        if self.fig is None:
            self.fig = plt.figure(figsize=self.fixed_figsize or self._grid_figsize(rows, cols))

        for ax in self.axes:
            ax.remove()

        axes = self.fig.subplots(rows, cols, sharex=False)
        self.axes = np.array(axes).reshape(-1)
        for ax in self.axes:
            ax.set_visible(False)

        self.fig.subplots_adjust(bottom=0.2, top=0.86, hspace=0.6, wspace=0.35)

    def _render_active_slot(self):
        if self.active_slot is None:
            return

        stream_idxs = self.slot_groups.get(self.active_slot, [])
        self._ensure_grid(len(stream_idxs))

        for ax in self.axes:
            ax.clear()
            ax.set_visible(False)
        self.stream_axis.clear()
        self.active_streams = set()

        for i, stream_idx in enumerate(stream_idxs):
            if i >= len(self.axes):
                break
            ax = self.axes[i]
            ax.set_visible(True)
            stream = self.mapper.streams[stream_idx]
            ax.set_title(self._build_group_title(stream))
            ax.grid(True, which="both", linestyle="--", linewidth=0.5)
            if stream.get("is_fft", False):
                ax.set_xlabel("Hz")
                ax.set_ylabel("dBFS")
            else:
                ax.set_xlim(-self.time_window, 0)
            self.stream_axis[stream_idx] = ax
            self.active_streams.add(stream_idx)
            self.lines[stream_idx] = {}

        self._update_window_title()
        if self.fig is not None:
            self.fig.canvas.draw_idle()

    def _update_window_title(self):
        if self.fig is None:
            return
        slot_label = self._slot_label(self.active_slot) if self.active_slot is not None else "Slot -"
        try:
            self.fig.canvas.manager.set_window_title(
                f"{self.mapper.system_name} ({slot_label})"
            )
        except Exception:
            pass

    def _slot_label(self, slot_id: int) -> str:
        if slot_id is None:
            return "Slot -"
        board = self.slot_boards.get(slot_id, "")
        if board:
            return f"Slot {slot_id} ({board})"
        return f"Slot {slot_id}"

    def _build_group_title(self, stream: dict) -> str:
        group = stream.get("group_name", "")
        out_rate = float(stream.get("rate_hz", 0.0) or 0.0)
        base_rate = float(stream.get("base_rate_hz", out_rate) or 0.0)
        ma_decim = int(stream.get("ma_decimation", 1) or 1)

        if stream.get("is_fft", False):
            fft_size = int(stream.get("fft_size", 0) or 0)
            window = str(stream.get("fft_window", "") or "")
            overlap = float(stream.get("fft_overlap", 0.0) or 0.0)
            rate = out_rate if out_rate > 0 else base_rate
            details = f"FFT N={fft_size}, Fs={rate:g}Hz"
            if window:
                details += f", {window}"
            if overlap > 0.0:
                details += f", ovl={overlap:g}"
        else:
            rate = out_rate if out_rate > 0 else base_rate
            details = f"Fs={rate:g}Hz"
            if ma_decim > 1 and base_rate > 0:
                details += f", MAx{ma_decim}"

        return f"{group} ({details})"

    def make_slot_callback(self, slot_id):
        return lambda _event: self.change_slot(slot_id)

    def change_slot(self, slot_id):
        if slot_id == self.active_slot:
            return
        self.active_slot = slot_id
        self._render_active_slot()
        self._sync_slot_buttons()

    def make_callback(self, label):
        return lambda _event: self.change_window(label)

    def change_window(self, label):
        val = 1.0
        if "ms" in label:
            val = float(label.replace("ms", "")) / 1000.0
        elif "S" in label:
            val = float(label.replace("S", ""))
        self.time_window = val

        for stream_idx in self.active_streams:
            stream = self.mapper.streams[stream_idx]
            if stream.get("is_fft", False):
                continue
            ax = self.stream_axis.get(stream_idx)
            if ax is not None:
                ax.set_xlim(-self.time_window, 0)

        self._sync_buttons(label)

    def _sync_buttons(self, active_label):
        for b in self.time_buttons:
            label = b.label.get_text()
            c = "orange" if label == active_label else "0.9"
            b.color = c
            b.ax.set_facecolor(c)

    def _sync_slot_buttons(self):
        for slot_id, btn in self.slot_buttons.items():
            c = "orange" if slot_id == self.active_slot else "0.9"
            btn.color = c
            btn.ax.set_facecolor(c)

    def _update_rx_stats(self, raw_bytes: bytes):
        now = time.time()

        with self._rx_lock:
            self._rx_bytes += len(raw_bytes)
            self._rx_packets += 1
            elapsed = now - self._rx_last_ts
            if elapsed >= 1.0:
                bps = (self._rx_bytes * 8.0) / elapsed
                bytes_per_sec = self._rx_bytes / elapsed
                pkts_per_sec = self._rx_packets / elapsed
                kb_per_sec = bytes_per_sec / 1000.0
                kbps = bps / 1000.0
                self._rx_text = (
                    f"RX: {pkts_per_sec:.1f} pkts/sec, {kb_per_sec:.1f} kB/sec, {kbps:.1f} kbps"
                )
                self._rx_dirty = True
                self._rx_bytes = 0
                self._rx_packets = 0
                self._rx_last_ts = now

    def udp_worker(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((self.bind_ip, self.bind_port))
        safe_print(f"[UDP] Listening on {self.bind_ip}:{self.bind_port} (CSV-like text)")

        while self.running:
            try:
                data, _addr = sock.recvfrom(BUFFER_SIZE)
                self._update_rx_stats(data)
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
          type, seq, slot_index, group_name, ...
        Supports:
          A: D,1,seq,slot,group,samples,raw...
          F: F,1,seq,slot,group,fft_size,bins,sample_rate_hz,window_type,overlap,num_channels,mag_db...
        """
        if len(parts) < 2:
            return None

        if parts[0] == "D" and parts[1] == "1":
            if len(parts) < 7:
                return None
            seq = int(parts[2])
            slot_index = int(parts[3])
            group_a = parts[4]
            try:
                spc_a = int(parts[5])
                key_a = (slot_index, group_a)
                if key_a in self.mapper.stream_index:
                    return {
                        "type": "D",
                        "seq": seq,
                        "slot_index": slot_index,
                        "group_name": group_a,
                        "samples_per_channel": spc_a,
                        "raw_list": parts[6:]
                    }
            except Exception:
                pass
            return None

        if parts[0] == "F" and parts[1] == "1":
            if len(parts) < 12:
                return None
            try:
                seq = int(parts[2])
                slot_index = int(parts[3])
                group_a = parts[4]
                fft_size = int(parts[5])
                bins = int(parts[6])
                sample_rate_hz = float(parts[7])
                window_type = parts[8]
                overlap = float(parts[9])
                num_ch = int(parts[10])
                key_a = (slot_index, group_a)
                if key_a in self.mapper.stream_index:
                    return {
                        "type": "F",
                        "seq": seq,
                        "slot_index": slot_index,
                        "group_name": group_a,
                        "fft_size": fft_size,
                        "bins": bins,
                        "sample_rate_hz": sample_rate_hz,
                        "window_type": window_type,
                        "overlap": overlap,
                        "num_channels": num_ch,
                        "mag_list": parts[11:]
                    }
            except Exception:
                pass
            return None

        return None

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

            stream_idx = self.mapper.stream_index.get((slot_index, group_name), None)
            if stream_idx is None:
                return

            stream = self.mapper.streams[stream_idx]
            ch_list = stream["channels"]
            num_ch = len(ch_list)

            if pkt["type"] == "D":
                samples_per_channel = pkt["samples_per_channel"]
                raw_list = pkt["raw_list"]

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

            elif pkt["type"] == "F":
                fft_size = pkt["fft_size"]
                bins = pkt["bins"]
                sample_rate_hz = pkt["sample_rate_hz"]
                window_type = pkt["window_type"]
                overlap = pkt["overlap"]
                pkt_num_ch = pkt["num_channels"]
                mag_list = pkt["mag_list"]

                if num_ch <= 0 or bins <= 0:
                    return
                if pkt_num_ch != num_ch:
                    safe_print(f"[Warn] FFT channel mismatch: pkt={pkt_num_ch} cfg={num_ch} "
                               f"(slot={slot_index} group={group_name})")
                    return

                expected = num_ch * bins
                if len(mag_list) != expected:
                    safe_print(f"[Warn] FFT size mismatch: got {len(mag_list)} vals, expected {expected} "
                               f"(slot={slot_index} group={group_name} bins={bins} ch={num_ch})")
                    return

                mag_db = np.fromiter((float(x) for x in mag_list), dtype=np.float64, count=expected)
                mag_mat = mag_db.reshape((num_ch, bins))

                with self.buffer_lock:
                    for j in range(num_ch):
                        ch_id = ch_list[j]
                        self.buffers[stream_idx][ch_id] = mag_mat[j].tolist()
                    self.fft_meta[stream_idx] = {
                        "fft_size": int(fft_size),
                        "bins": int(bins),
                        "sample_rate_hz": float(sample_rate_hz),
                        "window_type": str(window_type),
                        "overlap": float(overlap)
                    }

        except Exception as e:
            safe_print(f"[ParseError] {e}")

    def _update_rx_ui(self):
        with self._rx_lock:
            if not self._rx_dirty:
                return
            text = self._rx_text
            self._rx_dirty = False
        if self.status_artist is not None:
            self.status_artist.set_text(text)

    def _update_once(self, _frame=None):
        if len(self.mapper.streams) == 0:
            safe_print("[FATAL] No streams to plot. Fix JSON active flags / MA/FFT settings.")
            return []

        if not self.running:
            return []

        now = time.time()
        min_interval = 1.0 / max(1, SNAPSHOT_MAX_FPS)
        if now - self._last_snapshot_time < min_interval:
            self._update_rx_ui()
            return []
        self._last_snapshot_time = now

        if self.active_slot is None:
            self._update_rx_ui()
            return []

        for stream_idx in self.active_streams:
            stream = self.mapper.streams[stream_idx]
            ax = self.stream_axis.get(stream_idx)
            if ax is None:
                continue
            num_ch = len(stream.get("channels", []))
            if stream.get("is_fft", False):
                with self.buffer_lock:
                    slot_data = self.buffers[stream_idx]
                    meta = dict(self.fft_meta[stream_idx]) if self.fft_meta[stream_idx] else {}
                    if not slot_data or not meta:
                        continue
                    slot_snapshot = {ch_id: list(data) for ch_id, data in slot_data.items()}

                bins = int(meta.get("bins", 0))
                sample_rate_hz = float(meta.get("sample_rate_hz", 0.0) or 0.0)
                if bins <= 0 or sample_rate_hz <= 0:
                    continue
                freq = np.linspace(0, sample_rate_hz / 2.0, bins)

                has_update = False
                y_min = None
                y_max = None
                for ch_id, mag_db in slot_snapshot.items():
                    if len(mag_db) != bins:
                        continue
                    has_update = True
                    if ch_id not in self.lines[stream_idx]:
                        line_obj, = ax.plot([], [], label=f"Ch{ch_id}", lw=1)
                        self.lines[stream_idx][ch_id] = line_obj
                        ax.legend(
                            loc="upper center",
                            bbox_to_anchor=(0.5, 1.02),
                            ncol=max(1, num_ch),
                            fontsize=8,
                            frameon=False,
                        )

                    self.lines[stream_idx][ch_id].set_data(freq, mag_db)

                    dmin = float(min(mag_db))
                    dmax = float(max(mag_db))
                    y_min = dmin if y_min is None else min(y_min, dmin)
                    y_max = dmax if y_max is None else max(y_max, dmax)

                if has_update:
                    if y_min is not None and y_max is not None:
                        margin = (y_max - y_min) * 0.1 if y_max != y_min else 3.0
                        ax.set_ylim(y_min - margin, y_max + margin)
                    ax.set_xlim(0, sample_rate_hz / 2.0)
                    ax.grid(True, which="both", linestyle="--", linewidth=0.5)
            else:
                base_rate = float(stream.get("base_rate_hz", stream.get("rate_hz", 0.0)) or 0.0)
                ma_decim = int(stream.get("ma_decimation", 1) or 1)
                if ma_decim < 1:
                    ma_decim = 1
                output_rate = base_rate / ma_decim if base_rate > 0 else float(stream.get("rate_hz", 10.0) or 10.0)
                if output_rate <= 0:
                    output_rate = 10.0
                points_needed = max(2, int(output_rate * self.time_window))

                with self.buffer_lock:
                    slot_data = self.buffers[stream_idx]
                    if not slot_data:
                        continue
                    slot_snapshot = {ch_id: list(dq) for ch_id, dq in slot_data.items()}

                has_update = False
                y_min = None
                y_max = None
                for ch_id, full_data in slot_snapshot.items():
                    if len(full_data) < 2:
                        continue
                    has_update = True

                    display_data = full_data[-points_needed:] if len(full_data) > points_needed else full_data

                    if len(display_data) > PLOT_DISPLAY_LIMIT:
                        step = max(1, len(display_data) // PLOT_DISPLAY_LIMIT)
                        display_data = display_data[::step]

                    count = len(display_data)
                    real_duration = min(len(full_data), points_needed) / output_rate
                    x_data = np.linspace(-real_duration, 0, count)

                    if ch_id not in self.lines[stream_idx]:
                        line_obj, = ax.plot([], [], label=f"Ch{ch_id}", lw=1)
                        self.lines[stream_idx][ch_id] = line_obj
                        ax.legend(
                            loc="upper center",
                            bbox_to_anchor=(0.5, 1.02),
                            ncol=max(1, num_ch),
                            fontsize=8,
                            frameon=False,
                        )

                    self.lines[stream_idx][ch_id].set_data(x_data, display_data)

                    dmin = float(min(display_data))
                    dmax = float(max(display_data))
                    y_min = dmin if y_min is None else min(y_min, dmin)
                    y_max = dmax if y_max is None else max(y_max, dmax)

                if has_update:
                    if y_min is not None and y_max is not None:
                        margin = (y_max - y_min) * 0.1 if y_max != y_min else 1.0
                        ax.set_ylim(y_min - margin, y_max + margin)
                    ax.set_xlim(-self.time_window, 0)
                    ax.grid(True, which="both", linestyle="--", linewidth=0.5)

        self._update_rx_ui()
        return []

    def start_animation(self):
        interval_ms = max(1, int(1000 / MAX_FPS))
        if self.fig is None:
            safe_print("[FATAL] No figure available. Fix JSON active flags / MA/FFT settings.")
            return
        self.anims = []
        self.anims.append(FuncAnimation(
            self.fig,
            self._update_once,
            interval=interval_ms,
            blit=False,
            cache_frame_data=False
        ))
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
