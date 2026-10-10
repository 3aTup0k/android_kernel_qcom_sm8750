// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C), 2008-2021, OPPO Mobile Comm Corp., Ltd.
 *             https://www.oppo.com/
 */
#include <linux/sysfs.h>
#include <linux/kobject.h>

#include "internal.h"


enum {
	attr_feature,
	attr_pointer_ui,
	attr_pointer_bool,
#ifdef CONFIG_XIAOMI_EROFS_IOSTAT
	attr_iostat_enable,
	attr_iostat_config,
	attr_iostat_window_latency,
	attr_iostat_daily_latency,
#endif
};

enum {
	struct_erofs_sb_info,
	struct_erofs_mount_opts,
};

struct erofs_attr {
	struct attribute attr;
	short attr_id;
	int struct_type, offset;
};

#define EROFS_ATTR(_name, _mode, _id)					\
static struct erofs_attr erofs_attr_##_name = {				\
	.attr = {.name = __stringify(_name), .mode = _mode },		\
	.attr_id = attr_##_id,						\
}
#define EROFS_ATTR_FUNC(_name, _mode)	EROFS_ATTR(_name, _mode, _name)
#define EROFS_ATTR_FEATURE(_name)	EROFS_ATTR(_name, 0444, feature)

#define EROFS_ATTR_OFFSET(_name, _mode, _id, _struct)	\
static struct erofs_attr erofs_attr_##_name = {			\
	.attr = {.name = __stringify(_name), .mode = _mode },	\
	.attr_id = attr_##_id,					\
	.struct_type = struct_##_struct,			\
	.offset = offsetof(struct _struct, _name),\
}

#define EROFS_ATTR_RW(_name, _id, _struct)	\
	EROFS_ATTR_OFFSET(_name, 0644, _id, _struct)

#define EROFS_RO_ATTR(_name, _id, _struct)	\
	EROFS_ATTR_OFFSET(_name, 0444, _id, _struct)

#define EROFS_ATTR_RW_UI(_name, _struct)	\
	EROFS_ATTR_RW(_name, pointer_ui, _struct)

#define EROFS_ATTR_RW_BOOL(_name, _struct)	\
	EROFS_ATTR_RW(_name, pointer_bool, _struct)

#define ATTR_LIST(name) (&erofs_attr_##name.attr)

#ifdef CONFIG_EROFS_FS_ZIP
EROFS_ATTR_RW_UI(sync_decompress, erofs_mount_opts);
#endif

/*
 * HyperOS 4 I/O statistics. The implementation is fs/erofs/iostat.c; that
 * file spells out what was recovered from the stock image and what is our own
 * choice.
 */
#ifdef CONFIG_XIAOMI_EROFS_IOSTAT
EROFS_ATTR(iostat_enable, 0644, iostat_enable);
EROFS_ATTR(iostat_config, 0644, iostat_config);
EROFS_ATTR(iostat_window_latency, 0644, iostat_window_latency);
EROFS_ATTR(iostat_daily_latency, 0444, iostat_daily_latency);
#endif

static struct attribute *erofs_attrs[] = {
#ifdef CONFIG_EROFS_FS_ZIP
	ATTR_LIST(sync_decompress),
#endif
#ifdef CONFIG_XIAOMI_EROFS_IOSTAT
	ATTR_LIST(iostat_enable),
	ATTR_LIST(iostat_config),
	ATTR_LIST(iostat_window_latency),
	ATTR_LIST(iostat_daily_latency),
#endif
	NULL,
};
ATTRIBUTE_GROUPS(erofs);

/* Features this copy of erofs supports */
EROFS_ATTR_FEATURE(zero_padding);
EROFS_ATTR_FEATURE(compr_cfgs);
EROFS_ATTR_FEATURE(big_pcluster);
EROFS_ATTR_FEATURE(chunked_file);
EROFS_ATTR_FEATURE(device_table);
EROFS_ATTR_FEATURE(compr_head2);
EROFS_ATTR_FEATURE(sb_chksum);
EROFS_ATTR_FEATURE(ztailpacking);
EROFS_ATTR_FEATURE(fragments);
EROFS_ATTR_FEATURE(dedupe);

