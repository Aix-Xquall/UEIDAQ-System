import argparse
import json
import math
import random
import socket
import time


ADC_MAX_CODE = 16777216.0
ADC_RANGE_V = 20.0
ADC_OFFSET_V = 10.0


def vol_to_code(voltage: float) -> int:
    voltage = max(min(voltage, 10.0), -10.0)
    norm = (voltage + ADC_OFFSET_V) / ADC_RANGE_V
    code = int(norm * ADC_MAX_CODE)
    return max(min(code, 0xFFFFFF), 0)


def load_settings(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def build_streams(cfg: dict):
    streams = []
    num_samples = int(cfg.get("numSamplesPerChannel", 0) or 0)
    udp_ip = cfg.get("udp_target_ip", "127.0.0.1")
    udp_port = int(cfg.get("udp_target_port", 5005))

    for slot in cfg.get("slots", []) or []:
        if not slot.get("active", False):
            continue
        sr = slot.get("sample_rate", 0)
        if isinstance(sr, dict):
            sr = sr.get("hz", 0.0) if sr.get("active", False) else 0.0
        sr = float(sr)
        if sr <= 0.0:
            continue

        slot_index = int(slot.get("slot_index", 0))
        for g in slot.get("channel_groups", []) or []:
            if not g.get("active", False):
                continue
            channels = [int(c) for c in g.get("channels", []) or []]
            if not channels:
                continue
            group_name = g.get("group_name", f"slot_{slot_index}")
            streams.append({
                "slot_index": slot_index,
                "group_name": group_name,
                "channels": channels,
                "sample_rate_hz": sr,
                "samples_per_channel": num_samples,
                "udp_ip": udp_ip,
                "udp_port": udp_port,
            })

    return streams, num_samples, udp_ip, udp_port


def synth_sample(ch: int, t: float) -> float:
    noise = lambda amp=0.2: random.uniform(-amp, amp)

    if ch == 0:
        return 5.0 * math.sin(2 * math.pi * 10.0 * t) + 2*noise()
    if ch == 1:
        return 8.0 * math.sin(2 * math.pi * 50.0 * t) + 2*noise()
    if ch == 2:
        return 2.5
    if ch == 3:
        return random.uniform(-1, 1)
    if ch == 4:
        return 1.5 * math.sin(2 * math.pi * 5.0 * t)
    if ch == 5:
        return -3.0 * math.sin(2 * math.pi * 1.0 * t)
    if ch == 6:
        return 5.0 * math.sin(2 * math.pi * 10.0 * t) + 2*noise()
    if ch == 7:
        return 8.0 * math.sin(2 * math.pi * 50.0 * t) + 2*noise()
    return 0.0


def encode_csv(seq: int, slot_index: int, group_name: str, samples_per_channel: int, raw_values: list[int]) -> bytes:
    parts = ["D", "1", str(seq), str(slot_index), group_name, str(samples_per_channel)]
    parts.extend(str(v) for v in raw_values)
    return ",".join(parts).encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description="UEIPAC CSV sender simulator")
    parser.add_argument("--config", default="UEI_DAQ_Settings.json", help="path to UEI_DAQ_Settings.json")
    parser.add_argument("--verbose", action="store_true", help="print every send")
    args = parser.parse_args()

    cfg = load_settings(args.config)
    streams, num_samples, cfg_ip, cfg_port = build_streams(cfg)
    if num_samples <= 0 or not streams:
        raise SystemExit("Invalid config: need numSamplesPerChannel > 0 and at least one active stream")

    # 固定使用本機 127.0.0.1:5005 以測試 receiver
    udp_ip = "127.0.0.1"
    udp_port = 5005

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"=== UEIPAC CSV Sender Simulator ===")
    print(f"Target: {udp_ip}:{udp_port}")
    print(f"Streams: {len(streams)}")
    for s in streams:
        print(f"  - slot {s['slot_index']} / {s['group_name']} ch={s['channels']} @ {s['sample_rate_hz']} Hz, samples={s['samples_per_channel']}")
    print("Format: D,1,<seq>,<slot>,<group>,<samples_per_channel>,<raw...>\n")

    seq = 0
    sim_t = 0.0
    try:
        while True:
            t_loop = time.time()
            for s in streams:
                dt = 1.0 / s["sample_rate_hz"]
                raw = []
                for _ in range(s["samples_per_channel"]):
                    for ch in s["channels"]:
                        val = synth_sample(ch, sim_t)
                        raw.append(vol_to_code(val))
                    sim_t += dt

                packet = encode_csv(seq, s["slot_index"], s["group_name"], s["samples_per_channel"], raw)
                sock.sendto(packet, (udp_ip, udp_port))
                if args.verbose:
                    print(f"Sent seq={seq} slot={s['slot_index']} group={s['group_name']} bytes={len(packet)}")
                seq += 1

            # pace roughly by shortest stream rate
            min_dt = min(1.0 / s["sample_rate_hz"] for s in streams)
            elapsed = time.time() - t_loop
            sleep_t = max(0.0, (s["samples_per_channel"] * min_dt) - elapsed)
            if sleep_t > 0:
                time.sleep(sleep_t)

    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
