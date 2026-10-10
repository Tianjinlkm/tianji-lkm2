// SPDX-License-Identifier: GPL-2.0
/*
 * TianjiLKM v4.4 - pearl seven-layer anti-brick protection (reviewed).
 *
 * Fixes from v4.3:
 *  - printk moved out of kprobe pre_handler into a workqueue.
 *    pre_handler now does zero printk / no sleeping operations.
 *    Events are logged to ring buffer, workqueue prints them later.
 *
 * L1: Syscall  - reboot/poweroff monitoring
 * L2: VFS       - filp_open on /dev/block write-open monitoring
 * L3: Block     - blkdev_issue_discard monitoring
 * L4: Block     - blkdev_issue_zeroout monitoring
 * L5: BIO       - submit_bio write BIO monitoring (monitor only, never block)
 * L6: Policy    - partition major whitelist + trusted comm filter
 * L7: Telemetry - event ring buffer + procfs status
 */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/uidgid.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>

#include "config_device.h"

#define TJ_PROC_DIR     "tianji_lkm"
#define TJ_PROC_STATUS  "status"
#define TJ_STATUS_MAX   1024
#define TJ_CMD_MAX      32

MODULE_LICENSE("GPL");
MODULE_AUTHOR("@zxshijk");
MODULE_DESCRIPTION("TianjiLKM pearl seven-layer anti-brick v4.4");
MODULE_VERSION("4.6");

/* ===== Global state ===== */
static struct proc_dir_entry *tj_dir;
static struct proc_dir_entry *tj_status;
static struct proc_dir_entry *tj_ctrl;
static DEFINE_MUTEX(tj_lock);

/*
 * Operation mode.
 * 0 = MONITOR (default): hooks fire, record events, no blocking.
 * 1 = PROTECT: EXPERIMENTAL blocking enabled for L1-L4.
 *
 * Use READ_ONCE/WRITE_ONCE because pre_handler runs without locks.
 */
static atomic_t tj_protect_mode = ATOMIC_INIT(0);

/* kprobe registration success bits. */
enum {
    TJ_KP_L1_REBOOT = 0,
    TJ_KP_L2_FILP_OPEN,
    TJ_KP_L3_DISCARD,
    TJ_KP_L4_ZEROOUT,
    TJ_KP_L5_SUBMIT_BIO,
    TJ_KP_MAX
};
static unsigned long tj_kp_registered;

/* ===== Layer 7: Event log ring buffer ===== */
struct tj_event {
    u32 layer;
    u32 pid;
    u32 uid;
    u32 major;
    u32 minor;
    char comm[16];
    char action[24];
    bool blocked;
};
static struct tj_event tj_ring[TJ_EVENT_LOG_SIZE];
static u32 tj_ring_head;
static DEFINE_SPINLOCK(tj_ring_lock);

/* Per-layer hit counters (atomic for lock-free reads in pre_handler). */
static atomic64_t tj_count_l1 = ATOMIC64_INIT(0);
static atomic64_t tj_count_l2 = ATOMIC64_INIT(0);
static atomic64_t tj_count_l3 = ATOMIC64_INIT(0);
static atomic64_t tj_count_l4 = ATOMIC64_INIT(0);
static atomic64_t tj_count_l5 = ATOMIC64_INIT(0);
static atomic64_t tj_blocked_total = ATOMIC64_INIT(0);

/*
 * Workqueue for deferred printk.
 * kprobe pre_handler must not call printk directly (lock/reentry risk).
 * We record events in the ring buffer, then schedule work to print them later.
 */
static struct work_struct tj_log_work;
static atomic_t tj_log_pending = ATOMIC_INIT(0);

static void tj_log_event(u32 layer, u32 major, u32 minor,
                        const char *action, bool blocked)
{
    unsigned long flags;
    struct tj_event *e;

    spin_lock_irqsave(&tj_ring_lock, flags);
    e = &tj_ring[tj_ring_head % TJ_EVENT_LOG_SIZE];
    e->layer = layer;
    e->pid = current->pid;
    e->uid = from_kuid(&init_user_ns, current_uid());
    e->major = major;
    e->minor = minor;
    get_task_comm(e->comm, current);
    strscpy(e->action, action, sizeof(e->action));
    e->blocked = blocked;
    tj_ring_head++;
    spin_unlock_irqrestore(&tj_ring_lock, flags);

    if (blocked)
        atomic64_inc(&tj_blocked_total);
}

