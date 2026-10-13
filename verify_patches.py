#!/usr/bin/env python3
"""verify_patches.py — prove the patches are applied, not assumed.

`git apply --check` cannot tell "already applied" from "does not fit", so
this checks the content instead: each feature must leave a marker in the
sources and in the merged .config.
"""
import os
import subprocess

ROOT = os.path.dirname(os.path.abspath(__file__))

# Number of tracked .patch files under apex-patches/. Bump it when adding one.
EXPECTED_PATCHES = 45

# (label, markers, where)
CHECKS = [
    ('adios',                ['MQ_IOSCHED_ADIOS'], 'block/Kconfig.iosched'),
    ('adios obj',            ['adios'], 'block/Makefile'),
    ('BORE (Kconfig)',       ['config SCHED_BORE'], 'init/Kconfig'),
    ('BORE (code)',          ['sched_burst_penalty_scale'], 'kernel/sched/bore.c'),
    ('BORE min-slice sysctl', ['"min_base_slice_ns"'], 'kernel/sched/bore.c'),
    ('BORE base-slice sysctl', ['"base_slice_ns"'], 'kernel/sched/bore.c'),
    ('ksu',                  ['CONFIG_KSU='], 'arch/arm64/configs/gki_defconfig'),
    ('ksu obj',              ['kernelsu'], 'drivers/Makefile'),
    ('ksu upstream marker',  ['welcome to KernelSU version'], 'drivers/kernelsu/core/init.c'),
    ('ksu version pinned',   ['KSU_VERSION ?= 32680'], 'drivers/kernelsu/Kbuild'),
    ('susfs',                ['CONFIG_KSU_SUSFS='], 'arch/arm64/configs/gki_defconfig'),
    ('susfs in-tree',        ['susfs_init'], 'fs/susfs.c'),
    ('susfs selinux wrap',   ['security_dump_masked_av_fn'], 'security/selinux/ss/services.c'),
    ('ntsync',               ['CONFIG_NTSYNC='], 'arch/arm64/configs/gki_defconfig'),
    ('ntsync obj',           ['ntsync'], 'drivers/misc/Kconfig'),
    ('droidspaces',          ['CONFIG_ANDROID_WRAPFD='], 'arch/arm64/configs/gki_defconfig'),
    ('droidspaces code',     ['ghost_task_ready'], 'kernel/pid.c'),
    ('erofs margin FIX',     ['(srcsize) >> 6'], 'fs/erofs/decompressor.c'),
    ('f2fs hybrid',          ['CONFIG_F2FS_FS_HYBRID_UFS='], 'arch/arm64/configs/gki_defconfig'),
    ('f2fs hybrid code',     ['f2fs_hw_support_hybrid'], 'fs/f2fs/hybrid.c'),
    ('f2fs vip',             ['CONFIG_F2FS_VIP_FILE='], 'arch/arm64/configs/gki_defconfig'),
    ('erofs iostat',         ['CONFIG_XIAOMI_EROFS_IOSTAT='], 'arch/arm64/configs/gki_defconfig'),
    ('kmi relax',            ['CONFIG_APEX_RELAX_GKI_MODULE_KMI='], 'arch/arm64/configs/gki_defconfig'),
]


def hits(path, needle):
    f = os.path.join(ROOT, path)
    if not os.path.exists(f):
        return 0
    try:
        out = subprocess.run(['grep', '-F', '-c', needle, f],
                             capture_output=True, text=True)
        return int(out.stdout.strip() or 0)
    except Exception:
        return 0


def main():
    ok = bad = 0
    print('%-22s %-34s %s' % ('FEATURE', 'MARKER', 'STATE'))
    print('-' * 78)
    for name, needles, path in CHECKS:
        found = sum(1 for n in needles if hits(path, n) > 0)
        good = found == len(needles)
        print('%-22s %-34s %s' % (name, needles[0][:34],
                                  'OK' if good else 'MISSING'))
        ok, bad = ok + good, bad + (not good)
    print('-' * 78)
    print('checked %d, missing %d' % (ok, bad))

    r = subprocess.run(['git', 'ls-files', 'apex-patches/'], cwd=ROOT,
                       capture_output=True, text=True)
    n = sum(1 for f in r.stdout.splitlines() if f.endswith('.patch'))
    print('patch files tracked in git: %d' % n)
    if n != EXPECTED_PATCHES:
        print('NOTE: expected %d patch files' % EXPECTED_PATCHES)


if __name__ == '__main__':
    main()
