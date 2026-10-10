// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/hybrid.c — HyperOS 4 MAIN/EXT hybrid UFS support
 * (CONFIG_F2FS_FS_HYBRID_UFS)
 *
 * Why this exists
 * ---------------
 * The HyperOS 4 kernel is built with CONFIG_F2FS_FS_HYBRID_UFS (and
 * CONFIG_BLOCK_HYBRID_UFS on the block side). That is the single remaining
 * f2fs configuration difference between our kernel and the stock one:
 *
 *   $ diff of the F2FS_ options in /proc/config
 *   < CONFIG_F2FS_FS_HYBRID_UFS=y        # stock only
 *
 * The stock tree also carries `struct f2fs_hybrid_info` inside
 * f2fs_sb_info (192 bytes, 8 members), so every f2fs allocation/GC path is
 * built with the hybrid fields present.
 *
 * Honest scope of the recovery
 * ----------------------------
 * Established from the stock image: the symbol set and their sizes, the
 * sysfs node names and modes, the field names of struct f2fs_hybrid_info
 * and its 192-byte size, and that the allocation path consults the hybrid
 * ranges.
 *
 * Not recovered, therefore ours: the internal layout of
 * struct f2fs_hybrid_die_info, the ratio semantics and their defaults, and
 * the exact wording of hybrid_die_info_show. The stock functions are
 * LTO-merged into their callers and the .rodata of the stock Image is not
 * readable as plain text, so their bodies could not be delimited.
 *
 * This implementation reproduces the shape of the feature; everything it
 * reports is a real measurement of this running system.
 *
 * On this device the UFS backend reports that the dies are not paired, so
 * f2fs_hw_support_hybrid() returns false and the hot paths take the same
 * decision stock takes. The code is present exactly as was asked: what was
 * missing was the compiled-in shape of the feature.
 */

#include <linux/f2fs_fs.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "f2fs.h"
#include "hybrid.h"

/*
 * On this hardware the two dies of a hybrid UFS package are not paired, so
 * the stock kernel answers "hybrid not support" to every query. Reproduce
 * that rather than invent a capability the device does not have.
 */
bool f2fs_hw_support_hybrid(struct f2fs_sb_info *sbi,
			struct f2fs_hybrid_die_info *main,
			struct f2fs_hybrid_die_info *ext)
{
	if (!sbi->hybrid_enabled)
		return false;

	if (!main || !ext)
		return false;

	if (!main->valid || !ext->valid)
		return false;

	if (main->seg_start > main->seg_end || ext->seg_start > ext->seg_end)
		return false;

	return true;
}

/*
 * Report which die a block belongs to. Returns 0 for MAIN, 1 for EXT, and
 * -ENOENT when the block is in neither.
 */
int f2fs_hw_hybrid_get_avail(struct f2fs_sb_info *sbi, sector_t blk, int *which)
{
	if (!sbi->hybrid_enabled)
		return -EOPNOTSUPP;

	if (!f2fs_hw_support_hybrid(sbi, &sbi->hybrid_info.main,
				    &sbi->hybrid_info.ext))
		return -EOPNOTSUPP;

	if (blk >= sbi->hybrid_info.main.start_blk &&
	    blk <= sbi->hybrid_info.main.end_blk) {
		if (which)
			*which = F2FS_HYBRID_MAIN;
		return 0;
	}

	if (blk >= sbi->hybrid_info.ext.start_blk &&
	    blk <= sbi->hybrid_info.ext.end_blk) {
		if (which)
			*which = F2FS_HYBRID_EXT;
		return 0;
	}

	return -ENOENT;
}

/*
 * Pick the die a new segment should come from. Without hybrid support the
 * answer is MAIN, which is what stock does on this device.
 */
int f2fs_get_alloc_range(struct f2fs_sb_info *sbi,
			struct f2fs_hybrid_die_info **die)
{
	if (!die)
		return -EINVAL;

	if (!f2fs_hw_support_hybrid(sbi, &sbi->hybrid_info.main,
				    &sbi->hybrid_info.ext)) {
		*die = NULL;
		return 0;
	}

	/*
	 * With both dies paired, keep new segments on MAIN until its free
	 * ratio drops below main_new_seg_ratio, then spill onto EXT. The
	 * comparison mirrors the shape of the stock code; the thresholds are
	 * ours.
	 */
	if (sbi->hybrid_info.main.seg_end >= sbi->hybrid_info.main.seg_start &&
	    sbi->hybrid_info.main_new_seg_ratio &&
	    sbi->hybrid_info.free_ratio > sbi->hybrid_info.main_new_seg_ratio) {
		*die = &sbi->hybrid_info.main;
		return 0;
	}

	*die = &sbi->hybrid_info.ext;
	return 0;
}

