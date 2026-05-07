# ESP32 CPU Load Monitor — Linux Kernel Driver

> **Version:** 3.0.0  
> **License:** GPL  

ระบบ **Linux Kernel Driver** ที่ดึงค่า CPU Load Average จาก Kernel Space และส่งข้อมูลไปยังบอร์ด **ESP32** ผ่าน Serial USB แบบอัตโนมัติ รองรับ Plug and Play เต็มรูปแบบ — ไม่ต้องตั้งค่า udev rules เอง ไม่ต้องระบุพอร์ตตายตัว และ reconnect อัตโนมัติเมื่อถอด-เสียบสาย

---

## ภาพรวมระบบ

```
[ Linux Kernel ]
      │  avenrun[] — Load Average (normalized by CPU count)
      ▼
[ esp32_monitor.ko ]  ← Kernel Module (misc device + kthread)
      │
      │  Auto-scan: ttyUSB / ttyCH343USB / ttyACM (0..7)
      │  Handshake: "ESP32 Monitoring Device"
      │
      ▼
[ /dev/ttyCH343USB0 ]  ── Serial USB @ 115200 baud
      │
      ▼
[ ESP32 DevKit V1 ]  ← FreeRTOS (SerialTask + LEDTask)
      │
      │  map(cpuLoad, 1, 99, 1000ms, 50ms)
      ▼
[ LED Pin 2 — กะพริบตามโหลด CPU ]
```

---

## โครงสร้างโปรเจค

```
kernel-linux-monitoring-system/
├── esp32_monitor.c       # Linux Kernel Driver (C)
├── calculator.c          # Calculator Kernel Driver (procfs)
├── Makefile              # Build scriptสำหรับ out-of-tree module
├── esp32/
│   └── esp32.ino         # ESP32 Firmware (Arduino + FreeRTOS)
├── how_to_compile.md     # คู่มือคอมไพล์แบบละเอียด
├── how_to_run.md         # คู่มือการรัน Calculator Driver
└── README.md
```

---

## จุดเด่น

- **Zero-config PnP** — ไม่ต้องสร้าง `/etc/udev/rules.d/` เอง sysfs attributes พร้อมให้ udev อ่านได้ทันที
- **Auto-detect port** — scan `ttyUSB`, `ttyCH343USB`, และ `ttyACM` ครอบคลุม USB-Serial chip ทุกรุ่น
- **Handshake-based identification** — ยืนยันตัวตน ESP32 ด้วย challenge/response ก่อนส่งข้อมูล ป้องกันส่งข้อมูลไปยัง device ผิดตัว
- **Auto-reconnect** — ถอดแล้วเสียบ USB ใหม่ → kthread detect และเชื่อมต่อใหม่อัตโนมัติ
- **FreeRTOS firmware** — ESP32 ใช้ `SerialTask` และ `LEDTask` แยกกัน ป้องกัน heap fragmentation และ watchdog reset
- **Dual read interface** — อ่านค่า CPU load ได้ทั้งจาก `/dev/esp32_monitor` และ sysfs

---

## เครื่องคิดเลขผ่าน procfs (Calculator Driver)

เพิ่มความสามารถให้ Kernel รองรับการคำนวณพื้นฐานผ่าน `/proc/calculator` โดยรองรับเครื่องหมาย `+`, `-`, `*`, `/`, และ `%`

### การใช้งานเบื้องต้น
```bash
echo "10 + 5" > /proc/calculator
cat /proc/calculator
# ผลลัพธ์: 15
```
ดูรายละเอียดการติดตั้งใน `how_to_compile.md` (ข้อ 11)

---

## USB-Serial Chip ที่รองรับ

| USB ID | Chip | Kernel Module | Device Node |
|--------|------|--------------|-------------|
| `1a86:7523` | CH340 / CH341 | `ch341` (built-in) | `/dev/ttyUSB0` |
| `1a86:55d4` | **CH343 / CH9102** | **`ch343` (ต้องติดตั้งเพิ่ม)** | **`/dev/ttyCH343USB0`** |
| `10c4:ea60` | CP2102 | `cp210x` (built-in) | `/dev/ttyUSB0` |
| `2341:0043` | CDC ACM | `cdc_acm` (built-in) | `/dev/ttyACM0` |

> ⚠️ **ESP32 DevKit V1 บางล็อต** ใช้ chip **CH343** (USB ID `1a86:55d4`) ซึ่ง `ch341` module ไม่รองรับ device node จะเป็น `/dev/ttyCH343USB0` แทน `/dev/ttyUSB0` ดูวิธีติดตั้งได้ใน `how_to_compile.md`

---

## Handshake Protocol

```
Linux Driver (kthread)                ESP32 (SerialTask)
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
|------|-------|----------|--------|
| `SerialTask` | 2048 bytes | 1 | รับข้อมูลจาก Serial, parse CPU load, ตอบ handshake |
| `LEDTask` | 1024 bytes | 1 | ควบคุม LED ตาม `cpuLoad` + `blinkInterval` |

การใช้ `char buffer[32]` แทน `String` ป้องกัน heap fragmentation บน ESP32 และ `vTaskDelay()` แทน `delay()` ป้องกัน Watchdog Timer reset

---

## พฤติกรรม LED

| CPU Load | พฤติกรรม | blinkInterval |
|----------|----------|--------------|
| 0% | ดับ (LOW) | — |
| 1–99% | กะพริบ ยิ่งโหลดสูงยิ่งเร็ว | 1000ms → 50ms |
| 100% | ติดค้าง (HIGH) | — |

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
|---------|---------------|-------------|
| Handshake สำเร็จ | `connected` | เช่น `/dev/ttyCH343USB0` |
| ถอด / write error | `disconnected` | — |

```bash
# Monitor uevent แบบ real-time
udevadm monitor --environment
```

---

*Final Project — CS422 Operating System*
