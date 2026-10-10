// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/en_iostat.c — HyperOS 4 "enhanced iostat" (CONFIG_XIAOMI_ENHANCED_IOSTAT)
 *
 * Why this exists
 * ---------------
 * HyperOS 4 writes the node this provides at boot:
 *
 *   /system/etc/init/hw/init.rc:1842
 *     write /sys/fs/f2fs/${dev.mnt.dev.data}/en_iostat_enable 1
 *
 * On the HyperOS 4 kernel that node is backed by CONFIG_XIAOMI_ENHANCED_IOSTAT
 * (confirmed in the stock /proc/config.gz) together with
 * CONFIG_F2FS_IOSTAT. Before this port the node did not exist on this kernel at
 * all, so that write — and every later write of the same kind — failed.
 *
 * The sysfs front-end lives in fs/f2fs/sysfs.c next to the rest of the f2fs
 * attributes; this file only holds the counters and the accounting hooks.
 *
 * Nodes provided, under /sys/fs/f2fs/<dev>/:
 *
 *   en_iostat_enable            0644  master switch for this layer
 *   iostat_window_period_s      0644  window length in seconds
 *   iostat_latency_threshold_ms 0644  I/Os slower than this are counted
 *   iostat_window_latency       0644  report for the current window; write resets
 *   iostat_daily_latency        0444  report per day
 *
 * The HyperOS 4 symbol set these nodes correspond to (from the stock
 * /proc/kallsyms, 493 172 entries) is:
 *
 *   f2fs_get_iostat              iostat_array_size
 *   en_iostat_enable_show/store  iostat_window_period_s_show/store
 *   iostat_latency_threshold_ms_show/store
 *   iostat_daily_latency_show    iostat_window_latency_show/store
 *   f2fs_iostat_daily_latency_show / f2fs_iostat_window_latency_show
 *                                 f2fs_iostat_window_latency_store
 *   f2fs_attr_en_iostat_enable   f2fs_attr_iostat_window_period_s
 *   f2fs_attr_iostat_latency_threshold_ms / f2fs_attr_iostat_window_latency
 *                                 f2fs_attr_iostat_daily_latency
 *
 * Honest scope of the recovery
 * ----------------------------
 * The node names, their modes and the fact that the layer is gated by a
 * separate master switch are established facts (the stock kallsyms, the
 * observed /sys/fs/f2fs/dm-60 listing on stock, and the init.rc line above).
 * The exact numeric defaults and the exact text the stock emits could NOT be
 * recovered: those functions are LTO-merged into their callers in the stock
 * image, so their bodies cannot be delimited reliably. Therefore the
 * window/threshold defaults below are OURS, not recovered, and the report text
 * is OURS, not the stock wording.
 *
 * What this layer measures is real: it hooks the accounting points that
 * CONFIG_F2FS_IOSTAT already uses, so every number it reports is a true
 * measurement of this running system and generates no extra I/O of its own.
 */

#include <linux/f2fs_fs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "f2fs.h"
#include "iostat.h"

/* finished windows kept for reporting */
#define EN_IOSTAT_WIN_HISTORY		8
/* days kept for the daily report */
#define EN_IOSTAT_DAY_HISTORY		8

/*
 * Chosen by us, not recovered from the stock image (see the file header).
 * 60 s keeps the window report useful without turning over too often;
 * 500 ms is unambiguously slow on real storage.
 */
#define EN_IOSTAT_DEFAULT_WINDOW_S		60
#define EN_IOSTAT_MIN_WINDOW_S			1
#define EN_IOSTAT_MAX_WINDOW_S			86400
#define EN_IOSTAT_DEFAULT_THRESHOLD_MS		500
#define EN_IOSTAT_MAX_THRESHOLD_MS		600000

struct en_iostat_window {
	u64			bytes[NR_IO_TYPE];
	u64			count[NR_IO_TYPE];
	u64			slow_count;	/* I/Os >= threshold */
	u64			peak_ms;
	unsigned long		start_jiffies;	/* when the window opened */
	u64			start_sec;	/* real time, for the report */
};