/*
 * Deferred printk work. Runs in process context (can sleep).
 * Prints recent blocked events from the ring buffer.
 */
static void tj_log_work_fn(struct work_struct *w)
{
    unsigned long flags;
    u32 i, start;
    int printed = 0;

    atomic_set(&tj_log_pending, 0);

    spin_lock_irqsave(&tj_ring_lock, flags);
    start = (tj_ring_head > 4) ? (tj_ring_head - 4) : 0;
    for (i = start; i < tj_ring_head && printed < 4; i++) {
        struct tj_event *e = &tj_ring[i % TJ_EVENT_LOG_SIZE];
        if (!e->blocked)
            continue;
        pr_warn("tianji_lkm: L%u blocked pid=%u comm=%s dev=%u:%u action=%s\n",
                e->layer, e->pid, e->comm, e->major, e->minor, e->action);
        printed++;
    }
    spin_unlock_irqrestore(&tj_ring_lock, flags);
}

/* Schedule log work from pre_handler. Non-blocking, safe. */
static inline void tj_schedule_log(void)
{
    if (atomic_cmpxchg(&tj_log_pending, 0, 1) == 0)
        schedule_work(&tj_log_work);
}

/* ===== Layer 6: Policy helpers ===== */
static bool tj_is_trusted_comm(void)
{
    char comm[16];
    get_task_comm(comm, current);

    if (!strncmp(comm, TJ_TRUSTED_COMM_VOLD, 16) ||
        !strncmp(comm, TJ_TRUSTED_COMM_RECOVERY, 16) ||
        !strncmp(comm, TJ_TRUSTED_COMM_UPDATE, 16) ||
        !strncmp(comm, TJ_TRUSTED_COMM_INIT, 16))
        return true;
    return false;
}

static bool tj_is_protected_major(u32 major)
{
    static const u32 majors[] = { 8, 259 };
    int i;
    for (i = 0; i < ARRAY_SIZE(majors); i++) {
        if (major == majors[i])
            return true;
    }
    return false;
}

static bool tj_is_boot_partition(u32 major, u32 minor)
{
    /* Only the two eMMC boot hardware partitions are exempt. */
    if (major == TJ_BOOT_MAJOR &&
        (minor == TJ_BOOT_MINOR_0 || minor == TJ_BOOT_MINOR_1))
        return true;
    return false;
}

/*
 * EXPERIMENTAL arm64 syscall/function interception.
 *
 * WARNING: This modifies pt_regs to skip the original function.
 * It is NOT verified on pearl. Keep protect mode OFF by default.
 *
 * On arm64: x30 = LR (return address). We set x0 = -EPERM and
 * jump to LR to return immediately.
 */
static nokprobe_inline void tj_block_call(struct pt_regs *regs)
{
    regs->regs[0] = -EPERM;
    regs->pc = regs->regs[30];
}

/*
 * ===== L1: Syscall layer =====
 * Hook __arm64_sys_reboot.
 *
 * arm64 syscall wrapper ABI (CRITICAL):
 *   long __arm64_sys_reboot(struct pt_regs *regs);
 *
 * This function takes ONE argument: a pointer to the user-mode pt_regs
 * saved at syscall entry. It is NOT a normal C function with 4 args.
 *
 * Inside kprobe pre_handler:
 *   regs          = current CPU register state at function entry
 *   regs->regs[0] = the function's only argument = struct pt_regs *user_regs
 *
 * The actual reboot syscall arguments live inside user_regs:
 *   user_regs->regs[0] = magic1
 *   user_regs->regs[1] = magic2
 *   user_regs->regs[2] = cmd
 *   user_regs->regs[3] = arg
 *
 * To block: set regs->regs[0] = -EPERM (function return value) and
 * jump to LR. The syscall return path will propagate this to user_regs.
 */
static int tj_l1_reboot_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs *user_regs;
    u32 cmd;
    bool dangerous = false;

    atomic64_inc(&tj_count_l1);

    /* First argument = pointer to user pt_regs. */
    user_regs = (struct pt_regs *)regs->regs[0];
    if (!user_regs)
        return 0;

    /* cmd is the third syscall argument = user_regs->regs[2]. */
    cmd = (u32)user_regs->regs[2];

    /* RB_AUTOBOOT=0, RB_POWER_OFF=0x4321FEDC, RB_HALT=0xCDEF0123 */
    if (cmd == 0 || cmd == 0x4321FEDC || cmd == 0xCDEF0123)
        dangerous = true;

    if (!dangerous)
        return 0;

    bool protect = READ_ONCE(atomic_read(&tj_protect_mode));

    tj_log_event(1, 0, 0, "reboot/poweroff", protect && !tj_is_trusted_comm());

    if (protect && !tj_is_trusted_comm()) {
        tj_schedule_log();
        tj_block_call(regs);
    }
    return 0;
}

