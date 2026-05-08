# ESP32 CPU Load Monitor — Linux Kernel Driver

> **Version:** 4.0.0  
> **License:** GPL  
> **Author:** Rapeephat Wannasamran and BRC Group
> **Course:** CS422 Operating System

ระบบ **Linux Kernel Driver** ที่ดึงค่า CPU Load Average จาก Kernel Space และส่งข้อมูลไปยังบอร์ด **ESP32** ผ่าน Serial USB แบบอัตโนมัติ รองรับ Plug and Play เต็มรูปแบบ — ตรวจจับ USB device ผ่าน kernel event โดยตรง ไม่ต้องสแกน port เอง และ reconnect อัตโนมัติเมื่อถอด-เสียบสาย

---

## ภาพรวมระบบ

```
[ Linux Kernel ]
      │  avenrun[] — Load Average (normalized by CPU count)
      ▼
[ esp32_monitor.ko ]  ← Kernel Module (USB Driver + workqueue)
      │
      │  USB probe event (VID/PID matching — ไม่ scan)
      │  Handshake: ACK → "ESP32 Monitoring Device"
      │
      ▼
[ /dev/ttyCH343USB0 ]  ── Serial USB @ 115200 baud
      │
      ▼
[ ESP32 DevKit V1 ]  ← FreeRTOS (SerialTask + LEDTask)
      │
      │  map(cpuLoad, 1, 99, 1000ms, 50ms)
      ▼
[ Traffic LED Board ]
  LED2 — แสดงโหลด 0–49%
  LED3 — แสดงโหลด 50–100%
```

---

## โครงสร้างโปรเจค

```
kernel-linux-monitoring-system/
├── esp32_monitor.c       # Linux Kernel Driver v4.0.0 (USB event-driven)
├── calculator.c          # Calculator Kernel Driver (procfs)
├── Makefile              # Build script สำหรับ out-of-tree module
├── esp32/
│   └── esp32.ino         # ESP32 Firmware (Arduino + FreeRTOS)
├── how_to_compile.md     # คู่มือคอมไพล์แบบละเอียด
└── README.md
```

---

## จุดเด่น

- **Event-driven USB detection** — ใช้ `usb_driver.probe` แทนการ scan port วนซ้ำ ประหยัด CPU และตอบสนองทันทีที่เสียบสาย
- **Zero idle CPU** — ไม่มี polling loop ขณะรอ ESP32 kernel รู้จาก USB event โดยตรง
- **Instant disconnect detection** — รู้ทันทีเมื่อถอดสายผ่าน `usb_driver.disconnect` ไม่ต้องรอให้ `write()` fail
- **Async handshake** — `handshake_work_fn` ทำงานใน workqueue ไม่บล็อก USB probe
- **Accumulate buffer** — รับข้อมูล serial แบบ multi-chunk ป้องกัน handshake พลาดเมื่อข้อมูลมาหลายรอบ
- **Boot message** — พิมพ์ข้อความยืนยันลง `dmesg` ทันทีที่ module ถูกโหลด
- **FreeRTOS firmware** — ESP32 ใช้ `SerialTask` และ `LEDTask` แยกกัน ป้องกัน heap fragmentation และ watchdog reset
- **Dual read interface** — อ่านค่า CPU load ได้ทั้งจาก `/dev/esp32_monitor` และ sysfs

---

## USB-Serial Chip ที่รองรับ

| USB ID | Chip | Kernel Module | Device Node |
|--------|------|---------------|-------------|
| `1a86:7523` | CH340 / CH341 | `ch341` (built-in) | `/dev/ttyUSB0` |
| `1a86:55d4` | **CH343 / CH9102** | **`ch343` (ต้องติดตั้งเพิ่ม)** | **`/dev/ttyCH343USB0`** |
| `10c4:ea60` | CP2102 | `cp210x` (built-in) | `/dev/ttyUSB0` |
| `303a:1001` | ESP32-S3 native USB | `cdc_acm` (built-in) | `/dev/ttyACM0` |

> ⚠️ **ESP32 DevKit V1 บางล็อต** ใช้ chip **CH343** (USB ID `1a86:55d4`) ซึ่ง `ch341` ไม่รองรับ  
> device node จะเป็น `/dev/ttyCH343USB0` แทน `/dev/ttyUSB0` ดูวิธีติดตั้งได้ใน `how_to_compile.md`

---

## Handshake Protocol