struct en_iostat_day {
	u64			bytes[NR_IO_TYPE];
	u64			count[NR_IO_TYPE];
	u64			slow_count;
	u64			day;		/* seconds since the epoch / 86400 */
};

/*
 * Per-mount state. en_iostat_mem->lock protects the window rings and the
 * tunables alike: everything is touched under spin_lock_irqsave().
 */
struct f2fs_en_iostat {
	spinlock_t		lock;

	bool			enable;
	unsigned int		window_period_s;
	unsigned int		latency_threshold_ms;

	struct en_iostat_window	cur;

	/* ring of finished windows; hist_head points at the oldest */
	struct en_iostat_window	hist[EN_IOSTAT_WIN_HISTORY];
	unsigned int		hist_head;
	unsigned int		hist_fill;

	/* ring of finished days; day_head points at the oldest */
	struct en_iostat_day	days[EN_IOSTAT_DAY_HISTORY];
	unsigned int		day_head;
	unsigned int		day_fill;
	u64			cur_day;
};

static u64 f2fs_en_iostat_now_sec(void)
{
	return (u64)ktime_get_real_seconds();
}

/*
 * Close the current window when its time is up and, if the wall-clock day
 * changed, fold it into today's bucket. Returns true when a window was closed.
 * The caller must hold e->lock.
 */
static bool f2fs_en_iostat_close_window_locked(struct f2fs_en_iostat *e)
{
	u64 now = f2fs_en_iostat_now_sec();
	u64 day = now / 86400;

	if (!time_after(jiffies, e->cur.start_jiffies +
					e->window_period_s * HZ))
		return false;

	if (e->hist_fill == EN_IOSTAT_WIN_HISTORY) {
		e->hist[e->hist_head] = e->cur;
		e->hist_head = (e->hist_head + 1) % EN_IOSTAT_WIN_HISTORY;
	} else {
		e->hist[e->hist_fill++] = e->cur;
	}

	if (e->cur_day != day) {
		struct en_iostat_day *w = NULL;
		unsigned int i;

		/* today's bucket, if it is already in the ring */
		for (i = 0; i < EN_IOSTAT_DAY_HISTORY; i++) {
			if (e->days[i].day == day) {
				w = &e->days[i];
				break;
			}
		}

		if (!w) {
			/*
			 * No bucket for today yet: append after the last used
			 * one, and when the ring is full reuse the oldest.
			 */
			if (e->day_fill == EN_IOSTAT_DAY_HISTORY) {
				w = &e->days[e->day_head];
				e->day_head = (e->day_head + 1) %
						EN_IOSTAT_DAY_HISTORY;
			} else {
				w = &e->days[e->day_fill++];
			}
			memset(w, 0, sizeof(*w));
			w->day = day;
		}

		for (i = 0; i < NR_IO_TYPE; i++) {
			w->bytes[i] += e->cur.bytes[i];
			w->count[i] += e->cur.count[i];
		}
		w->slow_count += e->cur.slow_count;
		e->cur_day = day;
	}

	memset(&e->cur, 0, sizeof(e->cur));
	e->cur.start_jiffies = jiffies;
	e->cur.start_sec = now;
	return true;
}

static void f2fs_en_iostat_open_window(struct f2fs_en_iostat *e)
{
	memset(&e->cur, 0, sizeof(e->cur));
	e->cur.start_jiffies = jiffies;
	e->cur.start_sec = f2fs_en_iostat_now_sec();
}

static bool f2fs_en_iostat_on(struct f2fs_sb_info *sbi)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;

	if (!IS_ENABLED(CONFIG_XIAOMI_ENHANCED_IOSTAT) || !e)
		return false;
	return READ_ONCE(e->enable);
}

static void f2fs_en_iostat_rollover(struct f2fs_sb_info *sbi)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned long flags;

	if (!e)
		return;

	spin_lock_irqsave(&e->lock, flags);
	f2fs_en_iostat_close_window_locked(e);
	spin_unlock_irqrestore(&e->lock, flags);
}

