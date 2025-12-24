import socket
import json
import threading
import queue
import time
import sys
import struct
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.widgets import Button
from collections import deque

# ================= 設定區 =================
CONFIG_FILE = "DAQ_Settings.json"
UDP_IP = "0.0.0.0"       
UDP_PORT = 5005
BUFFER_SIZE = 65536      
MAX_FPS = 30             
PLOT_DISPLAY_LIMIT = 20000 
MAX_BUFFER_SEC = 20.0    
# ==========================================

class SystemMapper:
    def __init__(self, config_path):
        self.slot_titles = [] 
        self.device_map = {} 
        self.slot_rates = {}
        self.slot_modes = {}
        self.load_config(config_path)

    def load_config(self, path):
        try:
            with open(path, 'r', encoding='utf-8') as f:
                config = json.load(f)
            print(f"[System] Loading Config: {config.get('system_name')}")
            for task in config.get('tasks', []):
                if not task.get('active', False): continue
                task_rate = float(task.get('sample_rate', 1000.0))
                for ch in task.get('channels', []):
                    if not ch.get('active', True): continue
                    dev_name = ch.get('device_name')
                    is_fft = ch.get('fft', {}).get('active', False)
                    avg_win = int(ch.get('moving_avg', {}).get('window_size', 1)) if ch.get('moving_avg', {}).get('active') else 1
                    eff_rate = task_rate / avg_win
                    if dev_name not in self.device_map:
                        self.slot_titles.append(f"Slot {len(self.slot_titles)+1}: {dev_name}")
                        idx = len(self.slot_titles) - 1
                        self.device_map[dev_name] = idx
                        self.slot_rates[idx] = eff_rate
                        self.slot_modes[idx] = "FFT" if is_fft else "TIME"
        except Exception:
            self.device_map = {"Dev1": 0}
            self.slot_titles = ["Slot 1: Dev1 (Mock)"]
            self.slot_rates = {0: 100.0}
            self.slot_modes = {0: "TIME"}

