# Root: upstream KernelSU with in-tree SUSFS

v1.2 replaces SukiSU Ultra with upstream
[`tiann/KernelSU`](https://github.com/tiann/KernelSU) `main`, with SUSFS
grafted in from [`simonpunk/susfs4ksu`](https://gitlab.com/simonpunk/susfs4ksu),
`gki-android15-6.6` tree.

## Why upstream KernelSU and not KernelSU-Next

The plan for v1.2 was KernelSU-Next `dev`. It was rejected on evidence.

KernelSU-Next `dev` has **no in-tree SUSFS**. It vendors SUSFS as its own
loadable module under `kernel/susfs/` (`obj-m += susfs_guard_lkm.o`) and wires
its KSU core to call into that module - `susfs_supercall_init`,
`susfs_kstat_init`, `susfs_sus_mount_init`, `susfs_show_version` and about
ninety more symbols. None of those exist in `fs/susfs.c`, so "KernelSU-Next
with built-in SUSFS" is not a combination that branch supports. KernelSU-Next
also fails to build against the susfs4ksu patches.

Upstream `main` is the base those patches are written against. Its
`kernel/Kbuild` blob is `6c92a0130cdc`, which is exactly the pre-image recorded
in `10_enable_susfs_for_ksu.patch`, and that patch applies with **0 of 97
hunks failing**.

Vendored at commit `0c0fdb96` of `tiann/KernelSU` `main` (2680 commits,
reported as KSU_VERSION 32680).

## The patches in this directory

| File | What it is |
|---|---|
| `10_enable_susfs_for_ksu.patch` | Upstream susfs4ksu. Adds the SUSFS wiring and the `CONFIG_KSU_SUSFS*` Kconfig menu to the KSU driver. Apply with `a/kernel/` rewritten to `a/drivers/kernelsu/`. |
| `50_add_susfs_in_gki-android15-6.6.patch` | Upstream susfs4ksu. The kernel-side SUSFS integration: hooks in `fs/`, `kernel/`, `security/`, `mm/`, and the two wrapper functions in `security/selinux/ss/services.c`. |
| `Kbuild.reference` | The driver `Kbuild` as patched and built here, kept because two hunks of it are local. |

Both patches are byte-for-byte what susfs4ksu ships. Neither is modified.

## How to reproduce

```bash
# 1. vendor the driver and its UAPI headers
cp -r <KernelSU main>/kernel         common/drivers/kernelsu
mkdir -p common/drivers/uapi
cp <KernelSU main>/uapi/*.h          common/drivers/uapi/
#    include/uapi inside the driver is a symlink to ../../uapi, which points
#    outside the repo once kernel/ lives under drivers/

# 2. SUSFS wiring into the driver
sed -e 's|a/kernel/|a/drivers/kernelsu/|g' \
    -e 's|b/kernel/|b/drivers/kernelsu/|g' \
    10_enable_susfs_for_ksu.patch > /tmp/10.patch
patch -p1 -d common < /tmp/10.patch

# 3. kernel-side SUSFS hooks
patch -p1 -d common < 50_add_susfs_in_gki-android15-6.6.patch

# 4. the patch does not create these three files - copy them in
cp <susfs4ksu>/kernel_patches/fs/susfs.c               common/fs/
cp <susfs4ksu>/kernel_patches/include/linux/susfs.h    common/include/linux/
cp <susfs4ksu>/kernel_patches/include/linux/susfs_def.h common/include/linux/
```

Then enable `CONFIG_KSU=y` and the ten `CONFIG_KSU_SUSFS*` options in
`arch/arm64/configs/gki_defconfig`.

## Three build fixes, and why each is correct

These are forced by the patch, not tuning. Each one is justified by the
compiler or by the tree, and each is reported because leaving them out would
be a silent patch.

### 1. `drivers/kernelsu/feature/selinux_hide.c`

The patch guards two calls with `if (fn)`:

```c
if (security_dump_masked_av_fn)
        security_dump_masked_av_fn(...);
```

But `50_add_susfs` adds those names in `security/selinux/ss/services.c` as
**real functions**, not function pointers, so the test is always true. clang
refuses it:

```
error: address of function 'security_dump_masked_av_fn' will always evaluate
       to 'true' [-Werror,-Wpointer-bool-conversion]
```

The second one is worse: its `else` branch calls `context_struct_compute_av`
directly, and that symbol is `static` in `services.c`, so it would not link.
Both guards are removed and the calls kept.

### 2. `drivers/kernelsu/Kbuild`

The version falls back to a sentinel when the git-derived path does not fire:

```
ccflags-y += -DKSU_VERSION=16
```

kleaf builds from a sandboxed copy of the tree with no `.git`, so the
git-derived path never runs and the manager reads 16 - which its own numbering
treats as an ancient kernel. It is pinned to the vendored revision instead:

```
KSU_VERSION ?= 32680   # 30000 + 2680 commits
```

### 3. `fs/exec.c`

The patch declares `__ksu_is_allow_uid_for_current`, a SukiSU-only symbol. It
has no caller in this tree and upstream KernelSU does not define it, so the
declaration is removed.

## What was verified in the built image

```
welcome to KernelSU version 32680
Linux version 6.6.144-android15-8-4k-APEX-Foundry-v1.2
strings Image | grep susfs:   -> present
```

SUSFS is in-tree: no LKM to load, no Magisk module, no `init_boot.img` patch.
