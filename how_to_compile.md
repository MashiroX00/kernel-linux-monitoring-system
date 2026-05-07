# How to Compile & Run — ESP32 CPU Load Monitor

> Environment: **Ubuntu VM (VMware Workstation)** | Kernel: **6.17.13** | Arduino IDE: **2.x**

---
## 1. ความต้องการของระบบ

### Linux (Ubuntu VM)

| รายการ | รายละเอียด |
|--------|-----------|
| OS | Ubuntu 22.04 / 24.04 |
| Kernel | 6.17.13 (ต้องตรงกับที่รัน driver) |
| RAM | 4GB ขั้นต่ำ (แนะนำ 8GB+) |
| Disk | 20GB+ |
| Tools | `build-essential`, `linux-headers`, `git` |

### ESP32

| รายการ | รายละเอียด |
|--------|-----------|
| บอร์ด | ESP32 DevKit V1 |
| Arduino IDE | 2.x |
| Board Package | esp32 by Espressif Systems ≥ 2.0 |
| สาย | USB-A to Micro-USB (**data cable** — ไม่ใช่ charge-only) |
| CPU Frequency | 80 MHz (ตั้งใน firmware แล้ว) |

---

## 2. ตั้งค่า VMware USB Passthrough

> ⚠️ **ข้ามขั้นตอนนี้ไม่ได้** — ถ้าไม่ทำ VM จะไม่เห็น ESP32 เลยแม้เสียบสายแล้ว

### วิธีที่ 1: Connect แบบ Manual (ขณะ VM รันอยู่)

1. เสียบสาย USB ESP32 เข้าเครื่อง Windows
2. บน VMware menu bar: **VM → Removable Devices**
3. เลือก **QinHeng USB Single Serial** (หรือชื่ออุปกรณ์ที่ปรากฏ)
4. คลิก **Connect (Disconnect from Host)**

ตรวจสอบ:

```bash
lsusb | grep -i "QinHeng\|CH34\|Silicon\|CP21"
# ควรเห็น: ID 1a86:55d4 QinHeng Electronics USB Single Serial
```

### วิธีที่ 2: USB Filter (Auto-connect ทุกครั้ง — แนะนำ)

1. ปิด VM ก่อน
2. **VM → Settings → USB Controller → Add Filter**
3. เลือก ESP32 → Save → เปิด VM ใหม่

---

## 3. ตรวจสอบ USB-Serial Chip ของ ESP32

```bash
lsusb
```

เทียบ USB ID กับตารางนี้เพื่อดูว่าต้องติดตั้ง driver เพิ่มหรือไม่:

| USB ID | Chip | ต้องทำอะไรเพิ่ม | Device Node |
|--------|------|----------------|------------|
| `1a86:7523` | CH340 | ไม่ต้องทำอะไร | `/dev/ttyUSB0` |
| **`1a86:55d4`** | **CH343** | **→ ทำขั้นตอนที่ 4** | **`/dev/ttyCH343USB0`** |
| `10c4:ea60` | CP2102 | ไม่ต้องทำอะไร | `/dev/ttyUSB0` |
| `0403:6001` | FT232 | ไม่ต้องทำอะไร | `/dev/ttyUSB0` |

ถ้า USB ID **ไม่ใช่** `1a86:55d4` → ข้ามไปขั้นตอนที่ 5 ได้เลย

---

## 4. ติดตั้ง CH343 Driver (เฉพาะ chip `1a86:55d4`)

### วิธีเร็ว: ติดตั้ง module แยก

```bash
sudo apt install git build-essential linux-headers-$(uname -r)

git clone https://github.com/WCHSoftGroup/ch343ser_linux.git
cd ch343ser_linux
make
sudo make install
sudo modprobe ch343
```

ตรวจสอบ:

```bash
ls /dev/ttyCH343USB*
# ควรเห็น: /dev/ttyCH343USB0
```

### วิธี build เข้า Kernel Source (ถ้าจะ compile kernel ใหม่)

