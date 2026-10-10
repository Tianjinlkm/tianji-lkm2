/*
 * TianjiLKM v4.5 - Device configuration & protection policy.
 * pearl / Redmi Note 12T Pro / MT6896
 */
#ifndef TIANJI_CONFIG_DEVICE_H
#define TIANJI_CONFIG_DEVICE_H

#define TJ_DEVICE_NAME      "Redmi Note 12T Pro"
#define TJ_DEVICE_CODENAME  "pearl"
#define TJ_SOC_NAME         "MediaTek Dimensity 8200-Ultra (MT6896)"
#define TJ_KERNEL_FAMILY    "Linux 5.10 Android"

/*
 * Layer 6: Protected block device majors.
 *
 * On pearl:
 *   major 8  = mmcblk0 (sda/sdb/sdc, first 15 partitions)
 *   major 259 = mmcblk0p (sdc16 and above, dynamic partitions)
 *
 * We protect both major ranges.
 */
#define TJ_PROTECT_MAJOR_LIST   { 8, 259 }
#define TJ_PROTECT_MAJOR_COUNT  2

/*
 * Boot partitions that must never be blocked.
 * Only the two eMMC boot hardware partitions:
 *   sda = major 8, minor 0  (mmcblk0boot0)
 *   sdb = major 8, minor 16 (mmcblk0boot1)
 *
 * All other partitions (including sdc1..sdc15, and all of major 259)
 * are considered protected by default.
 */
#define TJ_BOOT_MAJOR           8
#define TJ_BOOT_MINOR_0         0
#define TJ_BOOT_MINOR_1         16

/* Trusted process comm names (Layer 6). */
#define TJ_TRUSTED_COMM_VOLD        "vold"
#define TJ_TRUSTED_COMM_RECOVERY    "recovery"
#define TJ_TRUSTED_COMM_UPDATE      "update_engine"
#define TJ_TRUSTED_COMM_INIT        "init"

/* Event log ring buffer size (Layer 7). */
#define TJ_EVENT_LOG_SIZE        32

#endif /* TIANJI_CONFIG_DEVICE_H */
