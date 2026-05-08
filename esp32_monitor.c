#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched/loadavg.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/usb.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rapeephat Wannasamran");
MODULE_DESCRIPTION("ESP32 CPU Monitor - USB Event-Driven Detection");
MODULE_VERSION("4.1.0");

/* ==================== CONFIG ==================== */
#define SEND_INTERVAL_MS    1000
#define HANDSHAKE_MSG       "ESP32 Monitoring Device"
#define HANDSHAKE_ACK       "ACK\n"
#define OUTPUT_BUF_SIZE     64
#define HANDSHAKE_TIMEOUT   3000 /* 3 วินาที */
#define UDEV_WAIT_TIMEOUT   10000 /* รอ udev สร้าง node สูงสุด 10 วินาที */

/* ==================== USB ID TABLE ==================== */
static const struct usb_device_id esp32_usb_ids[] = {
    { USB_DEVICE(0x1a86, 0x7523) }, /* CH340  */
    { USB_DEVICE(0x10c4, 0xea60) }, /* CP2102 */
    { USB_DEVICE(0x1a86, 0x55d4) }, /* CH9102 / CH343 */
    { USB_DEVICE(0x303a, 0x1001) }, /* ESP32-S3 native USB */
    { }
};
MODULE_DEVICE_TABLE(usb, esp32_usb_ids);

/* ==================== STATE ==================== */
static struct task_struct  *monitor_thread = NULL;
static struct miscdevice    cpu_dev;
static char   active_port[32] = "";
static bool   esp32_connected  = false;
static DEFINE_MUTEX(esp32_lock);

/* ==================== HELPER ==================== */
static unsigned long get_cpu_load_percent(void)
{
    unsigned long load = (avenrun[0] * 100) / FIXED_1 / num_online_cpus();
    return min(load, 100UL);
}