```
Linux Driver (workqueue)              ESP32 (SerialTask)
        │                                    │
        │ ── "ACK\n" ───────────────────────► │
        │                                    │  strcmp(buffer, "ACK") == 0
        │ ◄─────── "ESP32 Monitoring Device\n" ──
        │                                    │
        │  strstr(rxbuf, HANDSHAKE_MSG) ✅   │
        │                                    │
        │ ── "42\n" (CPU load %) ───────────► │  atoi(buffer) → cpuLoad
        │ ── "38\n" ────────────────────────► │  blinkInterval = map(...)
        │       (ทุก 1000ms)                  │
```

---

## ESP32 Firmware Architecture

firmware ใช้ **FreeRTOS** แบ่งการทำงานออกเป็น 2 tasks:

| Task | Stack | Priority | หน้าที่ |
|------|-------|----------|---------|
| `SerialTask` | 2048 bytes | 1 | รับข้อมูลจาก Serial, parse CPU load, ตอบ handshake |
| `LEDTask` | 1024 bytes | 1 | ควบคุม LED 2 ดวงตาม `cpuLoad` และ `blinkInterval` |

การใช้ `char buffer[32]` แทน `String` ป้องกัน heap fragmentation บน ESP32 และ `vTaskDelay()` แทน `delay()` ป้องกัน Watchdog Timer reset

---

## พฤติกรรม LED (Traffic LED Board)

บอร์ด LED มี 3 ดวง (ขา 1, 2, 3, G) โดยระบบใช้ **ดวงที่ 2 และ 3** ตาม logic ดังนี้:

| CPU Load | LED 2 | LED 3 |
|----------|-------|-------|
| 0% | ดับ | ดับ |
| 1–49% | กระพริบ 1000ms→50ms | ดับ |
| 50–99% | ติดค้าง | กระพริบ 1000ms→50ms |
| 100% | ติดค้าง | ติดค้าง |

`blinkInterval` คำนวณจาก `map(cpuLoad, 1, 99, 1000, 50)` — ยิ่งโหลดสูงยิ่งกระพริบเร็ว

---

## Boot Message (dmesg)

เมื่อโหลด module จะเห็นใน `dmesg`:

```
[    2.341] ========================================
[    2.341] ESP32_MONITOR: Kernel Driver Loaded
[    2.341] ESP32_MONITOR: Version 4.0.0
[    2.341] ESP32_MONITOR: Author - Rapeephat Wannasamran
[    2.341] ESP32_MONITOR: Waiting for ESP32 USB device...
[    2.341] ========================================
[   15.892] ESP32_MONITOR: USB device detected (VID=1a86 PID=55d4) on /dev/ttyCH343USB0
[   15.994] ESP32_MONITOR: Handshake OK on /dev/ttyCH343USB0
```

ดู log แบบ real-time:
```bash
sudo dmesg -w | grep ESP32_MONITOR
```

---

## API Reference

### `/dev/esp32_monitor`

อ่านค่า CPU load ปัจจุบัน (0–100%):

```bash
cat /dev/esp32_monitor
# output: 42
```

### sysfs Attributes

```bash
# CPU Load (normalize ด้วย num_online_cpus() แล้ว)
cat /sys/class/misc/esp32_monitor/cpu_load

# สถานะ ESP32 + พอร์ตที่กำลังใช้งาน
cat /sys/class/misc/esp32_monitor/esp32_status
# connected /dev/ttyCH343USB0
# disconnected none
```

### UEVENT Environment Variables

| Trigger | `ESP32_STATUS` | `ESP32_PORT` |
|---------|----------------|--------------|
| Handshake สำเร็จ | `connected` | เช่น `/dev/ttyCH343USB0` |
| ถอด / disconnect | `disconnected` | — |

```bash
# Monitor uevent แบบ real-time
udevadm monitor --environment
```

---

## เครื่องคิดเลขผ่าน procfs (Calculator Driver)

เพิ่มความสามารถให้ Kernel รองรับการคำนวณพื้นฐานผ่าน `/proc/calculator` รองรับเครื่องหมาย `+`, `-`, `*`, `/`, และ `%`

```bash
echo "10 + 5" > /proc/calculator
cat /proc/calculator
# ผลลัพธ์: 15
```

ดูรายละเอียดการติดตั้งใน `how_to_compile.md`

---

## Quick Start

```bash
# 1. Clone
git clone https://github.com/MashiroX00/kernel-linux-monitoring-system.git
cd kernel-linux-monitoring-system

# 2. Build
sudo apt install -y build-essential linux-headers-$(uname -r)
make

# 3. Load
sudo insmod esp32_monitor.ko

# 4. ตรวจสอบ
dmesg | grep ESP32_MONITOR
cat /sys/class/misc/esp32_monitor/esp32_status
```

ดูคู่มือฉบับเต็มที่ [how_to_compile.md](./how_to_compile.md)