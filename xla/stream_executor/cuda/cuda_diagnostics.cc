/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/stream_executor/cuda/cuda_diagnostics.h"

#if !defined(PLATFORM_WINDOWS)
#include <dirent.h>
#endif

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <IOKit/kext/KextManager.h>
#include <mach-o/dyld.h>
#else
#if !defined(PLATFORM_WINDOWS)
#include <link.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif
#include <sys/stat.h>
#endif
#include <algorithm>
#include <memory>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "xla/stream_executor/platform/logging.h"
#include "tsl/platform/host_info.h"
#include "tsl/platform/status.h"

namespace stream_executor {
namespace cuda {

std::string DriverVersionToString(DriverVersion version) {
  return absl::StrFormat("%d.%d.%d", std::get<0>(version), std::get<1>(version),
                         std::get<2>(version));
}

std::string DriverVersionStatusToString(tsl::StatusOr<DriverVersion> version) {
  if (!version.ok()) {
    return version.status().ToString();
  }

  return DriverVersionToString(version.value());
}

tsl::StatusOr<DriverVersion> StringToDriverVersion(const std::string &value) {
  std::vector<std::string> pieces = absl::StrSplit(value, '.');
  if (pieces.size() < 2 || pieces.size() > 4) {
    return tsl::Status(
        absl::StatusCode::kInvalidArgument,
        absl::StrFormat(
            "expected %%d.%%d, %%d.%%d.%%d, or %%d.%%d.%%d.%%d form "
            "for driver version; got \"%s\"",
            value.c_str()));
  }

  int major;
  int minor;
  int patch = 0;
  if (!absl::SimpleAtoi(pieces[0], &major)) {
    return tsl::Status(
        absl::StatusCode::kInvalidArgument,
        absl::StrFormat("could not parse major version number \"%s\" as an "
                        "integer from string \"%s\"",
                        pieces[0], value));
  }
  if (!absl::SimpleAtoi(pieces[1], &minor)) {
    return tsl::Status(
        absl::StatusCode::kInvalidArgument,
        absl::StrFormat("could not parse minor version number \"%s\" as an "
                        "integer from string \"%s\"",
                        pieces[1].c_str(), value.c_str()));
  }
  if (pieces.size() == 3 && !absl::SimpleAtoi(pieces[2], &patch)) {
    return tsl::Status(
        absl::StatusCode::kInvalidArgument,
        absl::StrFormat("could not parse patch version number \"%s\" as an "
                        "integer from string \"%s\"",
                        pieces[2], value));
  }

  DriverVersion result{major, minor, patch};
  VLOG(2) << "version string \"" << value << "\" made value "
          << DriverVersionToString(result);
  return result;
}

}  // namespace cuda
}  // namespace stream_executor

