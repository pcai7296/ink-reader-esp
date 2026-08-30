#!/usr/bin/env python3
# 后台串口采集: 持续从 COM 读原始字节写到日志文件。分析时 grep ABSAM/H/FSREQ/WDT/Exception。
# 用法: python capture_serial.py COM20 115200 ab_a_capture.log
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM20"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
out  = sys.argv[3] if len(sys.argv) > 3 else "ab_a_capture.log"

ser = serial.Serial(port, baud, timeout=0.2)
print(f"LISTENING {port}@{baud} -> {out}")
with open(out, "w", encoding="utf-8", errors="replace") as f:
    f.write(f"# capture start {time.strftime('%H:%M:%S')}\n")
    f.flush()
    while True:
        try:
            n = ser.in_waiting
            if n:
                data = ser.read(n)
                f.write(data.decode("utf-8", errors="replace"))
                f.flush()
        except Exception as e:
            print("capture err", e)
            break
        time.sleep(0.01)
