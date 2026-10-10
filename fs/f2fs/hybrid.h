// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/hybrid.c — HyperOS 4 MAIN/EXT hybrid UFS support
 * (CONFIG_F2FS_FS_HYBRID_UFS)
 *
 * See hybrid.c for what is recovered from the stock image and what is ours.
 * The struct sizes below deliberately match the stock BTF so that the
 * f2fs_sb_info layout carries the hybrid fields the stock tree carries.
 */
#ifndef __F2FS_HYBRID_H__
#define __F2FS_HYBRID_H__

struct f2fs_sb_info;

/* Which die a segment or a block belongs to. */
#define F2FS_HYBRID_MAIN		0
#define F2FS_HYBRID_EXT			1

/* The stock extension list is bounded; overflow returns -EINVAL. */
#define F2FS_HYBRID_EXTENSION_MAX	17
#define F2FS_HYBRID_EXTENSION_LEN	15

/*
 * A single die of a paired UFS package. 72 bytes in the stock layout; the
 * internal meaning of the trailing fields is not recovered, and they are
 * kept as reserved space so the size matches.
 */
struct f2fs_hybrid_die_info {
	unsigned long start_blk;	/* first sector of the die */
	unsigned long end_blk;		/* last sector of the die */
	unsigned long seg_start;	/* first f2fs segment index */
	unsigned long seg_end;		/* last f2fs segment index */
	unsigned int valid;		/* the die describes real storage */
	unsigned int reserved[9];	/* pad to 72 bytes */
};

/*
 * The whole hybrid state. 192 bytes in the stock BTF: two dies plus a run
 * of u32 ratios.
 */
struct f2fs_hybrid_info {
	struct f2fs_hybrid_die_info main;
	struct f2fs_hybrid_die_info ext;
	unsigned int free_ratio;
	unsigned int gc_threshold;
	unsigned int large_file_threshold;
	unsigned int main_new_seg_ratio;
	unsigned int ext_new_seg_ratio;
	unsigned int gc_alloc_mode;
	unsigned int extension_count;
	unsigned int reserved[5];	/* pad to 192 bytes */
};

#ifdef CONFIG_F2FS_FS_HYBRID_UFS

bool f2fs_hw_support_hybrid(struct f2fs_sb_info *sbi,
			struct f2fs_hybrid_die_info *main,
			struct f2fs_hybrid_die_info *ext);
int f2fs_hw_hybrid_get_avail(struct f2fs_sb_info *sbi, sector_t blk, int *which);
int f2fs_get_alloc_range(struct f2fs_sb_info *sbi,
			struct f2fs_hybrid_die_info **die);
void f2fs_update_die_usage(struct f2fs_sb_info *sbi,
			struct f2fs_hybrid_die_info *die, int used);
int f2fs_init_ext_cursegs(struct f2fs_sb_info *sbi);
void f2fs_disable_hybrid(struct f2fs_sb_info *sbi);
int f2fs_hybrid_info_init(struct f2fs_sb_info *sbi);
void f2fs_hybrid_info_exit(struct f2fs_sb_info *sbi);
int f2fs_update_hybrid_extension_list(struct f2fs_sb_info *sbi,
			const char *name, size_t len, bool set);

#endif /* CONFIG_F2FS_FS_HYBRID_UFS */

#endif /* __F2FS_HYBRID_H__ */
