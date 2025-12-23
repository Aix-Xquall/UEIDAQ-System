import socket
import json
import threading
import queue
import time
import sys
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.widgets import Button
from collections import deque

# ================= 設定區 =================
# 若沒有 JSON 檔，程式會自動使用預設值建立假的 Slot 以供測試
CONFIG_FILE = "DAQ_Settings.json"
UDP_IP = "0.0.0.0"       
UDP_PORT = 5005
BUFFER_SIZE = 65536      
MAX_FPS = 30             
PLOT_DISPLAY_LIMIT = 20000 # 繪圖優化：限制畫面上點數，避免 10S 模式卡頓
MAX_BUFFER_SEC = 20.0    # 關鍵：記憶體保留秒數 (必須大於 UI 上最大的 10S)
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
                    # 判斷是否為 FFT 模式
                    is_fft = ch.get('fft', {}).get('active', False)
                    # 計算有效頻率 (Moving Average)
                    avg_win = int(ch.get('moving_avg', {}).get('window_size', 1)) if ch.get('moving_avg', {}).get('active') else 1
                    eff_rate = task_rate / avg_win

                    if dev_name not in self.device_map:
                        self.slot_titles.append(f"Slot {len(self.slot_titles)+1}: {dev_name}")
                        idx = len(self.slot_titles) - 1
                        self.device_map[dev_name] = idx
                        self.slot_rates[idx] = eff_rate
                        self.slot_modes[idx] = "FFT" if is_fft else "TIME"
                    else:
                        # 若裝置已存在，取最大 Rate
                        idx = self.device_map[dev_name]
                        if eff_rate > self.slot_rates[idx]:
                            self.slot_rates[idx] = eff_rate
                        if is_fft: self.slot_modes[idx] = "FFT"

            print(f"[System] Configured {len(self.slot_titles)} slots.")
            
        except FileNotFoundError:
            print("[System] Config file not found. Using MOCK mode for testing.")
            # 建立兩個虛擬裝置供測試用
            self.device_map = {"Dev1": 0, "Dev2": 1}
            self.slot_titles = ["Slot 1: Dev1 (Vibration)", "Slot 2: Dev2 (Strain)"]
            self.slot_rates = {0: 1000.0, 1: 1000.0}
            self.slot_modes = {0: "TIME", 1: "TIME"}
        except Exception as e:
            print(f"[Error] Config load failed: {e}")
            sys.exit(1)

