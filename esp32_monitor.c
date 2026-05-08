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
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rapeephat Wannasamran");
MODULE_DESCRIPTION("ESP32 CPU Monitor - USB Event-Driven Detection");
MODULE_VERSION("4.0.0");

/* ==================== CONFIG ==================== */
#define SEND_INTERVAL_MS    1000
#define HANDSHAKE_MSG       "ESP32 Monitoring Device"
#define HANDSHAKE_ACK       "ACK\n"
#define OUTPUT_BUF_SIZE     64
#define HANDSHAKE_TIMEOUT   3000

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
static struct task_struct  *monitor_thread;
static struct miscdevice    cpu_dev;
static struct work_struct   handshake_work;
static char   active_port[32] = "";
static bool   esp32_connected  = false;
static DEFINE_MUTEX(esp32_lock);

/* ==================== HELPER ==================== */
static unsigned long get_cpu_load_percent(void)
{
    unsigned long load = (avenrun[0] * 100) / FIXED_1 / num_online_cpus();
    return min(load, 100UL);
}

/* ==================== HANDSHAKE ==================== */
static struct file *try_handshake(const char *port)
{
    struct file *tty;
    char rxbuf[128] = {0};  /* เพิ่มขนาดเผื่อข้อมูลมาหลายชิ้น */
    loff_t pos = 0;
    ssize_t n;
    int timeout = HANDSHAKE_TIMEOUT / 100;
    int total = 0;

    char *argv[] = { "/bin/stty", "-F", (char *)port, "115200", "-hupcl", NULL };
    char *envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
    call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);

    tty = filp_open(port, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
    if (IS_ERR(tty))
        return NULL;

    pos = 0;
    kernel_write(tty, HANDSHAKE_ACK, strlen(HANDSHAKE_ACK), &pos);

    /* Accumulate ข้อมูลจนกว่าจะเจอ handshake message */
    while (timeout-- > 0) {
        msleep(100);
        n = kernel_read(tty, rxbuf + total,
                        sizeof(rxbuf) - 1 - total, &pos);
        if (n > 0) {
            total += n;
            if (strstr(rxbuf, HANDSHAKE_MSG)) {
                pr_info("ESP32_MONITOR: Handshake OK on %s\n", port);
                return tty;
            }
        }
    }

    filp_close(tty, NULL);
    return NULL;
}

/* ==================== HANDSHAKE WORK ==================== */
/* ทำงานใน workqueue — ไม่บล็อก usb probe */
static void handshake_work_fn(struct work_struct *work)
{
    struct file *tty;
    char port[32];

    mutex_lock(&esp32_lock);
    strscpy(port, active_port, sizeof(port));
    mutex_unlock(&esp32_lock);

    if (!port[0])
        return;

    pr_info("ESP32_MONITOR: Starting handshake on %s\n", port);

    tty = try_handshake(port);
    if (!tty) {
        pr_warn("ESP32_MONITOR: Handshake failed on %s\n", port);
        mutex_lock(&esp32_lock);
        active_port[0]  = '\0';
        esp32_connected = false;
        mutex_unlock(&esp32_lock);
        return;
    }

    mutex_lock(&esp32_lock);
    esp32_connected = true;
    mutex_unlock(&esp32_lock);

    /* ส่ง uevent แจ้ง userspace */
    char env_port[48];
    snprintf(env_port, sizeof(env_port), "ESP32_PORT=%s", port);
    char *envp[] = { "ESP32_STATUS=connected", env_port, NULL };
    kobject_uevent_env(&cpu_dev.this_device->kobj, KOBJ_CHANGE, envp);

    /* ==================== SEND LOOP ==================== */
    char buf[OUTPUT_BUF_SIZE];
    loff_t pos;
    int len;

    while (!kthread_should_stop()) {
        len = snprintf(buf, sizeof(buf), "%lu\n", get_cpu_load_percent());
        pos = 0;

        if (kernel_write(tty, buf, len, &pos) < 0) {
            pr_info("ESP32_MONITOR: Lost connection on %s\n", port);
            break;
        }
        msleep_interruptible(SEND_INTERVAL_MS);
    }

    filp_close(tty, NULL);

    mutex_lock(&esp32_lock);
    esp32_connected = false;
    active_port[0]  = '\0';
    mutex_unlock(&esp32_lock);

    char *disc_envp[] = { "ESP32_STATUS=disconnected", NULL };
    kobject_uevent_env(&cpu_dev.this_device->kobj, KOBJ_CHANGE, disc_envp);

    pr_info("ESP32_MONITOR: Disconnected from %s\n", port);
}

/* ==================== USB PROBE / DISCONNECT ==================== */
static int esp32_usb_probe(struct usb_interface *intf,
                           const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    int ifnum = intf->cur_altsetting->desc.bInterfaceNumber;

    /* รับเฉพาะ interface 0 เพื่อไม่ให้ probe ซ้ำ */
    if (ifnum != 0)
        return -ENODEV;

    /* สร้าง tty path จาก minor number */
    char port[32];
    int minor = udev->devnum;
    snprintf(port, sizeof(port), "/dev/ttyUSB%d", minor);

    pr_info("ESP32_MONITOR: USB device detected (VID=%04x PID=%04x) on %s\n",
            id->idVendor, id->idProduct, port);

    mutex_lock(&esp32_lock);
    strscpy(active_port, port, sizeof(active_port));
    mutex_unlock(&esp32_lock);

    /* ทำ handshake แบบ async ไม่บล็อก probe */
    schedule_work(&handshake_work);

    return 0;
}

static void esp32_usb_disconnect(struct usb_interface *intf)
{
    pr_info("ESP32_MONITOR: USB device removed\n");

    mutex_lock(&esp32_lock);
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

/* ==================== SYSFS ==================== */
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
    return sysfs_emit(buf, "%s %s\n",
                      connected ? "connected" : "disconnected", port);
}
DEVICE_ATTR_RO(esp32_status);

static struct attribute *esp32_attrs[] = {
    &dev_attr_cpu_load.attr,
    &dev_attr_esp32_status.attr,
    NULL,
};
ATTRIBUTE_GROUPS(esp32);

/* ==================== /dev/esp32_monitor ==================== */
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

    /* ข้อความแจ้งตอน boot */
    pr_info("========================================\n");
    pr_info("ESP32_MONITOR: Kernel Driver Loaded\n");
    pr_info("ESP32_MONITOR: Version 4.0.0\n");
    pr_info("ESP32_MONITOR: Author - Rapeephat Wannasamran\n");
    pr_info("ESP32_MONITOR: Waiting for ESP32 USB device...\n");
    pr_info("========================================\n");

    INIT_WORK(&handshake_work, handshake_work_fn);

    ret = misc_register(&cpu_dev);
    if (ret) {
        pr_err("ESP32_MONITOR: Failed to register misc device (%d)\n", ret);
        return ret;
    }

    ret = usb_register(&esp32_usb_driver);
    if (ret) {
        pr_err("ESP32_MONITOR: Failed to register USB driver (%d)\n", ret);
        misc_deregister(&cpu_dev);
        return ret;
    }

    kobject_uevent(&cpu_dev.this_device->kobj, KOBJ_ADD);
    return 0;
}

static void __exit cpu_monitor_exit(void)
{
    cancel_work_sync(&handshake_work);
    usb_deregister(&esp32_usb_driver);
    kobject_uevent(&cpu_dev.this_device->kobj, KOBJ_REMOVE);
    misc_deregister(&cpu_dev);

    pr_info("ESP32_MONITOR: Kernel Driver Unloaded\n");
}

module_init(cpu_monitor_init);
module_exit(cpu_monitor_exit);