static struct attribute *erofs_feat_attrs[] = {
	ATTR_LIST(zero_padding),
	ATTR_LIST(compr_cfgs),
	ATTR_LIST(big_pcluster),
	ATTR_LIST(chunked_file),
	ATTR_LIST(device_table),
	ATTR_LIST(compr_head2),
	ATTR_LIST(sb_chksum),
	ATTR_LIST(ztailpacking),
	ATTR_LIST(fragments),
	ATTR_LIST(dedupe),
	NULL,
};
ATTRIBUTE_GROUPS(erofs_feat);

static unsigned char *__struct_ptr(struct erofs_sb_info *sbi,
					  int struct_type, int offset)
{
	if (struct_type == struct_erofs_sb_info)
		return (unsigned char *)sbi + offset;
	if (struct_type == struct_erofs_mount_opts)
		return (unsigned char *)&sbi->opt + offset;
	return NULL;
}

static ssize_t erofs_attr_show(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct erofs_sb_info *sbi = container_of(kobj, struct erofs_sb_info,
						s_kobj);
	struct erofs_attr *a = container_of(attr, struct erofs_attr, attr);
	unsigned char *ptr = __struct_ptr(sbi, a->struct_type, a->offset);

	switch (a->attr_id) {
	case attr_feature:
		return sysfs_emit(buf, "supported\n");
	case attr_pointer_ui:
		if (!ptr)
			return 0;
		return sysfs_emit(buf, "%u\n", *(unsigned int *)ptr);
	case attr_pointer_bool:
		if (!ptr)
			return 0;
		return sysfs_emit(buf, "%d\n", *(bool *)ptr);
#ifdef CONFIG_XIAOMI_EROFS_IOSTAT
	case attr_iostat_enable:
		return sysfs_emit(buf, "%d\n",
				erofs_iostat_enable_get(&sbi->iostat));
	case attr_iostat_config:
		return erofs_iostat_config_show(&sbi->iostat, buf);
	case attr_iostat_window_latency:
		return erofs_iostat_window_report(&sbi->iostat, buf);
	case attr_iostat_daily_latency:
		return erofs_iostat_daily_report(&sbi->iostat, buf);
#endif
	}
	return 0;
}

static ssize_t erofs_attr_store(struct kobject *kobj, struct attribute *attr,
						const char *buf, size_t len)
{
	struct erofs_sb_info *sbi = container_of(kobj, struct erofs_sb_info,
						s_kobj);
	struct erofs_attr *a = container_of(attr, struct erofs_attr, attr);
	unsigned char *ptr = __struct_ptr(sbi, a->struct_type, a->offset);
	unsigned long t;
	int ret;

	switch (a->attr_id) {
	case attr_pointer_ui:
		if (!ptr)
			return 0;
		ret = kstrtoul(skip_spaces(buf), 0, &t);
		if (ret)
			return ret;
		if (t != (unsigned int)t)
			return -ERANGE;
#ifdef CONFIG_EROFS_FS_ZIP
		if (!strcmp(a->attr.name, "sync_decompress") &&
		    (t > EROFS_SYNC_DECOMPRESS_FORCE_OFF))
			return -EINVAL;
#endif
		*(unsigned int *)ptr = t;
		return len;
	case attr_pointer_bool:
		if (!ptr)
			return 0;
		ret = kstrtoul(skip_spaces(buf), 0, &t);
		if (ret)
			return ret;
		if (t != 0 && t != 1)
			return -EINVAL;
		*(bool *)ptr = !!t;
		return len;
#ifdef CONFIG_XIAOMI_EROFS_IOSTAT
	case attr_iostat_enable: {
		int v;

		ret = kstrtoint(skip_spaces(buf), 0, &v);
		if (ret)
			return ret;
		if (v != 0 && v != 1)
			return -EINVAL;
		ret = erofs_iostat_enable_set(&sbi->iostat, v);
		if (ret)
			return ret;
		return len;
	}
	case attr_iostat_config:
		return erofs_iostat_config_store(&sbi->iostat, buf, len);
	case attr_iostat_window_latency:
		/*
		 * The stock image has a store handler for this node as well
		 * (erofs_iostat_latency_stats_reset); any write starts a fresh
		 * window, which is what a write to a report node can mean.
		 */
		if (!buf)
			return -EINVAL;
		erofs_iostat_reset(&sbi->iostat);
		return len;
#endif
	}
	return 0;
}

