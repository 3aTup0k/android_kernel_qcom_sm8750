# Vendor DTS subtree

Vendored from `xiaomi-dada/android_kernel_xiaomi_sm8750-devicetrees`,
branch `lineage-23.2`, copied here verbatim (`Makefile`, `qcom/`,
`bindings/`).

Platform for SM8750 (Xiaomi 15 / dada) is `sun`. Device overlay is
`qcom/dada-overlay.dts`:

    model       = "Qualcomm Technologies, Inc. Haotian based on SM8750"
    compatible  = "qcom,sun-mtp", "qcom,sun", "qcom,sunp-mtp", "qcom,sunp", "qcom,mtp"
    qcom,board-id = <0x40008 0x00>

`dada-overlay.dtbo` is registered in `qcom/Makefile` both via SUN_BOARDS
and via `dtbo-y`.

## Building the DTB/DTBO standalone

The kleaf targets for dtb.img/dtbo.img are not wired in this tree
(`define_msm_platforms()` is disabled), so build them directly:

    cd /data/kleaf_ws/common
    ln -sfn /data/kleaf_ws/common/vendor/qcom/kernel/arch/arm64/boot/dts/vendor \
            arch/arm64/boot/dts/vendor-qcom
    # append 'subdir-y += vendor-qcom' to arch/arm64/boot/dts/Makefile
    make -j$(nproc) O=<out> ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang \
         LD=ld.lld HOSTCC=clang vendor-qcom/dada-overlay.dtbo

NOTE: the symlink MUST be absolute. A relative `../../../../../vendor/...`
resolves one level too high (into /data/kleaf_ws/vendor) and dtc then sees
only `platform_map.bzl`.

Pack the result into a DTBO image with `dtbo_offset = 64` (see
/data/os4-re/dtb_built/mkdtbo.py) - the first version used 32 and the
bootloader could not find the DTB at all.