/*
 * Account one completed I/O group. Called from f2fs_update_iostat(), i.e. from
 * exactly the paths the base CONFIG_F2FS_IOSTAT accounting already walks, so
 * this adds no I/O of its own.
 */
void f2fs_en_iostat_account(struct f2fs_sb_info *sbi,
			enum iostat_type type, unsigned long long io_bytes)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned long flags;

	if (!f2fs_en_iostat_on(sbi) || type >= NR_IO_TYPE)
		return;

	spin_lock_irqsave(&e->lock, flags);
	if (!e->cur.start_jiffies)
		f2fs_en_iostat_open_window(e);
	e->cur.bytes[type] += io_bytes;
	e->cur.count[type]++;
	spin_unlock_irqrestore(&e->lock, flags);
}

/* Account one measurable I/O latency. Called from __update_iostat_latency(). */
void f2fs_en_iostat_latency(struct f2fs_sb_info *sbi, unsigned long ms)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned long flags;

	if (!f2fs_en_iostat_on(sbi))
		return;

	spin_lock_irqsave(&e->lock, flags);
	if (ms > e->cur.peak_ms)
		e->cur.peak_ms = ms;
	if (e->latency_threshold_ms && ms >= e->latency_threshold_ms)
		e->cur.slow_count++;
	spin_unlock_irqrestore(&e->lock, flags);
}

int f2fs_init_en_iostat(struct f2fs_sb_info *sbi)
{
	struct f2fs_en_iostat *e;

	/* Only ever compiled in, never allocated, when the Kconfig is off. */
	if (!IS_ENABLED(CONFIG_XIAOMI_ENHANCED_IOSTAT))
		return 0;

	e = f2fs_kzalloc(sbi, sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	spin_lock_init(&e->lock);
	e->enable = false;
	e->window_period_s = EN_IOSTAT_DEFAULT_WINDOW_S;
	e->latency_threshold_ms = EN_IOSTAT_DEFAULT_THRESHOLD_MS;
	f2fs_en_iostat_open_window(e);
	e->cur_day = f2fs_en_iostat_now_sec() / 86400;

	sbi->en_iostat_mem = e;
	return 0;
}

void f2fs_destroy_en_iostat(struct f2fs_sb_info *sbi)
{
	if (sbi->en_iostat_mem) {
		kfree(sbi->en_iostat_mem);
		sbi->en_iostat_mem = NULL;
	}
}

/* ------------------------------------------------------- sysfs front-end */

unsigned int f2fs_en_iostat_enable_get(struct f2fs_sb_info *sbi)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned int v;

	if (!e)
		return 0;

	spin_lock(&e->lock);
	v = e->enable;
	spin_unlock(&e->lock);
	return v;
}

int f2fs_en_iostat_enable_set(struct f2fs_sb_info *sbi, unsigned int t)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned long flags;

	if (!e)
		return -EINVAL;

	spin_lock_irqsave(&e->lock, flags);
	e->enable = !!t;
	if (!e->enable)
		f2fs_en_iostat_open_window(e);
	spin_unlock_irqrestore(&e->lock, flags);
	return 0;
}

unsigned int f2fs_en_iostat_period_get(struct f2fs_sb_info *sbi)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned int v;

	if (!e)
		return EN_IOSTAT_MIN_WINDOW_S;

	spin_lock(&e->lock);
	v = e->window_period_s;
	spin_unlock(&e->lock);
	return v;
}

int f2fs_en_iostat_period_set(struct f2fs_sb_info *sbi, unsigned int t)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned long flags;

	if (!e)
		return -EINVAL;
	if (t < EN_IOSTAT_MIN_WINDOW_S || t > EN_IOSTAT_MAX_WINDOW_S)
		return -EINVAL;

	spin_lock_irqsave(&e->lock, flags);
	e->window_period_s = t;
	f2fs_en_iostat_open_window(e);
	spin_unlock_irqrestore(&e->lock, flags);
	return 0;
}

