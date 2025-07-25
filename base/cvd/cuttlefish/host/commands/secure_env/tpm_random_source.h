//
// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <keymaster/random_source.h>

#include "cuttlefish/host/commands/secure_env/tpm_resource_manager.h"

namespace cuttlefish {

/**
 * Secure random number generator, pulling data from a TPM.
 *
 * RandomSource is used by the OpenSSL HMAC key and AES key implementations.
 */
class TpmRandomSource : public keymaster::RandomSource {
public:
 TpmRandomSource(TpmResourceManager& resource_manager);
 virtual ~TpmRandomSource() = default;

 keymaster_error_t GenerateRandom(uint8_t* buffer,
                                  size_t length) const override;

 keymaster_error_t AddRngEntropy(const uint8_t*, size_t) const;

private:
 TpmResourceManager& resource_manager_;
};

}  // namespace cuttlefish
