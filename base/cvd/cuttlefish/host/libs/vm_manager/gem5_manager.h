/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/common/libs/utils/result.h"
#include "cuttlefish/host/libs/feature/command_source.h"
#include "cuttlefish/host/libs/vm_manager/vm_manager.h"

namespace cuttlefish {
namespace vm_manager {

// Starts a guest VM using the gem5 command directly. It requires the host
// package to support the gem5 capability.
class Gem5Manager : public VmManager {
 public:
  static std::string name() { return "gem5"; }

  Gem5Manager(Arch);
  virtual ~Gem5Manager() = default;

  bool IsSupported() override;

  Result<std::unordered_map<std::string, std::string>> ConfigureGraphics(
      const CuttlefishConfig::InstanceSpecific& instance) override;

  Result<std::unordered_map<std::string, std::string>> ConfigureBootDevices(
      const CuttlefishConfig::InstanceSpecific& instance) override;

  Result<std::vector<MonitorCommand>> StartCommands(
      const CuttlefishConfig& config,
      std::vector<VmmDependencyCommand*>& dependencyCommands) override;

 private:
  Arch arch_;
};

} // namespace vm_manager
} // namespace cuttlefish
