import socket
import time
import math
import random
import struct

# ================= 模擬設定 =================
UDP_IP = "127.0.0.1"    # 本機測試
UDP_PORT = 5005
SAMPLE_RATE = 100.0     # 100Hz
BATCH_SIZE = 10         # 模擬 C++ 端每 10 點發送一次 (0.1s)
NUM_CHANNELS = 8        # 模擬 8 個通道

# 模擬 AI-217 的 24-bit ADC 特性
# Code 0 = -10V, Code 0x800000 = 0V, Code 0xFFFFFF = +10V
ADC_MAX_CODE = 16777216.0 # 2^24
ADC_RANGE_V = 20.0        # +/- 10V = 20V span
ADC_OFFSET_V = 10.0       # -10V offset

def vol_to_code(voltage):
    """將電壓轉換回 AI-217 的 24-bit Raw Code (模擬 ADC)"""
    # 限制範圍 +/- 10V
    voltage = max(min(voltage, 10.0), -10.0)
    
    # 逆向公式: Code = ((Voltage + 10) / 20) * 2^24
    norm = (voltage + ADC_OFFSET_V) / ADC_RANGE_V
    code = int(norm * ADC_MAX_CODE)
    
    # 確保不溢位
    return max(min(code, 0xFFFFFF), 0)

# ============================================

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
print(f"=== UEIPAC Binary Sender Simulator (Big Endian) ===")
print(f"Target: {UDP_IP}:{UDP_PORT}")
print(f"Rate: {SAMPLE_RATE} Hz, Batch: {BATCH_SIZE}")
print(f"Format: Binary struct (>IdHH) + Raw Data (>I)")
print(f"Press Ctrl+C to stop.\n")

seq_id = 0
start_time = time.time()
sim_time = 0.0 # 模擬時間軸
dt = 1.0 / SAMPLE_RATE

try:
    while True:
        loop_start = time.time()
        
        # 準備 Batch 容器
        batch_raw_data = [] # 這裡將會是平坦的列表 [Ch0, Ch1... Ch0, Ch1...]
        
        # 記錄這個 Batch 的起始時間
        batch_timestamp = time.time()

        # 生成 BATCH_SIZE 個取樣點
        for _ in range(BATCH_SIZE):
            # 針對每個通道生成數據
            for ch in range(NUM_CHANNELS):
                val = 0.0
                if ch == 0: # Ch0: 2Hz Sine
                    val = 5.0 * math.sin(2 * math.pi * 2.0 * sim_time)
                elif ch == 1: # Ch1: 0.5Hz Sine
                    val = 8.0 * math.sin(2 * math.pi * 0.5 * sim_time)
                elif ch == 2: # Ch2: DC Offset
                    val = 2.5
                elif ch == 3: # Ch3: Noise
                    val = random.uniform(-1, 1)
                else: # 其他通道歸零
                    val = 0.0
                
                # 轉成 Raw Code
                code = vol_to_code(val)
                batch_raw_data.append(code)
            
            sim_time += dt

        # === 封包打包 (Binary Packing) ===
        # 1. Header: Seq(I), TS(d), Samples(H), Channels(H)
        # 注意: 使用 '>' (Big Endian) 模擬 PowerPC
        header = struct.pack('>IdHH', seq_id, batch_timestamp, BATCH_SIZE, NUM_CHANNELS)
        
        # 2. Body: 將所有 uint32 code 打包
        # 格式字串例如: '>80I' (若 Batch=10, Ch=8 -> 80個整數)
        body_fmt = f'>{len(batch_raw_data)}I' 
        payload = struct.pack(body_fmt, *batch_raw_data)
        
        # 發送
        sock.sendto(header + payload, (UDP_IP, UDP_PORT))
        print(f"\rSent Seq: {seq_id} | Time: {sim_time:.2f}s | Bytes: {len(header)+len(payload)}", end='')

        seq_id += 1
        
        # 模擬採集發送間隔 (Batch 間隔 = BatchSize * dt)
        # 例如 10點 * 0.01s = 0.1s 發送一次
        wait_time = (BATCH_SIZE * dt) - (time.time() - loop_start)
        if wait_time > 0:
            time.sleep(wait_time)

except KeyboardInterrupt:
    print("\nStopped.")
    sock.close()