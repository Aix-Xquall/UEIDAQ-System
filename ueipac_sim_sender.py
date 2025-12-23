import socket
import time
import math
import random

# ================= 模擬設定 =================
UDP_IP = "127.0.0.1"  # 本機測試
UDP_PORT = 5005
SAMPLE_RATE = 100.0  # 1kHz
INTERVAL = 1.0 / SAMPLE_RATE

# 模擬兩個裝置
DEVICES = [
    {"name": "Dev_AI217", "channels": 4, "func": "sine"},
    {"name": "Dev_AI208", "channels": 2, "func": "noise"}
]
# ============================================

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
print(f"=== UEIPAC CSV Sender Simulator ===")
print(f"Target: {UDP_IP}:{UDP_PORT}")
print(f"Sample Rate: {SAMPLE_RATE} Hz")
print(f"Press Ctrl+C to stop.\n")

packet_id = 0
start_time = time.time()
t_phase = 0.0

try:
    while True:
        loop_start = time.time()
        
        # 產生數據
        t_now = loop_start - start_time
        
        for dev in DEVICES:
            name = dev["name"]
            ch_count = dev["channels"]
            values = []
            
            # 產生模擬數值
            for ch in range(ch_count):
                val = 0.0
                if dev["func"] == "sine":
                    # 不同通道不同頻率的正弦波
                    freq = 1 + (ch * 1)                                       
                    val = math.sin(2 * math.pi * freq * t_now) * 5
                else:
                    # 隨機雜訊
                    val = random.uniform(-2, 2)
                values.append(f"{val:.4f}")
            
            # 組裝 CSV 封包
            # 格式: Name, Timestamp(ms), PacketID, ChCount, Reserved, Val1, Val2...
            ts_ms = int(t_now * 1000)
            data_str = ",".join(values)
            msg = f"{name},{ts_ms},{packet_id},{ch_count},0,{data_str}"
            #print("Sending:", msg)
            sock.sendto(msg.encode('utf-8'), (UDP_IP, UDP_PORT))

        packet_id += 1
        
        # 精確延遲控制 (Busy Wait for precision or simple sleep)
        elapsed = time.time() - loop_start
        sleep_time = INTERVAL - elapsed
        if sleep_time > 0:
            time.sleep(sleep_time)

except KeyboardInterrupt:
    print("\nStopped.")
    sock.close()