unsigned int f2fs_en_iostat_threshold_get(struct f2fs_sb_info *sbi)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned int v;

	if (!e)
		return EN_IOSTAT_DEFAULT_THRESHOLD_MS;

	spin_lock(&e->lock);
	v = e->latency_threshold_ms;
	spin_unlock(&e->lock);
	return v;
}

int f2fs_en_iostat_threshold_set(struct f2fs_sb_info *sbi, unsigned int t)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned long flags;

	if (!e)
		return -EINVAL;
	if (t > EN_IOSTAT_MAX_THRESHOLD_MS)
		return -EINVAL;

	spin_lock_irqsave(&e->lock, flags);
	e->latency_threshold_ms = t;
	spin_unlock_irqrestore(&e->lock, flags);
	return 0;
}

/*
 * Writing either report node drops the window history and reopens the current
 * window; the write value itself is ignored.
 */
void f2fs_en_iostat_reset(struct f2fs_sb_info *sbi)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	unsigned long flags;

	if (!e)
		return;

	spin_lock_irqsave(&e->lock, flags);
	memset(e->hist, 0, sizeof(e->hist));
	e->hist_head = 0;
	e->hist_fill = 0;
	f2fs_en_iostat_open_window(e);
	spin_unlock_irqrestore(&e->lock, flags);
}

/*
 * Called from the base iostat period tick. Closes a window that expired while
 * the system was idle, so a read of the report is never stale.
 */
void f2fs_en_iostat_tick(struct f2fs_sb_info *sbi)
{
	f2fs_en_iostat_rollover(sbi);
}

static void en_iostat_emit_window(char **p, char *end,
			const struct en_iostat_window *w)
{
	u64 total_bytes = 0, total_count = 0;
	int i;

	for (i = 0; i < NR_IO_TYPE; i++) {
		total_bytes += w->bytes[i];
		total_count += w->count[i];
	}

	*p += scnprintf(*p, end - *p, "window_start %llu\n", w->start_sec);
	*p += scnprintf(*p, end - *p, "window_bytes %llu\n", total_bytes);
	*p += scnprintf(*p, end - *p, "window_ios %llu\n", total_count);
	*p += scnprintf(*p, end - *p, "peak_latency_ms %llu\n", w->peak_ms);
	*p += scnprintf(*p, end - *p, "slow_ios %llu\n", w->slow_count);
}

ssize_t f2fs_en_iostat_window_report(struct f2fs_sb_info *sbi, char *buf)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	char *p = buf, *end = buf + PAGE_SIZE;
	int i;

	if (!e)
		return -EINVAL;

	f2fs_en_iostat_rollover(sbi);

	spin_lock(&e->lock);
	en_iostat_emit_window(&p, end, &e->cur);
	for (i = 0; i < e->hist_fill; i++) {
		unsigned int idx = (e->hist_head + i) % EN_IOSTAT_WIN_HISTORY;
		struct en_iostat_window *w = &e->hist[idx];

		if (w->start_jiffies)
			en_iostat_emit_window(&p, end, w);
	}
	spin_unlock(&e->lock);

	return p - buf;
}

ssize_t f2fs_en_iostat_daily_report(struct f2fs_sb_info *sbi, char *buf)
{
	struct f2fs_en_iostat *e = sbi->en_iostat_mem;
	char *p = buf, *end = buf + PAGE_SIZE;
	int i, j;

	if (!e)
		return -EINVAL;

	f2fs_en_iostat_rollover(sbi);

	spin_lock(&e->lock);
	p += scnprintf(p, end - p, "day bytes ios slow_ios\n");
	for (i = 0; i < EN_IOSTAT_DAY_HISTORY; i++) {
		struct en_iostat_day *d = &e->days[i];
		u64 total_bytes = 0, total_count = 0;

		if (!d->day)
			continue;

		for (j = 0; j < NR_IO_TYPE; j++) {
			total_bytes += d->bytes[j];
			total_count += d->count[j];
		}
		p += scnprintf(p, end - p, "%llu %llu %llu %llu\n", d->day,
				total_bytes, total_count, d->slow_count);
	}
	spin_unlock(&e->lock);

	return p - buf;
}
