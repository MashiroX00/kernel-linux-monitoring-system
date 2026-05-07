# ESP32 CPU Load Monitor
> **Version:** 3.0.0  
> **License:** GPL  

ระบบ Kernel Driver สำหรับ Linux ที่ดึงค่า CPU Load จาก Kernel Space และส่งไปยังบอร์ด ESP32 ผ่าน Serial USB แบบอัตโนมัติ รองรับ **Plug and Play เต็มรูปแบบ** — ไม่ต้องตั้งค่า udev rules เอง และ detect พอร์ต ESP32 ได้เองด้วย Handshake Protocol

---

## ภาพรวมระบบ

```
[ Linux Kernel ]
      │  avenrun[] — Load Average
      ▼
[ esp32_monitor.ko ]  ← Kernel Module
      │  Auto-scan ttyUSB0..7
      │  Handshake: "ESP32 Monitoring Device"
      ▼
[ /dev/ttyUSB0 ]  ── Serial USB 115200 baud
      │
      ▼
[ ESP32 DevKit ]
      │  map(cpuLoad, 1, 99, 1000ms, 50ms)
      ▼
[ LED กะพริบตามโหลด CPU ]
```

---

## โครงสร้างโปรเจค

```
esp32_monitor/
├── esp32_monitor.c       # Linux Kernel Driver
├── esp32/
│   └── esp32.ino         # Arduino firmware สำหรับ ESP32
└── README.md
```

---

## จุดเด่น

- **Zero-config PnP** — ไม่ต้องสร้าง `/etc/udev/rules.d/` เอง udev อ่าน sysfs ได้โดยตรง
- **Auto-detect port** — scan `ttyUSB0`–`ttyUSB7` และยืนยันตัวตนด้วย Handshake Protocol
- **Auto-reconnect** — ถอดแล้วเสียบ USB ใหม่ → driver detect และเชื่อมต่อใหม่อัตโนมัติ
- **Dual interface** — อ่านค่าได้ทั้งจาก `/dev/esp32_monitor` และ sysfs attributes

---

## Handshake Protocol

ระบบไม่กำหนดพอร์ตตายตัว แต่ใช้การ "ถามตอบ" เพื่อยืนยันว่าอุปกรณ์ที่เสียบคือ ESP32 จริง

```
Linux Driver                      ESP32
     │                              │
     │ ── "ACK\n" ────────────────► │
     │                              │ (รับ ACK → ตอบกลับ)
     │ ◄──── "ESP32 Monitoring Device\n" ──
     │                              │
     │  ✅ Handshake สำเร็จ         │
     │ ── "CPU:42\n" ─────────────► │
     │ ── "CPU:38\n" ─────────────► │  (ทุก 1 วินาที)
```

---

## API Reference

### `/dev/esp32_monitor`

```bash
cat /dev/esp32_monitor
# output: 42
```

### sysfs Attributes

```bash
# CPU Load (normalize ตามจำนวน core แล้ว)
cat /sys/class/misc/esp32_monitor/cpu_load

# สถานะการเชื่อมต่อ + พอร์ตที่ใช้งาน
cat /sys/class/misc/esp32_monitor/esp32_status
# connected /dev/ttyUSB0
# disconnected none
```

### UEVENT Environment Variables

| Event | Variable | ค่า |
|-------|----------|-----|
| เสียบ ESP32 | `ESP32_STATUS` | `connected` |
| เสียบ ESP32 | `ESP32_PORT` | เช่น `/dev/ttyUSB0` |
| ถอด ESP32 | `ESP32_STATUS` | `disconnected` |

---

## พฤติกรรม LED บน ESP32

| CPU Load | พฤติกรรม LED |
|----------|--------------|
| 0% | ดับ |
| 1–99% | กะพริบ (ยิ่งโหลดสูง ยิ่งกะพริบเร็ว) |
| 100% | ติดค้าง |

---

*โปรเจคนี้เป็นส่วนหนึ่งของ Final Project วิชา CS422 — Linux Kernel Module & Embedded System Integration*