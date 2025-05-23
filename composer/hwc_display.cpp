/*
 * Copyright (c) 2014-2021, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright 2015 The Android Open Source Project
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

/*
 * Changes from Qualcomm Innovation Center are provided under the following license:
 *
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <cutils/properties.h>
#include <errno.h>
#include <math.h>
#include <sync/sync.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <utils/constants.h>
#include <utils/debug.h>
#include <utils/utils.h>
#include <utils/formats.h>
#include <utils/rect.h>
#include <QtiGralloc.h>

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "hwc_display.h"
#include "hwc_debugger.h"
#include "hwc_tonemapper.h"
#include "hwc_session.h"

#ifdef QTI_BSP
#include <hardware/display_defs.h>
#endif

#define __CLASS__ "HWCDisplay"

using aidl::android::hardware::graphics::common::StandardMetadataType;
namespace sdm {

bool HWCDisplay::mmrm_restricted_ = false;
uint32_t HWCDisplay::throttling_refresh_rate_ = 60;

bool NeedsToneMap(const LayerStack &layer_stack) {
  for (Layer *layer : layer_stack.layers) {
    if (layer->request.flags.tone_map) {
      return true;
    }
  }
  return false;
}

bool IsTimeAfterOrEqualVsyncTime(int64_t time, int64_t vsync_time) {
  return ((vsync_time != INT64_MAX) && ((time - vsync_time) >= 0));
}

HWCColorMode::HWCColorMode(DisplayInterface *display_intf) : display_intf_(display_intf) {}

HWC3::Error HWCColorMode::Init() {
  PopulateColorModes();
  return HWC3::Error::None;
}

HWC3::Error HWCColorMode::DeInit() {
  color_mode_map_.clear();
  return HWC3::Error::None;
}

uint32_t HWCColorMode::GetColorModeCount() {
  uint32_t count = UINT32(color_mode_map_.size());
  DLOGI("Supported color mode count = %d", count);
  return std::max(1U, count);
}

uint32_t HWCColorMode::GetRenderIntentCount(ColorMode mode) {
  uint32_t count = UINT32(color_mode_map_[mode].size());
  DLOGI("mode: %d supported rendering intent count = %d", mode, count);
  return std::max(1U, count);
}

HWC3::Error HWCColorMode::GetColorModes(uint32_t *out_num_modes, ColorMode *out_modes) {
  auto it = color_mode_map_.begin();
  *out_num_modes = std::min(*out_num_modes, UINT32(color_mode_map_.size()));
  for (uint32_t i = 0; i < *out_num_modes; it++, i++) {
    out_modes[i] = it->first;
    DLOGI("Color mode = %d is supported", out_modes[i]);
  }
  return HWC3::Error::None;
}

HWC3::Error HWCColorMode::GetRenderIntents(ColorMode mode, uint32_t *out_num_intents,
                                           RenderIntent *out_intents) {
  if (color_mode_map_.find(mode) == color_mode_map_.end()) {
    return HWC3::Error::BadParameter;
  }
  auto it = color_mode_map_[mode].begin();
  *out_num_intents = std::min(*out_num_intents, UINT32(color_mode_map_[mode].size()));
  for (uint32_t i = 0; i < *out_num_intents; it++, i++) {
    out_intents[i] = it->first;
    DLOGI("Color mode = %d is supported with render intent = %d", mode, out_intents[i]);
  }
  return HWC3::Error::None;
}

HWC3::Error HWCColorMode::ValidateColorModeWithRenderIntent(ColorMode mode, RenderIntent intent) {
  if (mode < ColorMode::NATIVE || mode > ColorMode::DISPLAY_BT2020) {
    DLOGE("Invalid mode: %d", mode);
    return HWC3::Error::BadParameter;
  }
  if (color_mode_map_.find(mode) == color_mode_map_.end()) {
    DLOGE("Could not find mode: %d", mode);
    return HWC3::Error::Unsupported;
  }
  if (color_mode_map_[mode].find(intent) == color_mode_map_[mode].end()) {
    DLOGE("Could not find render intent %d in mode %d", intent, mode);
    return HWC3::Error::Unsupported;
  }

  return HWC3::Error::None;
}

HWC3::Error HWCColorMode::SetColorModeWithRenderIntent(ColorMode mode, RenderIntent intent) {
  DTRACE_SCOPED();
  HWC3::Error hwc_error = ValidateColorModeWithRenderIntent(mode, intent);
  if (hwc_error != HWC3::Error::None) {
    return hwc_error;
  }

  if (current_color_mode_ == mode && current_render_intent_ == intent) {
    return HWC3::Error::None;
  }

  auto mode_string = color_mode_map_[mode][intent][kSdrType];
  DisplayError error = display_intf_->SetColorMode(mode_string);
  if (error != kErrorNone) {
    DLOGE("failed for mode = %d intent = %d name = %s", mode, intent, mode_string.c_str());
    return HWC3::Error::Unsupported;
  }
  // The mode does not have the PCC configured, restore the transform
  RestoreColorTransform();

  current_color_mode_ = mode;
  current_render_intent_ = intent;
  DLOGV_IF(kTagClient, "Successfully applied mode = %d intent = %d name = %s", mode, intent,
           mode_string.c_str());
  return HWC3::Error::None;
}

HWC3::Error HWCColorMode::CacheColorModeWithRenderIntent(ColorMode mode, RenderIntent intent) {
  HWC3::Error error = ValidateColorModeWithRenderIntent(mode, intent);
  if (error != HWC3::Error::None) {
    return error;
  }

  if (current_color_mode_ == mode && current_render_intent_ == intent) {
    return HWC3::Error::None;
  }

  current_color_mode_ = mode;
  current_render_intent_ = intent;
  apply_mode_ = true;

  return HWC3::Error::None;
}

HWC3::Error HWCColorMode::ApplyCurrentColorModeWithRenderIntent(bool hdr_present) {
  // If panel does not support color modes, do not set color mode.
  if (color_mode_map_.size() <= 1) {
    return HWC3::Error::None;
  }
  if (!apply_mode_) {
    if ((hdr_present && curr_dynamic_range_ == kHdrType) ||
        (!hdr_present && curr_dynamic_range_ == kSdrType))
      return HWC3::Error::None;
  }

  apply_mode_ = false;
  curr_dynamic_range_ = (hdr_present) ? kHdrType : kSdrType;

  // select mode according to the blend space and dynamic range
  std::string mode_string = preferred_mode_[current_color_mode_][curr_dynamic_range_];
  if (mode_string.empty()) {
    mode_string = color_mode_map_[current_color_mode_][current_render_intent_][curr_dynamic_range_];
    if (mode_string.empty() && hdr_present) {
      // Use the colorimetric HDR mode, if an HDR mode with the current render intent is not present
      mode_string = color_mode_map_[current_color_mode_][RenderIntent::COLORIMETRIC][kHdrType];
    }
    if (mode_string.empty() &&
        (current_color_mode_ == ColorMode::DISPLAY_P3 ||
         current_color_mode_ == ColorMode::DISPLAY_BT2020 ||
         current_color_mode_ == ColorMode::NATIVE) &&
        curr_dynamic_range_ == kHdrType) {
      // fall back to display_p3/display_bt2020/native SDR mode if there is no HDR mode
      mode_string = color_mode_map_[current_color_mode_][current_render_intent_][kSdrType];
    }

    if (mode_string.empty() && (current_color_mode_ == ColorMode::BT2100_PQ) &&
        (curr_dynamic_range_ == kSdrType)) {
      // fallback to hdr mode.
      mode_string = color_mode_map_[current_color_mode_][current_render_intent_][kHdrType];
      DLOGI("fall back to hdr mode for ColorMode::BT2100_PQ kSdrType");
    }
  }

  auto error = SetPreferredColorModeInternal(mode_string, false, NULL, NULL);
  if (error == HWC3::Error::None) {
    // The mode does not have the PCC configured, restore the transform
    RestoreColorTransform();
    DLOGV_IF(kTagClient, "Successfully applied mode = %d intent = %d range = %d name = %s",
             current_color_mode_, current_render_intent_, curr_dynamic_range_, mode_string.c_str());
  } else {
    DLOGE("Failed to apply mode = %d intent = %d range = %d name = %s", current_color_mode_,
          current_render_intent_, curr_dynamic_range_, mode_string.c_str());
  }

  return error;
}

HWC3::Error HWCColorMode::SetColorModeById(int32_t color_mode_id) {
  DLOGI("Applying mode: %d", color_mode_id);
  DisplayError error = display_intf_->SetColorModeById(color_mode_id);
  if (error != kErrorNone) {
    DLOGI_IF(kTagClient, "Failed to apply mode: %d", color_mode_id);
    return HWC3::Error::BadParameter;
  }
  return HWC3::Error::None;
}

HWC3::Error HWCColorMode::SetPreferredColorModeInternal(const std::string &mode_string,
                                                        bool from_client, ColorMode *color_mode,
                                                        DynamicRangeType *dynamic_range) {
  DisplayError error = kErrorNone;
  ColorMode mode = ColorMode::NATIVE;
  DynamicRangeType range = kSdrType;

  if (from_client) {
    // get blend space and dynamic range of the mode
    AttrVal attr;
    std::string color_gamut_string, dynamic_range_string;
    error = display_intf_->GetColorModeAttr(mode_string, &attr);
    if (error) {
      DLOGE("Failed to get mode attributes for mode %s", mode_string.c_str());
      return HWC3::Error::BadParameter;
    }

    if (!attr.empty()) {
      for (auto &it : attr) {
        if (it.first.find(kColorGamutAttribute) != std::string::npos) {
          color_gamut_string = it.second;
        } else if (it.first.find(kDynamicRangeAttribute) != std::string::npos) {
          dynamic_range_string = it.second;
        }
      }
    }

    if (color_gamut_string.empty() || dynamic_range_string.empty()) {
      DLOGE("Invalid attributes for mode %s: color_gamut = %s, dynamic_range = %s",
            mode_string.c_str(), color_gamut_string.c_str(), dynamic_range_string.c_str());
      return HWC3::Error::BadParameter;
    }

    if (color_gamut_string == kDcip3) {
      mode = ColorMode::DISPLAY_P3;
    } else if (color_gamut_string == kSrgb) {
      mode = ColorMode::SRGB;
    }
    if (dynamic_range_string == kHdr) {
      range = kHdrType;
    }

    if (color_mode) {
      *color_mode = mode;
    }
    if (dynamic_range) {
      *dynamic_range = range;
    }
  }

  // apply the mode from client if it matches
  // the current blend space and dynamic range,
  // skip the check for the mode from SF.
  if ((!from_client) || (current_color_mode_ == mode && curr_dynamic_range_ == range)) {
    DLOGI("Applying mode: %s", mode_string.c_str());
    error = display_intf_->SetColorMode(mode_string);
    if (error != kErrorNone) {
      DLOGE("Failed to apply mode: %s", mode_string.c_str());
      return HWC3::Error::BadParameter;
    }
  }

  return HWC3::Error::None;
}

HWC3::Error HWCColorMode::SetColorModeFromClientApi(std::string mode_string) {
  ColorMode mode = ColorMode::NATIVE;
  DynamicRangeType range = kSdrType;

  auto error = SetPreferredColorModeInternal(mode_string, true, &mode, &range);
  if (error == HWC3::Error::None) {
    preferred_mode_[mode][range] = mode_string;
    DLOGV_IF(kTagClient, "Put mode %s(mode %d, range %d) into preferred_mode", mode_string.c_str(),
             mode, range);
  }

  return error;
}

HWC3::Error HWCColorMode::RestoreColorTransform() {
  DisplayError error = display_intf_->SetColorTransform(kColorTransformMatrixCount, color_matrix_);
  if (error != kErrorNone) {
    DLOGE("Failed to set Color Transform");
    return HWC3::Error::BadParameter;
  }

  return HWC3::Error::None;
}

HWC3::Error HWCColorMode::SetColorTransform(const float *matrix,
                                            android_color_transform_t /*hint*/) {
  DTRACE_SCOPED();
  auto status = HWC3::Error::None;
  double color_matrix[kColorTransformMatrixCount] = {0};
  CopyColorTransformMatrix(matrix, color_matrix);

  DisplayError error = display_intf_->SetColorTransform(kColorTransformMatrixCount, color_matrix);
  if (error != kErrorNone) {
    DLOGE("Failed to set Color Transform Matrix");
    status = HWC3::Error::Unsupported;
  }
  CopyColorTransformMatrix(matrix, color_matrix_);
  return status;
}

void HWCColorMode::PopulateColorModes() {
  uint32_t color_mode_count = 0;
  // SDM returns modes which have attributes defining mode and rendering intent
  DisplayError error = display_intf_->GetColorModeCount(&color_mode_count);
  if (error != kErrorNone || (color_mode_count == 0)) {
    DLOGW("GetColorModeCount failed, use native color mode");
    color_mode_map_[ColorMode::NATIVE][RenderIntent::COLORIMETRIC][kSdrType] =
        "hal_native_identity";
    return;
  }

  DLOGV_IF(kTagClient, "Color Modes supported count = %d", color_mode_count);

  std::vector<std::string> color_modes(color_mode_count);
  error = display_intf_->GetColorModes(&color_mode_count, &color_modes);
  for (uint32_t i = 0; i < color_mode_count; i++) {
    std::string &mode_string = color_modes.at(i);
    DLOGV_IF(kTagClient, "Color Mode[%d] = %s", i, mode_string.c_str());
    AttrVal attr;
    error = display_intf_->GetColorModeAttr(mode_string, &attr);
    std::string color_gamut = kNative, dynamic_range = kSdr, pic_quality = kStandard, transfer;
    int int_render_intent = -1;
    if (!attr.empty()) {
      for (auto &it : attr) {
        if (it.first.find(kColorGamutAttribute) != std::string::npos) {
          color_gamut = it.second;
        } else if (it.first.find(kDynamicRangeAttribute) != std::string::npos) {
          dynamic_range = it.second;
        } else if (it.first.find(kPictureQualityAttribute) != std::string::npos) {
          pic_quality = it.second;
        } else if (it.first.find(kGammaTransferAttribute) != std::string::npos) {
          transfer = it.second;
        } else if (it.first.find(kRenderIntentAttribute) != std::string::npos) {
          int_render_intent = std::stoi(it.second);
        }
      }

      if (int_render_intent < 0 || int_render_intent > MAX_EXTENDED_RENDER_INTENT) {
        DLOGW("Invalid render intent %d for mode %s", int_render_intent, mode_string.c_str());
        continue;
      }
      DLOGV_IF(kTagClient,
               "color_gamut : %s, dynamic_range : %s, pic_quality : %s, "
               "render_intent : %d",
               color_gamut.c_str(), dynamic_range.c_str(), pic_quality.c_str(), int_render_intent);

      auto render_intent = static_cast<RenderIntent>(int_render_intent);
      if (color_gamut == kNative) {
        color_mode_map_[ColorMode::NATIVE][render_intent][kSdrType] = mode_string;
      }

      if (color_gamut == kSrgb && dynamic_range == kSdr) {
        color_mode_map_[ColorMode::SRGB][render_intent][kSdrType] = mode_string;
      }

      if (color_gamut == kDcip3 && dynamic_range == kSdr) {
        color_mode_map_[ColorMode::DISPLAY_P3][render_intent][kSdrType] = mode_string;
      }
      if (color_gamut == kDcip3 && dynamic_range == kHdr) {
        if (display_intf_->IsSupportSsppTonemap()) {
          color_mode_map_[ColorMode::DISPLAY_P3][render_intent][kHdrType] = mode_string;
        } else if (pic_quality == kStandard) {
          color_mode_map_[ColorMode::BT2100_PQ][render_intent][kHdrType] = mode_string;
          color_mode_map_[ColorMode::BT2100_HLG][render_intent][kHdrType] = mode_string;
        }
      } else if (color_gamut == kBt2020) {
        if (transfer == kSt2084) {
          color_mode_map_[ColorMode::BT2100_PQ][RenderIntent::COLORIMETRIC][kHdrType] = mode_string;
        } else if (transfer == kHlg) {
          color_mode_map_[ColorMode::BT2100_HLG][RenderIntent::COLORIMETRIC][kHdrType] =
              mode_string;
        } else if (transfer == kSrgb) {
          color_mode_map_[ColorMode::DISPLAY_BT2020][RenderIntent::COLORIMETRIC][kSdrType] =
              mode_string;
        }
      }
    } else {
      // Look at the mode names, if no attributes are found
      if (mode_string.find("hal_native") != std::string::npos) {
        color_mode_map_[ColorMode::NATIVE][RenderIntent::COLORIMETRIC][kSdrType] = mode_string;
      }
    }
  }
}

void HWCColorMode::Dump(std::ostringstream *os) {
  *os << "color modes supported: \n";
  for (auto it : color_mode_map_) {
    *os << "mode: " << static_cast<int32_t>(it.first) << " RIs { ";
    for (auto render_intent_it : color_mode_map_[it.first]) {
      *os << static_cast<int32_t>(render_intent_it.first) << " dynamic_range [ ";
      for (auto range_it : color_mode_map_[it.first][render_intent_it.first]) {
        *os << static_cast<int32_t>(range_it.first) << " ";
      }
      *os << "] ";
    }
    *os << "} \n";
  }
  *os << "current mode: " << static_cast<uint32_t>(current_color_mode_) << std::endl;
  *os << "current render_intent: " << static_cast<uint32_t>(current_render_intent_) << std::endl;
  if (curr_dynamic_range_ == kHdrType) {
    *os << "current dynamic_range: HDR" << std::endl;
  } else {
    *os << "current dynamic_range: SDR" << std::endl;
  }
  *os << "current transform: ";
  for (uint32_t i = 0; i < kColorTransformMatrixCount; i++) {
    if (i % 4 == 0) {
      *os << std::endl;
    }
    *os << std::fixed << std::setprecision(2) << std::setw(6) << std::setfill(' ')
        << color_matrix_[i] << " ";
  }
  *os << std::endl;
}

