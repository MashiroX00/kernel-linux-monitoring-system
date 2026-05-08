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
#include <linux/notifier.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rapeephat Wannasamran");
MODULE_DESCRIPTION("ESP32 CPU Monitor - notifier based, no USB bind conflict");
MODULE_VERSION("4.4.0");

#define SEND_INTERVAL_MS    1000
#define HANDSHAKE_MSG       "ESP32 Monitoring Device"
#define HANDSHAKE_ACK       "ACK\n"
#define OUTPUT_BUF_SIZE     64
#define HANDSHAKE_TIMEOUT   3000
#define UDEV_WAIT_MS        8000   /* รอ cdc_acm สร้าง node สูงสุด 8 วินาที */

#define ESP32_VID  0x1a86
#define ESP32_PID  0x55d4

/* ==================== STATE ==================== */
static struct miscdevice    cpu_dev;
static struct task_struct  *monitor_thread = NULL;
static char   active_port[32] = "";
static bool   esp32_connected  = false;
static bool   esp32_present    = false;   /* USB เสียบอยู่ไหม */
static DEFINE_MUTEX(esp32_lock);

/* ==================== HELPER ==================== */
static unsigned long get_cpu_load_percent(void)
{
    unsigned long load = (avenrun[0] * 100) / FIXED_1 / num_online_cpus();
    return min(load, 100UL);
}

