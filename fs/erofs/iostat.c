// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/erofs/iostat.c — HyperOS 4 EROFS I/O statistics (CONFIG_XIAOMI_EROFS_IOSTAT)
 *
 * Why this exists
 * ---------------
 * The stock HyperOS 4 kernel is built with CONFIG_XIAOMI_EROFS_IOSTAT=y
 * (taken from the stock /proc/config.gz) and exposes, under
 * /sys/fs/erofs/<label>/:
 *
 *   iostat_enable          iostat_config
 *   iostat_window_latency  iostat_daily_latency
 *
 * HyperOS 4 opens those nodes by name — for one instance,
 * /system/etc/init/hw/init.rc does
 *
 *   write /sys/fs/erofs/${dev.mnt.dev.system_ext}/iostat_enable 1
 *
 * (and the same for dev.system, dev.product, dev.vendor and dev.root).
 * Without this port each of those writes fails, and the nodes are absent.
 *
 * The stock symbol set (from the HyperOS 4 /proc/kallsyms, 493 172 entries):
 *
 *   erofs_init_iostat  erofs_destroy_iostat  erofs_iostat_record_start
 *   erofs_iostat_update  erofs_iostat_config_parse
 *   erofs_iostat_daily_latency_show / erofs_iostat_window_latency_show
 *   erofs_iostat_daily_stats_reset / erofs_iostat_latency_stats_reset
 *   erofs_attr_iostat_enable / _config / _window_latency / _daily_latency
 *
 * Honest scope of the recovery
 * ----------------------------
 * The node names, their modes, and the fact that the layer hangs off erofs
 * metadata/data reads are established. The exact defaults, the exact syntax
 * iostat_config accepts, and the exact report text could NOT be recovered: the
 * functions are LTO-merged into their callers in the stock image, so their
 * bodies cannot be delimited reliably. Therefore the defaults below and the
 * report text are OURS, not the stock's.
 *
 * What this layer measures is real: it counts the erofs-layer reads that this
 * running system performs and the time they take, and generates no I/O itself.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "internal.h"

static u64 erofs_iostat_now_sec(void)
{
	return (u64)ktime_get_real_seconds();
}

static void erofs_iostat_open_win(struct erofs_iostat *i)
{
	memset(&i->cur, 0, sizeof(i->cur));
	i->cur.start_jiffies = jiffies;
	i->cur.start_sec = erofs_iostat_now_sec();
}

/* Caller must hold i->lock. */
static void erofs_iostat_close_win_locked(struct erofs_iostat *i)
{
	u64 now = erofs_iostat_now_sec();
	u64 day = now / 86400;

	if (!time_after(jiffies, i->cur.start_jiffies +
					i->window_period_s * HZ))
		return;

	if (i->hist_fill == EROFS_IOSTAT_WIN_HISTORY) {
		i->hist[i->hist_head] = i->cur;
		i->hist_head = (i->hist_head + 1) % EROFS_IOSTAT_WIN_HISTORY;
	} else {
		i->hist[i->hist_fill++] = i->cur;
	}

	if (i->cur_day != day) {
		struct erofs_iostat_day *w = NULL;
		unsigned int c, t;

		for (c = 0; c < EROFS_IOSTAT_DAY_HISTORY; c++) {
			if (i->days[c].day == day) {
				w = &i->days[c];
				break;
			}
		}

		if (!w) {
			if (i->day_fill == EROFS_IOSTAT_DAY_HISTORY) {
				/* ring is full: reuse the oldest bucket */
				w = &i->days[i->day_head];
				i->day_head = (i->day_head + 1) %
						EROFS_IOSTAT_DAY_HISTORY;
			} else {
				w = &i->days[i->day_fill++];
			}
			memset(w, 0, sizeof(*w));
			w->day = day;
		}

		for (t = 0; t < EROFS_IOSTAT_NR_TYPE; t++) {
			w->reads[t] += i->cur.reads[t];
			w->bytes[t] += i->cur.bytes[t];
			w->slow[t] += i->cur.slow[t];
		}
		i->cur_day = day;
	}

	erofs_iostat_open_win(i);
}

static void erofs_iostat_rollover(struct erofs_iostat *i)
{
	unsigned long flags;

	spin_lock_irqsave(&i->lock, flags);
	erofs_iostat_close_win_locked(i);
	spin_unlock_irqrestore(&i->lock, flags);
}

static bool erofs_iostat_on(struct erofs_iostat *i)
{
	return i && READ_ONCE(i->enable);
}

