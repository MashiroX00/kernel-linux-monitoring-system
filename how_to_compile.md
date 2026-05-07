# How to Compile & Install — ESP32 CPU Load Monitor

> Environment: **Ubuntu VM** | Kernel: **6.17.13** | Arduino IDE: **2.x**

## 1. ความต้องการของระบบ

### Linux (Ubuntu VM)

| รายการ | รายละเอียด |
|--------|-----------|
| OS | Ubuntu 22.04 / 24.04 |
| Kernel | 6.17.13 (ต้องตรงกับที่รัน driver) |
| RAM | 4GB ขั้นต่ำ (แนะนำ 8GB+) |
| Disk | 20GB+ สำหรับ kernel headers |
| Tools | `build-essential`, `linux-headers` |

### ESP32

| รายการ | รายละเอียด |
|--------|-----------|
| บอร์ด | ESP32 DevKit V1 |
| Arduino IDE | 2.x |
| Board Package | esp32 by Espressif Systems ≥ 2.0 |
| สาย | USB-A to Micro-USB (data cable) |

---

## 2. คอมไพล์ Kernel Driver (`esp32_monitor.c`)

### ขั้นตอนที่ 1 — ติดตั้ง dependencies

```bash
sudo apt update
sudo apt install -y build-essential libncurses-dev libssl-dev libelf-dev bison flex gcc make wget bc fakeroot dwarves zstd install-info gawk debhelper libdw-dev
```


### ขั้นตอนที่ 2 — ดาวน์โหลดและแตกไฟล์ Kernel

สร้างไฟล์ `Makefile` ในโฟลเดอร์เดียวกับ `esp32_monitor.c`:

```bash
mkdir -p ~/kernel && cd ~/kernel
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.17.13.tar.xz
tar -xf linux-6.17.13.tar.xz
cd linux-6.17.13
```
###  ขั้นตอนที่ 3 — แทรกโค้ด Driver

1. นำไฟล์ esp32_monitor.c (ที่คุณมี) ไปวางใน drivers/char/  
2. แก้ไขไฟล์ drivers/char/Kconfig โดยเพิ่มโค้ดนี้ต่อท้าย:
```Kconfig
config ESP32_MONITOR
    tristate "ESP32 CPU Load Monitor Support"
    default y
```

3. แก้ไขไฟล์ drivers/char/Makefile โดยเพิ่มโค้ดนี้ต่อท้าย:  
```makefile
obj-$(CONFIG_ESP32_MONITOR) += esp32_monitor.o
```

> ⚠️ **สำคัญ:** บรรทัด `make -C ...` ต้องใช้ **Tab** ไม่ใช่ Space

### ขั้นตอนที่ 4 — ตั้งค่า Kernel

```bash
# ดึง config ปัจจุบันมาใช้
cp /boot/config-$(uname -r) .config

# ปิดระบบความปลอดภัยและระบบ Debug เพื่อประหยัด RAM/Disk
scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS
scripts/config --disable DEBUG_INFO
scripts/config --disable CONFIG_DEBUG_INFO_BTF

# เปิดใช้งาน Driver ของเรา
scripts/config --enable CONFIG_ESP32_MONITOR

# ยืนยันการตั้งค่า
make olddefconfig
```

### ขั้นตอนที่ 5 — คอมไพล์และสร้างไฟล์ติดตั้ง

```bash
make -j12 bindeb-pkg LOCALVERSION=-brc-monitoring
```

ถ้า VM มี RAM จำกัด (4GB) ให้จำกัด parallel jobs เพื่อป้องกัน OOM:  

```bash
make -j1
```

ผลลัพธ์ที่ควรได้:

```
  CC [M]  /path/to/esp32_monitor.o
  MODPOST /path/to/Module.symvers
  CC [M]  /path/to/esp32_monitor.mod.o
  LD [M]  /path/to/esp32_monitor.ko
```

ตรวจสอบไฟล์ที่ได้:

```bash
ls -lh esp32_monitor.ko
modinfo esp32_monitor.ko
```

### ขั้นตอนที่ 6 — ติดตั้ง Kernel ใหม่

```bash
cd ~/kernel
sudo dpkg -i linux-image-*.deb linux-headers-*.deb
sudo update-grub
sudo reboot
```

ตรวจสอบว่าโหลดสำเร็จ:

```bash
dmesg | grep ESP32_MONITOR
# ควรเห็น:
# ESP32_MONITOR: Loaded, scanning for ESP32...
```

ตรวจสอบว่า device ถูกสร้าง:

```bash
ls -la /dev/esp32_monitor
cat /sys/class/misc/esp32_monitor/esp32_status
```

---

## 3. อัปโหลด Firmware ESP32 (`esp32.ino`)

### ขั้นตอนที่ 1 — ติดตั้ง Board Package