HWCDisplay::HWCDisplay(CoreInterface *core_intf, BufferAllocator *buffer_allocator,
                       HWCCallbacks *callbacks, HWCDisplayEventHandler *event_handler,
                       qService::QService *qservice, DisplayType type, Display id, int32_t sdm_id,
                       DisplayClass display_class)
    : core_intf_(core_intf),
      callbacks_(callbacks),
      event_handler_(event_handler),
      type_(type),
      id_(id),
      sdm_id_(sdm_id),
      qservice_(qservice),
      display_class_(display_class) {
  buffer_allocator_ = static_cast<HWCBufferAllocator *>(buffer_allocator);
}

int HWCDisplay::Init() {
  DisplayError error = kErrorNone;

  error = core_intf_->CreateDisplay(sdm_id_, this, &display_intf_);
  if (error != kErrorNone) {
    if (kErrorResources == error || kErrorCriticalResource == error) {
      return -ENODEV;
    }

    if (kErrorDeviceRemoved == error) {
      DLOGW("Display creation cancelled. Display %d-%d removed.", sdm_id_, type_);
      return -ENODEV;
    }

    DLOGE("Display create failed. Error = %d display_id = %d event_handler = %p disp_intf = %p",
          error, sdm_id_, this, &display_intf_);
    return -EINVAL;
  }

  HWCDebugHandler::Get()->GetProperty(DISABLE_HDR, &disable_hdr_handling_);
  if (disable_hdr_handling_) {
    DLOGI("HDR Handling disabled");
  }

  HWCDebugHandler::Get()->GetProperty(DISABLE_SDR_HISTOGRAM, &disable_sdr_histogram_);
  if (disable_sdr_histogram_) {
    DLOGI("Non-HDR histogram handling disabled");
  }

  int property_swap_interval = 1;
  HWCDebugHandler::Get()->GetProperty(ZERO_SWAP_INTERVAL, &property_swap_interval);
  if (property_swap_interval == 0) {
    swap_interval_zero_ = true;
  }

  idle_active_ms_ = HWCDebugHandler::GetIdleTimeoutMs();

  client_target_ = new HWCLayer(id_, buffer_allocator_);

  error = display_intf_->GetNumVariableInfoConfigs(&num_configs_);
  if (error != kErrorNone) {
    DLOGE("Getting config count failed. Error = %d", error);
    return -EINVAL;
  }

  UpdateConfigs();

  int enable_gpu_tonemapper = 0;
  HWCDebugHandler::Get()->GetProperty(ENABLE_GPU_TONEMAPPER_PROP, &enable_gpu_tonemapper);
  // Disable instantiating HWCTonemapper when GPU tonemapping is not used.
  if (enable_gpu_tonemapper) {
    tone_mapper_ = new HWCToneMapper(buffer_allocator_);
  }

  display_intf_->GetQsyncFps(&qsync_fps_);

  display_intf_->GetRefreshRateRange(&min_refresh_rate_, &max_refresh_rate_);
  current_refresh_rate_ = max_refresh_rate_;

  GetUnderScanConfig();

  DisplayConfigFixedInfo fixed_info = {};
  display_intf_->GetConfig(&fixed_info);
  is_cmd_mode_ = fixed_info.is_cmdmode;

  game_supported_ = display_intf_->GameEnhanceSupported();

  DLOGI("Display created with id: %d, game_supported_: %d", UINT32(id_), game_supported_);

  return 0;
}

void HWCDisplay::UpdateConfigs() {
  // SF doesnt care about dynamic bit clk support.
  // Exposing all configs will result in getting/setting of redundant configs.

  // For each config store the corresponding index which client understands.
  hwc_config_map_.resize(num_configs_);

  for (uint32_t i = 0; i < num_configs_; i++) {
    DisplayConfigVariableInfo info = {};
    GetDisplayAttributesForConfig(INT(i), &info);
    bool config_exists = false;
    for (auto &config : variable_config_map_) {
      if (config.second == info) {
        config_exists = true;
        hwc_config_map_.at(i) = config.first;
        break;
      }
    }

    if (!config_exists) {
      variable_config_map_[i] = info;
      hwc_config_map_.at(i) = i;
    }
  }

  if (num_configs_ != 0) {
    Config active_config = hwc_config_map_.at(0);
    GetActiveConfig(&active_config);
    SetActiveConfigIndex(active_config);
  }

  // Update num config count.
  num_configs_ = UINT32(variable_config_map_.size());
  DLOGI("num_configs = %d", num_configs_);
}

int HWCDisplay::Deinit() {
  DisplayError error = core_intf_->DestroyDisplay(display_intf_);
  if (error != kErrorNone) {
    DLOGE("Display destroy failed. Error = %d", error);
    return -EINVAL;
  }

  delete client_target_;
  for (auto hwc_layer : layer_set_) {
    delete hwc_layer;
  }

  if (color_mode_) {
    color_mode_->DeInit();
    delete color_mode_;
  }

  if (tone_mapper_) {
    delete tone_mapper_;
    tone_mapper_ = nullptr;
  }

  return 0;
}

// LayerStack operations
HWC3::Error HWCDisplay::CreateLayer(LayerId *out_layer_id) {
  HWCLayer *layer = *layer_set_.emplace(new HWCLayer(id_, buffer_allocator_));
  if (disable_sdr_histogram_)
    layer->IgnoreSdrHistogramMetadata(true);

  layer_map_.emplace(std::make_pair(layer->GetId(), layer));
  *out_layer_id = layer->GetId();
  geometry_changes_ |= GeometryChanges::kAdded;
  layer_stack_invalid_ = true;

  return HWC3::Error::None;
}

HWCLayer *HWCDisplay::GetHWCLayer(LayerId layer_id) {
  const auto map_layer = layer_map_.find(layer_id);
  if (map_layer == layer_map_.end()) {
    DLOGW("[%" PRIu64 "] GetLayer(%" PRIu64 ") failed: no such layer", id_, layer_id);
    return nullptr;
  } else {
    return map_layer->second;
  }
}

HWC3::Error HWCDisplay::DestroyLayer(LayerId layer_id) {
  // ToDo: Replace layer destroy with smart pointer.
  // Work around to block main thread execution until async commit finishes.
  display_intf_->DestroyLayer();
  const auto map_layer = layer_map_.find(layer_id);
  if (map_layer == layer_map_.end()) {
    DLOGW("[%" PRIu64 "] destroyLayer(%" PRIu64 ") failed: no such layer", id_, layer_id);
    return HWC3::Error::BadLayer;
  }
  const auto layer = map_layer->second;
  layer_map_.erase(map_layer);
  const auto z_range = layer_set_.equal_range(layer);
  for (auto current = z_range.first; current != z_range.second; ++current) {
    if (*current == layer) {
      current = layer_set_.erase(current);
      delete layer;
      break;
    }
  }

  geometry_changes_ |= GeometryChanges::kRemoved;
  layer_stack_invalid_ = true;

  return HWC3::Error::None;
}

static bool IsHDRLayerPresent(Layer *layer) {
  if (layer->input_buffer.color_metadata.colorPrimaries == ColorPrimaries_BT2020 &&
      (layer->input_buffer.color_metadata.transfer == Transfer_SMPTE_ST2084 ||
       layer->input_buffer.color_metadata.transfer == Transfer_HLG)) {
    return true;
  } else if (IsExtendedRange(layer->input_buffer)) {
    // Treat input format FP16 with extended range as HDR layer
    return true;
  }
  return false;
}

void HWCDisplay::BuildLayerStack() {
  layer_stack_ = LayerStack();
  display_rect_ = LayerRect();
  layer_stack_.flags.use_metadata_refresh_rate = false;
  layer_stack_.flags.animating = animating_;
  layer_stack_.flags.layer_id_support = true;
  layer_stack_.solid_fill_enabled = solid_fill_enable_;
  layer_stack_.tonemapper_active = tone_mapper_ && tone_mapper_->IsActive();

  DTRACE_SCOPED();
  // Add one layer for fb target
  for (auto hwc_layer : layer_set_) {
    // Reset layer data which SDM may change
    hwc_layer->ResetPerFrameData();

    Layer *layer = hwc_layer->GetSDMLayer();
    layer->flags = {};  // Reset earlier flags
    // Mark all layers to skip, when client target handle is NULL
    if (hwc_layer->GetClientRequestedCompositionType() == Composition::CLIENT ||
        !client_target_->GetSDMLayer()->input_buffer.buffer_id) {
      layer->flags.skip = true;
    } else if (hwc_layer->GetClientRequestedCompositionType() == Composition::SOLID_COLOR) {
      layer->flags.solid_fill = true;
    }

#ifdef UDFPS_ZPOS
    if (hwc_layer->IsFodPressed()) {
      layer->flags.fod_pressed = true;
    }
#endif

    if (!hwc_layer->IsDataSpaceSupported()) {
      layer->flags.skip = true;
    }

    if (swap_interval_zero_) {
      layer->input_buffer.acquire_fence = nullptr;
    }

    bool is_secure = false;
    bool is_video = false;
    void *hdl = reinterpret_cast<native_handle_t *>(layer->input_buffer.buffer_id);
    if (hdl) {
      int buffer_type;
      gralloc::GetMetaDataValue(hdl, QTI_BUFFER_TYPE, &buffer_type);
      if (buffer_type == BUFFER_TYPE_VIDEO) {
        layer_stack_.flags.video_present = true;
        is_video = true;
      }
      // TZ Protected Buffer - L1
      // Gralloc Usage Protected Buffer - L3 - which needs to be treated as Secure & avoid fallback
      int32_t handle_flags;
      gralloc::GetMetaDataValue(hdl, QTI_PRIVATE_FLAGS, &handle_flags);
      if (handle_flags & qtigralloc::PRIV_FLAGS_SECURE_BUFFER) {
        layer_stack_.flags.secure_present = true;
        is_secure = true;
      }
      // UBWC PI format
      if (handle_flags & qtigralloc::PRIV_FLAGS_UBWC_ALIGNED_PI) {
        layer->input_buffer.flags.ubwc_pi = true;
      }
    }

    if (layer->input_buffer.flags.secure_display) {
      layer_stack_.flags.secure_present = true;
      is_secure = true;
    }

    if (IS_RGB_FORMAT(layer->input_buffer.format) && hwc_layer->IsScalingPresent()) {
      layer_stack_.flags.scaling_rgb_layer_present = true;
    }

    if (layer->input_buffer.usage &
        static_cast<uint64_t>(
            ::aidl::android::hardware::graphics::common::BufferUsage::FRONT_BUFFER)) {
      layer->flags.front_buffer = true;
      layer_stack_.flags.front_buffer_layer_present = true;
    }

    if (hwc_layer->IsSingleBuffered() &&
        !(hwc_layer->IsRotationPresent() || hwc_layer->IsScalingPresent())) {
      layer->flags.single_buffer = true;
      layer_stack_.flags.single_buffered_layer_present = true;
    }

    bool hdr_layer = IsHDRLayerPresent(layer);
    if (hdr_layer && !disable_hdr_handling_) {
      // Dont honor HDR when its handling is disabled
      layer->input_buffer.flags.hdr = true;
      layer_stack_.flags.hdr_present = true;
    } else {
      layer->input_buffer.flags.hdr = false;
    }

    if (game_supported_ && (hwc_layer->GetType() == kLayerGame) && !hdr_layer) {
      layer->flags.is_game = true;
      layer->input_buffer.flags.game = true;
    }

    if (hwc_layer->IsNonIntegralSourceCrop() && !is_secure && !hdr_layer &&
        !layer->flags.single_buffer && !layer->flags.solid_fill && !is_video &&
        !layer->flags.is_game) {
      layer->flags.skip = true;
    }

    if (!layer->flags.skip &&
        (hwc_layer->GetClientRequestedCompositionType() == Composition::CURSOR)) {
      // Currently we support only one HWCursor & only at top most z-order
      if ((*layer_set_.rbegin())->GetId() == hwc_layer->GetId()) {
        layer->flags.cursor = true;
        layer_stack_.flags.cursor_present = true;
      }
    }

    // VTS case failure with solid layer dimming due to no support on pipe
    // TODO(user): Remove this when we add support for solid_fill in pipe
    if (layer->flags.solid_fill && layer->layer_brightness != 1.0f) {
      layer->flags.skip = true;
      layer->flags.solid_fill = false;
    }

    if (layer->flags.skip) {
      layer_stack_.flags.skip_present = true;
    }

    // TODO(user): Move to a getter if this is needed at other places
    Rect scaled_display_frame = {INT(layer->dst_rect.left), INT(layer->dst_rect.top),
                                 INT(layer->dst_rect.right), INT(layer->dst_rect.bottom)};
    if (hwc_layer->GetGeometryChanges() & kDisplayFrame) {
      ApplyScanAdjustment(&scaled_display_frame);
    }
    hwc_layer->SetLayerDisplayFrame(scaled_display_frame);
    hwc_layer->ResetPerFrameData();
    // SDM requires these details even for solid fill
    if (layer->flags.solid_fill) {
      LayerBuffer *layer_buffer = &layer->input_buffer;
      layer_buffer->width = UINT32(layer->dst_rect.right - layer->dst_rect.left);
      layer_buffer->height = UINT32(layer->dst_rect.bottom - layer->dst_rect.top);
      layer_buffer->unaligned_width = layer_buffer->width;
      layer_buffer->unaligned_height = layer_buffer->height;
      layer->src_rect.left = 0;
      layer->src_rect.top = 0;
      layer->src_rect.right = layer_buffer->width;
      layer->src_rect.bottom = layer_buffer->height;
    }

    if (hwc_layer->HasMetaDataRefreshRate()) {
      layer->flags.has_metadata_refresh_rate = true;
    }

    display_rect_ = Union(display_rect_, layer->dst_rect);
    geometry_changes_ |= hwc_layer->GetGeometryChanges();

    layer->flags.updating = true;
    if (layer_set_.size() <= kMaxLayerCount) {
      layer->flags.updating = IsLayerUpdating(hwc_layer);
    }

    if (hwc_layer->IsColorTransformSet()) {
      layer->flags.color_transform = true;
    }
    if (hwc_layer->GetOrigClientRequestedCompositionType() == Composition::DISPLAY_DECORATION) {
      layer->input_buffer.flags.mask_layer = true;
    }
    layer_stack_.flags.mask_present |= layer->input_buffer.flags.mask_layer;

    layer->flags.compatible = hwc_layer->IsLayerCompatible();

    layer->layer_id = hwc_layer->GetId();
    layer->layer_name = hwc_layer->GetName();
    layer->geometry_changes = hwc_layer->GetGeometryChanges();
    layer_stack_.layers.push_back(layer);
  }

  // TODO(user): Set correctly when SDM supports geometry_changes as bitmask

  layer_stack_.flags.geometry_changed =
      UINT32((geometry_changes_ || geometry_changes_on_doze_suspend_) > 0);
  layer_stack_.flags.advance_fb_present = client_target_3_1_set_;
  // Append client target to the layer stack
  Layer *sdm_client_target = client_target_->GetSDMLayer();
  sdm_client_target->layer_id = client_target_->GetId();
  sdm_client_target->geometry_changes = client_target_->GetGeometryChanges();
  sdm_client_target->flags.updating = IsLayerUpdating(client_target_);
  sdm_client_target->layer_name = client_target_->GetName();

  // Derive client target dataspace based on the color mode - bug/115482728
  int32_t client_target_dataspace = GetDataspaceFromColorMode(GetCurrentColorMode());
  SetClientTargetDataSpace(client_target_dataspace);
  layer_stack_.layers.push_back(sdm_client_target);

  layer_stack_.elapse_timestamp = elapse_timestamp_;
  layer_stack_.expected_present_time = expected_present_time_;

  layer_stack_.client_incompatible =
      dump_frame_count_ && (dump_output_to_file_ || dump_input_layers_);
  DLOGV_IF(kTagClient, "layer_stack_.client_incompatible : %d", layer_stack_.client_incompatible);
  ATRACE_INT("HDRPresent ", layer_stack_.flags.hdr_present ? 1 : 0);
  ATRACE_INT("FrontBufferPresent ", layer_stack_.flags.front_buffer_layer_present ? 1 : 0);
}

void HWCDisplay::BuildSolidFillStack() {
  layer_stack_ = LayerStack();
  display_rect_ = LayerRect();

  layer_stack_.layers.push_back(solid_fill_layer_);
  layer_stack_.flags.geometry_changed = 1U;
  // Append client target to the layer stack
  layer_stack_.layers.push_back(client_target_->GetSDMLayer());

  layer_stack_.client_incompatible =
      dump_frame_count_ && (dump_output_to_file_ || dump_input_layers_);
  DLOGV_IF(kTagClient, "layer_stack_.client_incompatible : %d", layer_stack_.client_incompatible);
}