static void erofs_sb_release(struct kobject *kobj)
{
	struct erofs_sb_info *sbi = container_of(kobj, struct erofs_sb_info,
						 s_kobj);
	complete(&sbi->s_kobj_unregister);
}

static const struct sysfs_ops erofs_attr_ops = {
	.show	= erofs_attr_show,
	.store	= erofs_attr_store,
};

static const struct kobj_type erofs_sb_ktype = {
	.default_groups = erofs_groups,
	.sysfs_ops	= &erofs_attr_ops,
	.release	= erofs_sb_release,
};

static const struct kobj_type erofs_ktype = {
	.sysfs_ops	= &erofs_attr_ops,
};

static struct kset erofs_root = {
	.kobj	= {.ktype = &erofs_ktype},
};

static const struct kobj_type erofs_feat_ktype = {
	.default_groups = erofs_feat_groups,
	.sysfs_ops	= &erofs_attr_ops,
};

static struct kobject erofs_feat = {
	.kset	= &erofs_root,
};

int erofs_register_sysfs(struct super_block *sb)
{
	struct erofs_sb_info *sbi = EROFS_SB(sb);
	char *name;
	char *str = NULL;
	int err;

	if (erofs_is_fscache_mode(sb)) {
		if (sbi->domain_id) {
			str = kasprintf(GFP_KERNEL, "%s,%s", sbi->domain_id,
					sbi->fsid);
			if (!str)
				return -ENOMEM;
			name = str;
		} else {
			name = sbi->fsid;
		}
	} else {
		name = sb->s_id;
	}
	sbi->s_kobj.kset = &erofs_root;
	init_completion(&sbi->s_kobj_unregister);
	err = kobject_init_and_add(&sbi->s_kobj, &erofs_sb_ktype, NULL, "%s", name);
	kfree(str);
	if (err)
		goto put_sb_kobj;
	return 0;

put_sb_kobj:
	kobject_put(&sbi->s_kobj);
	wait_for_completion(&sbi->s_kobj_unregister);
	return err;
}

void erofs_unregister_sysfs(struct super_block *sb)
{
	struct erofs_sb_info *sbi = EROFS_SB(sb);

	if (sbi->s_kobj.state_in_sysfs) {
		kobject_del(&sbi->s_kobj);
		kobject_put(&sbi->s_kobj);
		wait_for_completion(&sbi->s_kobj_unregister);
	}
}

int __init erofs_init_sysfs(void)
{
	int ret;

	kobject_set_name(&erofs_root.kobj, "erofs");
	erofs_root.kobj.parent = fs_kobj;
	ret = kset_register(&erofs_root);
	if (ret)
		goto root_err;

	ret = kobject_init_and_add(&erofs_feat, &erofs_feat_ktype,
				   NULL, "features");
	if (ret)
		goto feat_err;
	return ret;

feat_err:
	kobject_put(&erofs_feat);
	kset_unregister(&erofs_root);
root_err:
	return ret;
}

void erofs_exit_sysfs(void)
{
	kobject_put(&erofs_feat);
	kset_unregister(&erofs_root);
}
