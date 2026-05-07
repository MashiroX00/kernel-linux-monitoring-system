// calculator_proc.c — Linux Kernel Module via procfs
// Interface: /proc/calculator
//
// Usage:
//   echo "10 + 5"  > /proc/calculator && cat /proc/calculator  → 15
//   echo "9 * 8"   > /proc/calculator && cat /proc/calculator  → 72
//   echo "10 / 3"  > /proc/calculator && cat /proc/calculator  → 3
//   echo "10 % 3"  > /proc/calculator && cat /proc/calculator  → 1
//   echo "5 - 99"  > /proc/calculator && cat /proc/calculator  → -94
// Supported operators: + - * / %

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>      // proc_create, proc_remove
#include <linux/uaccess.h>      // copy_from_user, copy_to_user
#include <linux/seq_file.h>     // seq_file (optional, used for clean read)
#include <linux/string.h>

#define PROC_NAME   "calculator"
#define BUF_SIZE    128

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("procfs-based /proc/calculator kernel module");
MODULE_VERSION("1.0");

static char result_buf[BUF_SIZE];
static size_t result_len = 0;

/* ── arithmetic ──────────────────────────────────────────────── */

static void do_calculate(const char *expr)
{
    long a = 0, b = 0, res = 0;
    char op = 0;

    if (sscanf(expr, "%ld %c %ld", &a, &op, &b) != 3) {
        snprintf(result_buf, BUF_SIZE,
                 "ERROR: bad format — use: <num> <op> <num>\n");
        result_len = strlen(result_buf);
        return;
    }

    switch (op) {
    case '+': res = a + b; break;
    case '-': res = a - b; break;
    case '*': res = a * b; break;
    case '/':
        if (b == 0) {
            snprintf(result_buf, BUF_SIZE, "ERROR: division by zero\n");
            result_len = strlen(result_buf);
            return;
        }
        res = a / b;
        break;
    case '%':
        if (b == 0) {
            snprintf(result_buf, BUF_SIZE, "ERROR: modulo by zero\n");
            result_len = strlen(result_buf);
            return;
        }
        res = a % b;
        break;
    default:
        snprintf(result_buf, BUF_SIZE,
                 "ERROR: unknown operator '%c' — use + - * / %%\n", op);
        result_len = strlen(result_buf);
        return;
    }

    snprintf(result_buf, BUF_SIZE, "%ld\n", res);
    result_len = strlen(result_buf);
    pr_info("calculator: %ld %c %ld = %ld\n", a, op, b, res);
}

/* ── proc read (cat /proc/calculator) ───────────────────────── */

static ssize_t calc_read(struct file *file, char __user *ubuf,
                          size_t count, loff_t *ppos)
{
    if (*ppos >= result_len)
        return 0;   /* EOF */

    if (count > result_len - *ppos)
        count = result_len - *ppos;

    if (copy_to_user(ubuf, result_buf + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

/* ── proc write (echo "..." > /proc/calculator) ─────────────── */

static ssize_t calc_write(struct file *file, const char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    char input[BUF_SIZE];

    if (count >= BUF_SIZE)
        count = BUF_SIZE - 1;

    if (copy_from_user(input, ubuf, count))
        return -EFAULT;

    input[count] = '\0';

    /* strip trailing newline added by echo */
    if (count > 0 && input[count - 1] == '\n')
        input[count - 1] = '\0';

    do_calculate(input);
    return count;
}

/* ── proc_ops (kernel ≥ 5.6) ────────────────────────────────── */

static const struct proc_ops calc_proc_ops = {
    .proc_read  = calc_read,
    .proc_write = calc_write,
};

/* ── module init / exit ──────────────────────────────────────── */

static struct proc_dir_entry *proc_entry;

static int __init calculator_init(void)
{
    /*
     * proc_create(name, mode, parent, proc_ops)
     *   name   — filename under /proc
     *   0666   — rw for all (use 0644 to make it root-write-only)
     *   NULL   — place directly under /proc
     */
    proc_entry = proc_create(PROC_NAME, 0666, NULL, &calc_proc_ops);
    if (!proc_entry) {
        pr_err("calculator: failed to create /proc/%s\n", PROC_NAME);
        return -ENOMEM;
    }

    /* seed a friendly default result */
    snprintf(result_buf, BUF_SIZE, "0\n");
    result_len = strlen(result_buf);

    pr_info("calculator: /proc/%s created\n", PROC_NAME);
    return 0;
}

static void __exit calculator_exit(void)
{
    proc_remove(proc_entry);
    pr_info("calculator: /proc/%s removed\n", PROC_NAME);
}

module_init(calculator_init);
module_exit(calculator_exit);