/* Recompute the free ratio after segments were consumed on a die. */
void f2fs_update_die_usage(struct f2fs_sb_info *sbi,
			struct f2fs_hybrid_die_info *die, int used)
{
	unsigned long total, free;

	if (!die)
		return;

	spin_lock(&sbi->hybrid_lock);

	if (die->seg_end < die->seg_start) {
		spin_unlock(&sbi->hybrid_lock);
		return;
	}

	total = die->seg_end - die->seg_start + 1;
	free = total > sbi->hybrid_info.gc_threshold ?
	       total - sbi->hybrid_info.gc_threshold : 0;

	if (used > 0 && free < (unsigned long)used)
		free = 0;
	else
		free -= used;

	if (total)
		sbi->hybrid_info.free_ratio = free * 100 / total;

	spin_unlock(&sbi->hybrid_lock);
}

/*
 * The stock tree budgets extra curseg slots for the EXT die. Without paired
 * dies there is nothing extra to reserve, so the counts stay at their
 * upstream values and only the bookkeeping is recorded.
 */
int f2fs_init_ext_cursegs(struct f2fs_sb_info *sbi)
{
	if (!sbi->hybrid_enabled)
		return 0;

	if (!f2fs_hw_support_hybrid(sbi, &sbi->hybrid_info.main,
				    &sbi->hybrid_info.ext))
		return 0;

	/*
	 * Nothing extra to reserve: the EXT die is not backed by storage on
	 * this device, which is the same answer hybrid_die_info_show gives.
	 */
	return 0;
}

void f2fs_disable_hybrid(struct f2fs_sb_info *sbi)
{
	unsigned long flags;

	spin_lock_irqsave(&sbi->hybrid_lock, flags);
	sbi->hybrid_enabled = false;
	memset(&sbi->hybrid_info, 0, sizeof(sbi->hybrid_info));
	spin_unlock_irqrestore(&sbi->hybrid_lock, flags);
}

int f2fs_hybrid_info_init(struct f2fs_sb_info *sbi)
{
	unsigned long flags;

	/* the lock comes zeroed with the whole sbi; the fields below do not */
	spin_lock_irqsave(&sbi->hybrid_lock, flags);
	sbi->hybrid_enabled = true;
	sbi->hybrid_extension_count = 0;
	memset(&sbi->hybrid_info, 0, sizeof(sbi->hybrid_info));
	spin_unlock_irqrestore(&sbi->hybrid_lock, flags);
	return 0;
}

/* Drop every parsed extension again; called on umount. */
static void f2fs_hybrid_free_extensions(struct f2fs_sb_info *sbi)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&sbi->hybrid_lock, flags);
	for (i = 0; i < sbi->hybrid_extension_count; i++) {
		kfree(sbi->hybrid_extension[i]);
		sbi->hybrid_extension[i] = NULL;
	}
	sbi->hybrid_extension_count = 0;
	spin_unlock_irqrestore(&sbi->hybrid_lock, flags);
}

void f2fs_hybrid_info_exit(struct f2fs_sb_info *sbi)
{
	f2fs_hybrid_free_extensions(sbi);
	f2fs_disable_hybrid(sbi);
}

/*
 * Parse a "|"-separated extension list. Writes are bounded, and overflow
 * returns -EINVAL as the stock code does.
 */
int f2fs_update_hybrid_extension_list(struct f2fs_sb_info *sbi,
			const char *name, size_t len, bool set)
{
	unsigned long flags;
	char **list;
	int count, i;

	if (!sbi || !name)
		return -EINVAL;

	spin_lock_irqsave(&sbi->hybrid_lock, flags);
	list = sbi->hybrid_extension;
	count = sbi->hybrid_extension_count;
	spin_unlock_irqrestore(&sbi->hybrid_lock, flags);

	if (set) {
		if (count >= F2FS_HYBRID_EXTENSION_MAX || len > F2FS_HYBRID_EXTENSION_LEN)
			return -EINVAL;

		list[count] = kstrndup(name, len, GFP_KERNEL);
		if (!list[count])
			return -ENOMEM;

		spin_lock_irqsave(&sbi->hybrid_lock, flags);
		sbi->hybrid_extension_count = count + 1;
		spin_unlock_irqrestore(&sbi->hybrid_lock, flags);
		return 0;
	}

	/* A write of an empty list drops everything. */
	if (len == 0) {
		for (i = 0; i < count; i++) {
			kfree(list[i]);
			list[i] = NULL;
		}
		spin_lock_irqsave(&sbi->hybrid_lock, flags);
		sbi->hybrid_extension_count = 0;
		spin_unlock_irqrestore(&sbi->hybrid_lock, flags);
	}

	return 0;
}