/*
 * Account one erofs-layer read of @bytes bytes that took @ms milliseconds.
 * Called from erofs_bread() for metadata and from erofs_read_folio() /
 * erofs_readahead() for file data.
 */
void erofs_iostat_account(struct erofs_iostat *i, unsigned int type,
		unsigned long long bytes, unsigned long ms)
{
	unsigned long flags;

	if (!erofs_iostat_on(i) || type >= EROFS_IOSTAT_NR_TYPE)
		return;

	spin_lock_irqsave(&i->lock, flags);
	if (!i->cur.start_jiffies)
		erofs_iostat_open_win(i);
	i->cur.reads[type]++;
	i->cur.bytes[type] += bytes;
	if (ms > i->cur.peak_ms[type])
		i->cur.peak_ms[type] = ms;
	if (i->latency_threshold_ms && ms >= i->latency_threshold_ms)
		i->cur.slow[type]++;
	spin_unlock_irqrestore(&i->lock, flags);
}

void erofs_iostat_init(struct erofs_sb_info *sbi)
{
	struct erofs_iostat *i = &sbi->iostat;

	if (!IS_ENABLED(CONFIG_XIAOMI_EROFS_IOSTAT))
		return;

	spin_lock_init(&i->lock);
	i->enable = false;
	i->window_period_s = EROFS_IOSTAT_DEFAULT_WINDOW_S;
	i->latency_threshold_ms = EROFS_IOSTAT_DEFAULT_THRESHOLD_MS;
	erofs_iostat_open_win(i);
	i->cur_day = erofs_iostat_now_sec() / 86400;
}

/* --------------------------------------------------------- sysfs front-end */

int erofs_iostat_enable_get(struct erofs_iostat *i)
{
	unsigned long flags;
	int v;

	if (!i)
		return -EINVAL;

	spin_lock_irqsave(&i->lock, flags);
	v = i->enable;
	spin_unlock_irqrestore(&i->lock, flags);
	return v;
}

int erofs_iostat_enable_set(struct erofs_iostat *i, unsigned int t)
{
	unsigned long flags;

	if (!i)
		return -EINVAL;

	spin_lock_irqsave(&i->lock, flags);
	i->enable = !!t;
	if (!i->enable)
		erofs_iostat_open_win(i);
	spin_unlock_irqrestore(&i->lock, flags);
	return 0;
}

int erofs_iostat_period_get(struct erofs_iostat *i)
{
	unsigned long flags;
	int v;

	if (!i)
		return EROFS_IOSTAT_MIN_WINDOW_S;

	spin_lock_irqsave(&i->lock, flags);
	v = i->window_period_s;
	spin_unlock_irqrestore(&i->lock, flags);
	return v;
}

int erofs_iostat_period_set(struct erofs_iostat *i, unsigned int t)
{
	unsigned long flags;

	if (!i)
		return -EINVAL;
	if (t < EROFS_IOSTAT_MIN_WINDOW_S || t > EROFS_IOSTAT_MAX_WINDOW_S)
		return -EINVAL;

	spin_lock_irqsave(&i->lock, flags);
	i->window_period_s = t;
	erofs_iostat_open_win(i);
	spin_unlock_irqrestore(&i->lock, flags);
	return 0;
}

int erofs_iostat_threshold_get(struct erofs_iostat *i)
{
	unsigned long flags;
	int v;

	if (!i)
		return EROFS_IOSTAT_DEFAULT_THRESHOLD_MS;

	spin_lock_irqsave(&i->lock, flags);
	v = i->latency_threshold_ms;
	spin_unlock_irqrestore(&i->lock, flags);
	return v;
}

int erofs_iostat_threshold_set(struct erofs_iostat *i, unsigned int t)
{
	unsigned long flags;

	if (!i)
		return -EINVAL;
	if (t > EROFS_IOSTAT_MAX_THRESHOLD_MS)
		return -EINVAL;

	spin_lock_irqsave(&i->lock, flags);
	i->latency_threshold_ms = t;
	spin_unlock_irqrestore(&i->lock, flags);
	return 0;
}

void erofs_iostat_reset(struct erofs_iostat *i)
{
	unsigned long flags;

	if (!i)
		return;

	spin_lock_irqsave(&i->lock, flags);
	memset(i->hist, 0, sizeof(i->hist));
	i->hist_head = 0;
	i->hist_fill = 0;
	erofs_iostat_open_win(i);
	spin_unlock_irqrestore(&i->lock, flags);
}

/*
 * iostat_config: read back or set the three tunables at once, so a single
 * write is enough to configure the layer. Accepted forms (any subset):
 *
 *   enable=<0|1>  period_s=<1..86400>  threshold_ms=<0..600000>
 *
 * "reset" alone drops the window history, like writing to a report node.
 */