/* ==================== KTHREAD ==================== */
static int esp32_monitor_kthread(void *data)
{
    struct file *tty = NULL;
    char port[32] = {0};
    int i, p, retry;
    bool found = false;

    static const char *patterns[] = {
        "/dev/ttyACM%d",
        "/dev/ttyUSB%d",
        "/dev/ttyCH343USB%d",
    };

    pr_info("ESP32_MONITOR: Thread started, waiting for tty node...\n");

    /* รอให้ cdc_acm หรือ driver อื่นสร้าง node ก่อน */
    for (retry = 0; retry < (UDEV_WAIT_MS / 200) && !kthread_should_stop(); retry++) {
        for (p = 0; p < ARRAY_SIZE(patterns) && !found; p++) {
            for (i = 0; i < 8 && !found; i++) {
                struct file *f;
                snprintf(port, sizeof(port), patterns[p], i);
                f = filp_open(port, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
                if (!IS_ERR(f)) {
                    filp_close(f, NULL);
                    found = true;
                }
            }
        }
        if (found) break;
        msleep_interruptible(200);
    }

    if (!found || kthread_should_stop()) {
        pr_warn("ESP32_MONITOR: No tty node found after %dms\n", UDEV_WAIT_MS);
        goto out;
    }

    pr_info("ESP32_MONITOR: Found tty node: %s\n", port);

    /* ตั้งค่า baud rate */
    {
        char *argv[] = { "/bin/stty", "-F", port, "115200", "-hupcl", NULL };
        char *envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
        call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
        msleep(300);
    }

    tty = filp_open(port, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
    if (IS_ERR(tty)) {
        pr_err("ESP32_MONITOR: Cannot open %s after stty\n", port);
        goto out;
    }

    /* Handshake */
    {
        char rxbuf[128] = {0};
        loff_t pos = 0;
        ssize_t n;
        int total = 0;
        int ticks = HANDSHAKE_TIMEOUT / 100;

        kernel_write(tty, HANDSHAKE_ACK, strlen(HANDSHAKE_ACK), &pos);

        while (ticks-- > 0 && !kthread_should_stop()) {
            msleep_interruptible(100);
            n = kernel_read(tty, rxbuf + total,
                            sizeof(rxbuf) - 1 - total, &pos);
            if (n > 0) {
                total += n;
                if (strstr(rxbuf, HANDSHAKE_MSG)) {
                    pr_info("ESP32_MONITOR: Handshake OK on %s\n", port);
                    goto handshake_ok;
                }
            }
        }

        pr_warn("ESP32_MONITOR: Handshake failed on %s\n", port);
        filp_close(tty, NULL);
        goto out;
    }

handshake_ok:
    mutex_lock(&esp32_lock);
    strscpy(active_port, port, sizeof(active_port));
    esp32_connected = true;
    mutex_unlock(&esp32_lock);

    {
        char env_port[48];
        snprintf(env_port, sizeof(env_port), "ESP32_PORT=%s", port);
        char *envp[] = { "ESP32_STATUS=connected", env_port, NULL };
        kobject_uevent_env(&cpu_dev.this_device->kobj, KOBJ_CHANGE, envp);
    }

    /* Send loop */
    while (!kthread_should_stop()) {
        char buf[OUTPUT_BUF_SIZE];
        loff_t wpos = 0;
        int len = snprintf(buf, sizeof(buf), "%lu\n", get_cpu_load_percent());

        if (kernel_write(tty, buf, len, &wpos) < 0) {
            pr_info("ESP32_MONITOR: Lost connection on %s\n", port);
            break;
        }
        msleep_interruptible(SEND_INTERVAL_MS);
    }

    filp_close(tty, NULL);

out:
    mutex_lock(&esp32_lock);
    esp32_connected = false;
    active_port[0]  = '\0';
    monitor_thread  = NULL;
    mutex_unlock(&esp32_lock);

    {
        char *envp[] = { "ESP32_STATUS=disconnected", NULL };
        kobject_uevent_env(&cpu_dev.this_device->kobj, KOBJ_CHANGE, envp);
    }
    return 0;
}

/* ==================== USB NOTIFIER (ไม่ bind — แค่สังเกต) ==================== */
static int esp32_usb_notify(struct notifier_block *nb,
                             unsigned long action, void *data)
{
    struct usb_device *udev = data;

    if (udev->descriptor.idVendor  != ESP32_VID ||
        udev->descriptor.idProduct != ESP32_PID)
        return NOTIFY_DONE;

    if (action == USB_DEVICE_ADD) {
        pr_info("ESP32_MONITOR: ESP32 plugged in (VID=%04x PID=%04x)\n",
                ESP32_VID, ESP32_PID);

        mutex_lock(&esp32_lock);
        esp32_present = true;

        /* หยุด thread เก่าถ้ายังค้างอยู่ */
        if (monitor_thread && !IS_ERR(monitor_thread)) {
            kthread_stop(monitor_thread);
            monitor_thread = NULL;
        }

        monitor_thread = kthread_run(esp32_monitor_kthread,
                                     NULL, "esp32_mon");
        mutex_unlock(&esp32_lock);

    } else if (action == USB_DEVICE_REMOVE) {
        pr_info("ESP32_MONITOR: ESP32 removed\n");

        mutex_lock(&esp32_lock);
        esp32_present   = false;
        esp32_connected = false;
        active_port[0]  = '\0';

        if (monitor_thread && !IS_ERR(monitor_thread)) {
            kthread_stop(monitor_thread);
            monitor_thread = NULL;
        }
        mutex_unlock(&esp32_lock);
    }

    return NOTIFY_OK;
}

static struct notifier_block esp32_nb = {
    .notifier_call = esp32_usb_notify,
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
    bool conn;
    char port[32];
    mutex_lock(&esp32_lock);
    conn = esp32_connected;
    strscpy(port, active_port[0] ? active_port : "none", sizeof(port));
    mutex_unlock(&esp32_lock);
    return sysfs_emit(buf, "%s %s\n", conn ? "connected" : "disconnected", port);
}
DEVICE_ATTR_RO(esp32_status);

static struct attribute *esp32_attrs[] = {
    &dev_attr_cpu_load.attr,
    &dev_attr_esp32_status.attr,
    NULL,
};
ATTRIBUTE_GROUPS(esp32);

static const struct file_operations cpu_fops = { .owner = THIS_MODULE };

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

    ret = misc_register(&cpu_dev);
    if (ret) return ret;

    usb_register_notify(&esp32_nb);

    pr_info("========================================\n");
    pr_info("ESP32_MONITOR: Kernel Driver Loaded\n");
    pr_info("ESP32_MONITOR: Version 4.3.0\n");
    pr_info("ESP32_MONITOR: Author - Rapeephat Wannasamran\n");
    pr_info("ESP32_MONITOR: Waiting for ESP32 USB device...\n");
    pr_info("========================================\n");

    return 0;
}

static void __exit cpu_monitor_exit(void)
{
    usb_unregister_notify(&esp32_nb);

    mutex_lock(&esp32_lock);
    if (monitor_thread && !IS_ERR(monitor_thread)) {
        kthread_stop(monitor_thread);
        monitor_thread = NULL;
    }
    mutex_unlock(&esp32_lock);

    misc_deregister(&cpu_dev);
    pr_info("ESP32_MONITOR: Driver Unloaded\n");
}

module_init(cpu_monitor_init);
module_exit(cpu_monitor_exit);