/*
 * ===== L2: VFS layer =====
 * Hook filp_open.
 * struct file *filp_open(const char *filename, int open_mode, umode_t mode);
 *
 * IMPORTANT: filename is a KERNEL pointer, not a user pointer.
 * Use strncpy(), NOT strncpy_from_user().
 */
static int tj_l2_filp_open_pre(struct kprobe *p, struct pt_regs *regs)
{
    const char *filename;  /* kernel pointer */
    int mode;
    char path[80];

    atomic64_inc(&tj_count_l2);

    filename = (const char *)regs->regs[0];
    mode = (int)regs->regs[1];

    if (!filename)
        return 0;

    /* Only care about write opens. */
    if (!(mode & (O_WRONLY | O_RDWR)))
        return 0;

    /* Safe strncpy from kernel pointer. */
    strscpy(path, filename, sizeof(path));

    if (strncmp(path, "/dev/block", 10) != 0)
        return 0;

    bool protect = READ_ONCE(atomic_read(&tj_protect_mode));

    tj_log_event(2, 0, 0, "block-open-write", protect && !tj_is_trusted_comm());

    if (protect && !tj_is_trusted_comm()) {
        tj_schedule_log();
        tj_block_call(regs);
    }
    return 0;
}

/*
 * ===== L3: Block layer - discard =====
 * int blkdev_issue_discard(struct block_device *bdev, sector_t sector,
 *                          sector_t nr_sects, gfp_t gfp_mask);
 */
static int tj_l3_discard_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct block_device *bdev;
    u32 major, minor;

    atomic64_inc(&tj_count_l3);

    bdev = (struct block_device *)regs->regs[0];
    if (!bdev)
        return 0;

    major = MAJOR(bdev->bd_dev);
    minor = MINOR(bdev->bd_dev);

    if (!tj_is_protected_major(major))
        return 0;
    if (tj_is_boot_partition(major, minor))
        return 0;

    bool protect = READ_ONCE(atomic_read(&tj_protect_mode));

    tj_log_event(3, major, minor, "discard", protect && !tj_is_trusted_comm());

    if (protect && !tj_is_trusted_comm()) {
        tj_schedule_log();
        tj_block_call(regs);
    }
    return 0;
}

/*
 * ===== L4: Block layer - zeroout =====
 * int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
 *                          sector_t nr_sects, gfp_t gfp_mask, unsigned flags);
 */
static int tj_l4_zeroout_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct block_device *bdev;
    u32 major, minor;

    atomic64_inc(&tj_count_l4);

    bdev = (struct block_device *)regs->regs[0];
    if (!bdev)
        return 0;

    major = MAJOR(bdev->bd_dev);
    minor = MINOR(bdev->bd_dev);

    if (!tj_is_protected_major(major))
        return 0;
    if (tj_is_boot_partition(major, minor))
        return 0;

    bool protect = READ_ONCE(atomic_read(&tj_protect_mode));

    tj_log_event(4, major, minor, "zeroout", protect && !tj_is_trusted_comm());

    if (protect && !tj_is_trusted_comm()) {
        tj_schedule_log();
        tj_block_call(regs);
    }
    return 0;
}

/*
 * ===== L5: BIO layer (monitor ONLY) =====
 * void submit_bio(struct bio *bio);
 *
 * We never block here. Blocking at BIO level can hang filesystems.
 * This layer is for visibility only.
 */
static int tj_l5_submit_bio_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct bio *bio;
    struct block_device *bdev;
    u32 major, minor;

    atomic64_inc(&tj_count_l5);

    bio = (struct bio *)regs->regs[0];
    if (!bio)
        return 0;

    bdev = bio->bi_bdev;
    if (!bdev)
        return 0;

    major = MAJOR(bdev->bd_dev);
    minor = MINOR(bdev->bd_dev);

    if (!tj_is_protected_major(major))
        return 0;

    /* Only log write BIOs. */
    if (bio_data_dir(bio) != WRITE)
        return 0;

    tj_log_event(5, major, minor, "bio-write", false);
    return 0;
}

