/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <functional>

#include <json/json.h>

#include "cuttlefish/common/libs/utils/result.h"

namespace cuttlefish {
namespace webrtc_streaming {

// The ConnectionObserver is the boundary between device specific code and
// general WebRTC streaming code. Device specific code should be left to
// implementations of this class while code that could be shared between any
// device using this streaming library should remain in the library.
// For example:
// - Parsing JSON messages to obtain input events is common to all android
// devices and should stay in the library.
// - Sending input events to the device by writing to a socket is cuttlefish
// specific and should be done in the ConnectionObserver implementation. Other
// devices could choose to send those events over ADB for example. A good rule
// of thumb is: if it was encoded client side in cf_webrtc.js it should be
// decoded in the library.
class ConnectionObserver {
 public:
  ConnectionObserver() = default;
  virtual ~ConnectionObserver() = default;

  virtual void OnConnected() = 0;

  virtual Result<void> OnMouseMoveEvent(int x, int y) = 0;
  virtual Result<void> OnMouseButtonEvent(int button, bool down) = 0;
  virtual Result<void> OnMouseWheelEvent(int pixels) = 0;
  virtual Result<void> OnTouchEvent(const std::string& device_label, int x,
                                    int y, bool down) = 0;
  virtual Result<void> OnMultiTouchEvent(const std::string& label,
                                         Json::Value id, Json::Value x,
                                         Json::Value y, bool down,
                                         int size) = 0;

  virtual Result<void> OnKeyboardEvent(uint16_t keycode, bool down) = 0;

  virtual Result<void> OnRotaryWheelEvent(int pixels) = 0;

  virtual void OnAdbChannelOpen(
      std::function<bool(const uint8_t*, size_t)> adb_message_sender) = 0;
  virtual void OnAdbMessage(const uint8_t* msg, size_t size) = 0;

  virtual void OnControlChannelOpen(
      std::function<bool(const Json::Value)> control_message_sender) = 0;
  virtual Result<void> OnLidStateChange(bool lid_open) = 0;
  virtual void OnHingeAngleChange(int hinge_angle) = 0;
  virtual Result<void> OnPowerButton(bool button_down) = 0;
  virtual Result<void> OnBackButton(bool button_down) = 0;
  virtual Result<void> OnHomeButton(bool button_down) = 0;
  virtual Result<void> OnMenuButton(bool button_down) = 0;
  virtual Result<void> OnVolumeDownButton(bool button_down) = 0;
  virtual Result<void> OnVolumeUpButton(bool button_down) = 0;
  virtual void OnCustomActionButton(const std::string& command,
                                    const std::string& button_state) = 0;

  virtual void OnCameraControlMsg(const Json::Value& msg) = 0;
  virtual void OnDisplayControlMsg(const Json::Value& msg) = 0;
  virtual void OnDisplayAddMsg(const Json::Value& msg) = 0;
  virtual void OnDisplayRemoveMsg(const Json::Value& msg) = 0;

  virtual void OnBluetoothChannelOpen(
      std::function<bool(const uint8_t*, size_t)> bluetooth_message_sender) = 0;
  virtual void OnBluetoothMessage(const uint8_t* msg, size_t size) = 0;
  virtual void OnSensorsChannelOpen(
      std::function<bool(const uint8_t*, size_t)> sensors_message_sender) = 0;
  virtual void OnSensorsMessage(const uint8_t* msg, size_t size) = 0;
  virtual void OnSensorsChannelClosed() = 0;
  virtual void OnLightsChannelOpen(
      std::function<bool(const Json::Value&)> lights_message_sender) = 0;
  virtual void OnLightsChannelClosed() = 0;
  virtual void OnLocationChannelOpen(
      std::function<bool(const uint8_t*, size_t)> location_message_sender) = 0;
  virtual void OnLocationMessage(const uint8_t* msg, size_t size) = 0;

  virtual void OnKmlLocationsChannelOpen(
      std::function<bool(const uint8_t*, size_t)>
          kml_locations_message_sender) = 0;

  virtual void OnGpxLocationsChannelOpen(
      std::function<bool(const uint8_t*, size_t)>
          gpx_locations_message_sender) = 0;
  virtual void OnKmlLocationsMessage(const uint8_t* msg, size_t size) = 0;
  virtual void OnGpxLocationsMessage(const uint8_t* msg, size_t size) = 0;

  virtual void OnCameraData(const std::vector<char>& data) = 0;
};

class ConnectionObserverFactory {
 public:
  virtual ~ConnectionObserverFactory() = default;
  // Called when a new connection is requested
  virtual std::shared_ptr<ConnectionObserver> CreateObserver() = 0;
};

}  // namespace webrtc_streaming
}  // namespace cuttlefish