```bash
# 1. copy source เข้า kernel tree
git clone https://github.com/WCHSoftGroup/ch343ser_linux.git
cp ch343ser_linux/driver/ch343.c ~/kernel/linux-6.17.13/drivers/usb/serial/
cp ch343ser_linux/driver/ch343.h ~/kernel/linux-6.17.13/drivers/usb/serial/

# 2. เพิ่มใน Kconfig (ก่อน endmenu)
cat >> ~/kernel/linux-6.17.13/drivers/usb/serial/Kconfig << 'EOF'

config USB_SERIAL_CH343
	tristate "USB QinHeng CH343 Single Serial"
	depends on USB_SERIAL
	help
	  Driver for CH343/CH9102 USB serial devices. Module name: ch343.
EOF

# 3. เพิ่มใน Makefile
echo 'obj-$(CONFIG_USB_SERIAL_CH343)	+= ch343.o' \
  >> ~/kernel/linux-6.17.13/drivers/usb/serial/Makefile

# 4. เปิดใช้งาน
cd ~/kernel/linux-6.17.13
scripts/config --module CONFIG_USB_SERIAL_CH343
make olddefconfig

# ตรวจสอบ
grep CH343 .config
# ต้องเห็น: CONFIG_USB_SERIAL_CH343=m
```

---

## 5. คอมไพล์และติดตั้ง Kernel Driver

### Clone โปรเจค

```bash
git clone https://github.com/MashiroX00/kernel-linux-monitoring-system.git
cd kernel-linux-monitoring-system
```

### Option A: Out-of-tree Module (แนะนำ — ไม่ต้อง compile kernel ทั้งหมด)

```bash
# ติดตั้ง dependencies
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

# ตรวจสอบว่า headers ตรงกับ kernel ที่รันอยู่
uname -r
ls /lib/modules/$(uname -r)/build

# Build
make

# ตรวจสอบผลลัพธ์
ls -lh esp32_monitor.ko
modinfo esp32_monitor.ko
```

โหลด module:

```bash
sudo insmod esp32_monitor.ko

# ตรวจสอบ
dmesg | grep ESP32_MONITOR
# ควรเห็น: ESP32_MONITOR: Loaded, scanning for ESP32...

ls /dev/esp32_monitor
```

### Option B: Build เข้า Kernel Source (สำหรับ compile kernel ใหม่ทั้งหมด)

```bash
# ติดตั้ง dependencies ทั้งหมด
sudo apt install -y build-essential libncurses-dev libssl-dev libelf-dev \
  bison flex gcc make wget bc fakeroot dwarves zstd install-info \
  gawk debhelper libdw-dev git

# ดาวน์โหลด kernel source
mkdir -p ~/kernel && cd ~/kernel
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.17.13.tar.xz
tar -xf linux-6.17.13.tar.xz
cd linux-6.17.13

# 1. เพิ่ม ESP32 Monitor Driver
cp /path/to/kernel-linux-monitoring-system/esp32_monitor.c drivers/char/

cat >> drivers/char/Kconfig << 'EOF'
config ESP32_MONITOR
	bool "ESP32 CPU Load Monitor Support"
	default y
	help
	  Sends CPU load to ESP32 via Serial USB using handshake auto-detection.
EOF

echo 'obj-$(CONFIG_ESP32_MONITOR) += esp32_monitor.o' >> drivers/char/Makefile

# 2. เพิ่ม Calculator Driver (procfs)
cp /path/to/kernel-linux-monitoring-system/calculator.c drivers/misc/

cat >> drivers/misc/Kconfig << 'EOF'
config CALCULATOR
       bool "Calculator via /proc/calculator"
       depends on PROC_FS
       default y
       help
         Adds a simple /proc/calculator interface.
         Write "a op b" to compute, read back the result.
EOF

echo 'obj-$(CONFIG_CALCULATOR) += calculator.o' >> drivers/misc/Makefile

# ตั้งค่า kernel
cp /boot/config-$(uname -r) .config
scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS
scripts/config --disable DEBUG_INFO
scripts/config --disable CONFIG_DEBUG_INFO_BTF
scripts/config --module CONFIG_ESP32_MONITOR
scripts/config --enable CONFIG_CALCULATOR
make olddefconfig

# Compile (สร้างเป็น .deb)
make -j$(nproc) bindeb-pkg LOCALVERSION=-brc-monitoring
```

> ⚠️ ถ้า VM มี RAM 4GB และพัง (OOM) ระหว่าง link:
> ```bash
> # เพิ่ม swap ชั่วคราว
> sudo fallocate -l 4G /swapfile && sudo chmod 600 /swapfile
> sudo mkswap /swapfile && sudo swapon /swapfile
>
> # แล้ว compile ด้วย job น้อยลง
> make -j1 bindeb-pkg LOCALVERSION=-brc-monitoring
> ```

ติดตั้ง kernel ใหม่:

