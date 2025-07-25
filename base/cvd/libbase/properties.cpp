/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "android-base/properties.h"

#if defined(__BIONIC__)
#include <sys/system_properties.h>
#endif

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <string>

#include <android-base/parsebool.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android-base/thread_annotations.h>

#if !defined(__BIONIC__)

// Here lies a rudimentary implementation of system properties for non-Bionic
// platforms. We are using weak symbols here because we want to allow
// downstream users of libbase to override with their own implementation.
// For example, on Ravenwood (host-side testing for platform development)
// we'd love to be able to fully control system properties exposed to tests,
// so we reimplement the entire system properties API there.

#if defined(__linux__)
// Weak symbols are not supported on Windows, and to prevent unnecessary
// complications, we strictly limit the use of weak symbols to Linux.
#define SYSPROP_WEAK __attribute__((weak))
#else
#define SYSPROP_WEAK
#endif

#define PROP_VALUE_MAX 92

struct prop_info {
  std::string key;
  mutable std::string value;
  mutable uint32_t serial;

  prop_info(const char* key, const char* value) : key(key), value(value), serial(0) {}
};

struct prop_info_cmp {
  using is_transparent = void;
  bool operator()(const prop_info& lhs, const prop_info& rhs) const { return lhs.key < rhs.key; }
  bool operator()(std::string_view lhs, const prop_info& rhs) const { return lhs < rhs.key; }
  bool operator()(const prop_info& lhs, std::string_view rhs) const { return lhs.key < rhs; }
};

static auto& g_properties_lock = *new std::mutex;
static auto& g_properties GUARDED_BY(g_properties_lock) = *new std::set<prop_info, prop_info_cmp>;

SYSPROP_WEAK int __system_property_set(const char* key, const char* value) {
  if (key == nullptr || *key == '\0') return -1;
  if (value == nullptr) value = "";
  bool read_only = !strncmp(key, "ro.", 3);
  if (!read_only && strlen(value) >= PROP_VALUE_MAX) return -1;

  std::lock_guard lock(g_properties_lock);
  auto [it, success] = g_properties.emplace(key, value);
  if (read_only) return success ? 0 : -1;
  if (!success) {
    it->value = value;
    ++it->serial;
  }
  return 0;
}

SYSPROP_WEAK int __system_property_get(const char* key, char* value) {
  std::lock_guard lock(g_properties_lock);
  auto it = g_properties.find(key);
  if (it == g_properties.end()) {
    *value = '\0';
    return 0;
  }
  snprintf(value, PROP_VALUE_MAX, "%s", it->value.c_str());
  return strlen(value);
}

SYSPROP_WEAK const prop_info* __system_property_find(const char* key) {
  std::lock_guard lock(g_properties_lock);
  auto it = g_properties.find(key);
  if (it == g_properties.end()) {
    return nullptr;
  } else {
    return &*it;
  }
}

SYSPROP_WEAK void __system_property_read_callback(const prop_info* pi,
                                                  void (*callback)(void*, const char*, const char*,
                                                                   uint32_t),
                                                  void* cookie) {
  std::lock_guard lock(g_properties_lock);
  callback(cookie, pi->key.c_str(), pi->value.c_str(), pi->serial);
}

#endif  // __BIONIC__