/* ===== kprobe descriptors ===== */
static struct kprobe kp_l1_reboot = {
    .symbol_name = "__arm64_sys_reboot",
    .pre_handler = tj_l1_reboot_pre,
};
static struct kprobe kp_l2_filp_open = {
    .symbol_name = "filp_open",
    .pre_handler = tj_l2_filp_open_pre,
};
static struct kprobe kp_l3_discard = {
    .symbol_name = "blkdev_issue_discard",
    .pre_handler = tj_l3_discard_pre,
};
static struct kprobe kp_l4_zeroout = {
    .symbol_name = "blkdev_issue_zeroout",
    .pre_handler = tj_l4_zeroout_pre,
};
static struct kprobe kp_l5_submit_bio = {
    .symbol_name = "submit_bio",
    .pre_handler = tj_l5_submit_bio_pre,
};

/* ===== procfs status read ===== */
static ssize_t tj_status_read(struct file *file, char __user *ubuf,
                              size_t count, loff_t *ppos)
{
    char buf[TJ_STATUS_MAX];
    int len;
    unsigned long flags;
    u32 i, start;
    bool protect;

    protect = READ_ONCE(atomic_read(&tj_protect_mode));

    len = scnprintf(buf, sizeof(buf),
        "project=TianjiLKM\n"
        "version=4.6\n"
        "device=%s\n"
        "codename=%s\n"
        "soc=%s\n"
        "kernel_release=%s\n"
        "mode=%s\n"
        "hooks_registered=%d/%d\n"
        "l1_reboot=%s\n"
        "l2_filp_open=%s\n"
        "l3_discard=%s\n"
        "l4_zeroout=%s\n"
        "l5_submit_bio=%s\n"
        "l1_syscall_hits=%lld\n"
        "l2_vfs_hits=%lld\n"
        "l3_discard_hits=%lld\n"
        "l4_zeroout_hits=%lld\n"
        "l5_bio_hits=%lld\n"
        "total_blocked=%lld\n"
        "recent_events:\n",
        TJ_DEVICE_NAME, TJ_DEVICE_CODENAME, TJ_SOC_NAME,
        utsname()->release,
        protect ? "PROTECT(EXPERIMENTAL)" : "MONITOR",
        (int)bitmap_weight(&tj_kp_registered, TJ_KP_MAX), TJ_KP_MAX,
        test_bit(TJ_KP_L1_REBOOT, &tj_kp_registered) ? "OK" : "FAIL",
        test_bit(TJ_KP_L2_FILP_OPEN, &tj_kp_registered) ? "OK" : "FAIL",
        test_bit(TJ_KP_L3_DISCARD, &tj_kp_registered) ? "OK" : "FAIL",
        test_bit(TJ_KP_L4_ZEROOUT, &tj_kp_registered) ? "OK" : "FAIL",
        test_bit(TJ_KP_L5_SUBMIT_BIO, &tj_kp_registered) ? "OK" : "FAIL",
        atomic64_read(&tj_count_l1),
        atomic64_read(&tj_count_l2),
        atomic64_read(&tj_count_l3),
        atomic64_read(&tj_count_l4),
        atomic64_read(&tj_count_l5),
        atomic64_read(&tj_blocked_total));

    spin_lock_irqsave(&tj_ring_lock, flags);
    start = (tj_ring_head > 8) ? (tj_ring_head - 8) : 0;
    for (i = start; i < tj_ring_head && len < (int)sizeof(buf) - 80; i++) {
        struct tj_event *e = &tj_ring[i % TJ_EVENT_LOG_SIZE];
        len += scnprintf(buf + len, sizeof(buf) - len,
            "  L%u pid=%u uid=%u comm=%s dev=%u:%u %s %s\n",
            e->layer, e->pid, e->uid, e->comm,
            e->major, e->minor, e->action,
            e->blocked ? "BLOCKED" : "obs");
    }
    spin_unlock_irqrestore(&tj_ring_lock, flags);

    return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

/* ===== procfs control write ===== */
static ssize_t tj_control_write(struct file *file, const char __user *ubuf,
                                size_t count, loff_t *ppos)
{
    char cmd[TJ_CMD_MAX];
    char *command;
    size_t n;

    if (!count)
        return -EINVAL;
    if (count >= sizeof(cmd))
        return -E2BIG;

    n = count;
    if (copy_from_user(cmd, ubuf, n))
        return -EFAULT;
    if (memchr(cmd, '\0', n))
        return -EINVAL;
    cmd[n] = '\0';

    command = strim(cmd);

    if (!strcmp(command, "on") || !strcmp(command, "protect on")) {
        WRITE_ONCE(atomic_set(&tj_protect_mode, 1));
        pr_warn("tianji_lkm: PROTECT mode ENABLED (experimental, may panic)\n");
    } else if (!strcmp(command, "off") || !strcmp(command, "protect off")) {
        WRITE_ONCE(atomic_set(&tj_protect_mode, 0));
        pr_info("tianji_lkm: PROTECT mode disabled, back to monitor\n");
    } else {
        return -EINVAL;
    }

    return count;
}

static const struct proc_ops tj_status_ops = {
    .proc_read = tj_status_read,
};

static const struct proc_ops tj_ctrl_ops = {
    .proc_write = tj_control_write,
};

/* ===== init / exit ===== */
static int __init tj_init(void)
{
    int ret;

    tj_dir = proc_mkdir(TJ_PROC_DIR, NULL);
    if (!tj_dir)
        return -ENOMEM;

    tj_status = proc_create(TJ_PROC_STATUS, 0444, tj_dir, &tj_status_ops);
    if (!tj_status)
        goto err;

    tj_ctrl = proc_create("ctrl", 0200, tj_dir, &tj_ctrl_ops);
    if (!tj_ctrl)
        goto err;

    INIT_WORK(&tj_log_work, tj_log_work_fn);

    /*
     * Register kprobes one by one. Track which succeed in tj_kp_registered.
     * If one fails, continue with the rest; exit() will unregister only
     * the ones that actually registered.
     */
#define REGISTER_KP(bit, kp) do {               \
    ret = register_kprobe(&(kp));               \
    if (!ret)                                   \
        set_bit((bit), &tj_kp_registered);      \
    else                                        \
        pr_warn("tianji_lkm: kprobe " #kp " failed: %d\n", ret); \
} while (0)

    REGISTER_KP(TJ_KP_L1_REBOOT, kp_l1_reboot);
    REGISTER_KP(TJ_KP_L2_FILP_OPEN, kp_l2_filp_open);
    REGISTER_KP(TJ_KP_L3_DISCARD, kp_l3_discard);
    REGISTER_KP(TJ_KP_L4_ZEROOUT, kp_l4_zeroout);
    REGISTER_KP(TJ_KP_L5_SUBMIT_BIO, kp_l5_submit_bio);

#undef REGISTER_KP

    pr_info("tianji_lkm: v4.1 loaded on %s (%s), mode=MONITOR (default)\n",
            TJ_DEVICE_NAME, TJ_DEVICE_CODENAME);
    pr_info("tianji_lkm: registered %d/%d kprobes\n",
            (int)bitmap_weight(&tj_kp_registered, TJ_KP_MAX), TJ_KP_MAX);
    return 0;

err:
    proc_remove(tj_dir);
    tj_ctrl = NULL;
    tj_status = NULL;
    tj_dir = NULL;
    return -ENOMEM;
}

static void __exit tj_exit(void)
{
    /* Unregister only the kprobes that actually registered. */
#define UNREGISTER_KP(bit, kp) do {             \
    if (test_bit((bit), &tj_kp_registered))      \
        unregister_kprobe(&(kp));               \
} while (0)

    UNREGISTER_KP(TJ_KP_L5_SUBMIT_BIO, kp_l5_submit_bio);
    UNREGISTER_KP(TJ_KP_L4_ZEROOUT, kp_l4_zeroout);
    UNREGISTER_KP(TJ_KP_L3_DISCARD, kp_l3_discard);
    UNREGISTER_KP(TJ_KP_L2_FILP_OPEN, kp_l2_filp_open);
    UNREGISTER_KP(TJ_KP_L1_REBOOT, kp_l1_reboot);

#undef UNREGISTER_KP

    /* Flush pending log work before unload. */
    cancel_work_sync(&tj_log_work);

    if (tj_dir)
        proc_remove(tj_dir);

    tj_ctrl = NULL;
    tj_status = NULL;
    tj_dir = NULL;
    pr_info("tianji_lkm: v4.4 unloaded\n");
}

module_init(tj_init);
module_exit(tj_exit);