```bash
cd ~/kernel
sudo dpkg -i linux-image-*.deb linux-headers-*.deb
sudo update-grub
sudo reboot
```

หลัง reboot:

```bash
uname -r
# ควรเห็น: 6.17.13-brc-monitoring
```

---

## 6. อัปโหลด Firmware ลง ESP32

> ทำบน **Windows host** (ไม่ใช่ใน VM) เพราะ Arduino IDE ต้องการ COM port ของ Windows

### ติดตั้ง Board Package

1. เปิด **Arduino IDE 2.x**
2. **File → Preferences → Additional boards manager URLs:**
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Tools → Board → Boards Manager** → ค้นหา `esp32` → ติดตั้ง **esp32 by Espressif Systems**

### อัปโหลด

1. **Tools → Board → esp32 → ESP32 Dev Module**
2. **Tools → Port → เลือก COM port** ของ ESP32 (ดูใน Device Manager ถ้าไม่แน่ใจ)
3. เปิดไฟล์ `esp32/esp32.ino`
4. กด **Upload (→)**

> ถ้าขึ้น `Connecting...` ค้าง ให้กดปุ่ม **BOOT** บนบอร์ดค้างไว้จนกว่าจะ upload ได้

### ตรวจสอบ Firmware

เปิด **Serial Monitor** (baud rate **115200**) ควรเห็น:

```
ESP32 Monitoring Device
```

เมื่อ firmware พร้อมแล้ว **disconnect ESP32 ออกจาก Windows** แล้ว **connect เข้า VM** (ขั้นตอนที่ 2)

---

## 7. ทดสอบระบบ

### 7.1 ESP32 CPU Monitor

```bash
# ดู log แบบ real-time
sudo dmesg -w | grep ESP32_MONITOR

# ตรวจสอบ sysfs
cat /sys/class/misc/esp32_monitor/cpu_load
cat /sys/class/misc/esp32_monitor/esp32_status
```

### 7.2 Calculator Driver (procfs)

```bash
# บวกเลข (ต้องมีช่องว่างระหว่างตัวเลขและเครื่องหมาย)
echo "10 + 5" > /proc/calculator
cat /proc/calculator
# ผลลัพธ์: 15

# คูณเลข
echo "9 * 8" > /proc/calculator
cat /proc/calculator
# ผลลัพธ์: 72

# รองรับเครื่องหมาย: + - * / %
```

---

## 8. โหลด Module อัตโนมัติตอน Boot (กรณีใช้ Option A)

```bash
sudo cp esp32_monitor.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
echo "esp32_monitor" | sudo tee -a /etc/modules
```

---

## 9. ถอนการติดตั้ง

```bash
# ถอด module (Option A)
sudo rmmod esp32_monitor

# ล้างไฟล์ build
make clean
```

---

## 10. Troubleshooting

**`lsusb` ไม่เห็น ESP32 เลย**

VM ยังไม่ได้รับ USB passthrough → ทำขั้นตอนที่ 2 ก่อน

**มี device ใน `lsusb` แต่ไม่มี `/dev/tty*` โผล่**

```bash
lsusb                          # ดู USB ID
sudo modprobe ch343            # ถ้า 1a86:55d4
sudo modprobe ch341            # ถ้า 1a86:7523
sudo modprobe cp210x           # ถ้า 10c4:ea60
ls /dev/tty* | grep -E "USB|ACM|CH343"
```

**Driver scan วนซ้ำไม่หยุด ไม่เจอ ESP32**

```bash
# 1. ESP32 connect เข้า VM แล้วหรือยัง?
lsusb

# 2. device node ชื่ออะไร?
ls /dev/tty* | grep -E "USB|ACM|CH343"

# 3. firmware ส่ง handshake ออกมาไหม?
#    เปิด Arduino Serial Monitor → ควรเห็น "ESP32 Monitoring Device"

# 4. permission
sudo usermod -aG dialout $USER   # logout/login ใหม่หลังรัน
```

**`insmod` ล้มเหลว: Invalid module format**

```bash
uname -r
modinfo esp32_monitor.ko | grep vermagic
# ต้องตรงกัน — ถ้าไม่ตรงต้อง build ใหม่ให้ตรงกับ kernel ที่รันอยู่
```

**OOM ระหว่าง compile kernel**

```bash
sudo fallocate -l 4G /swapfile
sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile
make -j1 bindeb-pkg LOCALVERSION=-brc-monitoring
```