namespace android {
namespace base {

bool GetBoolProperty(const std::string& key, bool default_value) {
  switch (ParseBool(GetProperty(key, ""))) {
    case ParseBoolResult::kError:
      return default_value;
    case ParseBoolResult::kFalse:
      return false;
    case ParseBoolResult::kTrue:
      return true;
  }
  __builtin_unreachable();
}

template <typename T>
T GetIntProperty(const std::string& key, T default_value, T min, T max) {
  T result;
  std::string value = GetProperty(key, "");
  if (!value.empty() && android::base::ParseInt(value, &result, min, max)) return result;
  return default_value;
}

template <typename T>
T GetUintProperty(const std::string& key, T default_value, T max) {
  T result;
  std::string value = GetProperty(key, "");
  if (!value.empty() && android::base::ParseUint(value, &result, max)) return result;
  return default_value;
}

template int8_t GetIntProperty(const std::string&, int8_t, int8_t, int8_t);
template int16_t GetIntProperty(const std::string&, int16_t, int16_t, int16_t);
template int32_t GetIntProperty(const std::string&, int32_t, int32_t, int32_t);
template int64_t GetIntProperty(const std::string&, int64_t, int64_t, int64_t);

template uint8_t GetUintProperty(const std::string&, uint8_t, uint8_t);
template uint16_t GetUintProperty(const std::string&, uint16_t, uint16_t);
template uint32_t GetUintProperty(const std::string&, uint32_t, uint32_t);
template uint64_t GetUintProperty(const std::string&, uint64_t, uint64_t);

std::string GetProperty(const std::string& key, const std::string& default_value) {
  std::string property_value;
  const prop_info* pi = __system_property_find(key.c_str());
  if (pi == nullptr) return default_value;

  __system_property_read_callback(
      pi,
      [](void* cookie, const char*, const char* value, unsigned) {
        auto property_value = reinterpret_cast<std::string*>(cookie);
        *property_value = value;
      },
      &property_value);
  // If the property exists but is empty, also return the default value.
  // Since we can't remove system properties, "empty" is traditionally
  // the same as "missing" (this was true for cutils' property_get).
  return property_value.empty() ? default_value : property_value;
}

bool SetProperty(const std::string& key, const std::string& value) {
  return (__system_property_set(key.c_str(), value.c_str()) == 0);
}

#if defined(__BIONIC__)

struct WaitForPropertyData {
  bool done;
  const std::string* expected_value;
  unsigned last_read_serial;
};

static void WaitForPropertyCallback(void* data_ptr, const char*, const char* value, unsigned serial) {
  WaitForPropertyData* data = reinterpret_cast<WaitForPropertyData*>(data_ptr);
  if (*data->expected_value == value) {
    data->done = true;
  } else {
    data->last_read_serial = serial;
  }
}

// TODO: chrono_utils?
static void DurationToTimeSpec(timespec& ts, const std::chrono::milliseconds d) {
  auto s = std::chrono::duration_cast<std::chrono::seconds>(d);
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d - s);
  ts.tv_sec = std::min<std::chrono::seconds::rep>(s.count(), std::numeric_limits<time_t>::max());
  ts.tv_nsec = ns.count();
}

using AbsTime = std::chrono::time_point<std::chrono::steady_clock>;

static void UpdateTimeSpec(timespec& ts, std::chrono::milliseconds relative_timeout,
                           const AbsTime& start_time) {
  auto now = std::chrono::steady_clock::now();
  auto time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
  if (time_elapsed >= relative_timeout) {
    ts = { 0, 0 };
  } else {
    auto remaining_timeout = relative_timeout - time_elapsed;
    DurationToTimeSpec(ts, remaining_timeout);
  }
}

// Waits for the system property `key` to be created.
// Times out after `relative_timeout`.
// Sets absolute_timeout which represents absolute time for the timeout.
// Returns nullptr on timeout.
static const prop_info* WaitForPropertyCreation(const std::string& key,
                                                const std::chrono::milliseconds& relative_timeout,
                                                const AbsTime& start_time) {
  // Find the property's prop_info*.
  const prop_info* pi;
  unsigned global_serial = 0;
  while ((pi = __system_property_find(key.c_str())) == nullptr) {
    // The property doesn't even exist yet.
    // Wait for a global change and then look again.
    timespec ts;
    UpdateTimeSpec(ts, relative_timeout, start_time);
    if (!__system_property_wait(nullptr, global_serial, &global_serial, &ts)) return nullptr;
  }
  return pi;
}

bool WaitForProperty(const std::string& key, const std::string& expected_value,
                     std::chrono::milliseconds relative_timeout) {
  auto start_time = std::chrono::steady_clock::now();
  const prop_info* pi = WaitForPropertyCreation(key, relative_timeout, start_time);
  if (pi == nullptr) return false;

  WaitForPropertyData data;
  data.expected_value = &expected_value;
  data.done = false;
  while (true) {
    timespec ts;
    // Check whether the property has the value we're looking for?
    __system_property_read_callback(pi, WaitForPropertyCallback, &data);
    if (data.done) return true;

    // It didn't, so wait for the property to change before checking again.
    UpdateTimeSpec(ts, relative_timeout, start_time);
    uint32_t unused;
    if (!__system_property_wait(pi, data.last_read_serial, &unused, &ts)) return false;
  }
}

bool WaitForPropertyCreation(const std::string& key,
                             std::chrono::milliseconds relative_timeout) {
  auto start_time = std::chrono::steady_clock::now();
  return (WaitForPropertyCreation(key, relative_timeout, start_time) != nullptr);
}

CachedProperty::CachedProperty(std::string property_name)
    : property_name_(std::move(property_name)),
      prop_info_(nullptr),
      cached_area_serial_(0),
      cached_property_serial_(0),
      is_read_only_(android::base::StartsWith(property_name, "ro.")),
      read_only_property_(nullptr) {
  static_assert(sizeof(cached_value_) == PROP_VALUE_MAX);
}

CachedProperty::CachedProperty(const char* property_name)
    : CachedProperty(std::string(property_name)) {}

const char* CachedProperty::Get(bool* changed) {
  std::optional<uint32_t> initial_property_serial = cached_property_serial_;

  // Do we have a `struct prop_info` yet?
  if (prop_info_ == nullptr) {
    // `__system_property_find` is expensive, so only retry if a property
    // has been created since last time we checked.
    uint32_t property_area_serial = __system_property_area_serial();
    if (property_area_serial != cached_area_serial_) {
      prop_info_ = __system_property_find(property_name_.c_str());
      cached_area_serial_ = property_area_serial;
    }
  }

  if (prop_info_ != nullptr) {
    // Only bother re-reading the property if it's actually changed since last time.
    uint32_t property_serial = __system_property_serial(prop_info_);
    if (property_serial != cached_property_serial_) {
      __system_property_read_callback(
          prop_info_,
          [](void* data, const char*, const char* value, uint32_t serial) {
            CachedProperty* instance = reinterpret_cast<CachedProperty*>(data);
            instance->cached_property_serial_ = serial;
            // Read only properties can be larger than PROP_VALUE_MAX, but also never change value
            // or location, thus we return the pointer from the shared memory directly.
            if (instance->is_read_only_) {
              instance->read_only_property_ = value;
            } else {
              strlcpy(instance->cached_value_, value, PROP_VALUE_MAX);
            }
          },
          this);
    }
  }

  if (changed) {
    *changed = cached_property_serial_ != initial_property_serial;
  }

  if (is_read_only_) {
    return read_only_property_;
  } else {
    return cached_value_;
  }
}

const char* CachedProperty::WaitForChange(std::chrono::milliseconds relative_timeout) {
  if (!prop_info_) {
    auto start_time = std::chrono::steady_clock::now();
    prop_info_ = WaitForPropertyCreation(property_name_, relative_timeout, start_time);
    if (!prop_info_) {
      return nullptr;
    }
  } else {
    timespec ts;
    DurationToTimeSpec(ts, relative_timeout);

    uint32_t old_serial = cached_property_serial_.value_or(0);
    uint32_t new_serial;
    if (!__system_property_wait(prop_info_, old_serial, &new_serial, &ts)) return nullptr;
  }

  return Get(nullptr);
}

CachedBoolProperty::CachedBoolProperty(std::string property_name)
    : cached_parsed_property_(std::move(property_name),
                              [](const char* value) -> std::optional<bool> {
                                switch (ParseBool(value)) {
                                  case ParseBoolResult::kError:
                                    return std::nullopt;
                                  case ParseBoolResult::kFalse:
                                    return false;
                                  case ParseBoolResult::kTrue:
                                    return true;
                                }
                              }) {}

std::optional<bool> CachedBoolProperty::GetOptional() {
  return cached_parsed_property_.Get();
}

bool CachedBoolProperty::Get(bool default_value) {
  return GetOptional().value_or(default_value);
}

#endif

}  // namespace base
}  // namespace android