/* ==================== KTHREAD MAIN LOOP ==================== */
static int esp32_monitor_kthread(void *data)
{
    struct file *tty = NULL;
    char port[32] = {0};
    int i, p, retry;
    bool found = false;
    bool handshake_ok = false;

    /* รูปแบบชื่อ TTY ที่เป็นไปได้ทั้งหมด รวมถึง CH343 */
    static const char *patterns[] = {
        "/dev/ttyACM%d",
        "/dev/ttyUSB%d",
        "/dev/ttyCH343USB%d",
        "/dev/ttyUSBCH343%d" /* เผื่อกรณีชื่อสลับ */
    };

    /* ---------------------------------------------------------
     * PHASE 1: SCAN & WAIT FOR UDEV
     * วนหา node สูงสุด 20 รอบ (รอบละ 500ms = 10 วินาที)
     * ---------------------------------------------------------*/
    pr_info("ESP32_MONITOR: Waiting for udev to create TTY node...\n");
    for (retry = 0; retry < (UDEV_WAIT_TIMEOUT / 500) && !kthread_should_stop(); retry++) {
        for (p = 0; p < ARRAY_SIZE(patterns) && !found; p++) {
            for (i = 0; i < 4 && !found; i++) {
                snprintf(port, sizeof(port), patterns[p], i);
                tty = filp_open(port, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
                if (!IS_ERR(tty)) {
                    filp_close(tty, NULL);
                    tty = NULL;
                    found = true;
                    break;
                }
            }
        }
        if (found) break;
        msleep_interruptible(500); 
    }

    if (!found || kthread_should_stop()) {
        pr_warn("ESP32_MONITOR: Failed to find TTY node (Timeout)\n");
        return 0; /* จบการทำงานของ Thread */
    }

    pr_info("ESP32_MONITOR: Node found at %s. Configuring stty...\n", port);

    /* หน่วงเวลาให้สิทธิ์ไฟล์ถูกตั้งค่าโดย udev อย่างสมบูรณ์ */
    msleep(500);

    /* ---------------------------------------------------------
     * PHASE 2: STTY SETUP
     * ---------------------------------------------------------*/
    {
        char *argv[] = { "/bin/stty", "-F", port, "115200", "-hupcl", NULL };
        char *envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
        call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
    }
    msleep(200); /* รอ stty คืนค่า */

    tty = filp_open(port, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
    if (IS_ERR(tty)) {
        pr_err("ESP32_MONITOR: Cannot open %s after stty setup\n", port);
        return 0;
    }

    /* ---------------------------------------------------------
     * PHASE 3: HANDSHAKE
     * ---------------------------------------------------------*/
    {
        char rxbuf[128] = {0};
        loff_t pos = 0;
        ssize_t n;
        int total = 0;
        int timeout_ticks = HANDSHAKE_TIMEOUT / 100;

        /* ส่ง ACK ไปก่อน */
        kernel_write(tty, HANDSHAKE_ACK, strlen(HANDSHAKE_ACK), &pos);
        pr_info("ESP32_MONITOR: Handshake ACK sent to %s. Waiting for reply...\n", port);

        /* รออ่านข้อความยืนยันจาก ESP32 */
        while (timeout_ticks-- > 0 && !kthread_should_stop()) {
            n = kernel_read(tty, rxbuf + total, sizeof(rxbuf) - 1 - total, &pos);
            if (n > 0) {
                total += n;
                rxbuf[total] = '\0'; /* ปิดสตริงกันเหนียว */
                if (strstr(rxbuf, HANDSHAKE_MSG)) {
                    handshake_ok = true;
                    break;
                }
            }
            msleep_interruptible(100);
        }
    }

    if (!handshake_ok) {
        pr_warn("ESP32_MONITOR: Handshake failed on %s\n", port);
        filp_close(tty, NULL);
        return 0;
    }

    pr_info("ESP32_MONITOR: Handshake Successful on %s!\n", port);

    /* อัปเดตสถานะ Global */
    mutex_lock(&esp32_lock);
    strscpy(active_port, port, sizeof(active_port));
    esp32_connected = true;
    mutex_unlock(&esp32_lock);

    /* ส่ง UEVENT ให้ Userspace รู้ */
    {
        char env_port[48];
        snprintf(env_port, sizeof(env_port), "ESP32_PORT=%s", port);
        char *envp[] = { "ESP32_STATUS=connected", env_port, NULL };
        kobject_uevent_env(&cpu_dev.this_device->kobj, KOBJ_CHANGE, envp);
    }

    /* ---------------------------------------------------------
     * PHASE 4: MAIN SEND LOOP
     * ---------------------------------------------------------*/
    {
        char buf[OUTPUT_BUF_SIZE];
        loff_t pos;
        int len;

        while (!kthread_should_stop()) {
            len = snprintf(buf, sizeof(buf), "%lu\n", get_cpu_load_percent());
            pos = 0;

            if (kernel_write(tty, buf, len, &pos) < 0) {
                pr_info("ESP32_MONITOR: Lost connection on %s (Write failed)\n", port);
                break;
            }
            msleep_interruptible(SEND_INTERVAL_MS);
        }
    }

    /* ---------------------------------------------------------
     * CLEANUP เมื่อ Thread จบหรือหลุด
     * ---------------------------------------------------------*/
    filp_close(tty, NULL);

    mutex_lock(&esp32_lock);
    esp32_connected = false;
    active_port[0]  = '\0';
    mutex_unlock(&esp32_lock);

    char *disc_envp[] = { "ESP32_STATUS=disconnected", NULL };
    kobject_uevent_env(&cpu_dev.this_device->kobj, KOBJ_CHANGE, disc_envp);

    pr_info("ESP32_MONITOR: Stopped monitoring %s\n", port);
    return 0;
}

/* ==================== USB PROBE / DISCONNECT ==================== */
static int esp32_usb_probe(struct usb_interface *intf,
                           const struct usb_device_id *id)
{
    int ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
    if (ifnum != 0)
        return -ENODEV;

    pr_info("ESP32_MONITOR: USB device plugged (VID=%04x PID=%04x)\n",
            id->idVendor, id->idProduct);

    mutex_lock(&esp32_lock);
    /* ถ้ามี Thread เก่าค้างอยู่ ให้หยุดก่อน */
    if (monitor_thread) {
        kthread_stop(monitor_thread);
        monitor_thread = NULL;
    }
    
    /* เริ่ม Kthread ตัวใหม่ */
    monitor_thread = kthread_run(esp32_monitor_kthread, NULL, "esp32_mon_th");
    mutex_unlock(&esp32_lock);

    return 0;
}

static void esp32_usb_disconnect(struct usb_interface *intf)
{
    pr_info("ESP32_MONITOR: USB device unplugged\n");

    mutex_lock(&esp32_lock);
    if (monitor_thread) {
        kthread_stop(monitor_thread); /* จะส่งสัญญาณ kthread_should_stop() ไปให้ thread ปิดตัวเอง */
        monitor_thread = NULL;
    }
    esp32_connected = false;
    active_port[0]  = '\0';
    mutex_unlock(&esp32_lock);

    char *envp[] = { "ESP32_STATUS=disconnected", NULL };
    kobject_uevent_env(&cpu_dev.this_device->kobj, KOBJ_CHANGE, envp);
}

static struct usb_driver esp32_usb_driver = {
    .name       = "esp32_monitor",
    .probe      = esp32_usb_probe,
    .disconnect = esp32_usb_disconnect,
    .id_table   = esp32_usb_ids,
};

/* ==================== SYSFS & MISC ==================== */
static ssize_t cpu_load_show(struct device *dev,
                              struct device_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%lu\n", get_cpu_load_percent());
}
DEVICE_ATTR_RO(cpu_load);

static ssize_t esp32_status_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    bool connected;
    char port[32];
    mutex_lock(&esp32_lock);
    connected = esp32_connected;
    strscpy(port, active_port[0] ? active_port : "none", sizeof(port));
    mutex_unlock(&esp32_lock);
    return sysfs_emit(buf, "%s %s\n", connected ? "connected" : "disconnected", port);
}
DEVICE_ATTR_RO(esp32_status);

static struct attribute *esp32_attrs[] = {
    &dev_attr_cpu_load.attr,
    &dev_attr_esp32_status.attr,
    NULL,
};
ATTRIBUTE_GROUPS(esp32);

static ssize_t cpu_read(struct file *file, char __user *buf,
                         size_t count, loff_t *ppos)
{
    char output[OUTPUT_BUF_SIZE];
    int len;

    if (*ppos > 0) return 0;

    len = snprintf(output, sizeof(output), "%lu\n", get_cpu_load_percent());
    if (copy_to_user(buf, output, len))
        return -EFAULT;

    *ppos += len;
    return len;
}

static const struct file_operations cpu_fops = {
    .owner = THIS_MODULE,
    .read  = cpu_read,
};

static struct miscdevice cpu_dev = {
    .minor  = MISC_DYNAMIC_MINOR,
    .name   = "esp32_monitor",
    .fops   = &cpu_fops,
    .groups = esp32_groups,
};

/* ==================== INIT / EXIT ==================== */
static int __init cpu_monitor_init(void)
{
    int ret;
    pr_info("========================================\n");
    pr_info("ESP32_MONITOR: Kernel Driver Loaded v4.1.0\n");
    pr_info("========================================\n");

    ret = misc_register(&cpu_dev);
    if (ret) return ret;

    ret = usb_register(&esp32_usb_driver);
    if (ret) {
        misc_deregister(&cpu_dev);
        return ret;
    }

    kobject_uevent(&cpu_dev.this_device->kobj, KOBJ_ADD);
    return 0;
}

static void __exit cpu_monitor_exit(void)
{
    /* หากตอนถอด Module ยังเสียบสายอยู่ ต้องสั่งหยุด Thread */
    mutex_lock(&esp32_lock);
    if (monitor_thread) {
        kthread_stop(monitor_thread);
        monitor_thread = NULL;
    }
    mutex_unlock(&esp32_lock);

    usb_deregister(&esp32_usb_driver);
    kobject_uevent(&cpu_dev.this_device->kobj, KOBJ_REMOVE);
    misc_deregister(&cpu_dev);

    pr_info("ESP32_MONITOR: Kernel Driver Unloaded\n");
}

module_init(cpu_monitor_init);
module_exit(cpu_monitor_exit);