HWC3::Error HWCDisplay::SetLayerType(LayerId layer_id, LayerType type) {
  const auto map_layer = layer_map_.find(layer_id);
  if (map_layer == layer_map_.end()) {
    DLOGW("display [%" PRIu64 "]-[%" PRIu64 "] SetLayerType (%" PRIu64 ") failed to find layer",
          id_, type_, layer_id);
    return HWC3::Error::BadLayer;
  }

  const auto layer = map_layer->second;
  layer->SetLayerType(type);
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::SetLayerZOrder(LayerId layer_id, uint32_t z) {
  const auto map_layer = layer_map_.find(layer_id);
  if (map_layer == layer_map_.end()) {
    DLOGW("[%" PRIu64 "] updateLayerZ failed to find layer", id_);
    return HWC3::Error::BadLayer;
  }

  const auto layer = map_layer->second;
  const auto z_range = layer_set_.equal_range(layer);
  bool layer_on_display = false;
  for (auto current = z_range.first; current != z_range.second; ++current) {
    if (*current == layer) {
      if ((*current)->GetZ() == z) {
        // Don't change anything if the Z hasn't changed
        return HWC3::Error::None;
      }
      current = layer_set_.erase(current);
      layer_on_display = true;
      break;
    }
  }

  if (!layer_on_display) {
    DLOGE("[%" PRIu64 "] updateLayerZ failed to find layer on display", id_);
    return HWC3::Error::BadLayer;
  }

  layer->SetLayerZOrder(z);
  layer_set_.emplace(layer);
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::SetVsyncEnabled(bool enabled) {
  DLOGV("Display ID: %" PRId64 " enabled: %d", id_, enabled);
  ATRACE_INT("SetVsyncState ", enabled);
  DisplayError error = kErrorNone;

  if (shutdown_pending_ || !callbacks_->VsyncCallbackRegistered()) {
    return HWC3::Error::None;
  }

  error = display_intf_->SetVSyncState(enabled);

  if (error != kErrorNone) {
    if (error == kErrorShutDown) {
      shutdown_pending_ = true;
      return HWC3::Error::None;
    }
    DLOGE("Failed. enabled = %d, error = %d", enabled, error);
    return HWC3::Error::BadDisplay;
  }

  return HWC3::Error::None;
}

void HWCDisplay::PostPowerMode() {
  if (release_fence_ == nullptr) {
    return;
  }

  for (auto hwc_layer : layer_set_) {
    hwc_layer->SetReleaseFence(release_fence_);
  }
}

HWC3::Error HWCDisplay::SetPowerMode(PowerMode mode, bool teardown) {
  DLOGI("display = %" PRId64 ", mode = %s", id_, to_string(mode).c_str());
  DisplayState state = kStateOff;
  bool flush_on_error = flush_on_error_;

  if (shutdown_pending_) {
    return HWC3::Error::None;
  }

  switch (mode) {
    case PowerMode::OFF:
      // During power off, all of the buffers are released.
      // Do not flush until a buffer is successfully submitted again.
      flush_on_error = false;
      state = kStateOff;
      if (tone_mapper_) {
        tone_mapper_->Terminate();
      }
      break;
    case PowerMode::ON:
      if (mmrm_restricted_ && (display_class_ != DISPLAY_CLASS_BUILTIN) &&
          (current_power_mode_ == PowerMode::OFF ||
           current_power_mode_ == PowerMode::DOZE_SUSPEND)) {
        return HWC3::Error::None;
      }
      RestoreColorTransform();
      state = kStateOn;
      break;
    case PowerMode::DOZE:
      if (mmrm_restricted_ && (display_class_ != DISPLAY_CLASS_BUILTIN) &&
          (current_power_mode_ == PowerMode::OFF ||
           current_power_mode_ == PowerMode::DOZE_SUSPEND)) {
        return HWC3::Error::None;
      }
      RestoreColorTransform();
      state = kStateDoze;
      break;
    case PowerMode::DOZE_SUSPEND:
      state = kStateDozeSuspend;
      break;
    default:
      return HWC3::Error::BadParameter;
  }
  shared_ptr<Fence> release_fence = nullptr;

  ATRACE_INT("SetPowerMode ", state);
  DisplayError error = display_intf_->SetDisplayState(state, teardown, &release_fence);

  if (error == kErrorNone) {
    flush_on_error_ = flush_on_error;
  } else {
    if (error == kErrorShutDown) {
      shutdown_pending_ = true;
      return HWC3::Error::None;
    }
    DLOGE("Set state failed. Error = %d", error);
    return HWC3::Error::BadParameter;
  }

  // Update release fence.
  release_fence_ = release_fence;
  current_power_mode_ = mode;

  PostPowerMode();

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetClientTargetSupport(uint32_t width, uint32_t height, int32_t format,
                                               int32_t dataspace) {
  ColorMetaData color_metadata = {};
  if (dataspace != INT32(Dataspace::UNKNOWN)) {
    dataspace = TranslateFromLegacyDataspace(dataspace);
    GetColorPrimary(dataspace, &(color_metadata.colorPrimaries));
    GetTransfer(dataspace, &(color_metadata.transfer));
    GetRange(dataspace, &(color_metadata.range));
  }

  LayerBufferFormat sdm_format = HWCLayer::GetSDMFormat(format, 0);
  if (display_intf_->GetClientTargetSupport(width, height, sdm_format, color_metadata) !=
      kErrorNone) {
    return HWC3::Error::Unsupported;
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetColorModes(uint32_t *out_num_modes, ColorMode *out_modes) {
  if (out_modes == nullptr) {
    *out_num_modes = 1;
  } else if (out_modes && *out_num_modes > 0) {
    *out_num_modes = 1;
    out_modes[0] = ColorMode::NATIVE;
  }
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::getDisplayDecorationSupport(PixelFormat_V3 *format,
                                                    AlphaInterpretation *alpha) {
  // ScreenDecoration layers supported even if RC HW is disabled since its coming from framework
  // and is independent of RC HW support.
  *format = PixelFormat_V3::R_8;
  *alpha = AlphaInterpretation::COVERAGE;
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetRenderIntents(ColorMode mode, uint32_t *out_num_intents,
                                         RenderIntent *out_intents) {
  if (mode != ColorMode::NATIVE) {
    return HWC3::Error::Unsupported;
  }
  if (out_intents == nullptr) {
    *out_num_intents = 1;
  } else if (out_intents && *out_num_intents > 0) {
    *out_num_intents = 1;
    out_intents[0] = RenderIntent::COLORIMETRIC;
  }
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetDisplayConfigs(uint32_t *out_num_configs, Config *out_configs) {
  if (out_num_configs == nullptr) {
    return HWC3::Error::BadParameter;
  }

  if (out_configs == nullptr) {
    *out_num_configs = num_configs_;
    return HWC3::Error::None;
  }

  *out_num_configs = std::min(*out_num_configs, num_configs_);

  // Expose all unique config ids to client.
  uint32_t i = 0;
  for (auto &info : variable_config_map_) {
    if (i == *out_num_configs) {
      break;
    }
    out_configs[i++] = info.first;
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetDisplayAttribute(Config config, HwcAttribute attribute,
                                            int32_t *out_value) {
  if (variable_config_map_.find(config) == variable_config_map_.end()) {
    DLOGE("Get variable config failed");
    return HWC3::Error::BadConfig;
  }

  DisplayConfigVariableInfo variable_config = variable_config_map_.at(config);

  variable_config.x_pixels -= UINT32(window_rect_.right + window_rect_.left);
  variable_config.y_pixels -= UINT32(window_rect_.bottom + window_rect_.top);
  if (variable_config.x_pixels <= 0 || variable_config.y_pixels <= 0) {
    DLOGE("window rects are not within the supported range");
    return HWC3::Error::BadDisplay;
  }

  switch (attribute) {
    case HwcAttribute::VSYNC_PERIOD:
      *out_value = INT32(variable_config.vsync_period_ns);
      break;
    case HwcAttribute::WIDTH:
      *out_value = INT32(variable_config.x_pixels);
      break;
    case HwcAttribute::HEIGHT:
      *out_value = INT32(variable_config.y_pixels);
      break;
    case HwcAttribute::DPI_X:
      *out_value = INT32(variable_config.x_dpi * 1000.0f);
      break;
    case HwcAttribute::DPI_Y:
      *out_value = INT32(variable_config.y_dpi * 1000.0f);
      break;
    case HwcAttribute::CONFIG_GROUP:
      *out_value = GetDisplayConfigGroup(variable_config);
      break;
    default:
      DLOGW("Spurious attribute type = %s", composer_V3::toString(attribute).c_str());
      *out_value = -1;
      return HWC3::Error::BadParameter;
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetDisplayConfigurations(std::vector<DisplayConfiguration> *out_configs) {
  out_configs->clear();
  out_configs->reserve(variable_config_map_.size());
  for (const auto &[config_id, variable_config] : variable_config_map_) {
    DisplayConfiguration display_configuration;
    display_configuration.configId = config_id;
    display_configuration.width = variable_config.x_pixels;
    display_configuration.height = variable_config.y_pixels;
    display_configuration.dpi = {static_cast<float>(variable_config.x_dpi),
                                 static_cast<float>(variable_config.y_dpi)};
    display_configuration.vsyncPeriod = variable_config.vsync_period_ns;
    display_configuration.configGroup = GetDisplayConfigGroup(variable_config);
    display_configuration.vrrConfig = {
        static_cast<int32_t>((1000.f / static_cast<float>(variable_config.fps)) * 1000000)};
    DLOGI(
        "GetDisplayConfigurations ConfigId[%d] vsyncPeriod= %d, configGroup= %d, minFrameInterval= "
        "%d",
        config_id, variable_config.vsync_period_ns, display_configuration.configGroup,
        display_configuration.vrrConfig->minFrameIntervalNs);
    out_configs->emplace_back(display_configuration);
  }
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetDisplayName(uint32_t *out_size, char *out_name) {
  // TODO(user): Get panel name and EDID name and populate it here
  if (out_size == nullptr) {
    return HWC3::Error::BadParameter;
  }

  std::string name;
  switch (type_) {
    case kBuiltIn:
      name = "Built-in Display";
      break;
    case kPluggable:
      name = "Pluggable Display";
      break;
    case kVirtual:
      name = "Virtual Display";
      break;
    default:
      name = "Unknown";
      break;
  }

  if (out_name == nullptr) {
    *out_size = UINT32(name.size()) + 1;
  } else {
    *out_size = std::min((UINT32(name.size()) + 1), *out_size);
    if (*out_size > 0) {
      strlcpy(out_name, name.c_str(), *out_size);
      out_name[*out_size - 1] = '\0';
    } else {
      DLOGW("Invalid size requested");
    }
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetDisplayType(int32_t *out_type) {
  if (out_type == nullptr) {
    return HWC3::Error::BadParameter;
  }

  *out_type = INT32(DisplayBasicType::kPhysical);

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetPerFrameMetadataKeys(uint32_t *out_num_keys,
                                                PerFrameMetadataKey *out_keys) {
  if (out_num_keys == nullptr) {
    return HWC3::Error::BadParameter;
  }

  DisplayConfigFixedInfo fixed_info = {};
  display_intf_->GetConfig(&fixed_info);
  uint32_t num_keys = 0;
  if (fixed_info.hdr_plus_supported) {
    num_keys = UINT32(PerFrameMetadataKey::HDR10_PLUS_SEI) + 1;
  } else {
    num_keys = UINT32(PerFrameMetadataKey::MAX_FRAME_AVERAGE_LIGHT_LEVEL) + 1;
  }
  if (out_keys == nullptr) {
    *out_num_keys = num_keys;
  } else {
    uint32_t max_out_key_elements = std::min(*out_num_keys, num_keys);
    for (int32_t i = 0; i < max_out_key_elements; i++) {
      out_keys[i] = static_cast<PerFrameMetadataKey>(i);
    }
  }
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::SetDisplayAnimating(bool animating) {
  // Trigger refresh, when animation ends.
  if (!animating) {
    callbacks_->Refresh(id_);
  }

  animating_ = animating;
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetActiveConfig(Config *out_config) {
  if (out_config == nullptr) {
    return HWC3::Error::BadDisplay;
  }

  if (pending_config_) {
    *out_config = pending_config_index_;
  } else {
    GetActiveDisplayConfig(out_config);
  }

  if (*out_config < hwc_config_map_.size()) {
    *out_config = hwc_config_map_.at(*out_config);
  }
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::SetClientTarget(buffer_handle_t target, shared_ptr<Fence> acquire_fence,
                                        int32_t dataspace, Region damage) {
  DTRACE_SCOPED();
  // TODO(user): SurfaceFlinger gives us a null pointer here when doing full SDE composition
  // The error is problematic for layer caching as it would overwrite our cached client target.
  // Reported bug 28569722 to resolve this.
  // For now, continue to use the last valid buffer reported to us for layer caching.
  if (target == nullptr) {
    return HWC3::Error::None;
  }

  if (acquire_fence == nullptr) {
    DLOGV_IF(kTagClient, "Re-using cached buffer");
  }

  Layer *sdm_layer = client_target_->GetSDMLayer();
  sdm_layer->frame_rate = std::min(current_refresh_rate_, HWCDisplay::GetThrottlingRefreshRate());

  SetClientTargetDataSpace(dataspace);
  client_target_->SetLayerSurfaceDamage(damage);
  client_target_->SetLayerBuffer(target, acquire_fence);
  client_target_handle_ = target;
  client_acquire_fence_ = acquire_fence;
  client_dataspace_ = dataspace;
  client_damage_region_ = damage;

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetClientTarget(buffer_handle_t target, shared_ptr<Fence> acquire_fence,
                                        int32_t dataspace, Region damage) {
  target = client_target_handle_;
  acquire_fence = client_acquire_fence_;
  dataspace = client_dataspace_;
  damage = client_damage_region_;

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::SetClientTarget_3_1(buffer_handle_t target, shared_ptr<Fence> acquire_fence,
                                            int32_t dataspace, Region damage) {
  DTRACE_SCOPED();
  auto status = SetClientTarget(target, acquire_fence, dataspace, damage);
  if (status != HWC3::Error::None) {
    return status;
  }

  client_target_3_1_set_ = true;

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::SetActiveConfig(Config config) {
  DTRACE_SCOPED();
  Config current_config = 0;
  GetActiveConfig(&current_config);
  if (current_config == config) {
    return HWC3::Error::None;
  }

  if (!IsModeSwitchAllowed(config)) {
    return HWC3::Error::BadConfig;
  }

  // DRM driver expects DRM_PREFERRED_MODE to be set as part of first commit.
  if (!IsFirstCommitDone()) {
    // Store client's config.
    // Set this as part of post commit.
    pending_first_commit_config_ = true;
    pending_first_commit_config_index_ = config;
    DLOGI("Defer config change to %d until first commit", UINT32(config));
    return HWC3::Error::None;
  } else if (pending_first_commit_config_) {
    // Config override request from client.
    // Honour latest request.
    pending_first_commit_config_ = false;
  }

  DLOGI("Active configuration changed to: %d", config);

  // Cache refresh rate set by client.
  DisplayConfigVariableInfo info = {};
  GetDisplayAttributesForConfig(INT(config), &info);
  active_refresh_rate_ = info.fps;

  // Store config index to be applied upon refresh.
  pending_config_ = true;
  pending_config_index_ = config;

  // Trigger refresh. This config gets applied on next commit.
  callbacks_->Refresh(id_);

  return HWC3::Error::None;
}

DisplayError HWCDisplay::SetMixerResolution(uint32_t width, uint32_t height) {
  return kErrorNotSupported;
}

HWC3::Error HWCDisplay::SetFrameDumpConfig(uint32_t count, uint32_t bit_mask_layer_type,
                                           int32_t format) {
  dump_frame_count_ = count;
  dump_frame_index_ = 0;
  dump_input_layers_ = ((bit_mask_layer_type & (1 << INPUT_LAYER_DUMP)) != 0);

  if (dump_input_layers_) {
    dump_input_frame_count_ = count;
    dump_input_frame_index_ = 0;
  }

  if (tone_mapper_) {
    tone_mapper_->SetFrameDumpConfig(count);
  }

  DLOGI("num_frame_dump %d, input_layer_dump_enable %d", dump_frame_count_, dump_input_layers_);

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::SetFrameDumpConfig(uint32_t count, uint32_t bit_mask_layer_type,
                                           int32_t format, CwbConfig &cwb_config) {
  bool dump_output_to_file = bit_mask_layer_type & (1 << OUTPUT_LAYER_DUMP);
  DLOGI("Requested o/p dump enable = %d", dump_output_to_file);

  if (secure_event_ != kSecureEventMax) {
    DLOGW("Frame dump is not supported as TUI transition is in progress.");
    return HWC3::Error::None;
  }

  if (!count) {
    DLOGW("No frame will dump as requested output frame count = 0.");
    return HWC3::Error::None;
  } else {
    // If buffer is being freed, wait using lock synchronization before checking buffer.
    std::unique_lock<std::mutex> lock(frame_dump_config_lock_);
    if (dump_output_to_file && (output_buffer_info_.alloc_buffer_info.fd >= 0)) {
      DLOGW("FrameDump Not enabled Framecount = %d dump_output_to_file = %d o/p fd = %d", count,
            dump_output_to_file, output_buffer_info_.alloc_buffer_info.fd);
      return HWC3::Error::None;
    }
  }

  SetFrameDumpConfig(count, bit_mask_layer_type, format);

  if (!dump_output_to_file) {
    // output(cwb) not requested, return
    return HWC3::Error::None;
  }

  // Allocate and map output buffer
  const CwbTapPoint &tap_point = cwb_config.tap_point;
  if (GetCwbBufferResolution(&cwb_config, &output_buffer_info_.buffer_config.width,
                             &output_buffer_info_.buffer_config.height)) {
    DLOGW("Buffer Resolution setting failed.");
    return HWC3::Error::BadConfig;
  }

  DLOGV_IF(kTagQDCM, "CWB output buffer resolution: width:%d height:%d tap point:%s",
           output_buffer_info_.buffer_config.width, output_buffer_info_.buffer_config.height,
           UINT32(tap_point) ? (UINT32(tap_point) == 1) ? "DSPP" : "DEMURA" : "LM");

  output_buffer_info_.buffer_config.format = HWCLayer::GetSDMFormat(format, 0);
  output_buffer_info_.buffer_config.buffer_count = 1;
  if (buffer_allocator_->AllocateBuffer(&output_buffer_info_) != 0) {
    DLOGE("Buffer allocation failed");
    std::unique_lock<std::mutex> lock(frame_dump_config_lock_);
    output_buffer_info_ = {};
    return HWC3::Error::NoResources;
  }
  DLOGI("Output Frame dumping buffer is allocated!");

  void *buffer = mmap(NULL, output_buffer_info_.alloc_buffer_info.size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, output_buffer_info_.alloc_buffer_info.fd, 0);

  if (buffer == MAP_FAILED) {
    DLOGE("mmap failed with err %d", errno);
    std::unique_lock<std::mutex> lock(frame_dump_config_lock_);
    buffer_allocator_->FreeBuffer(&output_buffer_info_);
    output_buffer_info_ = {};
    dump_frame_count_ = 0;
    DLOGI("Output Frame dumping buffer is freed!");
    return HWC3::Error::NoResources;
  }

  const native_handle_t *handle = static_cast<native_handle_t *>(output_buffer_info_.private_data);
  HWC3::Error err = SetReadbackBuffer(handle, nullptr, cwb_config, kCWBClientFrameDump);
  if (err != HWC3::Error::None) {
    std::unique_lock<std::mutex> lock(frame_dump_config_lock_);
    munmap(buffer, output_buffer_info_.alloc_buffer_info.size);
    buffer_allocator_->FreeBuffer(&output_buffer_info_);
    output_buffer_info_ = {};
    dump_frame_count_ = 0;
    DLOGI("Output Frame dumping buffer is freed!");
    return err;
  }
  dump_output_to_file_ = dump_output_to_file;
  output_buffer_base_ = buffer;
  output_buffer_cwb_config_ = cwb_config;

  return HWC3::Error::None;
}

PowerMode HWCDisplay::GetCurrentPowerMode() {
  return current_power_mode_;
}

DisplayError HWCDisplay::VSync(const DisplayEventVSync &vsync) {
  VsyncPeriodNanos vsync_period;
  if (GetDisplayVsyncPeriod(&vsync_period) != HWC3::Error::None) {
    vsync_period = 0;
  }
  ATRACE_INT("VsyncPeriod", INT32(vsync_period));
  callbacks_->Vsync(id_, vsync.timestamp, vsync_period);

  return kErrorNone;
}

DisplayError HWCDisplay::Refresh() {
  callbacks_->Refresh(id_);
  return kErrorNone;
}

DisplayError HWCDisplay::CECMessage(char *message) {
  if (qservice_) {
    qservice_->onCECMessageReceived(message, 0);
  } else {
    DLOGW("Qservice instance not available.");
  }

  return kErrorNone;
}

DisplayError HWCDisplay::HandleEvent(DisplayEvent event) {
  switch (event) {
    case kPanelDeadEvent:
    case kDisplayPowerResetEvent: {
      // TODO(user): Following scenario need to be addressed
      // If panel or HW is in bad state for either ESD or HWR, there is no acquired lock between
      // this scope and call to DisplayPowerReset.
      // Prepare or commit could operate on the display since locker_[id_] is free and most likely
      // result in a failure since ESD/HWR has been requested during this time period.
      if (event_handler_) {
        event_handler_->DisplayPowerReset();
      } else {
        DLOGW("Cannot execute DisplayPowerReset (client_id = %" PRId64 "), event_handler_ is null",
              id_);
      }
    } break;
    case kPostIdleTimeout:
      display_idle_ = true;
      if (NotifyIdleNow()) {
        event_handler_->NotifyIdleStatus(true);
      }
      break;
    case kVmReleaseDone: {
      if (event_handler_) {
        event_handler_->VmReleaseDone(id_);
      } else {
        DLOGW("Cannot execute VmReleaseDone (client_id = %" PRId64 "), event_handler_ is null",
              id_);
      }
    } break;
    case kIdleTimeout:
      ReqPerfHintRelease();
      break;
    default:
      DLOGW("Unknown event: %d", event);
      break;
  }

  return kErrorNone;
}

DisplayError HWCDisplay::HistogramEvent(int /* fd */, uint32_t /* blob_fd */) {
  return kErrorNone;
}

HWC3::Error HWCDisplay::PrepareLayerStack(uint32_t *out_num_types, uint32_t *out_num_requests) {
  layer_changes_.clear();
  layer_requests_.clear();
  has_client_composition_ = false;
  display_idle_ = false;

  DTRACE_SCOPED();
  if (shutdown_pending_) {
    return HWC3::Error::BadDisplay;
  }

  if (CanSkipSdmPrepare(out_num_types, out_num_requests)) {
    return ((*out_num_types > 0) ? HWC3::Error::HasChanges : HWC3::Error::None);
  }

  UpdateRefreshRate();
  UpdateActiveConfig();
  DisplayError error = display_intf_->Prepare(&layer_stack_);
  auto status = HandlePrepareError(error);
  if (status != HWC3::Error::None) {
    return status;
  }

  return PostPrepareLayerStack(out_num_types, out_num_requests);
}

HWC3::Error HWCDisplay::HandlePrepareError(DisplayError error) {
  if (error == kErrorNone || error == kErrorNeedsCommit) {
    return HWC3::Error::None;
  }

  if (error == kErrorShutDown) {
    shutdown_pending_ = true;
  } else if (error == kErrorPermission) {
    WaitOnPreviousFence();
    MarkLayersForGPUBypass();
    geometry_changes_on_doze_suspend_ |= geometry_changes_;
  } else {
    DLOGW("Prepare failed. Error = %d", error);
    // Prepare cycle can fail on a newly connected display if insufficient pipes
    // are available at this moment. Trigger refresh so that the other displays
    // can free up pipes and a valid content can be attached to virtual display.
    callbacks_->Refresh(id_);
    return HWC3::Error::BadDisplay;
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::PostPrepareLayerStack(uint32_t *out_num_types, uint32_t *out_num_requests) {
  DTRACE_SCOPED();
  // clear geometry_changes_on_doze_suspend_ on successful prepare.
  geometry_changes_on_doze_suspend_ = GeometryChanges::kNone;

  layer_changes_.clear();
  layer_requests_.clear();
  has_client_composition_ = false;
  for (auto hwc_layer : layer_set_) {
    Layer *layer = hwc_layer->GetSDMLayer();
    LayerComposition &composition = layer->composition;

    if (composition == kCompositionSDE || composition == kCompositionStitch) {
      layer_requests_[hwc_layer->GetId()] = DisplayRequest::LayerRequest::CLEAR_CLIENT_TARGET;
    }

    Composition requested_composition = hwc_layer->GetClientRequestedCompositionType();
    // Set SDM composition to HWC3 type in HWCLayer
    hwc_layer->SetComposition(composition);
    Composition device_composition = hwc_layer->GetDeviceSelectedCompositionType();
    if (device_composition == Composition::CLIENT) {
      has_client_composition_ = true;
    }
    // Update the changes list only if the requested composition is different from SDM comp type
    if (requested_composition != device_composition) {
      layer_changes_[hwc_layer->GetId()] = device_composition;
    }
    hwc_layer->ResetValidation();
  }

  client_target_->ResetValidation();
  *out_num_types = UINT32(layer_changes_.size());
  *out_num_requests = UINT32(layer_requests_.size());
  layer_stack_invalid_ = false;

  layer_stack_.client_incompatible = false;

  validate_done_ = true;

  return (((*out_num_types > 0) || (has_client_composition_ && *out_num_requests > 0))
              ? HWC3::Error::HasChanges
              : HWC3::Error::None);
}

HWC3::Error HWCDisplay::AcceptDisplayChanges() {
  if (layer_set_.empty()) {
    return HWC3::Error::None;
  }

  if (!validate_done_) {
    return HWC3::Error::NotValidated;
  }

  for (const auto &change : layer_changes_) {
    auto hwc_layer = layer_map_[change.first];
    auto composition = change.second;
    if (hwc_layer != nullptr) {
      hwc_layer->UpdateClientCompositionType(composition);
    } else {
      DLOGW("Invalid layer: %" PRIu64, change.first);
    }
  }

  // Clear layer changes, so that they don't get applied in next commit ie;
  // cases where Prepare doesn't go through.
  layer_changes_.clear();

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetChangedCompositionTypes(uint32_t *out_num_elements, LayerId *out_layers,
                                                   int32_t *out_types) {
  if (layer_set_.empty()) {
    return HWC3::Error::None;
  }

  if (!validate_done_) {
    DLOGW("Display is not validated");
    return HWC3::Error::NotValidated;
  }

  *out_num_elements = UINT32(layer_changes_.size());
  if (out_layers != nullptr && out_types != nullptr) {
    int i = 0;
    for (auto change : layer_changes_) {
      out_layers[i] = change.first;
      out_types[i] = INT32(change.second);
      i++;
    }
  }
  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetReleaseFences(uint32_t *out_num_elements, LayerId *out_layers,
                                         std::vector<shared_ptr<Fence>> *out_fences) {
  if (out_num_elements == nullptr) {
    return HWC3::Error::BadParameter;
  }

  if (out_layers != nullptr && out_fences != nullptr) {
    *out_num_elements = std::min(*out_num_elements, UINT32(layer_set_.size()));
    auto it = layer_set_.begin();
    for (uint32_t i = 0; i < *out_num_elements; i++, it++) {
      auto hwc_layer = *it;
      out_layers[i] = hwc_layer->GetId();

      shared_ptr<Fence> &fence = (*out_fences)[i];
      fence = hwc_layer->GetReleaseFence();
    }
  } else {
    *out_num_elements = UINT32(layer_set_.size());
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetDisplayRequests(int32_t *out_display_requests,
                                           uint32_t *out_num_elements, LayerId *out_layers,
                                           int32_t *out_layer_requests) {
  if (layer_set_.empty()) {
    return HWC3::Error::None;
  }

  if (out_display_requests == nullptr || out_num_elements == nullptr) {
    return HWC3::Error::BadParameter;
  }

  // No display requests for now
  // Use for sharing blit buffers and
  // writing wfd buffer directly to output if there is full GPU composition
  // and no color conversion needed
  if (!validate_done_) {
    DLOGW("Display is not validated");
    return HWC3::Error::NotValidated;
  }

  *out_display_requests = 0;
  if (out_layers != nullptr && out_layer_requests != nullptr) {
    *out_num_elements = std::min(*out_num_elements, UINT32(layer_requests_.size()));
    auto it = layer_requests_.begin();
    for (uint32_t i = 0; i < *out_num_elements; i++, it++) {
      out_layers[i] = it->first;
      out_layer_requests[i] = INT32(it->second);
    }
  } else {
    *out_num_elements = UINT32(layer_requests_.size());
  }

  auto client_target_layer = client_target_->GetSDMLayer();
  if (client_target_layer->request.flags.flip_buffer) {
    *out_display_requests = INT32(DisplayRequest::FLIP_CLIENT_TARGET);
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetHdrCapabilities(uint32_t *out_num_types, int32_t *out_types,
                                           float *out_max_luminance,
                                           float *out_max_average_luminance,
                                           float *out_min_luminance) {
  int32_t supported_types[static_cast<int32_t>(Hdr::HDR10_PLUS)];
  if (out_num_types == nullptr || out_max_luminance == nullptr ||
      out_max_average_luminance == nullptr || out_min_luminance == nullptr) {
    return HWC3::Error::BadParameter;
  }

  DisplayConfigFixedInfo fixed_info = {};
  display_intf_->GetConfig(&fixed_info);

  if (!fixed_info.hdr_supported) {
    *out_num_types = 0;
    DLOGI("HDR is not supported");
    return HWC3::Error::None;
  }

  uint32_t num_types = 0;
  if (fixed_info.hdr_plus_supported) {
    supported_types[num_types] = static_cast<int32_t>(Hdr::HDR10_PLUS);
    num_types++;
  }

  if (fixed_info.dolby_vision_supported) {
    supported_types[num_types] = static_cast<int32_t>(Hdr::DOLBY_VISION);
    num_types++;
  }

  if (fixed_info.hdr_supported) {
    supported_types[num_types] = static_cast<int32_t>(Hdr::HDR10);
    num_types++;
  }

  supported_types[num_types] = static_cast<int32_t>(Hdr::HLG);
  num_types++;

  if (out_types == nullptr) {
    *out_num_types = num_types;
  } else {
    uint32_t max_out_types = std::min(*out_num_types, num_types);
    for (int32_t i = 0; i < max_out_types; i++) {
      out_types[i] = supported_types[i];
    }
    *out_max_luminance = fixed_info.max_luminance;
    *out_max_average_luminance = fixed_info.average_luminance;
    *out_min_luminance = fixed_info.min_luminance;
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::CommitOrPrepare(bool validate_only, shared_ptr<Fence> *out_retire_fence,
                                        uint32_t *out_num_types, uint32_t *out_num_requests,
                                        bool *needs_commit) {
  DTRACE_SCOPED();

  if (shutdown_pending_) {
    return HWC3::Error::BadDisplay;
  }

  UpdateRefreshRate();
  UpdateActiveConfig();
  validate_done_ = false;
  bool exit_validate = false;
  PreValidateDisplay(&exit_validate);
  if (exit_validate) {
    validate_done_ = true;
    client_target_3_1_set_ = false;
    return HWC3::Error::None;
  }

  layer_stack_.validate_only = validate_only;

  DisplayError error = display_intf_->CommitOrPrepare(&layer_stack_);
  // Mask error if needed.
  auto status = HandlePrepareError(error);
  if (status != HWC3::Error::None) {
    client_target_3_1_set_ = false;
    return status;
  }

  *needs_commit = error == kErrorNeedsCommit;

  if (!(*needs_commit)) {
    PostCommitLayerStack(out_retire_fence);
  }

  return PostPrepareLayerStack(out_num_types, out_num_requests);
}

HWC3::Error HWCDisplay::CommitLayerStack(void) {
  if (flush_) {
    return HWC3::Error::None;
  }

  DTRACE_SCOPED();

  if (shutdown_pending_ || layer_set_.empty()) {
    return HWC3::Error::None;
  }

  if (!validate_done_) {
    DLOGV_IF(kTagClient, "Display %" PRIu64 "is not validated", id_);
    return HWC3::Error::NotValidated;
  }

  if (skip_commit_) {
    DLOGV_IF(kTagClient, "Skipping Refresh on display %" PRIu64, id_);
    return HWC3::Error::None;
  }

  DisplayError error = kErrorUndefined;
  int status = 0;
  if (tone_mapper_) {
    if (NeedsToneMap(layer_stack_)) {
      status = tone_mapper_->HandleToneMap(&layer_stack_);
      if (status != 0) {
        DLOGE("Error handling HDR in ToneMapper");
      }
    } else {
      tone_mapper_->Terminate();
    }
  }

  error = display_intf_->Commit(&layer_stack_);

  if (error == kErrorNone) {
    // A commit is successfully submitted, start flushing on failure now onwards.
    flush_on_error_ = true;
    first_cycle_ = false;
  } else {
    if (error == kErrorShutDown) {
      shutdown_pending_ = true;
    } else if (error == kErrorNotValidated) {
      return HWC3::Error::NotValidated;
    } else if (error != kErrorPermission) {
      DLOGE("Commit failed. Error = %d", error);
      // To prevent surfaceflinger infinite wait, flush the previous frame during Commit()
      // so that previous buffer and fences are released, and override the error.
      flush_ = true;
    }
    return HWC3::Error::Unsupported;
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::PostCommitLayerStack(shared_ptr<Fence> *out_retire_fence) {
  DTRACE_SCOPED();
  auto status = HWC3::Error::None;

  // Do no call flush on errors, if a successful buffer is never submitted.
  if (flush_ && flush_on_error_) {
    display_intf_->Flush(&layer_stack_);
  }

  if (tone_mapper_ && tone_mapper_->IsActive()) {
    tone_mapper_->PostCommit(&layer_stack_);
  }

  DumpInputBuffers();

  RetrieveFences(out_retire_fence);
  client_target_->ResetGeometryChanges();

  for (auto hwc_layer : layer_set_) {
    hwc_layer->ResetGeometryChanges();
    Layer *layer = hwc_layer->GetSDMLayer();
    LayerBuffer *layer_buffer = &layer->input_buffer;
    layer->request.flags = {};
    layer_buffer->acquire_fence = nullptr;
  }

  client_target_->GetSDMLayer()->request.flags = {};

  layer_stack_.flags.geometry_changed = false;
  geometry_changes_ = GeometryChanges::kNone;
  flush_ = false;
  skip_commit_ = false;

  layer_stack_.flags.geometry_changed = false;
  geometry_changes_ = GeometryChanges::kNone;
  flush_ = false;
  skip_commit_ = false;
  client_target_3_1_set_ = false;

  if (display_pause_pending_) {
    DLOGI("Pause display %d-%d", sdm_id_, type_);
    display_paused_ = true;
    display_pause_pending_ = false;
  }
  if (secure_event_ == kSecureDisplayEnd || secure_event_ == kTUITransitionUnPrepare) {
    secure_event_ = kSecureEventMax;
  }

  // Handle pending config changes.
  if (pending_first_commit_config_) {
    DLOGI("Changing active config to %d", UINT32(pending_first_commit_config_));
    pending_first_commit_config_ = false;
    SetActiveConfig(pending_first_commit_config_index_);
  }

  return status;
}

void HWCDisplay::RetrieveFences(shared_ptr<Fence> *out_retire_fence) {
  // TODO(user): No way to set the client target release fence on SvF
  for (auto hwc_layer : layer_set_) {
    Layer *layer = hwc_layer->GetSDMLayer();
    LayerBuffer *layer_buffer = &layer->input_buffer;

    if (!flush_) {
      // If swapinterval property is set to 0 or for single buffer layers, do not update f/w
      // release fences and discard fences from driver
      if (!swap_interval_zero_ && !layer->flags.single_buffer) {
        // It may so happen that layer gets marked to GPU & app layer gets queued
        // to MDP for composition. In those scenarios, release fence of buffer should
        // have mdp and gpu sync points merged.
        hwc_layer->SetReleaseFence(layer_buffer->release_fence);
      }
    } else {
      // In case of flush or display paused, we don't return an error to f/w, so it will
      // get a release fence out of the hwc_layer's release fence queue
      // We should push a -1 to preserve release fence circulation semantics.
      hwc_layer->SetReleaseFence(nullptr);
    }

    layer_buffer->acquire_fence = nullptr;
  }

  // if swapinterval property is set to 0 then close and reset the list retire fence
  if (!swap_interval_zero_) {
    *out_retire_fence = layer_stack_.retire_fence;
  }
}

void HWCDisplay::SetIdleTimeoutMs(uint32_t timeout_ms, uint32_t inactive_ms) {
  return;
}

DisplayError HWCDisplay::SetMaxMixerStages(uint32_t max_mixer_stages) {
  DisplayError error = kErrorNone;

  if (display_intf_) {
    error = display_intf_->SetMaxMixerStages(max_mixer_stages);
  }

  return error;
}

void HWCDisplay::DumpInputBuffers() {
  char dir_path[PATH_MAX];
  int status;
  int dump_metadata = 0;

  if (!dump_input_frame_count_ || flush_ || !dump_input_layers_) {
    return;
  }

  DLOGI("dump_frame_count %d dump_input_layers %d", dump_input_frame_count_, dump_input_layers_);
  snprintf(dir_path, sizeof(dir_path), "%s/frame_dump_disp_id_%02u_%s", HWCDebugHandler::DumpDir(),
           UINT32(id_), GetDisplayString());

  status = mkdir(dir_path, 777);
  if ((status != 0) && errno != EEXIST) {
    DLOGW("Failed to create %s directory errno = %d, desc = %s", dir_path, errno, strerror(errno));
    return;
  }

  // Even if directory exists already, need to explicitly change the permission.
  if (chmod(dir_path, 0777) != 0) {
    DLOGW("Failed to change permissions on %s directory", dir_path);
    return;
  }

  bool dump_gpu_target = false;  // whether to dump GPU Target layer.
  for (uint32_t i = 0; i < layer_stack_.layers.size(); i++) {
    auto layer = layer_stack_.layers.at(i);
    if (!dump_gpu_target) {
      if (layer->composition == kCompositionGPU) {
        dump_gpu_target = true;  // Dump GPU Target layer only if its not a full MDP composition.
      } else if (layer->composition == kCompositionGPUTarget) {
        DLOGI("Skipping dumping target layer. dump_gpu_target : %d", dump_gpu_target);
        break;  // Skip dumping GPU Target layer.
      }
    }

    const native_handle_t *handle =
        reinterpret_cast<const native_handle_t *>(layer->input_buffer.buffer_id);
    Fence::Wait(layer->input_buffer.acquire_fence);

    if (!handle) {
      DLOGW(
          "Buffer handle is detected as null for layer: %s(%d) out of %lu layers with layer "
          "flag value: %u",
          layer->layer_name.c_str(), layer->layer_id, layer_stack_.layers.size(), layer->flags);
      continue;
    }

    DLOGI("Dump layer[%d] of %lu handle %p", i, layer_stack_.layers.size(), handle);

    void *base_ptr = NULL;
    int error = buffer_allocator_->MapBuffer(handle, nullptr, &base_ptr);
    if (error != kErrorNone) {
      DLOGE("Failed to map buffer, error = %d", error);
      continue;
    }

    char dump_file_name[PATH_MAX];
    size_t result = 0;

    uint32_t width = 0, height = 0, alloc_size = 0;

    buffer_allocator_->GetWidth((void *)handle, width);
    buffer_allocator_->GetHeight((void *)handle, height);
    buffer_allocator_->GetAllocationSize((void *)handle, alloc_size);

    snprintf(dump_file_name, sizeof(dump_file_name), "%s/input_layer%d_%dx%d_%s_frame%d.raw",
             dir_path, i, width, height, GetFormatString(layer->input_buffer.format),
             dump_input_frame_index_);

    if (base_ptr != nullptr) {
      FILE *fp = fopen(dump_file_name, "w+");
      if (fp) {
        result = fwrite(base_ptr, alloc_size, 1, fp);
        fclose(fp);
      }
    }

    int release_fence = -1;
    error = buffer_allocator_->UnmapBuffer(handle, &release_fence);
    if (error != 0) {
      DLOGE("Failed to unmap buffer, error = %d", error);
      continue;
    }

    DLOGI("Frame Dump %s: is %s", dump_file_name, result ? "Successful" : "Failed");

    HWCDebugHandler::Get()->GetProperty(ENABLE_METADATA_DUMPING, &dump_metadata);
    if (dump_metadata) {
      // Dump only extended content metadata for now. Property named generically for future extension
      std::shared_ptr<CustomContentMetadata> c_md = layer->input_buffer.extended_content_metadata;
      if (c_md) {
        result = 0;
        snprintf(dump_file_name, sizeof(dump_file_name), "%s/input_layer%d_content_md_frame%d.raw",
                 dir_path, i, dump_frame_index_);
        FILE *fp = fopen(dump_file_name, "w+");
        if (fp) {
          result = fwrite(&c_md->metadataPayload, c_md->size, 1, fp);
          fclose(fp);
        }

        DLOGI("Frame Metadata Dump %s: is %s", dump_file_name, result ? "Successful" : "Failed");
      }
    }

    if (layer->composition == kCompositionGPUTarget) {  // Skip dumping the layers that follow
      // follow GPU Target layer in layers list (i.e. stitch layers, noise layer, demura layer).
      break;
    }
  }
  dump_input_frame_count_--;
  dump_input_frame_index_++;
}

void HWCDisplay::DumpOutputBuffer(const BufferInfo &buffer_info, void *base,
                                  shared_ptr<Fence> &retire_fence) {
  char dir_path[PATH_MAX];
  int status;

  snprintf(dir_path, sizeof(dir_path), "%s/frame_dump_disp_id_%02u_%s", HWCDebugHandler::DumpDir(),
           UINT32(id_), GetDisplayString());

  status = mkdir(dir_path, 777);
  if ((status != 0) && errno != EEXIST) {
    DLOGW("Failed to create %s directory errno = %d, desc = %s", dir_path, errno, strerror(errno));
    return;
  }

  // Even if directory exists already, need to explicitly change the permission.
  if (chmod(dir_path, 0777) != 0) {
    DLOGW("Failed to change permissions on %s directory", dir_path);
    return;
  }

  if (base) {
    char dump_file_name[PATH_MAX];
    size_t result = 0;

    if (Fence::Wait(retire_fence) != kErrorNone) {
      DLOGW("sync_wait error errno = %d, desc = %s", errno, strerror(errno));
      return;
    }

    snprintf(dump_file_name, sizeof(dump_file_name), "%s/output_layer_%dx%d_%s_frame%d.raw",
             dir_path, buffer_info.alloc_buffer_info.aligned_width,
             buffer_info.alloc_buffer_info.aligned_height,
             GetFormatString(buffer_info.buffer_config.format), dump_frame_index_);

    FILE *fp = fopen(dump_file_name, "w+");
    if (fp) {
      result = fwrite(base, buffer_info.alloc_buffer_info.size, 1, fp);
      fclose(fp);
    }
    // Need to clear buffer after dumping of current frame to provide empty buffer for next frame.
    // But avoid this in case of virtual display frame dump, else it would provide empty buffer
    // to virtual display client, because it uses client buffer for dumping output.
    if (type_ != kVirtual) {
      memset(base, 0, buffer_info.alloc_buffer_info.size);
    }
    DLOGI("Frame Dump of %s is %s", dump_file_name, result ? "Successful" : "Failed");
  }
}

const char *HWCDisplay::GetDisplayString() {
  switch (type_) {
    case kBuiltIn:
      return "builtin";
    case kPluggable:
      return "pluggable";
    case kVirtual:
      return "virtual";
    default:
      return "invalid";
  }
}

int HWCDisplay::SetFrameBufferConfig(uint32_t x_pixels, uint32_t y_pixels) {
  DTRACE_SCOPED();
  if (x_pixels <= 0 || y_pixels <= 0) {
    DLOGW("Unsupported config: x_pixels=%d, y_pixels=%d", x_pixels, y_pixels);
    return -EINVAL;
  }

  DisplayConfigVariableInfo fb_config;
  DisplayError error = display_intf_->GetFrameBufferConfig(&fb_config);
  if (error != kErrorNone) {
    DLOGV("Get frame buffer config failed. Error = %d", error);
    return -EINVAL;
  }

  fb_config.x_pixels = x_pixels;
  fb_config.y_pixels = y_pixels;

  error = display_intf_->SetFrameBufferConfig(fb_config);
  if (error != kErrorNone) {
    DLOGV("Set frame buffer config failed. Error = %d", error);
    return -EINVAL;
  }

  // Reduce the src_rect and dst_rect as per FBT config.
  // SF sending reduced FBT but here the src_rect is equal to mixer which is
  // higher than allocated buffer of FBT.
  if (windowed_display_) {
    x_pixels -= UINT32(window_rect_.right + window_rect_.left);
    y_pixels -= UINT32(window_rect_.bottom + window_rect_.top);
  }

  if (x_pixels <= 0 || y_pixels <= 0) {
    DLOGE("window rects are not within the supported range");
    return -EINVAL;
  }

  // Create rects to represent the new source and destination crops
  LayerRect crop = LayerRect(0, 0, FLOAT(x_pixels), FLOAT(y_pixels));
  Rect scaled_display_frame = {0, 0, INT(x_pixels), INT(y_pixels)};
  auto client_target_layer = client_target_->GetSDMLayer();
  client_target_layer->src_rect = crop;
  ApplyScanAdjustment(&scaled_display_frame);
  client_target_->SetLayerDisplayFrame(scaled_display_frame);
  client_target_->ResetPerFrameData();

  DLOGI("New framebuffer resolution (%dx%d)", fb_config.x_pixels, fb_config.y_pixels);

  return 0;
}

int HWCDisplay::SetFrameBufferResolution(uint32_t x_pixels, uint32_t y_pixels) {
  int error = SetFrameBufferConfig(x_pixels, y_pixels);
  if (error < 0) {
    DLOGV("SetFrameBufferConfig failed. Error = %d", error);
    return error;
  }

  if (windowed_display_) {
    x_pixels -= UINT32(window_rect_.right + window_rect_.left);
    y_pixels -= UINT32(window_rect_.bottom + window_rect_.top);
  }
  auto client_target_layer = client_target_->GetSDMLayer();

  int aligned_width;
  int aligned_height;
  uint32_t usage = GRALLOC_USAGE_HW_FB;
  int format = static_cast<int>(PixelFormat_V3::RGBA_8888);
  int ubwc_disabled = 0;
  int flags = 0;

  // By default UBWC is enabled and below property is global enable/disable for all
  // buffers allocated through gralloc , including framebuffer targets.
  HWCDebugHandler::Get()->GetProperty(DISABLE_UBWC_PROP, &ubwc_disabled);
  if (!ubwc_disabled) {
    usage |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
    flags |= qtigralloc::PRIV_FLAGS_UBWC_ALIGNED;
  }

  buffer_allocator_->GetAlignedWidthAndHeight(INT(x_pixels), INT(y_pixels), format, usage,
                                              &aligned_width, &aligned_height);

  // TODO(user): How does the dirty region get set on the client target? File bug on Google
  client_target_layer->composition = kCompositionGPUTarget;
  client_target_layer->input_buffer.format = HWCLayer::GetSDMFormat(format, flags);
  client_target_layer->input_buffer.width = UINT32(aligned_width);
  client_target_layer->input_buffer.height = UINT32(aligned_height);
  client_target_layer->input_buffer.unaligned_width = x_pixels;
  client_target_layer->input_buffer.unaligned_height = y_pixels;
  client_target_layer->plane_alpha = 255;

  return 0;
}

void HWCDisplay::GetFrameBufferResolution(uint32_t *x_pixels, uint32_t *y_pixels) {
  DTRACE_SCOPED();
  DisplayConfigVariableInfo fb_config;
  display_intf_->GetFrameBufferConfig(&fb_config);

  *x_pixels = fb_config.x_pixels;
  *y_pixels = fb_config.y_pixels;
}

DisplayError HWCDisplay::GetMixerResolution(uint32_t *x_pixels, uint32_t *y_pixels) {
  return display_intf_->GetMixerResolution(x_pixels, y_pixels);
}

uint32_t HWCDisplay::GetAvailableMixerCount() {
  return display_intf_->GetAvailableMixerCount();
}

void HWCDisplay::GetPanelResolution(uint32_t *x_pixels, uint32_t *y_pixels) {
  DisplayConfigVariableInfo display_config;
  uint32_t active_index = 0;

  display_intf_->GetActiveConfig(&active_index);
  display_intf_->GetConfig(active_index, &display_config);

  *x_pixels = display_config.x_pixels;
  *y_pixels = display_config.y_pixels;
}

void HWCDisplay::GetRealPanelResolution(uint32_t *x_pixels, uint32_t *y_pixels) {
  DisplayConfigVariableInfo display_config;
  uint32_t active_index = 0;

  display_intf_->GetActiveConfig(&active_index);
  display_intf_->GetRealConfig(active_index, &display_config);

  *x_pixels = display_config.x_pixels;
  *y_pixels = display_config.y_pixels;
}

int HWCDisplay::SetDisplayStatus(DisplayStatus display_status) {
  int status = 0;

  if (secure_event_ != kSecureEventMax) {
    DLOGW("SetDisplayStatus is not supported when TUI transition in progress");
    return -ENOTSUP;
  }
  switch (display_status) {
    case kDisplayStatusResume:
      display_paused_ = false;
      status = INT32(SetPowerMode(PowerMode::ON, false /* teardown */));
      break;
    case kDisplayStatusOnline:
      status = INT32(SetPowerMode(PowerMode::ON, false /* teardown */));
      break;
    case kDisplayStatusPause:
      display_paused_ = true;
      status = INT32(SetPowerMode(PowerMode::OFF, false /* teardown */));
      break;
    case kDisplayStatusOffline:
      status = INT32(SetPowerMode(PowerMode::OFF, false /* teardown */));
      break;
    default:
      DLOGW("Invalid display status %d", display_status);
      return -EINVAL;
  }

  return status;
}

HWC3::Error HWCDisplay::SetCursorPosition(LayerId layer, int x, int y) {
  if (shutdown_pending_) {
    return HWC3::Error::None;
  }

  if (!layer_stack_.flags.cursor_present) {
    DLOGW("Cursor layer not present");
    return HWC3::Error::BadLayer;
  }

  HWCLayer *hwc_layer = GetHWCLayer(layer);
  if (hwc_layer == nullptr) {
    return HWC3::Error::BadLayer;
  }
  if (hwc_layer->GetDeviceSelectedCompositionType() != Composition::CURSOR) {
    return HWC3::Error::None;
  }
  if (display_intf_->IsValidated()) {
    // the device is currently in the middle of the validate/present sequence,
    // cannot set the Position(as per HWC3 spec)
    return HWC3::Error::NotValidated;
  }

  DisplayState state;
  if (display_intf_->GetDisplayState(&state) == kErrorNone) {
    if (state != kStateOn) {
      return HWC3::Error::None;
    }
  }

  // TODO(user): HWC1.5 was not letting SetCursorPosition before validateDisplay,
  // but HWC3.0 doesn't let setting cursor position after validate before present.
  // Need to revisit.

  auto error = display_intf_->SetCursorPosition(x, y);
  if (error != kErrorNone) {
    if (error == kErrorShutDown) {
      shutdown_pending_ = true;
      return HWC3::Error::None;
    }

    DLOGE("Failed for x = %d y = %d, Error = %d", x, y, error);
    return HWC3::Error::BadDisplay;
  }

  return HWC3::Error::None;
}

int HWCDisplay::OnMinHdcpEncryptionLevelChange(uint32_t min_enc_level) {
  DisplayError error = display_intf_->OnMinHdcpEncryptionLevelChange(min_enc_level);
  if (error != kErrorNone) {
    DLOGE("Failed. Error = %d", error);
    return -1;
  }

  return 0;
}

void HWCDisplay::MarkLayersForGPUBypass() {
  for (auto hwc_layer : layer_set_) {
    auto layer = hwc_layer->GetSDMLayer();
    layer->composition = kCompositionSDE;
  }
}

void HWCDisplay::MarkLayersForClientComposition() {
  // ClientComposition - GPU comp, to acheive this, set skip flag so that
  // SDM does not handle this layer and hwc_layer composition will be
  // set correctly at the end of Prepare.
  DLOGV_IF(kTagClient, "HWC Layers marked for GPU comp");
  for (auto hwc_layer : layer_set_) {
    Layer *layer = hwc_layer->GetSDMLayer();
    layer->flags.skip = true;
  }
  layer_stack_.flags.skip_present = true;
}

void HWCDisplay::ApplyScanAdjustment(Rect *display_frame) {}

int HWCDisplay::ToggleScreenUpdates(bool enable) {
  if (secure_event_ != kSecureEventMax) {
    DLOGW("Toggle screen updates is not supported when TUI transition in progress");
    return -ENOTSUP;
  }
  display_paused_ = enable ? false : true;
  callbacks_->Refresh(id_);
  return 0;
}

int HWCDisplay::ColorSVCRequestRoute(const PPDisplayAPIPayload &in_payload,
                                     PPDisplayAPIPayload *out_payload,
                                     PPPendingParams *pending_action) {
  int ret = 0;

  if (display_intf_)
    ret = display_intf_->ColorSVCRequestRoute(in_payload, out_payload, pending_action);
  else
    ret = -EINVAL;

  return ret;
}

void HWCDisplay::SolidFillPrepare() {
  if (solid_fill_enable_) {
    if (solid_fill_layer_ == NULL) {
      // Create a dummy layer here
      solid_fill_layer_ = new Layer();
    }
    uint32_t primary_width = 0, primary_height = 0;
    GetMixerResolution(&primary_width, &primary_height);

    LayerBuffer *layer_buffer = &solid_fill_layer_->input_buffer;
    layer_buffer->width = primary_width;
    layer_buffer->height = primary_height;
    layer_buffer->unaligned_width = primary_width;
    layer_buffer->unaligned_height = primary_height;

    solid_fill_layer_->composition = kCompositionGPU;
    solid_fill_layer_->src_rect = solid_fill_rect_;
    solid_fill_layer_->dst_rect = solid_fill_rect_;

    solid_fill_layer_->blending = kBlendingPremultiplied;
    solid_fill_layer_->solid_fill_color = 0;
    solid_fill_layer_->solid_fill_info.bit_depth = solid_fill_color_.bit_depth;
    solid_fill_layer_->solid_fill_info.red = solid_fill_color_.red;
    solid_fill_layer_->solid_fill_info.blue = solid_fill_color_.blue;
    solid_fill_layer_->solid_fill_info.green = solid_fill_color_.green;
    solid_fill_layer_->solid_fill_info.alpha = solid_fill_color_.alpha;
    solid_fill_layer_->frame_rate = 60;
    solid_fill_layer_->visible_regions.push_back(solid_fill_layer_->dst_rect);
    solid_fill_layer_->flags.updating = 1;
    solid_fill_layer_->flags.solid_fill = true;
  } else {
    // delete the dummy layer
    delete solid_fill_layer_;
    solid_fill_layer_ = NULL;
  }

  if (solid_fill_enable_ && solid_fill_layer_) {
    BuildSolidFillStack();
    MarkLayersForGPUBypass();
  }

  return;
}

int HWCDisplay::GetVisibleDisplayRect(Rect *visible_rect) {
  if (!IsValid(display_rect_)) {
    return -EINVAL;
  }

  visible_rect->left = INT(display_rect_.left);
  visible_rect->top = INT(display_rect_.top);
  visible_rect->right = INT(display_rect_.right);
  visible_rect->bottom = INT(display_rect_.bottom);
  DLOGI("Visible Display Rect(%d %d %d %d)", visible_rect->left, visible_rect->top,
        visible_rect->right, visible_rect->bottom);

  return 0;
}

int HWCDisplay::HandleSecureSession(const std::bitset<kSecureMax> &secure_sessions,
                                    bool *power_on_pending, bool is_active_secure_display) {
  if (!power_on_pending) {
    return -EINVAL;
  }

  if (active_secure_sessions_[kSecureDisplay] != secure_sessions[kSecureDisplay]) {
    if (secure_sessions[kSecureDisplay]) {
      pending_power_mode_ = current_power_mode_;
      HWC3::Error error = SetPowerMode(PowerMode::OFF, true /* teardown */);
      if (error != HWC3::Error::None) {
        DLOGE("SetPowerMode failed. Error = %d", error);
      }
    } else {
      *power_on_pending = (pending_power_mode_ != PowerMode::OFF) ? true : false;
    }

    DLOGI("SecureDisplay state changed from %d to %d for display %" PRId64 " %d-%d",
          active_secure_sessions_.test(kSecureDisplay), secure_sessions.test(kSecureDisplay), id_,
          sdm_id_, type_);
  }

  if (active_secure_sessions_[kSecureCamera] != secure_sessions[kSecureCamera]) {
    if (secure_sessions[kSecureCamera]) {
      pending_power_mode_ = current_power_mode_;
      HWC3::Error error = SetPowerMode(PowerMode::OFF, true /* teardown */);
      if (error != HWC3::Error::None) {
        DLOGE("SetPowerMode failed. Error = %d", error);
      }
    } else {
      *power_on_pending = (pending_power_mode_ != PowerMode::OFF) ? true : false;
    }

    DLOGI("SecureCamera state changed from %d to %d for display %" PRId64 " %d-%d",
          active_secure_sessions_.test(kSecureCamera), secure_sessions.test(kSecureCamera), id_,
          sdm_id_, type_);
  }
  active_secure_sessions_ = secure_sessions;
  return 0;
}

int HWCDisplay::SetActiveDisplayConfig(uint32_t config) {
  uint32_t current_config = 0;
  display_intf_->GetActiveConfig(&current_config);
  if (config == current_config) {
    return 0;
  }

  DisplayError error = display_intf_->SetActiveConfig(config);
  if (error != kErrorNone) {
    DLOGE("Failed to set %d config! Error: %d", config, error);
    return -EINVAL;
  }

  SetActiveConfigIndex(config);
  return 0;
}

int HWCDisplay::SetNoisePlugInOverride(bool override_en, int32_t attn, int32_t noise_zpos) {
  DisplayError error = display_intf_->SetNoisePlugInOverride(override_en, attn, noise_zpos);
  if (error != kErrorNone) {
    DLOGE("Display ID: %" PRId64 " failed to override NoisePlugIn! Error: %d", id_, error);
    return -EINVAL;
  }
  callbacks_->Refresh(id_);
  return 0;
}

int HWCDisplay::GetActiveDisplayConfig(uint32_t *config) {
  return display_intf_->GetActiveConfig(config) == kErrorNone ? 0 : -1;
}

int HWCDisplay::GetDisplayConfigCount(uint32_t *count) {
  return display_intf_->GetNumVariableInfoConfigs(count) == kErrorNone ? 0 : -1;
}

int HWCDisplay::GetDisplayAttributesForConfig(int config,
                                              DisplayConfigVariableInfo *display_attributes) {
  return display_intf_->GetConfig(UINT32(config), display_attributes) == kErrorNone ? 0 : -1;
}

int HWCDisplay::GetSupportedDisplayRefreshRates(std::vector<uint32_t> *supported_refresh_rates) {
  if (!supported_refresh_rates) {
    return -1;
  }

  Config active_config = 0;
  GetActiveConfig(&active_config);

  int32_t config_group, active_config_group;
  auto error = GetDisplayAttribute(active_config, HwcAttribute::CONFIG_GROUP, &active_config_group);
  if (error != HWC3::Error::None) {
    DLOGE("Failed to get config group of active config");
    return -1;
  }

  supported_refresh_rates->resize(0);
  for (auto &config : variable_config_map_) {
    error = GetDisplayAttribute(config.first, HwcAttribute::CONFIG_GROUP, &config_group);
    if (error != HWC3::Error::None) {
      DLOGE("Failed to get config group for config index: %u", config.first);
      return -1;
    }
    if (active_config_group == config_group) {
      DisplayConfigVariableInfo const &config_info = config.second;
      supported_refresh_rates->push_back(config_info.fps);
    }
  }

  DLOGI("Count of supported refresh rates = %u for active config group = %d",
        UINT32(supported_refresh_rates->size()), active_config_group);
  return 0;
}

bool HWCDisplay::IsLayerUpdating(HWCLayer *hwc_layer) {
  auto layer = hwc_layer->GetSDMLayer();
  // Layer should be considered updating if
  //   a) layer is in single buffer mode, or
  //   b) layer is front buffer rendering, or
  //   c) valid dirty_regions(android specific hint for updating status), or
  //   d) layer stack geometry has changed (TODO(user): Remove when SDM accepts
  //      geometry_changed as bit fields), or
  //   e) layer is marked as video.
  return (layer->flags.single_buffer || layer->flags.front_buffer ||
          hwc_layer->IsSurfaceUpdated() || hwc_layer->GetGeometryChanges() ||
          layer->input_buffer.flags.video);
}

DisplayClass HWCDisplay::GetDisplayClass() {
  return display_class_;
}

void HWCDisplay::Dump(std::ostringstream *os) {
  *os << "\n------------HWC----------------\n";
  *os << "HWC3 display_id: " << id_ << std::endl;
  for (auto layer : layer_set_) {
    auto sdm_layer = layer->GetSDMLayer();
    auto transform = sdm_layer->transform;
    *os << "layer: " << std::setw(4) << layer->GetId();
    *os << " name: " << std::setw(100) << layer->GetName();
    *os << " z: " << layer->GetZ();
    *os << " composition: " << to_string(layer->GetOrigClientRequestedCompositionType()).c_str();
    *os << "/" << to_string(layer->GetDeviceSelectedCompositionType()).c_str();
    *os << " alpha: " << std::to_string(sdm_layer->plane_alpha).c_str();
    *os << " format: " << std::setw(22) << GetFormatString(sdm_layer->input_buffer.format);
    *os << " dataspace:" << std::hex << "0x" << std::setw(8) << std::setfill('0')
        << layer->GetLayerDataspace() << std::dec << std::setfill(' ');
    *os << " transform: " << transform.rotation << "/" << transform.flip_horizontal << "/"
        << transform.flip_vertical;
    *os << " buffer_id: " << std::hex << "0x" << sdm_layer->input_buffer.buffer_id << std::dec;
    *os << " secure: " << layer->IsProtected() << std::endl;
  }

  if (has_client_composition_) {
    *os << "\n---------client target---------\n";
    auto sdm_layer = client_target_->GetSDMLayer();
    *os << "format: " << std::setw(14) << GetFormatString(sdm_layer->input_buffer.format);
    *os << " dataspace:" << std::hex << "0x" << std::setw(8) << std::setfill('0')
        << client_target_->GetLayerDataspace() << std::dec << std::setfill(' ');
    *os << "  buffer_id: " << std::hex << "0x" << sdm_layer->input_buffer.buffer_id << std::dec;
    *os << " secure: " << client_target_->IsProtected() << std::endl;
  }

  if (layer_stack_invalid_) {
    *os << "\n Layers added or removed but not reflected to SDM's layer stack yet\n";
    return;
  }

  if (color_mode_) {
    *os << "\n----------Color Modes---------\n";
    color_mode_->Dump(os);
  }

  if (display_intf_) {
    *os << "\n------------SDM----------------\n";
    *os << display_intf_->Dump();
  }

  *os << "\n";
}

HWC3::Error HWCDisplay::GetDisplayIdentificationData(uint8_t *out_port, uint32_t *out_data_size,
                                                     uint8_t *out_data) {
  DisplayError ret = display_intf_->GetDisplayIdentificationData(out_port, out_data_size, out_data);
  if (ret != kErrorNone) {
    DLOGE("Failed due to SDM/Driver (err = %d, disp id = %" PRIu64 " %d-%d", ret, id_, sdm_id_,
          type_);
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::SetDisplayElapseTime(uint64_t time) {
  elapse_timestamp_ = time;
  return HWC3::Error::None;
}

bool HWCDisplay::IsDisplayCommandMode() {
  return is_cmd_mode_;
}

HWC3::Error HWCDisplay::SetDisplayedContentSamplingEnabledVndService(bool enabled) {
  return HWC3::Error::Unsupported;
}

HWC3::Error HWCDisplay::SetDisplayedContentSamplingEnabled(bool enabled, uint8_t component_mask,
                                                           uint64_t max_frames) {
  DLOGV("Request to start/stop histogram thread not supported on this display");
  return HWC3::Error::Unsupported;
}

HWC3::Error HWCDisplay::GetDisplayedContentSamplingAttributes(int32_t *format, int32_t *dataspace,
                                                              uint8_t *supported_components) {
  return HWC3::Error::Unsupported;
}

HWC3::Error HWCDisplay::GetDisplayedContentSample(
    uint64_t max_frames, uint64_t timestamp, uint64_t *numFrames,
    int32_t samples_size[NUM_HISTOGRAM_COLOR_COMPONENTS],
    uint64_t *samples[NUM_HISTOGRAM_COLOR_COMPONENTS]) {
  return HWC3::Error::Unsupported;
}

// Skip SDM prepare if all the layers in the current draw cycle are marked as Skip and
// previous draw cycle had GPU Composition, as the resources for GPU Target layer have
// already been validated and configured to the driver.
bool HWCDisplay::CanSkipSdmPrepare(uint32_t *num_types, uint32_t *num_requests) {
  if (!display_intf_->IsValidated() || layer_set_.empty()) {
    return false;
  }

  if (display_intf_->HasDemura()) {
    return false;
  }

  bool skip_prepare = true;
  for (auto hwc_layer : layer_set_) {
    if (!hwc_layer->GetSDMLayer()->flags.skip ||
        (hwc_layer->GetDeviceSelectedCompositionType() != Composition::CLIENT)) {
      skip_prepare = false;
      layer_changes_.clear();
      break;
    }
    if (hwc_layer->GetClientRequestedCompositionType() != Composition::CLIENT) {
      layer_changes_[hwc_layer->GetId()] = Composition::CLIENT;
    }
  }

  if (skip_prepare) {
    *num_types = UINT32(layer_changes_.size());
    *num_requests = 0;
    layer_stack_invalid_ = false;
    has_client_composition_ = true;
  }

  return skip_prepare;
}

void HWCDisplay::UpdateRefreshRate() {
  for (auto hwc_layer : layer_set_) {
    if (hwc_layer->HasMetaDataRefreshRate()) {
      continue;
    }
    auto layer = hwc_layer->GetSDMLayer();
    layer->frame_rate = std::min(current_refresh_rate_, HWCDisplay::GetThrottlingRefreshRate());
  }
}

int32_t HWCDisplay::SetClientTargetDataSpace(int32_t dataspace) {
  if (client_target_->GetLayerDataspace() != dataspace) {
    client_target_->SetLayerDataspace(dataspace);
    Layer *sdm_layer = client_target_->GetSDMLayer();
    // Data space would be validated at GetClientTargetSupport, so just use here.
    sdm::GetSDMColorSpace(client_target_->GetLayerDataspace(),
                          &sdm_layer->input_buffer.color_metadata);
  }

  return 0;
}

void HWCDisplay::WaitOnPreviousFence() {
  DisplayConfigFixedInfo display_config;
  display_intf_->GetConfig(&display_config);
  if (!display_config.is_cmdmode) {
    return;
  }

  if (Fence::Wait(release_fence_) != kErrorNone) {
    DLOGW("sync_wait error errno = %d, desc = %s", errno, strerror(errno));
    return;
  }
}

void HWCDisplay::GetLayerStack(HWCLayerStack *stack) {
  stack->client_target = client_target_;
  stack->layer_map = layer_map_;
  stack->layer_set = layer_set_;
}

void HWCDisplay::SetLayerStack(HWCLayerStack *stack) {
  client_target_ = stack->client_target;
  layer_map_ = stack->layer_map;
  layer_set_ = stack->layer_set;
}

bool HWCDisplay::CheckResourceState(bool *res_exhausted) {
  if (display_intf_) {
    return display_intf_->CheckResourceState(res_exhausted);
  }

  return false;
}

void HWCDisplay::UpdateActiveConfig() {
  if (!pending_config_) {
    return;
  }

  DisplayError error = display_intf_->SetActiveConfig(pending_config_index_);
  if (error != kErrorNone) {
    DLOGI("Failed to set %d config", INT(pending_config_index_));
  } else {
    SetActiveConfigIndex(pending_config_index_);
  }

  // Reset pending config.
  pending_config_ = false;
}

int32_t HWCDisplay::GetDisplayConfigGroup(DisplayConfigGroupInfo variable_config) {
  for (auto &config : variable_config_map_) {
    DisplayConfigGroupInfo const &group_info = config.second;
    if (group_info == variable_config) {
      return INT32(config.first);
    }
  }

  return -1;
}

bool HWCDisplay::IsModeSwitchAllowed(uint32_t config) {
  DisplayError error = kErrorNone;
  uint32_t allowed_mode_switch = 0;

  error = display_intf_->IsSupportedOnDisplay(kSupportedModeSwitch, &allowed_mode_switch);
  if (error != kErrorNone) {
    if (error == kErrorResources) {
      DLOGW("Not allowed to switch to mode:%d", config);
      return false;
    }
    DLOGW("Unable to retrieve supported modes for the current device configuration.");
  }

  if (allowed_mode_switch == 0 || (allowed_mode_switch & (1 << config))) {
    DLOGV_IF(kTagClient, "Allowed to switch to mode:%d", config);
    return true;
  }

  DLOGW("Not allowed to switch to mode:%d", config);
  return false;
}

HWC3::Error HWCDisplay::GetDisplayVsyncPeriod(VsyncPeriodNanos *vsync_period) {
  if (GetTransientVsyncPeriod(vsync_period)) {
    return HWC3::Error::None;
  }

  return GetVsyncPeriodByActiveConfig(vsync_period);
}

HWC3::Error HWCDisplay::SetActiveConfigWithConstraints(
    Config config, const VsyncPeriodChangeConstraints *vsync_period_change_constraints,
    VsyncPeriodChangeTimeline *out_timeline) {
  DTRACE_SCOPED();

  if (variable_config_map_.find(config) == variable_config_map_.end()) {
    DLOGE("Invalid config: %d", config);
    return HWC3::Error::BadConfig;
  }

  if (!IsModeSwitchAllowed(config)) {
    return HWC3::Error::BadConfig;
  }

  // DRM driver expects DRM_PREFERRED_MODE to be set as part of first commit
  if (!IsFirstCommitDone()) {
    // Store client's config.
    // Set this as part of post commit.
    pending_first_commit_config_ = true;
    pending_first_commit_config_index_ = config;
    DLOGI("Defer config change to %d until first commit", UINT32(config));
    return HWC3::Error::None;
  } else if (pending_first_commit_config_) {
    // Config override request from client.
    // Honour latest request.
    pending_first_commit_config_ = false;
  }

  // Cache refresh rate set by client.
  DisplayConfigVariableInfo info = {};
  GetDisplayAttributesForConfig(INT(config), &info);
  active_refresh_rate_ = info.fps;

  if (vsync_period_change_constraints->seamlessRequired && !AllowSeamless(config)) {
    DLOGE("Seamless switch to the config: %d, is not allowed!", config);
    return HWC3::Error::SeamlessNotAllowed;
  }

  VsyncPeriodNanos vsync_period;
  if (GetDisplayVsyncPeriod(&vsync_period) != HWC3::Error::None) {
    return HWC3::Error::BadConfig;
  }

  std::tie(out_timeline->refreshTimeNanos, out_timeline->newVsyncAppliedTimeNanos) =
      RequestActiveConfigChange(config, vsync_period,
                                vsync_period_change_constraints->desiredTimeNanos);

  out_timeline->refreshRequired = true;
  if (is_client_up_ && (info.x_pixels != fb_width_ || info.y_pixels != fb_height_)) {
    out_timeline->refreshRequired = false;
    fb_width_ = info.x_pixels;
    fb_height_ = info.y_pixels;
  }
  return HWC3::Error::None;
}

void HWCDisplay::ProcessActiveConfigChange() {
  if (!IsActiveConfigReadyToSubmit(systemTime(SYSTEM_TIME_MONOTONIC))) {
    return;
  }

  DTRACE_SCOPED();
  VsyncPeriodNanos vsync_period;
  if (GetVsyncPeriodByActiveConfig(&vsync_period) == HWC3::Error::None) {
    SubmitActiveConfigChange(vsync_period);
  }
}

HWC3::Error HWCDisplay::GetVsyncPeriodByActiveConfig(VsyncPeriodNanos *vsync_period) {
  Config active_config;

  auto error = GetCachedActiveConfig(&active_config);
  if (error != HWC3::Error::None) {
    DLOGE("Failed to get active config!");
    return error;
  }

  int32_t active_vsync_period;
  error = GetDisplayAttribute(active_config, HwcAttribute::VSYNC_PERIOD, &active_vsync_period);
  if (error != HWC3::Error::None) {
    DLOGE("Failed to get VsyncPeriod of config: %d", active_config);
    return error;
  }

  *vsync_period = static_cast<VsyncPeriodNanos>(active_vsync_period);
  return HWC3::Error::None;
}

bool HWCDisplay::GetTransientVsyncPeriod(VsyncPeriodNanos *vsync_period) {
  std::lock_guard<std::mutex> lock(transient_refresh_rate_lock_);
  auto now = systemTime(SYSTEM_TIME_MONOTONIC);

  while (!transient_refresh_rate_info_.empty()) {
    if (IsActiveConfigApplied(now, transient_refresh_rate_info_.front().vsync_applied_time)) {
      transient_refresh_rate_info_.pop_front();
    } else {
      *vsync_period = transient_refresh_rate_info_.front().transient_vsync_period;
      return true;
    }
  }

  return false;
}

std::tuple<int64_t, int64_t> HWCDisplay::RequestActiveConfigChange(
    Config config, VsyncPeriodNanos current_vsync_period, int64_t desired_time) {
  int64_t refresh_time = 0;
  int64_t applied_time = 0;
  std::tie(refresh_time, applied_time) =
      EstimateVsyncPeriodChangeTimeline(current_vsync_period, desired_time);

  pending_refresh_rate_config_ = config;
  pending_refresh_rate_refresh_time_ = refresh_time;
  pending_refresh_rate_applied_time_ = applied_time;

  return std::make_tuple(refresh_time, applied_time);
}

std::tuple<int64_t, int64_t> HWCDisplay::EstimateVsyncPeriodChangeTimeline(
    VsyncPeriodNanos current_vsync_period, int64_t desired_time) {
  const auto now = systemTime(SYSTEM_TIME_MONOTONIC);
  const auto delta = desired_time - now;
  const auto refresh_rate_activate_period = current_vsync_period * vsyncs_to_apply_rate_change_;
  nsecs_t refresh_time;

  if (delta < 0) {
    refresh_time = now + (delta % current_vsync_period);
  } else if (delta < refresh_rate_activate_period) {
    refresh_time = now + (delta % current_vsync_period) - current_vsync_period;
  } else {
    refresh_time = desired_time - refresh_rate_activate_period;
  }

  const auto applied_time = refresh_time + refresh_rate_activate_period;
  return std::make_tuple(refresh_time, applied_time);
}

void HWCDisplay::SubmitActiveConfigChange(VsyncPeriodNanos current_vsync_period) {
  HWC3::Error error = SubmitDisplayConfig(pending_refresh_rate_config_);
  if (error != HWC3::Error::None) {
    return;
  }

  std::lock_guard<std::mutex> lock(transient_refresh_rate_lock_);
  VsyncPeriodChangeTimeline timeline = {};
  std::tie(timeline.refreshTimeNanos, timeline.newVsyncAppliedTimeNanos) =
      EstimateVsyncPeriodChangeTimeline(current_vsync_period, pending_refresh_rate_refresh_time_);

  transient_refresh_rate_info_.push_back({current_vsync_period, timeline.newVsyncAppliedTimeNanos});
  if (timeline.newVsyncAppliedTimeNanos != pending_refresh_rate_applied_time_) {
    timeline.refreshRequired = false;
    callbacks_->VsyncPeriodTimingChanged(id_, &timeline);
  }

  pending_refresh_rate_config_ = UINT_MAX;
  pending_refresh_rate_refresh_time_ = INT64_MAX;
  pending_refresh_rate_applied_time_ = INT64_MAX;
}

bool HWCDisplay::IsActiveConfigReadyToSubmit(int64_t time) {
  return ((pending_refresh_rate_config_ != UINT_MAX) &&
          IsTimeAfterOrEqualVsyncTime(time, pending_refresh_rate_refresh_time_));
}

bool HWCDisplay::IsActiveConfigApplied(int64_t time, int64_t vsync_applied_time) {
  return IsTimeAfterOrEqualVsyncTime(time, vsync_applied_time);
}

bool HWCDisplay::IsSameGroup(Config config_id1, Config config_id2) {
  const auto &variable_config1 = variable_config_map_.find(config_id1);
  const auto &variable_config2 = variable_config_map_.find(config_id2);

  if ((variable_config1 == variable_config_map_.end()) ||
      (variable_config2 == variable_config_map_.end())) {
    DLOGE("Invalid config: %u, %u", config_id1, config_id2);
    return false;
  }

  const DisplayConfigGroupInfo &config_group1 = variable_config1->second;
  const DisplayConfigGroupInfo &config_group2 = variable_config2->second;

  return (config_group1 == config_group2);
}

bool HWCDisplay::AllowSeamless(Config config) {
  Config active_config;
  auto error = GetCachedActiveConfig(&active_config);
  if (error != HWC3::Error::None) {
    DLOGE("Failed to get active config!");
    return false;
  }

  return IsSameGroup(active_config, config);
}

HWC3::Error HWCDisplay::SubmitDisplayConfig(Config config) {
  DTRACE_SCOPED();

  Config current_config = 0;
  GetActiveConfig(&current_config);

  DisplayError error = display_intf_->SetActiveConfig(config);
  if (error == kErrorDeferred) {
    DLOGW("Failed to set new config:%d from current config:%d! Error: %d", config, current_config,
          error);
    return HWC3::Error::BadConfig;
  } else if (error != kErrorNone) {
    DLOGE("Failed to set new config:%d from current config:%d! Error: %d", config, current_config,
          error);
    return HWC3::Error::BadConfig;
  }

  SetActiveConfigIndex(config);
  DLOGI("Active configuration changed from config %d to %d", current_config, config);

  // Cache refresh rate set by client.
  DisplayConfigVariableInfo info = {};
  GetDisplayAttributesForConfig(INT(config), &info);
  active_refresh_rate_ = info.fps;

  DisplayConfigVariableInfo current_config_info = {};
  GetDisplayAttributesForConfig(INT(current_config), &current_config_info);
  // Set fb config if new resolution differs
  if (info.x_pixels != current_config_info.x_pixels ||
      info.y_pixels != current_config_info.y_pixels) {
    if (SetFrameBufferResolution(info.x_pixels, info.y_pixels)) {
      return HWC3::Error::BadParameter;
    }
  }

  return HWC3::Error::None;
}

HWC3::Error HWCDisplay::GetCachedActiveConfig(Config *active_config) {
  int config_index = GetActiveConfigIndex();
  if ((config_index < 0) || (config_index >= hwc_config_map_.size())) {
    return GetActiveConfig(active_config);
  }

  *active_config = static_cast<Config>(hwc_config_map_.at(config_index));
  return HWC3::Error::None;
}

void HWCDisplay::SetActiveConfigIndex(int index) {
  std::lock_guard<std::mutex> lock(active_config_lock_);
  active_config_index_ = index;
}

int HWCDisplay::GetActiveConfigIndex() {
  std::lock_guard<std::mutex> lock(active_config_lock_);
  return active_config_index_;
}

DisplayError HWCDisplay::ValidateTUITransition(SecureEvent secure_event) {
  switch (secure_event) {
    case kTUITransitionPrepare:
      if (secure_event_ != kSecureEventMax) {
        DLOGE("Invalid TUI transition from %d to %d", secure_event_, secure_event);
        return kErrorParameters;
      }
      break;
    case kTUITransitionUnPrepare:
      if (secure_event_ != kTUITransitionPrepare) {
        DLOGE("Invalid TUI transition from %d to %d", secure_event_, secure_event);
        return kErrorParameters;
      }
      break;
    case kTUITransitionStart:
      if (secure_event_ != kTUITransitionPrepare) {
        DLOGE("Invalid TUI transition from %d to %d", secure_event_, secure_event);
        return kErrorParameters;
      }
      break;
    case kTUITransitionEnd:
      if (secure_event_ != kTUITransitionStart) {
        DLOGE("Invalid TUI transition from %d to %d", secure_event_, secure_event);
        return kErrorParameters;
      }
      break;
    default:
      DLOGE("Invalid secure event %d", secure_event);
      return kErrorParameters;
  }
  return kErrorNone;
}

DisplayError HWCDisplay::HandleSecureEvent(SecureEvent secure_event, bool *needs_refresh,
                                           bool update_event_only) {
  if (secure_event == secure_event_) {
    DLOGW("Same requested secure_event=%d", secure_event);
    return kErrorNone;
  }

  if (update_event_only) {
    if (secure_event == kTUITransitionPrepare) {
      secure_event_ = kTUITransitionPrepare;
    } else {
      secure_event_ = kSecureEventMax;
    }
    return kErrorNone;
  }

  DisplayError err = ValidateTUITransition(secure_event);
  if (err != kErrorNone) {
    return err;
  }

  err = display_intf_->HandleSecureEvent(secure_event, needs_refresh);
  if (err != kErrorNone) {
    if (err == kErrorPermission) {
      DLOGW("Handle secure event failed");
    } else {
      DLOGE("Handle secure event failed");
    }
    return err;
  }

  if (secure_event == kTUITransitionEnd)
    color_mode_->ReapplyMode();

  if (secure_event == kTUITransitionEnd || secure_event == kTUITransitionUnPrepare) {
    DLOGI("Resume display %d-%d", sdm_id_, type_);
    display_paused_ = false;
    display_pause_pending_ = false;
    if (*needs_refresh == false) {
      secure_event_ = kSecureEventMax;
      return kErrorNone;
    }
  } else if (secure_event == kTUITransitionPrepare || secure_event == kTUITransitionStart) {
    if (*needs_refresh) {
      display_pause_pending_ = true;
    } else {
      DLOGI("Pause display %d-%d", sdm_id_, type_);
      display_paused_ = true;
    }
  }

  secure_event_ = secure_event;

  return kErrorNone;
}

DisplayError HWCDisplay::PostHandleSecureEvent(SecureEvent secure_event) {
  DisplayError err = display_intf_->PostHandleSecureEvent(secure_event);
  if (err == kErrorNone) {
    if (secure_event == kTUITransitionEnd || secure_event == kTUITransitionUnPrepare) {
      secure_event_ = kSecureEventMax;
      return kErrorNone;
    }
    DLOGV("Set secure_event to %d", secure_event);
    secure_event_ = secure_event;
  }
  return err;
}

int HWCDisplay::GetCwbBufferResolution(CwbConfig *cwb_config, uint32_t *x_pixels,
                                       uint32_t *y_pixels) {
  if (!x_pixels || !y_pixels) {
    return -1;
  }
  DisplayError ret = display_intf_->GetCwbBufferResolution(cwb_config, x_pixels, y_pixels);
  if (ret != kErrorNone) {
    DLOGE("Failed to get Output buffer resolution.");
    return -1;
  }
  return 0;
}

void HWCDisplay::ReleaseFrameDumpResources() {
  std::unique_lock<std::mutex> lock(frame_dump_config_lock_);
  if (output_buffer_info_.alloc_buffer_info.fd < 0 && !output_buffer_base_ && !dump_frame_count_) {
    return;
  }

  if (output_buffer_base_ &&
      munmap(output_buffer_base_, output_buffer_info_.alloc_buffer_info.size) != 0) {
    DLOGW("unmap failed with err %d", errno);
  }

  if (output_buffer_info_.alloc_buffer_info.fd > 0 && buffer_allocator_ &&
      buffer_allocator_->FreeBuffer(&output_buffer_info_) != 0) {
    DLOGW("FreeBuffer failed");
  }

  output_buffer_info_ = {};
  output_buffer_cwb_config_ = {};
  output_buffer_base_ = nullptr;
  dump_frame_count_ = 0;
  dump_frame_index_ = 0;
  dump_output_to_file_ = false;
  DLOGI("Output Frame dumping buffer is freed for display %d-%d!", sdm_id_, type_);
}

DisplayError HWCDisplay::TeardownConcurrentWriteback() {
  if (!display_intf_->HandleCwbTeardown()) {
    return kErrorNotSupported;
  }

  bool pending_cwb_request = false;
  {
    std::unique_lock<std::mutex> lock(cwb_mutex_);
    pending_cwb_request = !!cwb_buffer_map_.size();
  }

  if (!pending_cwb_request) {
    frame_capture_buffer_queued_ = false;
    frame_capture_status_ = 0;
    ReleaseFrameDumpResources();
  }

  return kErrorNone;
}

void HWCDisplay::MMRMEvent(bool restricted) {
  mmrm_restricted_ = restricted;
  callbacks_->Refresh(id_);
}

void HWCDisplay::SetDrawMethod() {
  if (draw_method_set_) {
    return;
  }

  // Default behaviour.
  // Init draw method from fixed config.
  // Update it if client supports setting next FBT.
  DisplayConfigFixedInfo fixed_info = {};
  display_intf_->GetConfig(&fixed_info);

  draw_method_ = kDrawDefault;
  if (fixed_info.supports_unified_draw) {
    // Composer extn is not present.
    draw_method_ = kDrawUnified;
  }

  DLOGI("Set draw method: %d", draw_method_);
  display_intf_->SetDrawMethod(draw_method_);

  draw_method_set_ = true;
}

HWC3::Error HWCDisplay::TryDrawMethod(DrawMethod client_drawMethod) {
  auto status = HWC3::Error::None;
  DisplayConfigFixedInfo fixed_config;
  display_intf_->GetConfig(&fixed_config);
  bool supports_unified_draw = fixed_config.supports_unified_draw;
  if (!supports_unified_draw) {
    // Check if driver support is present.
    // If driver doesn't support return unsupported and set default method.
    draw_method_ = kDrawDefault;
    status = HWC3::Error::Unsupported;
    DLOGI("Enabling default draw method");
  } else if (client_drawMethod != DrawMethod::UNIFIED_DRAW) {
    // Driver supports unified draw.
    // If client doesnt support unified draw, limit to kDrawUnified.
    draw_method_ = kDrawUnified;
    status = HWC3::Error::Unsupported;
    DLOGI("Enabling unified draw");
  } else {
    // Driver and client supports unified draw.
    draw_method_ = kDrawUnifiedWithGPUTarget;
    status = HWC3::Error::None;
    DLOGI("Enabling unified draw with GPU target");
  }

  display_intf_->SetDrawMethod(draw_method_);

  draw_method_set_ = true;

  return status;
}

HWC3::Error HWCDisplay::SetReadbackBuffer(const native_handle_t *buffer,
                                          shared_ptr<Fence> acquire_fence, CwbConfig cwb_config,
                                          CWBClient client) {
  if (current_power_mode_ == PowerMode::OFF || current_power_mode_ == PowerMode::DOZE_SUSPEND) {
    DLOGW("CWB requested on either Powered-Off or Doze-Suspended display.");
    return HWC3::Error::BadDisplay;
  }

  if (secure_event_ != kSecureEventMax) {
    DLOGW("CWB is not supported as TUI transition is in progress");
    return HWC3::Error::Unsupported;
  }

  void *hdl = const_cast<native_handle_t *>(buffer);

  if (!buffer) {
    DLOGE("Bad parameter: handle is null");
    return HWC3::Error::BadParameter;
  }

  int fd;
  gralloc::GetMetaDataValue(hdl, (int64_t)qtigralloc::MetadataType_FD.value, &fd);
  if (fd < 0) {
    DLOGE("Bad parameter: fd is null");
    return HWC3::Error::BadParameter;
  }

  LayerBuffer output_buffer = {};
  // Configure the output buffer as Readback buffer
  auto err = gralloc::GetMetaDataValue(
      hdl, (int64_t)qtigralloc::MetadataType_AlignedWidthInPixels.value, &output_buffer.width);
  if (err != gralloc::Error::NONE) {
    DLOGE("Failed to retrieve aligned width");
  }
  err = gralloc::GetMetaDataValue(
      hdl, (int64_t)qtigralloc::MetadataType_AlignedHeightInPixels.value, &output_buffer.height);
  if (err != gralloc::Error::NONE) {
    DLOGE("Failed to retrieve aligned height");
  }
  err = gralloc::GetMetaDataValue(hdl, (int64_t)StandardMetadataType::WIDTH,
                                  &output_buffer.unaligned_width);
  if (err != gralloc::Error::NONE) {
    DLOGE("Failed to retrieve unaligned width");
  }
  err = gralloc::GetMetaDataValue(hdl, (int64_t)StandardMetadataType::HEIGHT,
                                  &output_buffer.unaligned_height);
  if (err != gralloc::Error::NONE) {
    DLOGE("Failed to retrieve unaligned height");
  }
  int format, flag;
  err = gralloc::GetMetaDataValue(hdl, (int64_t)StandardMetadataType::PIXEL_FORMAT_REQUESTED,
                                  &format);
  if (err != gralloc::Error::NONE) {
    DLOGE("Failed to retrieve format");
  }
  err = gralloc::GetMetaDataValue(hdl, (int64_t)QTI_PRIVATE_FLAGS, &flag);
  if (err != gralloc::Error::NONE) {
    DLOGE("Failed to retrieve flag");
  }
  output_buffer.format = HWCLayer::GetSDMFormat(format, flag);
  err = gralloc::GetMetaDataValue(hdl, (int64_t)QTI_FD, &output_buffer.planes[0].fd);
  if (err != gralloc::Error::NONE) {
    DLOGE("Failed to retrieve file descriptor");
  }
  err = gralloc::GetMetaDataValue(hdl, (int64_t)QTI_ALIGNED_WIDTH_IN_PIXELS,
                                  &output_buffer.planes[0].stride);
  if (err != gralloc::Error::NONE) {
    DLOGE("Failed to retrieve stride");
  }
  err = gralloc::GetMetaDataValue(hdl, (int64_t)StandardMetadataType::BUFFER_ID,
                                  &output_buffer.handle_id);
  if (err != gralloc::Error::NONE) {
    DLOGE("Failed to retrieve buffer id");
  }

  output_buffer.acquire_fence = acquire_fence;

  if (output_buffer.format == kFormatInvalid) {
    DLOGW("Format %d is not supported by SDM", format);
    return HWC3::Error::BadParameter;
  } else if (!display_intf_->IsWriteBackSupportedFormat(output_buffer.format)) {
    DLOGW("WB doesn't support color format : %s .", GetFormatString(output_buffer.format));
    return HWC3::Error::BadParameter;
  }

  CwbConfig config = cwb_config;
  LayerRect &roi = config.cwb_roi;
  LayerRect &full_rect = config.cwb_full_rect;
  CwbTapPoint &tap_point = config.tap_point;

  DisplayError error = kErrorNone;
  error = display_intf_->CaptureCwb(output_buffer, config);
  if (error) {
    if (error == kErrorParameters) {
      DLOGE("Invalid input parameter detected (display %d-%d)!", sdm_id_, type_);
      return HWC3::Error::BadParameter;
    } else if (error == kErrorShutDown) {
      DLOGW("Display %d-%d is not registered for readback!", sdm_id_, type_);
    } else if (error == kErrorResources) {
      DLOGW("Writeback block might busy or not available for display %d-%d!", sdm_id_, type_);
    } else {
      DLOGW("Readback feature is not supported for display %d-%d!", sdm_id_, type_);
    }
    return HWC3::Error::Unsupported;
  }

  {
    std::unique_lock<std::mutex> lock(cwb_mutex_);
    cwb_buffer_map_.emplace(output_buffer.handle_id, client);
    if (cwb_capture_status_map_[client].handle_id == output_buffer.handle_id) {
      cwb_capture_status_map_.erase(client);
    }
  }

  DLOGV_IF(kTagClient,
           "CWB config from client: tap_point %d, CWB ROI Rect(%f %f %f %f), "
           "PU_as_CWB_ROI %d, Cwb full rect : (%f %f %f %f)",
           tap_point, roi.left, roi.top, roi.right, roi.bottom, config.pu_as_cwb_roi,
           full_rect.left, full_rect.top, full_rect.right, full_rect.bottom);

  DLOGV_IF(kTagClient, "Successfully configured the output buffer: cwb_client %d", client);

  return HWC3::Error::None;
}

CWBReleaseFenceError HWCDisplay::GetReadbackBufferFenceForClient(CWBClient client,
                                                                 shared_ptr<Fence> *release_fence) {
  if (client == kCWBClientNone) {
    DLOGE("Invalid CWB client(%d) as argument detected!", client);
    return kCWBReleaseFenceUnknownError;
  }

  if (!release_fence) {
    DLOGE("Null storage for shared pointer of release_fence argument detected, for client: %d",
          client);
    return kCWBReleaseFenceUnknownError;
  }

  auto status = kCWBReleaseFenceErrorNone;
  uint64_t handle_id = 0;

  *release_fence = nullptr;
  // If release fence is available, then try to get it first and keep it out of lock to avoid
  // deadlock with GetOutputBufferAcquireFence call, and validate it later with handle id.
  display_intf_->GetOutputBufferAcquireFence(release_fence);
  {
    std::unique_lock<std::mutex> lock(cwb_mutex_);
    auto &cwb_resp = cwb_capture_status_map_[client];

    if (cwb_resp.handle_id != 0) {
      // If this function is called after either PostCommitLayerStack or NotifyCwbDone call,
      // then release fence can be successfully retrieved from cwb_capture_status_map_.
      handle_id = cwb_resp.handle_id;
      if (cwb_resp.status != kCWBReleaseFenceNotChecked) {
        *release_fence = cwb_resp.release_fence;
      }
      status = cwb_resp.status;
    } else if (layer_stack_.output_buffer != nullptr) {
      // If this function is called before both PostCommitLayerStack and NotifyCwbDone call,
      // then handle_id may be retrieved directly from layer_stack_.output_buffer corresponding
      // to available release fence.
      const auto map_cwb_buffer = cwb_buffer_map_.find(layer_stack_.output_buffer->handle_id);
      if (map_cwb_buffer != cwb_buffer_map_.end() && client == map_cwb_buffer->second) {
        handle_id = layer_stack_.output_buffer->handle_id;
        status = kCWBReleaseFenceNotChecked;
      }
    } else {
      // If this function is called too early, then just need to check that cwb request is
      // persisting, which helps to decide the return status.
      for (auto [id, cl] : cwb_buffer_map_) {
        if (cl == client) {
          handle_id = id;
          break;
        }
      }
      // Avoid to return old release fence, in case of too early call of this function.
      *release_fence = nullptr;
    }

    if (*release_fence != nullptr) {
      // If release fence is successfully retrieved, then no need to keep buffer-client map
      // for this fence any more.
      cwb_buffer_map_.erase(handle_id);
      cwb_capture_status_map_.erase(client);
    }
  }

  if (handle_id != 0) {
    if (*release_fence == nullptr) {
      // If this function is called before commit call or during prepare call, and layer_stack
      // output_buffer is configured, then need to call this function again after few
      // milliseconds, such that commit could execute during this period to get updated
      // release fence.
      DLOGV_IF(kTagQDCM,
               "Need to wait for release fence, and retry to get it for client:%d, "
               "buffer_id: %u",
               client, handle_id);
      status = kCWBReleaseFencePending;
    }
  } else {
    DLOGV_IF(kTagQDCM, "There is no anymore readback buffer fence available for client = %d.",
             client);
    status = kCWBReleaseFenceNotAvailable;
  }

  // Add logs out of sync locked section instead of inline at status update location to avoid
  // unnecessary increment in latency for execution of another resource dependent threads.
  if (status == kCWBReleaseFenceNotChecked || status == kCWBReleaseFenceWaitTimedOut) {
    DLOGV_IF(kTagQDCM,
             "Fence is available, but either fence wait is timed-out, or "
             "CWB Manager is not yet notified for client:%d, buffer_id: %u",
             client, handle_id);
  } else if (status == kCWBReleaseFenceUnknownError) {
    DLOGE("CWB Manager notified unknown error for client:%d, buffer_id: %u", client, handle_id);
  }

  return status;
}

HWC3::Error HWCDisplay::GetReadbackBufferFence(shared_ptr<Fence> *release_fence) {
  auto status = HWC3::Error::None;
  // Get the release fence for kCWBClientComposer only, because this function is designed for
  // accessing through composer interface, for which, it is following standard prototype and
  // return type provided by HWC3 interface. According to HWC3 interface, only two supported
  // return types are provided, where one is HWC3::Error::None and another is
  // HWC3::Error::Unsupported.
  auto error = GetReadbackBufferFenceForClient(kCWBClientComposer, release_fence);

  // if release fence is null pointer, then just return with error.
  if (!release_fence || *release_fence == nullptr) {
    status = HWC3::Error::Unsupported;
    DLOGW("Readback buffer fence is not available! CWBReleaseFenceError: %d", error);
  }

  return status;
}

void HWCDisplay::HandleFrameOutput() {
  // Block on output buffer fence if client is internal.
  // External clients will wait on their thread.
  CWBClient client = kCWBClientNone;
  uint64_t handle_id = 0;

  {
    std::unique_lock<std::mutex> lock(cwb_mutex_);
    if (layer_stack_.output_buffer != nullptr) {
      handle_id = layer_stack_.output_buffer->handle_id;
      const auto map_cwb_buffer = cwb_buffer_map_.find(handle_id);
      if (map_cwb_buffer != cwb_buffer_map_.end()) {
        client = map_cwb_buffer->second;
        auto &cwb_resp = cwb_capture_status_map_[client];
        cwb_resp.handle_id = handle_id;
        cwb_resp.client = client;
        cwb_resp.status = kCWBReleaseFenceNotChecked;  // CWB request status is not yet notified
      } else {
        for (auto &[_, ccs] : cwb_capture_status_map_) {
          if (ccs.handle_id == handle_id) {
            client = ccs.client;
            break;
          }
        }
      }
    } else {
      for (auto &[_, ccs] : cwb_capture_status_map_) {
        if (ccs.handle_id != 0) {
          client = ccs.client;
          handle_id = ccs.handle_id;
          break;
        }
      }
    }
  }

  if (client == kCWBClientColor) {
    DLOGV_IF(kTagQDCM, "frame_capture_buffer_queued_ is in use. Handle frame capture.");
    HandleFrameCapture();
  } else if (client == kCWBClientFrameDump) {
    DLOGV_IF(kTagQDCM, "dump_output_to_file is in use. Handle frame dump.");
    HandleFrameDump();
  }
}

void HWCDisplay::HandleFrameDump() {
  if (!dump_frame_count_) {
    return;
  }

  auto ret = kCWBReleaseFenceErrorNone;
  {
    std::unique_lock<std::mutex> lock(cwb_mutex_);
    auto &cwb_resp = cwb_capture_status_map_[kCWBClientFrameDump];
    // If CWB request status is not notified, then need to wait for the notification.
    if (cwb_resp.status == kCWBReleaseFenceNotChecked) {
      if (cwb_cv_.wait_until(
              lock, std::chrono::system_clock::now() + std::chrono::milliseconds(kCwbWaitMs)) ==
          std::cv_status::timeout) {
        DLOGW("CWB notification wait timed out, it would be handled in next cycle.");
        // Return to handle the notification in next cycle, if release fence is not yet notified.
        return;
      }
    }
    ret = cwb_resp.status;
    cwb_capture_status_map_.erase(kCWBClientFrameDump);
  }

  if (!ret || ret == kCWBReleaseFenceWaitTimedOut) {
    // On fence wait timeout, we could dump the frame, because timeout means it waited for
    // one second for signal, and which might got delayed due to some flushing and resource
    // releasing operations during certain power glitch event. So, we can assume that buffer
    // writing operation is over after timeout.
    DumpOutputBuffer(output_buffer_info_, output_buffer_base_, layer_stack_.retire_fence);
    if (ret == kCWBReleaseFenceWaitTimedOut) {
      DLOGW("CWB frame-%d dump may be empty due to fence timeout on any unexpected event!",
            dump_frame_index_);
    }
  } else {
    // CwbManager notifies -1 (unknown error) on power down or tear down, where -1 means, last
    // request is not processed by CwbManager and tried to flush the CWB pending requests from
    // CWB request queue.
    DLOGW("Probably, power/tear down occured during the cwb request. So, dropped off frame-%d.",
          dump_frame_index_);
  }

  bool stop_frame_dump = false;
  if (0 == (dump_frame_count_ - 1)) {
    stop_frame_dump = true;
  } else {
    const native_handle_t *hnd = static_cast<native_handle_t *>(output_buffer_info_.private_data);
    HWC3::Error err =
        SetReadbackBuffer(hnd, nullptr, output_buffer_cwb_config_, kCWBClientFrameDump);
    if (err != HWC3::Error::None) {
      stop_frame_dump = true;
      DLOGW("Unexpectedly stopped dumping of remaining %d frames for frame indices %d onwards!",
            dump_frame_count_, dump_frame_index_);
    } else {
      dump_frame_count_--;
      dump_frame_index_++;
    }
  }

  if (stop_frame_dump) {
    ReleaseFrameDumpResources();
  }
}

DisplayError HWCDisplay::HandleQsyncState(const QsyncEventData &qsync_data) {
  event_handler_->PerformQsyncCallback(id_, qsync_data.enabled, qsync_data.refresh_rate,
                                       qsync_data.qsync_refresh_rate);
  return kErrorNone;
}

HWC3::Error HWCDisplay::GetClientTargetProperty(ClientTargetProperty *out_client_target_property) {
  Layer *client_layer = client_target_->GetSDMLayer();
  if (!client_layer->request.flags.update_format) {
    return HWC3::Error::None;
  }
  int32_t format = 0;
  uint64_t flags = 0;
  auto err = buffer_allocator_->SetBufferInfo(client_layer->request.format, &format, &flags);
  if (err) {
    DLOGE("Invalid format: %s requested", GetFormatString(client_layer->request.format));
    return HWC3::Error::BadParameter;
  }
  Dataspace dataspace;
  DisplayError error = ColorMetadataToDataspace(client_layer->request.color_metadata, &dataspace);
  if (error != kErrorNone) {
    DLOGE("Invalid Dataspace requested: Primaries = %d Transfer = %d ds = %d",
          client_layer->request.color_metadata.colorPrimaries,
          client_layer->request.color_metadata.transfer, dataspace);
    return HWC3::Error::BadParameter;
  }
  out_client_target_property->dataspace = dataspace;
  out_client_target_property->pixelFormat = (PixelFormat_V3)format;

  return HWC3::Error::None;
}

void HWCDisplay::GetConfigInfo(std::map<uint32_t, DisplayConfigVariableInfo> *variable_config_map,
                               int *active_config_index, uint32_t *num_configs) {
  *variable_config_map = variable_config_map_;
  *active_config_index = active_config_index_;
  *num_configs = num_configs_;
}

void HWCDisplay::NotifyCwbDone(int32_t status, const LayerBuffer &buffer) {
  CWBClient client = kCWBClientNone;
  uint64_t handle_id = buffer.handle_id;

  {
    std::unique_lock<std::mutex> lock(cwb_mutex_);

    const auto map_cwb_buffer = cwb_buffer_map_.find(handle_id);
    if (map_cwb_buffer == cwb_buffer_map_.end()) {
      DLOGV_IF(kTagClient, "CWB Buffer(id = %u) not found in buffer-client map", handle_id);
      return;
    }
    client = map_cwb_buffer->second;

    auto &cwb_cap_status = cwb_capture_status_map_[client];
    cwb_cap_status.handle_id = handle_id;
    cwb_cap_status.client = client;
    cwb_cap_status.release_fence = buffer.release_fence;

    if (!status) {
      cwb_cap_status.status = kCWBReleaseFenceSignaled;
    } else if (status == -ETIME) {
      cwb_cap_status.status = kCWBReleaseFenceWaitTimedOut;
    } else {
      cwb_cap_status.status = kCWBReleaseFenceUnknownError;
    }
    cwb_buffer_map_.erase(handle_id);
    if (client == kCWBClientFrameDump || client == kCWBClientColor) {
      cwb_cv_.notify_one();
    } else if (client == kCWBClientExternal && event_handler_) {
      // Clear the backup data like release fence and status corresponding to handle id,
      // when successfully notified to client.
      if (!event_handler_->NotifyCwbDone(id_, status, handle_id)) {
        cwb_capture_status_map_.erase(client);
      }
    }
  }

  DLOGV_IF(kTagClient, "CWB notified for client = %d with buffer = %u, return status = %s(%d)",
           client, handle_id,
           (!status)            ? "Handled"
           : (status == -ETIME) ? "Timedout"
                                : "Error",
           status);
}

void HWCDisplay::Abort() {
  display_intf_->Abort();
}

void HWCDisplay::MarkClientActive(bool is_client_up) {
  is_client_up_ = is_client_up;
}

bool HWCDisplay::NotifyIdleNow() {
  if (IsDisplayIdle() && (IsDisplayCommandMode() || idle_active_ms_ <= 0)) {
    return true;
  }

  return false;
}

}  // namespace sdm