class RealTimePlotter:
    def __init__(self, mapper):
        self.mapper = mapper
        self.running = True
        self.packet_queue = queue.Queue()
        self.time_window = 1.0  # 預設 1秒
        
        # 初始化 Buffer
        self.buffers = [{} for _ in range(len(self.mapper.slot_titles))]
        self.fft_axis_info = [None] * len(self.mapper.slot_titles)
        
        # === 關鍵修復：預先計算並鎖定 Buffer 長度 ===
        self.slot_max_lens = {}
        for slot_idx, rate in self.mapper.slot_rates.items():
            # 確保 Buffer 至少能存 MAX_BUFFER_SEC 秒的資料
            maxlen = int(rate * MAX_BUFFER_SEC) + 500
            self.slot_max_lens[slot_idx] = maxlen
            print(f"Slot {slot_idx} Max Buffer Size: {maxlen} points ({rate} Hz)")

        self.lines = [{} for _ in range(len(self.mapper.slot_titles))]
        self.init_plot()
        
        self.udp_thread = threading.Thread(target=self.udp_worker, daemon=True)
        self.udp_thread.start()

    def init_plot(self):
        plt.ion() 
        self.fig, self.axes = plt.subplots(len(self.mapper.slot_titles), 1, figsize=(10, 8), sharex=False)
        if len(self.mapper.slot_titles) == 1: self.axes = [self.axes]
        self.axes = np.array(self.axes).flatten()
        
        self.fig.canvas.manager.set_window_title('UEI/NI DAQ Monitor')
        plt.subplots_adjust(bottom=0.15, hspace=0.4)

        for i, ax in enumerate(self.axes):
            title = self.mapper.slot_titles[i]
            rate = self.mapper.slot_rates.get(i, 0)
            mode = self.mapper.slot_modes.get(i, "TIME")
            
            if mode == "FFT":
                ax.set_title(f"{title} [FFT]", fontsize=10, fontweight='bold', color='darkblue')
                ax.set_xlabel("Hz")
            else:
                ax.set_title(f"{title} ({rate:.0f}Hz)", fontsize=10, fontweight='bold')
                ax.set_xlabel("Time (s)")
                ax.set_xlim(-self.time_window, 0)
            
            ax.grid(True, linestyle=':', alpha=0.7)

        # === 時間按鈕 ===
        ax_area = plt.axes([0.1, 0.02, 0.8, 0.05], frameon=False)
        ax_area.set_xticks([])
        ax_area.set_yticks([])
        
        labels = ['10ms', '100ms', '500ms', '1S', '5S', '10S']
        self.btns = []
        
        # 計算按鈕位置
        btn_width = 0.12
        start_x = 0.15
        for i, label in enumerate(labels):
            ax_btn = plt.axes([start_x + i * (btn_width + 0.01), 0.02, btn_width, 0.04])
            btn = Button(ax_btn, label, color='0.9', hovercolor='0.8')
            btn.label_text = label # 儲存標籤以便識別
            btn.on_clicked(self.make_callback(label, btn)) # 使用 closure 綁定
            self.btns.append(btn)
            
            # 預設選中 1S
            if label == '1S':
                btn.color = 'orange'
                ax_btn.set_facecolor('orange')

    def make_callback(self, label, btn):
        return lambda event: self.change_window(label, btn)

    def change_window(self, label, clicked_btn):
        val = 1.0
        if 'ms' in label:
            val = float(label.replace('ms', '')) / 1000.0
        elif 'S' in label:
            val = float(label.replace('S', ''))
        
        self.time_window = val
        print(f"[UI] Window set to: {self.time_window}s")
        
        # 更新按鈕顏色
        for b in self.btns:
            c = 'orange' if b == clicked_btn else '0.9'
            b.color = c
            b.ax.set_facecolor(c)
        
        # 更新所有 Time Mode 的 X 軸
        for i, ax in enumerate(self.axes):
            if self.mapper.slot_modes.get(i) == "TIME":
                ax.set_xlim(-self.time_window, 0)
        
        self.fig.canvas.draw_idle()

    def udp_worker(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((UDP_IP, UDP_PORT))
        print(f"[UDP] Listening on {UDP_PORT}...")
        while self.running:
            try:
                data, _ = sock.recvfrom(BUFFER_SIZE)
                self.packet_queue.put(data)
            except:
                break
        sock.close()

    def process_packet(self, raw_data):
        try:
            msg = raw_data.decode('utf-8').strip()
            parts = msg.split(',')
            
            # 格式檢查: Header, Timestamp, PacketID, ChCount, Res, Data...
            if len(parts) < 6: return
            header_name = parts[0]
            
            target_slot = self.mapper.device_map.get(header_name)
            if target_slot is None: return 

            mode = self.mapper.slot_modes.get(target_slot, "TIME")
            
            if mode == "TIME":
                try:
                    ch_count = int(parts[3])
                    # 資料從第 5 個索引開始 (Header=0, Time=1, PID=2, Count=3, Res=4, Data=5...)
                    data_values = [float(x) for x in parts[5:]]
                except ValueError:
                    return

                # 使用預先計算好的 maxlen，確保不會被截斷
                maxlen = self.slot_max_lens.get(target_slot, 20000)

                for i in range(ch_count):
                    if i >= len(data_values): break
                    val = data_values[i]
                    if i not in self.buffers[target_slot]:
                        self.buffers[target_slot][i] = deque(maxlen=maxlen)
                    self.buffers[target_slot][i].append(val)

        except Exception as e:
            # print(f"Parse Error: {e}") 
            pass

    def update_plot(self):
        while self.running:
            t_start = time.time()
            
            # 消化 Queue 中所有封包
            while not self.packet_queue.empty():
                self.process_packet(self.packet_queue.get())

            for slot_idx, ax in enumerate(self.axes):
                slot_data = self.buffers[slot_idx]
                if not slot_data: continue
                
                mode = self.mapper.slot_modes.get(slot_idx, "TIME")
                if mode == "TIME":
                    eff_rate = self.mapper.slot_rates.get(slot_idx, 1000.0)
                    
                    # 計算需要的點數
                    points_needed = int(eff_rate * self.time_window)
                    if points_needed < 2: points_needed = 2

                    has_update = False
                    for ch_idx, dq in slot_data.items():
                        if len(dq) < 2: continue
                        has_update = True
                        
                        # 轉成 List 處理
                        full_data = list(dq)
                        
                        # 根據時間視窗擷取數據
                        if len(full_data) > points_needed:
                            display_data = full_data[-points_needed:]
                        else:
                            display_data = full_data

                        # 降採樣優化 (避免 10S 模式下繪圖太慢)
                        if len(display_data) > PLOT_DISPLAY_LIMIT:
                            step = len(display_data) // PLOT_DISPLAY_LIMIT
                            display_data = display_data[::step]
                        
                        # 產生 X 軸 (時間倒推)
                        count = len(display_data)
                        duration = count / eff_rate * (len(full_data[-points_needed:]) / count) # 修正 step 造成的縮放
                        # 簡易 X 軸: 0 是最新
                        x_data = np.linspace(- (count / eff_rate), 0, count)
                        if step := (len(full_data[-points_needed:]) // len(display_data)) > 1:
                             x_data = np.linspace(- (len(full_data[-points_needed:]) / eff_rate), 0, count)

                        if ch_idx not in self.lines[slot_idx]:
                            line, = ax.plot([], [], label=f"Ch{ch_idx}", lw=1)
                            self.lines[slot_idx][ch_idx] = line
                            ax.legend(loc='upper left', fontsize=8)
                        
                        self.lines[slot_idx][ch_idx].set_data(x_data, display_data)

                    if has_update:
                        ax.relim()
                        ax.autoscale_view(scalex=False, scaley=True)
                        ax.set_xlim(-self.time_window, 0) # 強制鎖定 X 軸

            self.fig.canvas.flush_events()
            
            # FPS 控制
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