namespace stream_executor {
namespace gpu {

#ifdef __APPLE__
static const CFStringRef kDriverKextIdentifier = CFSTR("com.nvidia.CUDA");
#elif !defined(PLATFORM_WINDOWS)
static const char *kDriverVersionPath = "/proc/driver/nvidia/version";
#endif

// -- class Diagnostician

std::string Diagnostician::GetDevNodePath(int dev_node_ordinal) {
  return absl::StrCat("/dev/nvidia", dev_node_ordinal);
}

void Diagnostician::LogDiagnosticInformation() {
#ifdef __APPLE__
  CFStringRef kext_ids[1];
  kext_ids[0] = kDriverKextIdentifier;
  CFArrayRef kext_id_query = CFArrayCreate(nullptr, (const void **)kext_ids, 1,
                                           &kCFTypeArrayCallBacks);
  CFDictionaryRef kext_infos =
      KextManagerCopyLoadedKextInfo(kext_id_query, nullptr);
  CFRelease(kext_id_query);

  CFDictionaryRef cuda_driver_info = nullptr;
  if (CFDictionaryGetValueIfPresent(kext_infos, kDriverKextIdentifier,
                                    (const void **)&cuda_driver_info)) {
    bool started = CFBooleanGetValue((CFBooleanRef)CFDictionaryGetValue(
        cuda_driver_info, CFSTR("OSBundleStarted")));
    if (!started) {
      LOG(INFO) << "kernel driver is installed, but does not appear to be "
                   "running on this host "
                << "(" << tsl::port::Hostname() << ")";
    }
  } else {
    LOG(INFO) << "kernel driver does not appear to be installed on this host "
              << "(" << tsl::port::Hostname() << ")";
  }
  CFRelease(kext_infos);
#elif !defined(PLATFORM_WINDOWS)
  if (access(kDriverVersionPath, F_OK) != 0) {
    VLOG(1) << "kernel driver does not appear to be running on this host "
            << "(" << tsl::port::Hostname() << "): "
            << "/proc/driver/nvidia/version does not exist";
    return;
  }
  auto dev0_path = GetDevNodePath(0);
  if (access(dev0_path.c_str(), F_OK) != 0) {
    VLOG(1) << "no NVIDIA GPU device is present: " << dev0_path
            << " does not exist";
    return;
  }
#endif

  LOG(INFO) << "retrieving CUDA diagnostic information for host: "
            << tsl::port::Hostname();

  LogDriverVersionInformation();
}

/* static */ void Diagnostician::LogDriverVersionInformation() {
  LOG(INFO) << "hostname: " << tsl::port::Hostname();
#ifndef PLATFORM_WINDOWS
  if (VLOG_IS_ON(1)) {
    const char *value = getenv("LD_LIBRARY_PATH");
    std::string library_path = value == nullptr ? "" : value;
    VLOG(1) << "LD_LIBRARY_PATH is: \"" << library_path << "\"";

    std::vector<std::string> pieces = absl::StrSplit(library_path, ':');
    for (const auto &piece : pieces) {
      if (piece.empty()) {
        continue;
      }
      DIR *dir = opendir(piece.c_str());
      if (dir == nullptr) {
        VLOG(1) << "could not open \"" << piece << "\"";
        continue;
      }
      while (dirent *entity = readdir(dir)) {
        VLOG(1) << piece << " :: " << entity->d_name;
      }
      closedir(dir);
    }
  }
  tsl::StatusOr<DriverVersion> dso_version = FindDsoVersion();
  LOG(INFO) << "libcuda reported version is: "
            << cuda::DriverVersionStatusToString(dso_version);

  tsl::StatusOr<DriverVersion> kernel_version = FindKernelDriverVersion();
  LOG(INFO) << "kernel reported version is: "
            << cuda::DriverVersionStatusToString(kernel_version);
#endif

  // OS X kernel driver does not report version accurately
#if !defined(__APPLE__) && !defined(PLATFORM_WINDOWS)
  if (kernel_version.ok() && dso_version.ok()) {
    WarnOnDsoKernelMismatch(dso_version, kernel_version);
  }
#endif
}

// Iterates through loaded DSOs with DlIteratePhdrCallback to find the
// driver-interfacing DSO version number. Returns it as a string.
tsl::StatusOr<DriverVersion> Diagnostician::FindDsoVersion() {
  tsl::StatusOr<DriverVersion> result(tsl::Status(
      tsl::error::NOT_FOUND,
      "was unable to find libcuda.so DSO loaded into this program"));

#if defined(__APPLE__)
  // OSX CUDA libraries have names like: libcuda_310.41.15_mercury.dylib
  const string prefix("libcuda_");
  const string suffix("_mercury.dylib");
  for (uint32_t image_index = 0; image_index < _dyld_image_count();
       ++image_index) {
    const string path(_dyld_get_image_name(image_index));
    const size_t suffix_pos = path.rfind(suffix);
    const size_t prefix_pos = path.rfind(prefix, suffix_pos);
    if (prefix_pos == string::npos || suffix_pos == string::npos) {
      // no match
      continue;
    }
    const size_t start = prefix_pos + prefix.size();
    if (start >= suffix_pos) {
      // version not included
      continue;
    }
    const size_t length = suffix_pos - start;
    const string version = path.substr(start, length);
    result = cuda::StringToDriverVersion(version);
  }
#else
#if !defined(PLATFORM_WINDOWS) && !defined(ANDROID_TEGRA)
  // Callback used when iterating through DSOs. Looks for the driver-interfacing
  // DSO and yields its version number into the callback data, when found.
  auto iterate_phdr =
      [](struct dl_phdr_info *info, size_t size, void *data) -> int {
    if (strstr(info->dlpi_name, "libcuda.so.1")) {
      VLOG(1) << "found DLL info with name: " << info->dlpi_name;
      char resolved_path[PATH_MAX] = {0};
      if (realpath(info->dlpi_name, resolved_path) == nullptr) {
        return 0;
      }
      VLOG(1) << "found DLL info with resolved path: " << resolved_path;
      const char *slash = rindex(resolved_path, '/');
      if (slash == nullptr) {
        return 0;
      }
      const char *so_suffix = ".so.";
      const char *dot = strstr(slash, so_suffix);
      if (dot == nullptr) {
        return 0;
      }
      std::string dso_version = dot + strlen(so_suffix);
      // TODO(b/22689637): Eliminate the explicit namespace if possible.
      auto stripped_dso_version = absl::StripSuffix(dso_version, ".ld64");
      auto result = static_cast<tsl::StatusOr<DriverVersion> *>(data);
      *result = cuda::StringToDriverVersion(std::string(stripped_dso_version));
      return 1;
    }
    return 0;
  };

  dl_iterate_phdr(iterate_phdr, &result);
#endif
#endif

  return result;
}

tsl::StatusOr<DriverVersion> Diagnostician::FindKernelModuleVersion(
    const std::string &driver_version_file_contents) {
  static const char *kDriverFilePrelude = "Kernel Module  ";
  size_t offset = driver_version_file_contents.find(kDriverFilePrelude);
  if (offset == std::string::npos) {
    return tsl::Status(
        tsl::error::NOT_FOUND,
        absl::StrCat("could not find kernel module information in "
                     "driver version file contents: \"",
                     driver_version_file_contents, "\""));
  }

  std::string version_and_rest = driver_version_file_contents.substr(
      offset + strlen(kDriverFilePrelude), std::string::npos);
  size_t space_index = version_and_rest.find(' ');
  auto kernel_version = version_and_rest.substr(0, space_index);
  // TODO(b/22689637): Eliminate the explicit namespace if possible.
  auto stripped_kernel_version = absl::StripSuffix(kernel_version, ".ld64");
  return cuda::StringToDriverVersion(std::string(stripped_kernel_version));
}

void Diagnostician::WarnOnDsoKernelMismatch(
    tsl::StatusOr<DriverVersion> dso_version,
    tsl::StatusOr<DriverVersion> kernel_version) {
  if (kernel_version.ok() && dso_version.ok() &&
      dso_version.value() == kernel_version.value()) {
    LOG(INFO) << "kernel version seems to match DSO: "
              << cuda::DriverVersionToString(kernel_version.value());
  } else {
    LOG(ERROR) << "kernel version "
               << cuda::DriverVersionStatusToString(kernel_version)
               << " does not match DSO version "
               << cuda::DriverVersionStatusToString(dso_version)
               << " -- cannot find working devices in this configuration";
  }
}

tsl::StatusOr<DriverVersion> Diagnostician::FindKernelDriverVersion() {
#if defined(__APPLE__)
  CFStringRef kext_ids[1];
  kext_ids[0] = kDriverKextIdentifier;
  CFArrayRef kext_id_query = CFArrayCreate(nullptr, (const void **)kext_ids, 1,
                                           &kCFTypeArrayCallBacks);
  CFDictionaryRef kext_infos =
      KextManagerCopyLoadedKextInfo(kext_id_query, nullptr);
  CFRelease(kext_id_query);

  CFDictionaryRef cuda_driver_info = nullptr;
  if (CFDictionaryGetValueIfPresent(kext_infos, kDriverKextIdentifier,
                                    (const void **)&cuda_driver_info)) {
    // NOTE: OSX CUDA driver does not currently store the same driver version
    // in kCFBundleVersionKey as is returned by cuDriverGetVersion
    CFRelease(kext_infos);
    const CFStringRef str = (CFStringRef)CFDictionaryGetValue(
        cuda_driver_info, kCFBundleVersionKey);
    const char *version = CFStringGetCStringPtr(str, kCFStringEncodingUTF8);

    // version can be NULL in which case treat it as empty string
    // see
    // https://developer.apple.com/library/mac/documentation/CoreFoundation/Conceptual/CFStrings/Articles/AccessingContents.html#//apple_ref/doc/uid/20001184-100980-TPXREF112
    if (version == NULL) {
      return cuda::StringToDriverVersion("");
    }
    return cuda::StringToDriverVersion(version);
  }
  CFRelease(kext_infos);
  auto status = tsl::Status(
      tsl::error::INTERNAL,
      absl::StrCat(
          "failed to read driver bundle version: ",
          CFStringGetCStringPtr(kDriverKextIdentifier, kCFStringEncodingUTF8)));
  return status;
#elif defined(PLATFORM_WINDOWS)
  auto status =
      tsl::Status(tsl::error::UNIMPLEMENTED,
                  "kernel reported driver version not implemented on Windows");
  return status;
#else
  FILE *driver_version_file = fopen(kDriverVersionPath, "r");
  if (driver_version_file == nullptr) {
    return tsl::Status(
        tsl::error::PERMISSION_DENIED,
        absl::StrCat("could not open driver version path for reading: ",
                     kDriverVersionPath));
  }

  static const int kContentsSize = 1024;
  absl::InlinedVector<char, 4> contents(kContentsSize);
  size_t retcode =
      fread(contents.begin(), 1, kContentsSize - 2, driver_version_file);
  if (retcode < kContentsSize - 1) {
    contents[retcode] = '\0';
  }
  contents[kContentsSize - 1] = '\0';

  if (retcode != 0) {
    VLOG(1) << "driver version file contents: \"\"\"" << contents.begin()
            << "\"\"\"";
    fclose(driver_version_file);
    return FindKernelModuleVersion(contents.begin());
  }

  auto status = tsl::Status(
      tsl::error::INTERNAL,
      absl::StrCat(
          "failed to read driver version file contents: ", kDriverVersionPath,
          "; ferror: ", ferror(driver_version_file)));
  fclose(driver_version_file);
  return status;
#endif
}

}  // namespace gpu
}  // namespace stream_executor