1. เปิด Arduino IDE 2.x
2. ไปที่ **File → Preferences**
3. ใส่ URL ต่อไปนี้ใน **Additional boards manager URLs:**
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. ไปที่ **Tools → Board → Boards Manager**
5. ค้นหา `esp32` และติดตั้ง **esp32 by Espressif Systems**

### ขั้นตอนที่ 2 — เลือกบอร์ดและพอร์ต

1. **Tools → Board → esp32 → ESP32 Dev Module**
2. **Tools → Port → เลือก COM port** (Windows) หรือ `/dev/ttyUSB0` (Linux/Mac)

> ถ้าไม่เห็น port ให้ตรวจสอบสาย USB และ driver ของ CP210x หรือ CH340

### ขั้นตอนที่ 3 — เปิดและอัปโหลด

1. เปิดไฟล์ `esp32/esp32.ino` ใน Arduino IDE
2. กด **Verify (✓)** เพื่อตรวจสอบโค้ดก่อน
3. กด **Upload (→)** เพื่ออัปโหลด

ถ้า Upload ล้มเหลวให้กดปุ่ม **BOOT** บนบอร์ดค้างไว้ระหว่างที่ขึ้น `Connecting...`

### ขั้นตอนที่ 4 — ตรวจสอบ Firmware

เปิด **Serial Monitor** (Tools → Serial Monitor) ตั้ง baud rate เป็น **115200** ควรเห็น:

```
ESP32 Monitoring Device
```

ถ้าเห็น output นี้แสดงว่า firmware พร้อมแล้ว

---

## 4. ทดสอบระบบ

### เสียบ ESP32 และดู log

```bash
# เปิด terminal และ monitor log แบบ real-time
dmesg -w | grep ESP32_MONITOR
```

เมื่อเสียบสาย USB ควรเห็น:

```
ESP32_MONITOR: Scanning /dev/ttyUSB0...
ESP32_MONITOR: Handshake OK on /dev/ttyUSB0
ESP32_MONITOR: ESP32 connected on /dev/ttyUSB0
```

### ตรวจสอบ sysfs

```bash
# ดูค่า CPU Load
cat /sys/class/misc/esp32_monitor/cpu_load

# ดูสถานะและพอร์ตที่เชื่อมต่อ
cat /sys/class/misc/esp32_monitor/esp32_status
# output: connected /dev/ttyUSB0
```

### ทดสอบอ่านค่าจาก device

```bash
cat /dev/esp32_monitor
# output: 42
```

### Monitor UEVENT

```bash
udevadm monitor --environment
# ดู event ที่ driver ส่งออกมาเมื่อ ESP32 เสียบ/ถอด
```

### ทดสอบ Reconnect

ถอดสาย USB แล้วเสียบใหม่ — ดู log ควรเห็น driver scan และ reconnect ใหม่อัตโนมัติ

---

## 5. โหลด Module อัตโนมัติตอน Boot

```bash
# copy module เข้า kernel modules directory
sudo cp esp32_monitor.ko /lib/modules/$(uname -r)/extra/

# อัปเดต module dependency
sudo depmod -a

# เพิ่มให้โหลดอัตโนมัติ
echo "esp32_monitor" | sudo tee -a /etc/modules

# ทดสอบโหลดผ่าน modprobe
sudo modprobe esp32_monitor
```

---

## 6. ถอนการติดตั้ง

### ถอด Module ชั่วคราว

```bash
sudo rmmod esp32_monitor
dmesg | grep ESP32_MONITOR
# ควรเห็น: ESP32_MONITOR: Unloaded
```

### ลบออกจาก Boot (ถ้าตั้งค่าไว้)

```bash
sudo nano /etc/modules
# ลบบรรทัด esp32_monitor ออก

sudo rm /lib/modules/$(uname -r)/extra/esp32_monitor.ko
sudo depmod -a
```

### ล้างไฟล์ที่ build

```bash
cd esp32_monitor/
make clean
```

---

## Troubleshooting

**`make` ล้มเหลว: No rule to make target**

ตรวจสอบว่า `linux-headers` ของ kernel ที่รันอยู่ติดตั้งแล้ว:
```bash
apt list --installed | grep linux-headers
```

**`insmod` ล้มเหลว: Invalid module format**

kernel version ของ module ไม่ตรงกับที่รันอยู่:
```bash
uname -r
modinfo esp32_monitor.ko | grep vermagic
# ต้องตรงกัน
```

**ไม่เห็น `/dev/ttyUSB0` หลังเสียบ ESP32**

ตรวจสอบ driver USB-to-Serial:
```bash
lsusb
dmesg | tail -20
sudo usermod -aG dialout $USER  # แล้ว logout/login ใหม่
```

**ESP32 ไม่ตอบสนอง Handshake**

เปิด Serial Monitor ใน Arduino IDE ตรวจสอบว่า ESP32 ส่ง `ESP32 Monitoring Device` ออกมา และตั้ง baud rate ตรงเป็น **115200**