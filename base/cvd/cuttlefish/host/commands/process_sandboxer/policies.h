/*
 * Copyright (C) 2024 The Android Open Source Project
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
#ifndef ANDROID_DEVICE_GOOGLE_CUTTLEFISH_HOST_COMMANDS_SANDBOX_PROCESS_POLICIES_H
#define ANDROID_DEVICE_GOOGLE_CUTTLEFISH_HOST_COMMANDS_SANDBOX_PROCESS_POLICIES_H

#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "sandboxed_api/sandbox2/policybuilder.h"

namespace cuttlefish::process_sandboxer {

struct HostInfo {
  absl::Status EnsureOutputDirectoriesExist();
  std::string HostToolExe(std::string_view exe) const;
  std::string EnvironmentsUdsDir() const;
  std::string InstanceUdsDir() const;
  std::string VsockDeviceDir() const;

  std::string assembly_dir;
  std::string cuttlefish_config_path;
  std::string environments_dir;
  std::string guest_image_path;
  std::string host_artifacts_path;
  std::string log_dir;
  std::string runtime_dir;
  std::string tmp_dir;
};

std::ostream& operator<<(std::ostream&, const HostInfo&);

sandbox2::PolicyBuilder BaselinePolicy(const HostInfo&, std::string_view exe);

sandbox2::PolicyBuilder AdbConnectorPolicy(const HostInfo&);
sandbox2::PolicyBuilder AssembleCvdPolicy(const HostInfo&);
sandbox2::PolicyBuilder AvbToolPolicy(const HostInfo&);
sandbox2::PolicyBuilder CasimirPolicy(const HostInfo&);
sandbox2::PolicyBuilder CfVhostUserInput(const HostInfo&);
sandbox2::PolicyBuilder CasimirControlServerPolicy(const HostInfo&);
sandbox2::PolicyBuilder ControlEnvProxyServerPolicy(const HostInfo&);
sandbox2::PolicyBuilder CvdInternalStartPolicy(const HostInfo&);
sandbox2::PolicyBuilder EchoServerPolicy(const HostInfo&);
sandbox2::PolicyBuilder GnssGrpcProxyPolicy(const HostInfo&);
sandbox2::PolicyBuilder KernelLogMonitorPolicy(const HostInfo&);
sandbox2::PolicyBuilder LogTeePolicy(const HostInfo&);
sandbox2::PolicyBuilder LogcatReceiverPolicy(const HostInfo&);
sandbox2::PolicyBuilder MetricsPolicy(const HostInfo& host);
sandbox2::PolicyBuilder MkEnvImgSlimPolicy(const HostInfo& host);
sandbox2::PolicyBuilder ModemSimulatorPolicy(const HostInfo&);
sandbox2::PolicyBuilder NetsimdPolicy(const HostInfo&);
sandbox2::PolicyBuilder NewFsMsDosPolicy(const HostInfo&);
sandbox2::PolicyBuilder OpenWrtControlServerPolicy(const HostInfo& host);
sandbox2::PolicyBuilder OperatorProxyPolicy(const HostInfo& host);
sandbox2::PolicyBuilder ProcessRestarterPolicy(const HostInfo&);
sandbox2::PolicyBuilder RunCvdPolicy(const HostInfo&);
sandbox2::PolicyBuilder ScreenRecordingServerPolicy(const HostInfo&);
sandbox2::PolicyBuilder SecureEnvPolicy(const HostInfo&);
sandbox2::PolicyBuilder Simg2ImgPolicy(const HostInfo&);
sandbox2::PolicyBuilder SocketVsockProxyPolicy(const HostInfo&);
sandbox2::PolicyBuilder TcpConnectorPolicy(const HostInfo&);
sandbox2::PolicyBuilder TombstoneReceiverPolicy(const HostInfo&);
sandbox2::PolicyBuilder VhostDeviceVsockPolicy(const HostInfo&);
sandbox2::PolicyBuilder WebRtcPolicy(const HostInfo&);
sandbox2::PolicyBuilder WebRtcOperatorPolicy(const HostInfo&);
sandbox2::PolicyBuilder WmediumdPolicy(const HostInfo&);
sandbox2::PolicyBuilder WmediumdGenConfigPolicy(const HostInfo&);

std::set<std::string> NoPolicy(const HostInfo&);

std::unique_ptr<sandbox2::Policy> PolicyForExecutable(
    const HostInfo& host_info, std::string_view server_socket_outside_path,
    std::string_view executable_path);

}  // namespace cuttlefish::process_sandboxer

#endif
