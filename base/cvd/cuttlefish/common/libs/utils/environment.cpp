/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "cuttlefish/common/libs/utils/environment.h"

#include <cstdlib>
#include <string>

namespace cuttlefish {

std::optional<std::string> StringFromEnv(const std::string& varname) {

  const char* const valstr = getenv(varname.c_str());
  if (!valstr) {
    return std::nullopt;
  }
  return valstr;
}

std::string StringFromEnv(const std::string& varname,
                          const std::string& defval) {
  return StringFromEnv(varname).value_or(defval);
}

}  // namespace cuttlefish