class RealTimePlotter:
    def __init__(self, mapper):
        self.mapper = mapper
        self.running = True
        self.packet_queue = queue.Queue()
        self.time_window = 1.0
        self.buffers = [{} for _ in range(len(self.mapper.slot_titles))]
        self.slot_max_lens = {}
        for slot_idx, rate in self.mapper.slot_rates.items():
            maxlen = int(rate * MAX_BUFFER_SEC) + 500
            self.slot_max_lens[slot_idx] = maxlen

        self.lines = [{} for _ in range(len(self.mapper.slot_titles))]
        self.init_plot()
        self.udp_thread = threading.Thread(target=self.udp_worker, daemon=True)
        self.udp_thread.start()

    def init_plot(self):
        plt.ion() 
        self.fig, self.axes = plt.subplots(len(self.mapper.slot_titles), 1, figsize=(10, 8), sharex=False)
        if len(self.mapper.slot_titles) == 1: self.axes = [self.axes]
        self.axes = np.array(self.axes).flatten()
        self.fig.canvas.manager.set_window_title('UEI/NI DAQ Monitor (Binary Mode)')
        plt.subplots_adjust(bottom=0.15, hspace=0.4)
        
        # UI Setup
        ax_area = plt.axes([0.1, 0.02, 0.8, 0.05], frameon=False)
        ax_area.set_xticks([]); ax_area.set_yticks([])
        labels = ['10ms', '100ms', '500ms', '1S', '5S', '10S']
        self.btns = []
        start_x = 0.15
        for i, label in enumerate(labels):
            ax_btn = plt.axes([start_x + i * 0.13, 0.02, 0.12, 0.04])
            btn = Button(ax_btn, label, color='0.9', hovercolor='0.8')
            btn.on_clicked(self.make_callback(label, btn))
            self.btns.append(btn)
            if label == '1S': btn.color = 'orange'; ax_btn.set_facecolor('orange')

    def make_callback(self, label, btn):
        return lambda event: self.change_window(label, btn)

    def change_window(self, label, clicked_btn):
        val = 1.0
        if 'ms' in label: val = float(label.replace('ms', '')) / 1000.0
        elif 'S' in label: val = float(label.replace('S', ''))
        self.time_window = val
        for b in self.btns:
            c = 'orange' if b == clicked_btn else '0.9'
            b.color = c
            b.ax.set_facecolor(c)
        for ax in self.axes: ax.set_xlim(-self.time_window, 0)

    def udp_worker(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((UDP_IP, UDP_PORT))
        print(f"[UDP] Listening on {UDP_PORT} (Binary)...")
        while self.running:
            try:
                data, _ = sock.recvfrom(BUFFER_SIZE)
                self.packet_queue.put(data)
            except: break
        sock.close()

    def process_packet(self, raw_data):
        HEADER_SIZE = 16
        if len(raw_data) < HEADER_SIZE: return

        try:
            # 1. Header 解析
            seq_id, timestamp, num_samples, num_ch = struct.unpack('>IdHH', raw_data[:HEADER_SIZE])
            
            # 2. [修正] 使用 '>u4' (Unsigned Big Endian) 讀取 Raw Data
            raw_array = np.frombuffer(raw_data, dtype='>u4', offset=HEADER_SIZE)
            
            if len(raw_array) != num_samples * num_ch: return

            raw_matrix = raw_array.reshape((num_samples, num_ch))
            
            # 3. [關鍵修正] 24-bit Offset Binary 轉 Voltage
            # 步驟 A: 強制濾除高 8 bit (保留低 24 bit)，避免 32-bit 擴充雜訊
            raw_matrix = raw_matrix & 0x00FFFFFF
            
            # 步驟 B: 轉換公式
            # AI-217 規格: 
            # 0x000000 = -10V
            # 0x800000 = 0V
            # 0xFFFFFF = +10V
            # 公式: V = ((Code - 0x800000) / 0x800000) * 10.0
            
            # 先轉 float 運算
            codes = raw_matrix.astype(float)
            volt_matrix = ((codes - 8388608.0) / 8388608.0) * 10.0

            # 4. 存入 Buffer (維持原樣)
            target_slot = 0 
            maxlen = self.slot_max_lens.get(target_slot, 20000)

            for ch in range(num_ch):
                if ch >= len(self.buffers[target_slot]):
                    self.buffers[target_slot][ch] = deque(maxlen=maxlen)
                self.buffers[target_slot][ch].extend(volt_matrix[:, ch])

        except Exception as e:
            print(f"Parse Error: {e}")

    def update_plot(self):
        while self.running:
            t_start = time.time()
            cnt = 0
            while not self.packet_queue.empty() and cnt < 50:
                self.process_packet(self.packet_queue.get())
                cnt += 1

            for slot_idx, ax in enumerate(self.axes):
                slot_data = self.buffers[slot_idx]
                if not slot_data: continue
                
                eff_rate = self.mapper.slot_rates.get(slot_idx, 1000.0)
                points_needed = int(eff_rate * self.time_window)
                if points_needed < 2: points_needed = 2

                has_update = False
                for ch_idx, dq in slot_data.items():
                    if len(dq) < 2: continue
                    has_update = True
                    
                    full_data = list(dq)
                    display_data = full_data[-points_needed:] if len(full_data) > points_needed else full_data
                    
                    # 降採樣顯示優化
                    if len(display_data) > PLOT_DISPLAY_LIMIT:
                        step = len(display_data) // PLOT_DISPLAY_LIMIT
                        display_data = display_data[::step]
                    
                    # X軸生成 (時間倒推)
                    count = len(display_data)
                    real_duration = len(full_data[-points_needed:]) / eff_rate if len(full_data) > points_needed else len(full_data) / eff_rate
                    x_data = np.linspace(-real_duration, 0, count)

                    if ch_idx not in self.lines[slot_idx]:
                        line, = ax.plot([], [], label=f"Ch{ch_idx}", lw=1)
                        self.lines[slot_idx][ch_idx] = line
                        ax.legend(loc='upper left', fontsize=8)
                    
                    self.lines[slot_idx][ch_idx].set_data(x_data, display_data)
                    
                    # 動態調整 Y 軸 (避免一開始被突波拉壞)
                    if ch_idx == 0:
                        y_min, y_max = min(display_data), max(display_data)
                        margin = (y_max - y_min) * 0.1 if y_max != y_min else 1.0
                        ax.set_ylim(y_min - margin, y_max + margin)

                if has_update:
                    ax.set_xlim(-self.time_window, 0)
                    ax.grid(True, which='both', linestyle='--', linewidth=0.5)

            self.fig.canvas.flush_events()
            
            elapsed = time.time() - t_start
            wait = (1.0/MAX_FPS) - elapsed
            if wait > 0: time.sleep(wait)

    def close(self):
        self.running = False
        plt.close('all')
        sys.exit(0)

if __name__ == "__main__":
    mapper = SystemMapper(CONFIG_FILE)
    plotter = RealTimePlotter(mapper)
    try:
        plotter.update_plot()
    except KeyboardInterrupt:
        plotter.close()