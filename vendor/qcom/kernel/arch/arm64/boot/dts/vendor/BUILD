# Vendor DTS subtree, vendored in-tree from
# xiaomi-dada/android_kernel_xiaomi_sm8750-devicetrees (branch lineage-23.2).
exports_files(["qcom/platform_map.bzl"])

filegroup(
    name = "msm_dt",
    srcs = glob(
        [
            "Makefile",
            "qcom/**/*.dtsi",
            "qcom/**/*.dts",
            "qcom/**/Makefile",
            "qcom/**/*.dtsi",
            "bindings/**/*",
        ],
        exclude = [
            "**/.*",
            "**/.*/**",
            "**/*.bazel",
            "**/*.bzl",
        ],
    ),
    visibility = ["//visibility:public"],
)
