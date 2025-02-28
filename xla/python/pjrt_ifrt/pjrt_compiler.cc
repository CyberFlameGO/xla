/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/python/pjrt_ifrt/pjrt_compiler.h"

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "xla/pjrt/mlir_to_hlo.h"
#include "xla/python/pjrt_ifrt/pjrt_client.h"
#include "xla/python/pjrt_ifrt/pjrt_executable.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace ifrt {

char PjRtCompiler::ID = 0;

StatusOr<std::unique_ptr<LoadedExecutable>> PjRtCompiler::Compile(
    mlir::ModuleOp mlir_module, CompileOptions options) {
  DCHECK(this);
  return PjRtLoadedExecutable::Create(client_, mlir_module, std::move(options));
}

StatusOr<std::unique_ptr<LoadedExecutable>>
PjRtCompiler::DeserializeLoadedExecutable(absl::string_view serialized,
                                          CompileOptions options) {
  DCHECK(this);
  TF_ASSIGN_OR_RETURN(auto pjrt_loaded_executble,
                      client_->pjrt_client()->DeserializeExecutable(
                          serialized, std::move(options)));
  return PjRtLoadedExecutable::Create(client_,
                                      std::move(pjrt_loaded_executble));
}

}  // namespace ifrt
}  // namespace xla
