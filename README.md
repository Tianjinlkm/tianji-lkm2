# TianjiLKM v4.4 — Reviewed Build 4

## Fixes from v4.3 (per fourth ChatGPT review)

| Issue | Risk | Fix |
|-------|------|-----|
| `pr_warn()` in kprobe pre_handler — lock contention, reentry, latency | Medium-High | **Moved all printk out of pre_handler into a workqueue.** pre_handler now does zero printk. Events are recorded to ring buffer, `tj_schedule_log()` defers printing to process context. |

## What changed in v4.4

- Added `work_struct tj_log_work` for deferred logging.
- `tj_schedule_log()` uses `atomic_cmpxchg` to deduplicate — only one work scheduled at a time.
- Work function prints up to 4 recent blocked events from the ring buffer.
- `cancel_work_sync()` in module exit ensures no pending work after unload.
- **pre_handler is now pure memory operations**: ring buffer write + atomic counter increment + conditional schedule_work. No printk, no sleeping, no lock that could reenter.

## Review status summary (v4.0 → v4.4)

| Issue | Risk | Status |
|-------|------|--------|
| L2 used `strncpy_from_user()` on kernel pointer | High | ✅ v4.1 |
| Root/system UID bypass — attackers have root | High | ✅ v4.1 (comm-name whitelist) |
| Partition policy missed major 259 dynamic partitions | High | ✅ v4.1 |
| L1 arm64 syscall ABI — wrong register for cmd | Critical | ✅ v4.2 |
| `pr_warn()` in kprobe pre_handler (lock/reentry) | Medium-High | ✅ v4.4 (workqueue deferred) |
| Non-atomic counters / mode flag races | Medium | ✅ v4.1 (atomic_t) |
| Unregistered kprobe unconditionally unregistered | Medium | ✅ v4.1 (bitmap) |

## Items that need RUNTIME verification

1. **`submit_bio` symbol name**: In 5.10 GKI may be `submit_bio_noacct`. Check dmesg.
2. **kprobe blacklist**: Some functions blocked from probing.
3. **`tj_block_call()` PC-jump interception**: Theoretically correct, unverified on pearl.
4. **Exact userdata partition minor number**: Runtime confirmation needed.

## Status

- Default: **MONITOR mode**. Safe to load.
- Protect mode: **EXPERIMENTAL**. Test only after monitoring is verified on-device.
