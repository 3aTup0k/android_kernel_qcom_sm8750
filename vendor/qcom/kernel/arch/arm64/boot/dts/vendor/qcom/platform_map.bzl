# Stub: the QCOM vendor DTS subtree is not part of the LineageOS kernel
# repository. We only build the kernel Image for this device - the DTB/DTBO
# images come from the stock firmware partition, so empty lists are correct.
def get_dtb_list(target):
    return []

def get_dtbo_list(target):
    return []
