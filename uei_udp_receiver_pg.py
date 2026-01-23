#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uei_udp_receiver_pg.py

Realtime UDP receiver + plotter for UEI CSV-like packets (pyqtgraph version).
"""

import argparse
import json
import queue
import socket
import sys
import threading
import time
from collections import deque
import itertools
import multiprocessing as mp
from multiprocessing import shared_memory

import numpy as np

try:
    import pyqtgraph as pg
    from pyqtgraph.Qt import QtCore, QtWidgets, QtGui
except Exception as exc:
    raise SystemExit(f"pyqtgraph/Qt not available: {exc}")

try:
    ALIGN_HCENTER = QtCore.Qt.AlignmentFlag.AlignHCenter
    ALIGN_LEFT = QtCore.Qt.AlignmentFlag.AlignLeft
    ALIGN_VCENTER = QtCore.Qt.AlignmentFlag.AlignVCenter
except AttributeError:
    ALIGN_HCENTER = QtCore.Qt.AlignHCenter
    ALIGN_LEFT = QtCore.Qt.AlignLeft
    ALIGN_VCENTER = QtCore.Qt.AlignVCenter


# ================= Tunables =================
BUFFER_SIZE = 65536
MAX_FPS = 30
SNAPSHOT_MAX_FPS = 10
PLOT_DISPLAY_LIMIT = 10000
MAX_BUFFER_SEC = 5.0
AUTO_Y_RANGE = True
Y_RANGE_UPDATE_EVERY = 5
DOWNSAMPLE_METHOD = "peak"
DOWNSAMPLE_AUTO = True
STYLE_FONT_FAMILY = "Segoe UI"
STYLE_TITLE_PT = 12
STYLE_SUBTITLE_PT = 10
STYLE_AXIS_PT = 9
STYLE_BASE_PT = 10
STYLE_BTN_H = 28
STYLE_BTN_TIME_H = 24
STYLE_BTN_TIME_W = 68
STYLE_BG = "#f2f4f8"
STYLE_PANEL = "#ffffff"
STYLE_GRID_ALPHA = 0.25
STYLE_ACCENT = "#2b6cb0"
STYLE_ACCENT_SOFT = "#e6eef8"
STYLE_TEXT = "#1f2937"
STYLE_TEXT_MUTED = "#4b5563"
# ===========================================


def safe_print(msg: str):
    print(msg, flush=True)


class SystemMapper:
    """
    Load UEI_DAQ_Settings.json and build stream mapping.
    """
    def __init__(self, config_path: str):
        self.config_path = config_path
        self.system_name = "UEI_SYSTEM"
        self.udp_target_ip = ""
        self.udp_target_port = 5005
        self.numSamplesPerChannel = 1000
        self.streams = []
        self.stream_index = {}
        self.load_config(config_path)

    @staticmethod
    def _parse_sample_rate_hz(slot: dict) -> float:
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
                    streams.append({
                        "slot_index": slot_index,
                        "board_name": board_name,
                        "group_name": group_name,
                        "channels": [int(c) for c in channels],
                        "rate_hz": output_rate_hz,
                        "base_rate_hz": base_rate_hz,
                        "ma_decimation": ma_decim,
                        "is_fft": fft_active,
                        "fft_size": fft_size,
                        "fft_window": fft_window,
                        "fft_overlap": fft_overlap,
                        "sample_rate_hz": output_rate_hz,
                    })

            self.streams = streams
            self.stream_index = {(s["slot_index"], s["group_name"]): i for i, s in enumerate(self.streams)}

            safe_print(f"[System] Loaded Config: {self.system_name}")
            safe_print(f"[System] UDP target (from config): {self.udp_target_ip}:{self.udp_target_port}")
            safe_print(f"[System] numSamplesPerChannel: {self.numSamplesPerChannel}")
            safe_print(f"[System] Expect streams (Rule A): {len(self.streams)}")
            for s in self.streams:
                safe_print(f"  - Slot {s['slot_index']}: {s['board_name']} / {s['group_name']} ch={s['channels']}")

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
            }]
            self.stream_index = {(1, "ai217_10hz"): 0}


def convert_ai217_raw_to_volt(raw_u32: np.ndarray) -> np.ndarray:
    codes = (raw_u32 & 0x00FFFFFF).astype(np.float64)
    return ((codes - 8388608.0) / 8388608.0) * 10.0


def decode_packet_fields(parts, stream_index):
    if len(parts) < 2:
        return None

    if parts[0] == "D" and parts[1] == "1":
        if len(parts) < 7:
            return None
        try:
            seq = int(parts[2])
            slot_index = int(parts[3])
            group_a = parts[4]
            spc_a = int(parts[5])
            key_a = (slot_index, group_a)
            if key_a in stream_index:
                return {
                    "type": "D",
                    "seq": seq,
                    "slot_index": slot_index,
                    "group_name": group_a,
                    "samples_per_channel": spc_a,
                    "raw_list": parts[6:],
                }
        except Exception:
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
            if key_a in stream_index:
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
                    "mag_list": parts[11:],
                }
        except Exception:
            return None

    return None


def parse_packet_fast(line_bytes, stream_index):
    if not line_bytes:
        return None

    if line_bytes.startswith(b"D,1,"):
        parts = line_bytes.split(b",", 6)
        if len(parts) < 7:
            return None
        try:
            seq = int(parts[2])
            slot_index = int(parts[3])
            group_a = parts[4].decode("utf-8", errors="ignore")
            spc_a = int(parts[5])
        except Exception:
            return None
        key_a = (slot_index, group_a)
        if key_a not in stream_index:
            return None
        return {
            "type": "D",
            "seq": seq,
            "slot_index": slot_index,
            "group_name": group_a,
            "samples_per_channel": spc_a,
            "raw_bytes": parts[6],
        }

    if line_bytes.startswith(b"F,1,"):
        parts = line_bytes.split(b",", 11)
        if len(parts) < 12:
            return None
        try:
            seq = int(parts[2])
            slot_index = int(parts[3])
            group_a = parts[4].decode("utf-8", errors="ignore")
            fft_size = int(parts[5])
            bins = int(parts[6])
            sample_rate_hz = float(parts[7])
            window_type = parts[8].decode("utf-8", errors="ignore")
            overlap = float(parts[9])
            num_ch = int(parts[10])
        except Exception:
            return None
        key_a = (slot_index, group_a)
        if key_a not in stream_index:
            return None
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
            "mag_bytes": parts[11],
        }

    return None


def calc_output_rate(stream: dict) -> float:
    base_rate = float(stream.get("base_rate_hz", stream.get("rate_hz", 0.0)) or 0.0)
    ma_decim = int(stream.get("ma_decimation", 1) or 1)
    if ma_decim < 1:
        ma_decim = 1
    output_rate = base_rate / ma_decim if base_rate > 0 else float(stream.get("rate_hz", 10.0) or 10.0)
    if output_rate <= 0:
        output_rate = 10.0
    return output_rate


def calc_maxlen(stream: dict) -> int:
    if stream.get("is_fft", False):
        return 0
    output_rate = calc_output_rate(stream)
    return int(output_rate * MAX_BUFFER_SEC) + 100


def worker_main(config_path, bind_ip, bind_port, convert_volts, ctrl_queue, out_queue, shm_info, head_arr, size_arr):
    mapper = SystemMapper(config_path)
    slot_groups = {}
    for i, s in enumerate(mapper.streams):
        slot_groups.setdefault(s["slot_index"], []).append(i)
    slot_ids = sorted(slot_groups.keys())
    active_slot = slot_ids[0] if slot_ids else None
    time_window = 1.0

    shm_objs = []
    shm_arrays = []
    for info in shm_info:
        shm = shared_memory.SharedMemory(name=info["name"])
        arr = np.ndarray(info["shape"], dtype=np.float64, buffer=shm.buf)
        shm_objs.append(shm)
        shm_arrays.append(arr)

    def ring_write(buf, head, size, values):
        ring_len = buf.shape[0]
        n = values.shape[0]
        if ring_len <= 0 or n <= 0:
            return head, size
        if n >= ring_len:
            buf[:] = values[-ring_len:]
            return 0, ring_len
        end = head + n
        if end <= ring_len:
            buf[head:end, :] = values
        else:
            first = ring_len - head
            buf[head:, :] = values[:first]
            buf[:end - ring_len, :] = values[first:]
        head = end % ring_len
        size = min(ring_len, size + n)
        return head, size

    rx_bytes = 0
    rx_packets = 0
    rx_last_ts = time.time()
    rx_text = "RX: -"

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind_ip, bind_port))
    sock.settimeout(0.1)
    safe_print(f"[UDP] Listening on {bind_ip}:{bind_port} (CSV-like text)")

    last_snapshot = 0.0
    running = True

    while running:
        try:
            while True:
                msg = ctrl_queue.get_nowait()
                if msg.get("type") == "stop":
                    running = False
                    break
                if "active_slot" in msg:
                    active_slot = msg["active_slot"]
                if "time_window" in msg:
                    time_window = msg["time_window"]
        except queue.Empty:
            pass

        if not running:
            break

        try:
            data, _addr = sock.recvfrom(BUFFER_SIZE)
            rx_bytes += len(data)
            rx_packets += 1
            now = time.time()
            elapsed = now - rx_last_ts
            if elapsed >= 1.0:
                bps = (rx_bytes * 8.0) / elapsed
                bytes_per_sec = rx_bytes / elapsed
                pkts_per_sec = rx_packets / elapsed
                kb_per_sec = bytes_per_sec / 1000.0
                kbps = bps / 1000.0
                rx_text = f"RX: {pkts_per_sec:.1f} pkts/sec, {kb_per_sec:.1f} kB/sec, {kbps:.1f} kbps"
                rx_bytes = 0
                rx_packets = 0
                rx_last_ts = now
        except socket.timeout:
            data = None
        except Exception:
            break

        if data:
            line_bytes = data.strip()
            if line_bytes:
                if active_slot is not None:
                    quick = line_bytes.split(b",", 4)
                    if len(quick) > 3 and quick[0] in (b"D", b"F") and quick[1] == b"1":
                        try:
                            slot_quick = int(quick[3])
                            if slot_quick != active_slot:
                                line_bytes = None
                        except Exception:
                            line_bytes = None
                if line_bytes:
                    pkt = parse_packet_fast(line_bytes, mapper.stream_index)
                    if pkt is not None:
                        slot_index = pkt["slot_index"]
                        group_name = pkt["group_name"]
                        stream_idx = mapper.stream_index.get((slot_index, group_name))
                        if stream_idx is not None:
                            stream = mapper.streams[stream_idx]
                            ch_list = stream["channels"]
                            num_ch = len(ch_list)
                            if pkt["type"] == "D":
                                samples_per_channel = pkt["samples_per_channel"]
                                expected = num_ch * samples_per_channel
                                raw_str = pkt["raw_bytes"].decode("ascii", errors="ignore")
                                raw_i32 = np.fromstring(raw_str, sep=",", dtype=np.int32)
                                if raw_i32.size == expected:
                                    raw_mat = raw_i32.reshape((samples_per_channel, num_ch))
                                    if convert_volts and stream["board_name"] == "DNA-AI-217":
                                        raw_u32 = raw_mat.view(np.uint32)
                                        y_mat = convert_ai217_raw_to_volt(raw_u32)
                                    else:
                                        y_mat = raw_mat.astype(np.float64)
                                    info = shm_info[stream_idx]
                                    if not info["is_fft"]:
                                        head = head_arr[stream_idx]
                                        size = size_arr[stream_idx]
                                        head, size = ring_write(shm_arrays[stream_idx], head, size, y_mat)
                                        head_arr[stream_idx] = head
                                        size_arr[stream_idx] = size
                            elif pkt["type"] == "F":
                                bins = pkt["bins"]
                                pkt_num_ch = pkt["num_channels"]
                                expected = num_ch * bins
                                if pkt_num_ch == num_ch:
                                    mag_str = pkt["mag_bytes"].decode("ascii", errors="ignore")
                                    mag_db = np.fromstring(mag_str, sep=",", dtype=np.float64)
                                    if mag_db.size == expected:
                                        mag_mat = mag_db.reshape((num_ch, bins))
                                        info = shm_info[stream_idx]
                                        if info["is_fft"] and info["shape"][1] == bins:
                                            shm_arrays[stream_idx][:, :] = mag_mat
                                            epoch = size_arr[stream_idx] + 1
                                            if epoch >= 2147483647:
                                                epoch = 0
                                            size_arr[stream_idx] = epoch

        now = time.time()
        if now - last_snapshot >= 1.0 / max(1, SNAPSHOT_MAX_FPS):
            if active_slot is not None:
                stream_idxs = slot_groups.get(active_slot, [])
                snapshot = {
                    "slot": active_slot,
                    "rx_text": rx_text,
                }
                try:
                    out_queue.put_nowait(snapshot)
                except queue.Full:
                    try:
                        out_queue.get_nowait()
                    except queue.Empty:
                        pass
                    try:
                        out_queue.put_nowait(snapshot)
                    except queue.Full:
                        pass
            last_snapshot = now

    for shm in shm_objs:
        shm.close()
    sock.close()


class RealTimePlotterPg(QtWidgets.QMainWindow):
    def __init__(self, mapper: SystemMapper, bind_ip: str, bind_port: int, convert_volts: bool):
        super().__init__()
        self.mapper = mapper
        self.bind_ip = bind_ip
        self.bind_port = bind_port
        self.convert_volts = convert_volts

        self.running = True
        self.packet_queue = None
        self.buffer_lock = threading.Lock()
        self.ctrl_queue = mp.Queue()
        self.data_queue = mp.Queue(maxsize=2)
        self.worker_proc = None

        self._rx_lock = threading.Lock()
        self._rx_text = "RX: -"
        self._rx_dirty = True
        self._rx_bytes = 0
        self._rx_packets = 0
        self._rx_last_ts = time.time()

        self.time_window = 1.0
        self._last_snapshot_time = 0.0
        self.latest_snapshot = None

        self.buffers = []
        self.maxlens = []
        self.fft_meta = []

        self.slot_groups = {}
        self.slot_boards = {}
        self.slot_ids = []
        self.active_slot = None
        self.active_streams = set()

        self.plot_widgets = {}
        self.plot_lines = {}
        self.channel_labels = {}
        self.time_x_cache = {}
        self.fft_x_cache = {}
        self.y_update_counter = {}
        self.shm_info = []
        self.shm_objects = []
        self.shared_arrays = []
        self.head_arr = None
        self.size_arr = None
        self.last_head = []
        self.last_size = []
        self.last_fft_epoch = []

        for _s in self.mapper.streams:
            self.maxlens.append(0)
            self.buffers.append({})
            self.fft_meta.append({})

        for i, s in enumerate(self.mapper.streams):
            self.slot_groups.setdefault(s["slot_index"], []).append(i)
            if s["slot_index"] not in self.slot_boards:
                self.slot_boards[s["slot_index"]] = s.get("board_name", "")
        self.slot_ids = sorted(self.slot_groups.keys())
        if self.slot_ids:
            self.active_slot = self.slot_ids[0]

        num_streams = len(self.mapper.streams)
        self.head_arr = mp.Array("i", num_streams, lock=False)
        self.size_arr = mp.Array("i", num_streams, lock=False)
        self.shared_arrays = [None] * num_streams
        self.last_head = [0] * num_streams
        self.last_size = [0] * num_streams
        self.last_fft_epoch = [0] * num_streams
        for i, s in enumerate(self.mapper.streams):
            num_ch = len(s.get("channels", []))
            if s.get("is_fft", False):
                fft_size = int(s.get("fft_size", 0))
                bins = max(1, fft_size // 2 + 1)
                ring_len = 0
                shape = (num_ch, bins)
            else:
                ring_len = max(2, calc_maxlen(s))
                shape = (ring_len, max(1, num_ch))
                self.head_arr[i] = 0
                self.size_arr[i] = 0

            shm = shared_memory.SharedMemory(create=True, size=int(np.prod(shape)) * 8)
            arr = np.ndarray(shape, dtype=np.float64, buffer=shm.buf)
            arr.fill(0)

            self.shm_objects.append(shm)
            self.shared_arrays[i] = arr
            self.shm_info.append({
                "name": shm.name,
                "shape": shape,
                "is_fft": bool(s.get("is_fft", False)),
                "num_channels": num_ch,
                "ring_len": ring_len,
                "bins": shape[1] if len(shape) > 1 else 0,
            })

        self._build_ui()
        self._start_worker()
        self._send_control()

        self.timer = QtCore.QTimer(self)
        interval_ms = max(1, int(1000 / MAX_FPS))
        self.timer.timeout.connect(self._update_once)
        self.timer.start(interval_ms)

    def _build_ui(self):
        pg.setConfigOptions(antialias=True)
        self.setWindowTitle(self._window_title())
        base_font = QtGui.QFont(STYLE_FONT_FAMILY)
        base_font.setPointSize(STYLE_BASE_PT)
        self.setFont(base_font)

        central = QtWidgets.QWidget()
        central.setStyleSheet(f"background-color: {STYLE_BG}; color: {STYLE_TEXT};")
        main_layout = QtWidgets.QVBoxLayout(central)
        main_layout.setContentsMargins(10, 8, 10, 8)
        main_layout.setSpacing(8)

        # Slot buttons (top)
        slot_bar = QtWidgets.QHBoxLayout()
        slot_bar.setSpacing(6)
        slot_bar.addStretch(1)
        self.slot_buttons = {}
        slot_btn_style = (
            "QPushButton {"
            f" background-color: {STYLE_PANEL};"
            " border: 1px solid #cbd5e1;"
            " border-radius: 4px;"
            " padding: 4px 10px;"
            f" color: {STYLE_TEXT};"
            "}"
            "QPushButton:checked {"
            f" background-color: {STYLE_ACCENT_SOFT};"
            f" border-color: {STYLE_ACCENT};"
            "}"
        )
        for slot_id in self.slot_ids:
            label = self._slot_label(slot_id)
            btn = QtWidgets.QPushButton(label)
            btn.setCheckable(True)
            btn.setFixedHeight(STYLE_BTN_H)
            font = btn.font()
            font.setPointSize(STYLE_TITLE_PT)
            btn.setFont(font)
            btn.setStyleSheet(slot_btn_style)
            btn.clicked.connect(lambda _checked, sid=slot_id: self.change_slot(sid))
            self.slot_buttons[slot_id] = btn
            slot_bar.addWidget(btn)
        slot_bar.addStretch(1)
        main_layout.addLayout(slot_bar)

        # Plot grid (middle)
        self.plot_grid_widget = QtWidgets.QWidget()
        self.plot_grid = QtWidgets.QGridLayout(self.plot_grid_widget)
        self.plot_grid.setContentsMargins(0, 0, 0, 0)
        self.plot_grid.setSpacing(10)
        main_layout.addWidget(self.plot_grid_widget, stretch=1)

        # Bottom area: time buttons + RX label
        bottom = QtWidgets.QGridLayout()
        bottom.setContentsMargins(0, 0, 0, 0)
        bottom.setSpacing(6)
        self.time_buttons = {}
        time_labels = ["100ms", "200ms", "500ms", "1S", "2S", "5S"]
        time_bar = QtWidgets.QHBoxLayout()
        time_bar.setSpacing(6)
        time_btn_style = (
            "QPushButton {"
            f" background-color: {STYLE_PANEL};"
            " border: 1px solid #cbd5e1;"
            " border-radius: 4px;"
            " padding: 3px 8px;"
            f" color: {STYLE_TEXT};"
            "}"
            "QPushButton:checked {"
            f" background-color: {STYLE_ACCENT_SOFT};"
            f" border-color: {STYLE_ACCENT};"
            "}"
        )
        for label in time_labels:
            btn = QtWidgets.QPushButton(label)
            btn.setCheckable(True)
            btn.setFixedHeight(STYLE_BTN_TIME_H)
            btn.setFixedWidth(STYLE_BTN_TIME_W)
            tfont = btn.font()
            tfont.setPointSize(STYLE_SUBTITLE_PT)
            btn.setFont(tfont)
            btn.setStyleSheet(time_btn_style)
            btn.clicked.connect(lambda _checked, lb=label: self.change_window(lb))
            self.time_buttons[label] = btn
            time_bar.addWidget(btn)
        time_container = QtWidgets.QWidget()
        time_container.setLayout(time_bar)
        bottom.addWidget(time_container, 0, 1, alignment=ALIGN_HCENTER)

        self.rx_label = QtWidgets.QLabel("RX: -")
        rx_font = self.rx_label.font()
        rx_font.setPointSize(STYLE_SUBTITLE_PT)
        self.rx_label.setFont(rx_font)
        self.rx_label.setStyleSheet(f"color: {STYLE_TEXT_MUTED};")
        self.rx_label.setAlignment(ALIGN_LEFT | ALIGN_VCENTER)
        bottom.addWidget(self.rx_label, 1, 0, alignment=ALIGN_LEFT)
        bottom.setColumnStretch(0, 1)
        bottom.setColumnStretch(1, 3)
        bottom.setColumnStretch(2, 1)
        main_layout.addLayout(bottom)

        self.setCentralWidget(central)
        self._sync_slot_buttons()
        self._sync_time_buttons("1S")
        self._rebuild_plots_for_slot()

    def _select_grid(self, group_count: int):
        if group_count <= 4:
            cols = 1
        elif group_count <= 9:
            cols = 2
        else:
            cols = 3
        rows = max(1, int(np.ceil(group_count / cols))) if group_count > 0 else 1
        return rows, cols

    def _calc_output_rate(self, stream: dict) -> float:
        base_rate = float(stream.get("base_rate_hz", stream.get("rate_hz", 0.0)) or 0.0)
        ma_decim = int(stream.get("ma_decimation", 1) or 1)
        if ma_decim < 1:
            ma_decim = 1
        output_rate = base_rate / ma_decim if base_rate > 0 else float(stream.get("rate_hz", 10.0) or 10.0)
        if output_rate <= 0:
            output_rate = 10.0
        return output_rate

    def _get_time_x(self, stream_idx: int, points_needed: int, output_rate: float):
        cached = self.time_x_cache.get(stream_idx)
        key = (points_needed, output_rate, self.time_window)
        if cached and cached[0] == key:
            return cached[1]
        real_duration = points_needed / output_rate
        x_data = np.linspace(-real_duration, 0, points_needed)
        self.time_x_cache[stream_idx] = (key, x_data)
        return x_data

    def _get_fft_x(self, stream_idx: int, bins: int, sample_rate_hz: float):
        cached = self.fft_x_cache.get(stream_idx)
        key = (bins, sample_rate_hz)
        if cached and cached[0] == key:
            return cached[1]
        freq = np.linspace(0, sample_rate_hz / 2.0, bins)
        self.fft_x_cache[stream_idx] = (key, freq)
        return freq

    def _clear_plot_grid(self):
        while self.plot_grid.count():
            item = self.plot_grid.takeAt(0)
            w = item.widget()
            if w is not None:
                w.setParent(None)
                w.deleteLater()
        self.plot_widgets.clear()
        self.plot_lines.clear()
        self.channel_labels.clear()

    def _rebuild_plots_for_slot(self):
        self._clear_plot_grid()
        self.time_x_cache.clear()
        self.fft_x_cache.clear()
        self.y_update_counter.clear()
        if self.active_slot is None:
            return

        stream_idxs = self.slot_groups.get(self.active_slot, [])
        rows, cols = self._select_grid(len(stream_idxs))
        self.active_streams = set(stream_idxs)

        for idx, stream_idx in enumerate(stream_idxs):
            stream = self.mapper.streams[stream_idx]
            row = idx // cols
            col = idx % cols

            container = QtWidgets.QWidget()
            vbox = QtWidgets.QVBoxLayout(container)
            container.setStyleSheet(
                f"background-color: {STYLE_PANEL};"
                " border: 1px solid #e5e7eb;"
                " border-radius: 4px;"
            )
            vbox.setContentsMargins(8, 6, 8, 6)
            vbox.setSpacing(4)

            title = QtWidgets.QLabel(self._build_group_title(stream))
            title.setAlignment(ALIGN_HCENTER)
            tfont = title.font()
            tfont.setPointSize(STYLE_TITLE_PT)
            tfont.setBold(True)
            title.setFont(tfont)
            title.setStyleSheet(f"color: {STYLE_TEXT};")
            vbox.addWidget(title)

            ch_label = QtWidgets.QLabel(self._channel_label_text(stream))
            ch_label.setAlignment(ALIGN_HCENTER)
            cfont = ch_label.font()
            cfont.setPointSize(STYLE_SUBTITLE_PT)
            ch_label.setFont(cfont)
            ch_label.setStyleSheet(f"color: {STYLE_TEXT_MUTED};")
            vbox.addWidget(ch_label)
            self.channel_labels[stream_idx] = ch_label

            plot = pg.PlotWidget()
            plot.showGrid(x=True, y=True, alpha=STYLE_GRID_ALPHA)
            plot.setBackground(STYLE_PANEL)
            axis_pen = pg.mkPen(STYLE_TEXT_MUTED)
            axis_font = QtGui.QFont(STYLE_FONT_FAMILY, STYLE_AXIS_PT)
            left_axis = plot.getAxis("left")
            bottom_axis = plot.getAxis("bottom")
            left_axis.setTextPen(axis_pen)
            bottom_axis.setTextPen(axis_pen)
            left_axis.setPen(axis_pen)
            bottom_axis.setPen(axis_pen)
            left_axis.setStyle(tickFont=axis_font)
            bottom_axis.setStyle(tickFont=axis_font)
            plot.disableAutoRange()
            if stream.get("is_fft", False):
                plot.setLabel(
                    "bottom",
                    "Hz",
                    **{
                        "color": STYLE_TEXT_MUTED,
                        "font-size": f"{STYLE_AXIS_PT}pt",
                        "font-family": STYLE_FONT_FAMILY,
                    },
                )
                plot.setLabel(
                    "left",
                    "dBFS",
                    **{
                        "color": STYLE_TEXT_MUTED,
                        "font-size": f"{STYLE_AXIS_PT}pt",
                        "font-family": STYLE_FONT_FAMILY,
                    },
                )
            vbox.addWidget(plot, stretch=1)

            self.plot_widgets[stream_idx] = plot
            self.plot_lines[stream_idx] = {}
            self.y_update_counter[stream_idx] = 0
            self.plot_grid.addWidget(container, row, col)

        self._sync_time_buttons(self._current_time_label())
        self.setWindowTitle(self._window_title())
        self._send_control()

    def _start_worker(self):
        ctx = mp.get_context("spawn")
        self.worker_proc = ctx.Process(
            target=worker_main,
            args=(
                self.mapper.config_path,
                self.bind_ip,
                self.bind_port,
                self.convert_volts,
                self.ctrl_queue,
                self.data_queue,
                self.shm_info,
                self.head_arr,
                self.size_arr,
            ),
            daemon=True,
        )
        self.worker_proc.start()

    def _send_control(self):
        try:
            self.ctrl_queue.put_nowait({
                "active_slot": self.active_slot,
                "time_window": self.time_window,
            })
        except queue.Full:
            pass

    def _slot_label(self, slot_id: int) -> str:
        board = self.slot_boards.get(slot_id, "")
        if board:
            return f"Slot {slot_id} ({board})"
        return f"Slot {slot_id}"

    def _window_title(self) -> str:
        if self.active_slot is None:
            return self.mapper.system_name
        return f"{self.mapper.system_name} ({self._slot_label(self.active_slot)})"

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

    def _channel_label_text(self, stream: dict) -> str:
        channels = stream.get("channels", [])
        if not channels:
            return ""
        return "  ".join([f"Ch{c}" for c in channels])

    def _current_time_label(self) -> str:
        if self.time_window < 1.0:
            return f"{int(self.time_window * 1000)}ms"
        return f"{int(self.time_window)}S"

    def _sync_time_buttons(self, active_label: str):
        for label, btn in self.time_buttons.items():
            btn.setChecked(label == active_label)

    def _sync_slot_buttons(self):
        for slot_id, btn in self.slot_buttons.items():
            btn.setChecked(slot_id == self.active_slot)

    def change_slot(self, slot_id):
        if slot_id == self.active_slot:
            return
        self.active_slot = slot_id
        self._sync_slot_buttons()
        self._rebuild_plots_for_slot()
        self._send_control()

    def change_window(self, label):
        val = 1.0
        if "ms" in label:
            val = float(label.replace("ms", "")) / 1000.0
        elif "S" in label:
            val = float(label.replace("S", ""))
        self.time_window = val
        self._sync_time_buttons(label)
        self.time_x_cache.clear()
        self._send_control()
        for stream_idx in self.active_streams:
            stream = self.mapper.streams[stream_idx]
            if stream.get("is_fft", False):
                continue
            plot = self.plot_widgets.get(stream_idx)
            if plot is not None:
                plot.setXRange(-self.time_window, 0, padding=0)

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
                        "raw_list": parts[6:],
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
                        "mag_list": parts[11:],
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
            if self.active_slot is not None and len(parts) > 3:
                if parts[0] in ("D", "F") and parts[1] == "1":
                    try:
                        slot_quick = int(parts[3])
                        if slot_quick != self.active_slot:
                            return
                    except Exception:
                        return
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
                        "overlap": float(overlap),
                    }

        except Exception as e:
            safe_print(f"[ParseError] {e}")

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

    def _update_rx_ui(self):
        if self.latest_snapshot and "rx_text" in self.latest_snapshot:
            self.rx_label.setText(self.latest_snapshot["rx_text"])

    def _update_once(self):
        if len(self.mapper.streams) == 0:
            return
        if not self.running:
            return

        while True:
            try:
                snap = self.data_queue.get_nowait()
                self.latest_snapshot = snap
            except queue.Empty:
                break

        self._update_rx_ui()

        if self.active_slot is None:
            return

        for stream_idx in self.active_streams:
            stream = self.mapper.streams[stream_idx]
            plot = self.plot_widgets.get(stream_idx)
            if plot is None:
                continue

            info = self.shm_info[stream_idx]
            arr = self.shared_arrays[stream_idx]
            num_ch = info["num_channels"]

            if info["is_fft"]:
                bins = info["bins"]
                if bins <= 0 or num_ch <= 0:
                    continue
                sample_rate_hz = float(stream.get("sample_rate_hz", 0.0) or 0.0)
                if sample_rate_hz <= 0:
                    continue
                fft_epoch = self.size_arr[stream_idx]
                if fft_epoch == self.last_fft_epoch[stream_idx]:
                    continue
                self.last_fft_epoch[stream_idx] = fft_epoch
                freq = self._get_fft_x(stream_idx, bins, sample_rate_hz)

                do_y_update = False
                if AUTO_Y_RANGE:
                    self.y_update_counter[stream_idx] = self.y_update_counter.get(stream_idx, 0) + 1
                    do_y_update = (self.y_update_counter[stream_idx] % Y_RANGE_UPDATE_EVERY) == 0
                y_min = None
                y_max = None
                for idx in range(num_ch):
                    ch_id = stream["channels"][idx]
                    mag_db = arr[idx, :]
                    line = self.plot_lines[stream_idx].get(ch_id)
                    if line is None:
                        line = plot.plot(pen=pg.mkPen(pg.intColor(idx)), name=f"Ch{ch_id}")
                        if DOWNSAMPLE_AUTO:
                            line.setDownsampling(auto=True, method=DOWNSAMPLE_METHOD)
                            line.setClipToView(True)
                        self.plot_lines[stream_idx][ch_id] = line
                    line.setData(freq, mag_db)
                    if do_y_update:
                        dmin = float(np.min(mag_db))
                        dmax = float(np.max(mag_db))
                        y_min = dmin if y_min is None else min(y_min, dmin)
                        y_max = dmax if y_max is None else max(y_max, dmax)

                if do_y_update and y_min is not None and y_max is not None:
                    margin = (y_max - y_min) * 0.1 if y_max != y_min else 3.0
                    plot.setYRange(y_min - margin, y_max + margin, padding=0)
                plot.setXRange(0, sample_rate_hz / 2.0, padding=0)
            else:
                ring_len = info["ring_len"]
                if ring_len <= 0 or num_ch <= 0:
                    continue
                head = self.head_arr[stream_idx]
                size = self.size_arr[stream_idx]
                if head == self.last_head[stream_idx] and size == self.last_size[stream_idx]:
                    continue
                self.last_head[stream_idx] = head
                self.last_size[stream_idx] = size
                if size < 2:
                    continue

                output_rate = calc_output_rate(stream)
                points_needed = max(2, int(output_rate * self.time_window))
                points_needed = min(points_needed, size, ring_len)

                start = (head - points_needed) % ring_len
                if start < head:
                    segment = arr[start:head, :]
                else:
                    if head == 0:
                        segment = arr[start:, :]
                    else:
                        segment = np.concatenate((arr[start:, :], arr[:head, :]), axis=0)

                if segment.shape[0] == 0:
                    continue

                x_full = self._get_time_x(stream_idx, points_needed, output_rate)
                step = max(1, segment.shape[0] // PLOT_DISPLAY_LIMIT) if segment.shape[0] > PLOT_DISPLAY_LIMIT else 1
                x_data = x_full[::step]

                do_y_update = False
                if AUTO_Y_RANGE:
                    self.y_update_counter[stream_idx] = self.y_update_counter.get(stream_idx, 0) + 1
                    do_y_update = (self.y_update_counter[stream_idx] % Y_RANGE_UPDATE_EVERY) == 0
                y_min = None
                y_max = None
                for idx in range(num_ch):
                    ch_id = stream["channels"][idx]
                    display_data = segment[:, idx]
                    if step > 1:
                        display_data = display_data[::step]

                    line = self.plot_lines[stream_idx].get(ch_id)
                    if line is None:
                        line = plot.plot(pen=pg.mkPen(pg.intColor(idx)), name=f"Ch{ch_id}")
                        if DOWNSAMPLE_AUTO:
                            line.setDownsampling(auto=True, method=DOWNSAMPLE_METHOD)
                            line.setClipToView(True)
                        self.plot_lines[stream_idx][ch_id] = line
                    line.setData(x_data, display_data)

                    if do_y_update:
                        dmin = float(np.min(display_data))
                        dmax = float(np.max(display_data))
                        y_min = dmin if y_min is None else min(y_min, dmin)
                        y_max = dmax if y_max is None else max(y_max, dmax)

                if do_y_update and y_min is not None and y_max is not None:
                    margin = (y_max - y_min) * 0.1 if y_max != y_min else 1.0
                    plot.setYRange(y_min - margin, y_max + margin, padding=0)
                plot.setXRange(-self.time_window, 0, padding=0)

    def closeEvent(self, _event):
        self.running = False
        if self.timer is not None:
            self.timer.stop()
        try:
            self.ctrl_queue.put_nowait({"type": "stop"})
        except Exception:
            pass
        if self.worker_proc and self.worker_proc.is_alive():
            self.worker_proc.join(timeout=1.0)
            if self.worker_proc.is_alive():
                self.worker_proc.terminate()
        for shm in self.shm_objects:
            try:
                shm.close()
                shm.unlink()
            except Exception:
                pass


def main():
    try:
        mp.set_start_method("spawn", force=True)
    except RuntimeError:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="UEI_DAQ_Settings.json", help="Path to UEI_DAQ_Settings.json")
    ap.add_argument("--bind-ip", default="0.0.0.0", help="UDP bind IP (default 0.0.0.0)")
    ap.add_argument("--bind-port", default=None, help="UDP bind port (default: udp_target_port from config)")
    ap.add_argument("--no-volts", action="store_true", help="Do not convert AI-217 raw to volts")
    args = ap.parse_args()

    mapper = SystemMapper(args.config)
    bind_port = int(args.bind_port) if args.bind_port is not None else mapper.udp_target_port

    app = QtWidgets.QApplication(sys.argv)
    win = RealTimePlotterPg(
        mapper,
        bind_ip=args.bind_ip,
        bind_port=bind_port,
        convert_volts=(not args.no_volts),
    )
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
