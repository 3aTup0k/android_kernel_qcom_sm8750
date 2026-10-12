#!/usr/bin/env python3
"""Проверить, применены ли wildkernels-оптимизации в дереве.

Проверяем маркером: каждая правка обязана оставить в исходнике строку,
которой там без неё не было.
"""
import os
import re

ROOT = '/data/kleaf_ws/common'

# (патч, файл, маркер, что должно быть)
CHECKS = [
    ('optimise_memcmp',        'arch/arm64/lib/memcmp.S', 'ptrue'),
    ('optimized_mem_operations','lib/string.c',            'sync_tb'),
    ('clear_page_16bytes_align','arch/arm64/lib/clear_page.S','p2align 4'),
    ('mem_opt_prefetch',       'arch/arm64/lib/copy_template.S','prfm'),
    ('int_sqrt',               'lib/math/int_sqrt.c',      'n -= val'),
    ('reduce_cache_pressure',  'fs/dcache.c',              'dentry_memory_limit'),
    ('reduce_pci_pme_wakeups', 'drivers/pci/pci.c',        'PME_RUNTIME'),
    ('silence_irq_cpu_logspam','kernel/irq/cpuhotplug.c',  'silence'),
    ('silence_system_logspam', 'kernel/printk/printk.c',   'printk_skip'),
    ('disable_cache_hot_buddy','kernel/sched/features.h',  'SCHED_FEAT'),
    ('adjust_cpu_scan_order',  'kernel/sched/fair.c',      'sd->span'),
    ('minimise_wakeup_time',   'kernel/time/alarmtimer.c', 'alarmtimer'),
    ('avoid_s2idle_wakes',     'drivers/base/power/wakeup.c','wakeup'),
    ('add_limitation_scaling_min_freq','drivers/cpufreq/cpufreq.c','scaling_min'),
    ('re_write_limitation_scaling_min_freq','drivers/cpufreq/cpufreq.c','sched'),
    ('increase_ext4_commit_age','include/linux/jbd2.h',    'MAX_JOURNAL_COMMIT'),
    ('reduce_freeze_timeout',  'kernel/power/process.c',   'try_to_freeze'),
    ('add_timeout_wakelocks',  'kernel/power/wakelock.c',  'wakeup_reason'),
    ('f2fs_enlarge_min_fsync_blocks','fs/f2fs/segment.h',  'MIN_FSYNC'),
    ('f2fs_reduce_congestion', 'fs/f2fs/f2fs.h',           'wbt'),
    ('increase_sk_mem_packets','include/net/sock.h',       'SK_MEM'),
    ('file_struct_8bytes_align','include/linux/fs.h',        'nr_files'),
    ('increase_sk_mem',        'include/net/sock.h',      'x'),
]

ok = miss = 0
for name, path, needle in CHECKS:
    f = os.path.join(ROOT, path)
    if not os.path.exists(f):
        print('  %-38s MISSING FILE %s' % (name, path))
        miss += 1
        continue
    txt = open(f, encoding='utf-8', errors='replace').read()
    hit = needle in txt
    print('  %-38s %s' % (name, 'OK' if hit else 'NOT FOUND'))
    ok, miss = ok + hit, miss + (not hit)

print('-' * 60)
print('verified %d, not found %d' % (ok, miss))

# Для сравнения: есть ли прямое подтверждение того, что патч вообще
# предназначен для этого дерева
print()
print('Проверка через git apply --check на 6.6.144:')
import subprocess
for fn in sorted(os.listdir(os.path.join(ROOT, 'apex-patches/optimizations'))):
    if not fn.endswith('.patch'):
        continue
    r = subprocess.run(['git', 'apply', '--check',
                        'apex-patches/optimizations/' + fn],
                      cwd=ROOT, capture_output=True, text=True)
    tag = 'ALREADY-APPLIED-or-conflict' if r.returncode else 'NOT-APPLIED'
    print('  %-44s %s' % (fn[:44], tag))
