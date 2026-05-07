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
#include <linux/umh.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rapeephat Wannasamran");
MODULE_DESCRIPTION("ESP32 CPU Monitor - Auto USB Detection via Handshake");
MODULE_VERSION("3.0.0");

/* ==================== CONFIG ==================== */
#define SEND_INTERVAL_MS    1000
#define HANDSHAKE_MSG       "ESP32 Monitoring Device"
#define HANDSHAKE_ACK       "ACK\n"
#define MAX_TTY_SCAN        8        /* scan ttyUSB0 - ttyUSB7 */
#define OUTPUT_BUF_SIZE     64
#define HANDSHAKE_TIMEOUT   3000     /* รอ handshake 3 วินาที */

static const char *tty_patterns[] = {
    "/dev/ttyUSB%d",       /* CH340, CP210x */
    "/dev/ttyCH343USB%d",  /* CH343, CH9102 */
    "/dev/ttyACM%d",       /* ESP32-S3      */
};
/* ==================== STATE ==================== */
static struct task_struct *monitor_thread;
static struct miscdevice   cpu_dev;
static char  active_port[32] = "";   /* เก็บ port ที่ใช้งานอยู่ เช่น "/dev/ttyUSB0" */
static bool  esp32_connected  = false;
static DEFINE_MUTEX(esp32_lock);

/* ==================== HELPER ==================== */
static unsigned long get_cpu_load_percent(void)
{
    unsigned long load = (avenrun[0] * 100) / FIXED_1 / num_online_cpus();
    return min(load, 100UL);
}

/* ==================== HANDSHAKE ====================
 * เปิด ttyUSB port → ส่ง ACK → รอรับ "ESP32 Monitoring Device"
 * return: file* ถ้าสำเร็จ, NULL ถ้าไม่ใช่ ESP32
 */
static struct file *try_handshake(const char *port)
{
    struct file *tty;
    char rxbuf[64];
    loff_t pos = 0;
    ssize_t n;
    int timeout = HANDSHAKE_TIMEOUT / 100;
    char *argv[] = { "/bin/stty", "-F", (char *)port, "115200", "-hupcl", NULL };
    char *envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
    call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
    
    tty = filp_open(port, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
    if (IS_ERR(tty))
        return NULL;

    /* ส่ง ACK เพื่อกระตุ้นให้ ESP32 ตอบ */
    pos = 0;
    kernel_write(tty, HANDSHAKE_ACK, strlen(HANDSHAKE_ACK), &pos);

    /* รอรับ handshake message จาก ESP32 */
    while (timeout-- > 0) {
        msleep(100);
        memset(rxbuf, 0, sizeof(rxbuf));
        pos = 0;
        n = kernel_read(tty, rxbuf, sizeof(rxbuf) - 1, &pos);
        if (n > 0 && strstr(rxbuf, HANDSHAKE_MSG)) {
            pr_info("ESP32_MONITOR: Handshake OK on %s\n", port);
            return tty;   /* ✅ พบ ESP32 */
        }
    }

    /* ไม่ใช่ ESP32 → ปิดและข้ามไป */
    filp_close(tty, NULL);
    return NULL;
}

/* ==================== AUTO SCAN ====================
 * วน scan tty_patterns หา ESP32 ด้วย handshake
 * return: file* ของ port ที่ใช่, NULL ถ้าไม่เจอ
 */
static struct file *scan_for_esp32(void)
{
    char port[32];
    struct file *tty;
    int i, p;

    for (p = 0; p < ARRAY_SIZE(tty_patterns); p++) {
        for (i = 0; i < MAX_TTY_SCAN; i++) {
            snprintf(port, sizeof(port), tty_patterns[p], i);
            pr_info("ESP32_MONITOR: Scanning %s...\n", port);

            tty = try_handshake(port);
            if (tty) {
                mutex_lock(&esp32_lock);
                strscpy(active_port, port, sizeof(active_port));
                esp32_connected = true;
                mutex_unlock(&esp32_lock);

                char env_port[48];
                snprintf(env_port, sizeof(env_port), "ESP32_PORT=%s", port);
                char *envp[] = { "ESP32_STATUS=connected", env_port, NULL };
                kobject_uevent_env(&cpu_dev.this_device->kobj,
                                   KOBJ_CHANGE, envp);
                return tty;
            }
        }
    }
    return NULL;
}

/* ==================== MONITOR THREAD ==================== */
static int monitor_thread_fn(void *data)
{
    struct file *tty = NULL;
    char buf[OUTPUT_BUF_SIZE];
    loff_t pos;
    int len;

    pr_info("ESP32_MONITOR: Monitor thread started\n");

    while (!kthread_should_stop()) {

        /* ถ้ายังไม่ได้ connect → scan หา ESP32 */
        if (!tty) {
            tty = scan_for_esp32();
            if (!tty) {
                /* ไม่เจอ ESP32 เลย → รอแล้วลองใหม่ */
                msleep_interruptible(2000);
                continue;
            }
        }

        /* ส่งค่า CPU load ไปยัง ESP32 */
        len = snprintf(buf, sizeof(buf), "%lu\n", get_cpu_load_percent());
        pos = 0;
        if (kernel_write(tty, buf, len, &pos) < 0) {
            /* เขียนไม่ได้ = ESP32 ถูกถอดออก */
            pr_info("ESP32_MONITOR: Lost connection on %s\n", active_port);
            filp_close(tty, NULL);
            tty = NULL;

            mutex_lock(&esp32_lock);
            esp32_connected = false;
            active_port[0] = '\0';
            mutex_unlock(&esp32_lock);

            char *envp[] = { "ESP32_STATUS=disconnected", NULL };
            kobject_uevent_env(&cpu_dev.this_device->kobj, KOBJ_CHANGE, envp);
            continue;   /* กลับไป scan ใหม่ทันที */
        }

        msleep_interruptible(SEND_INTERVAL_MS);
    }

    if (tty && !IS_ERR(tty))
        filp_close(tty, NULL);

    pr_info("ESP32_MONITOR: Monitor thread stopped\n");
    return 0;
}

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
    return sysfs_emit(buf, "%s %s\n", connected ? "connected" : "disconnected", port);
}
DEVICE_ATTR_RO(esp32_status);

static struct attribute *esp32_attrs[] = {
    &dev_attr_cpu_load.attr,
    &dev_attr_esp32_status.attr,
    NULL,
};
ATTRIBUTE_GROUPS(esp32);

/* ==================== /dev/esp32_monitor READ ==================== */
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

    ret = misc_register(&cpu_dev);
    if (ret) {
        pr_err("ESP32_MONITOR: Failed to register device (%d)\n", ret);
        return ret;
    }

    monitor_thread = kthread_run(monitor_thread_fn, NULL, "esp32_monitor");
    if (IS_ERR(monitor_thread)) {
        pr_err("ESP32_MONITOR: Failed to start thread\n");
        misc_deregister(&cpu_dev);
        return PTR_ERR(monitor_thread);
    }

    kobject_uevent(&cpu_dev.this_device->kobj, KOBJ_ADD);
    pr_info("ESP32_MONITOR: Loaded, scanning for ESP32...\n");
    return 0;
}

static void __exit cpu_monitor_exit(void)
{
    if (monitor_thread)
        kthread_stop(monitor_thread);

    kobject_uevent(&cpu_dev.this_device->kobj, KOBJ_REMOVE);
    misc_deregister(&cpu_dev);
    pr_info("ESP32_MONITOR: Unloaded\n");
}

module_init(cpu_monitor_init);
module_exit(cpu_monitor_exit);