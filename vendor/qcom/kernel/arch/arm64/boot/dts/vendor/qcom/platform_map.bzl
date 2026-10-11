# Real platform map: DTS sources are now vendored in-tree from
# xiaomi-dada/android_kernel_xiaomi_sm8750-devicetrees (branch lineage-23.2),
# copied into this directory.
#
# Platform for SM8750 (Xiaomi 15 / dada) is "sun". Base DTBs are sun.dtb /
# sun-v2.dtb / sun-tp.dtb / sun-tp-v2.dtb; the device-specific overlay is
# dada-overlay.dtbo (qcom/dada-overlay.dts, compatible "qcom,sun-mtp",
# qcom,board-id = <0x40008 0x00>).
#
# This replaces the previous empty stub, which forced the DTB/DTBO to come
# from the stock firmware partition. Building them here means a user only
# has to flash our kernel.

def get_dtb_list(target):
    return [
        "sun.dtb",
        "sun-v2.dtb",
        "sun-tp.dtb",
        "sun-tp-v2.dtb",
    ]


def get_dtbo_list(target):
    return [
        "dada-overlay.dtbo",
    ]
