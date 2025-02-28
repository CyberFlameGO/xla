# Description:
#   Implementation of XLA’s HLO utilities used for higher-level transformations.

load("@tsl//tsl/platform:rules_cc.bzl", "cc_library")
load(
    "//xla:xla.bzl",
    "xla_cc_test",
)

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = [":friends"],
    licenses = ["notice"],
)

package_group(
    name = "friends",
    includes = [
        "//xla:friends",
    ],
)

cc_library(
    name = "hlo_live_range",
    srcs = [
        "hlo_live_range.cc",
    ],
    hdrs = [
        "hlo_live_range.h",
    ],
    deps = [
        "//xla:statusor",
        "//xla:types",
        "//xla/hlo/ir:hlo",
        "//xla/service:hlo_alias_analysis",
        "//xla/service:hlo_buffer",
        "//xla/service:hlo_dataflow_analysis",
        "//xla/service:hlo_ordering",
        "//xla/service:hlo_value",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/types:span",
        "@tsl//tsl/platform:status",
    ],
)

xla_cc_test(
    name = "hlo_live_range_test",
    srcs = ["hlo_live_range_test.cc"],
    deps = [
        ":hlo_live_range",
        "//xla:literal",
        "//xla:shape_util",
        "//xla:status_macros",
        "//xla:statusor",
        "//xla:test",
        "//xla:test_helpers",
        "//xla:types",
        "//xla:util",
        "//xla:xla_data_proto_cc",
        "//xla/hlo/ir:hlo",
        "//xla/service:call_graph",
        "//xla/service:hlo_alias_analysis",
        "//xla/service:hlo_memory_scheduler",
        "//xla/service:hlo_ordering",
        "//xla/service:hlo_parser",
        "//xla/service:hlo_value",
        "//xla/tests:hlo_test_base",
        "//xla/tests:xla_internal_test_main",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/strings:str_format",
        "@tsl//tsl/lib/core:status_test_util",
        "@tsl//tsl/platform:logging",
        "@tsl//tsl/platform:test",
    ],
)