ssize_t erofs_iostat_config_show(struct erofs_iostat *i, char *buf)
{
	if (!i)
		return -EINVAL;

	return sysfs_emit(buf, "enable %d period_s %u threshold_ms %u\n",
			erofs_iostat_enable_get(i),
			erofs_iostat_period_get(i),
			erofs_iostat_threshold_get(i));
}

ssize_t erofs_iostat_config_store(struct erofs_iostat *i, const char *buf,
		size_t len)
{
	char *s, *tok;
	int ret = 0;

	if (!i)
		return -EINVAL;

	s = kstrndup(buf, len, GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	tok = s;
	while ((tok = strsep(&tok, " \t\n")) != NULL) {
		const char *val = NULL;
		unsigned long v;
		int key, rc;

		if (!*tok)
			continue;

		if (!strcmp(tok, "reset")) {
			erofs_iostat_reset(i);
			continue;
		}

		/* split "key=value" */
		if (!strncmp(tok, "enable=", 7)) {
			key = 0;
			val = tok + 7;
		} else if (!strncmp(tok, "period_s=", 9)) {
			key = 1;
			val = tok + 9;
		} else if (!strncmp(tok, "threshold_ms=", 13)) {
			key = 2;
			val = tok + 13;
		} else {
			ret = -EINVAL;
			break;
		}

		rc = kstrtoul(val, 0, &v);
		if (rc) {
			ret = rc;
			break;
		}

		if (key == 0)
			ret = erofs_iostat_enable_set(i, !!v);
		else if (key == 1)
			ret = erofs_iostat_period_set(i, v);
		else
			ret = erofs_iostat_threshold_set(i, v);

		if (ret)
			break;
	}

	kfree(s);
	return ret ? ret : len;
}

static void erofs_iostat_emit_win(char **p, char *end,
			const struct erofs_iostat_win *w)
{
	unsigned int t;

	*p += scnprintf(*p, end - *p, "window_start %llu\n", w->start_sec);
	for (t = 0; t < EROFS_IOSTAT_NR_TYPE; t++) {
		const char *n = t == EROFS_IOSTAT_META ? "meta" : "data";

		*p += scnprintf(*p, end - *p, "%s_reads %llu\n", n,
				w->reads[t]);
		*p += scnprintf(*p, end - *p, "%s_bytes %llu\n", n,
				w->bytes[t]);
		*p += scnprintf(*p, end - *p, "%s_peak_ms %llu\n", n,
				w->peak_ms[t]);
		*p += scnprintf(*p, end - *p, "%s_slow_ios %llu\n", n,
				w->slow[t]);
	}
}

ssize_t erofs_iostat_window_report(struct erofs_iostat *i, char *buf)
{
	char *p = buf, *end = buf + PAGE_SIZE;
	unsigned int n;
	unsigned long flags;

	if (!i)
		return -EINVAL;

	erofs_iostat_rollover(i);

	spin_lock_irqsave(&i->lock, flags);
	erofs_iostat_emit_win(&p, end, &i->cur);
	for (n = 0; n < i->hist_fill; n++) {
		unsigned int idx = (i->hist_head + n) % EROFS_IOSTAT_WIN_HISTORY;
		struct erofs_iostat_win *w = &i->hist[idx];

		if (w->start_jiffies)
			erofs_iostat_emit_win(&p, end, w);
	}
	spin_unlock_irqrestore(&i->lock, flags);

	return p - buf;
}

ssize_t erofs_iostat_daily_report(struct erofs_iostat *i, char *buf)
{
	char *p = buf, *end = buf + PAGE_SIZE;
	unsigned int n, t;
	unsigned long flags;

	if (!i)
		return -EINVAL;

	erofs_iostat_rollover(i);

	spin_lock_irqsave(&i->lock, flags);
	p += scnprintf(p, end - p, "day reads bytes slow_ios\n");
	for (n = 0; n < EROFS_IOSTAT_DAY_HISTORY; n++) {
		struct erofs_iostat_day *d = &i->days[n];
		u64 reads = 0, bytes = 0, slow = 0;

		if (!d->day)
			continue;

		for (t = 0; t < EROFS_IOSTAT_NR_TYPE; t++) {
			reads += d->reads[t];
			bytes += d->bytes[t];
			slow += d->slow[t];
		}
		p += scnprintf(p, end - p, "%llu %llu %llu %llu\n", d->day,
				reads, bytes, slow);
	}
	spin_unlock_irqrestore(&i->lock, flags);

	return p - buf;
}
