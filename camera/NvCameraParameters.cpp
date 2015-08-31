/*
* Copyright (c) 2011, NVIDIA CORPORATION. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* Copyright (C) 2009 The Android Open Source Project
*/

#include <camera/NvCameraParameters.h>

namespace android {

// Value for number of nagative shutter lag buffers
const char NvCameraParameters::NV_NSL_NUM_BUFFERS[] = "nv-nsl-num-buffers";

// Value for skip count at NSL burst mode
const char NvCameraParameters::NV_NSL_SKIP_COUNT[] = "nv-nsl-burst-skip-count";

// Value for picture count at NSL burst mode
const char NvCameraParameters::NV_NSL_BURST_PICTURE_COUNT[] = "nv-nsl-burst-picture-count";

// Value for skip count at burst mode
const char NvCameraParameters::NV_SKIP_COUNT[] = "nv-burst-skip-count";

// Value for picture count at burst mode
const char NvCameraParameters::NV_BURST_PICTURE_COUNT[] = "nv-burst-picture-count";

// Value for raw dump flag
const char NvCameraParameters::NV_RAW_DUMP_FLAG[] = "nv-raw-dump-flag";

// Value for focus areas
const char NvCameraParameters::NV_FOCUS_AREAS[] = "focus-areas";

// Value for metering areas
const char NvCameraParameters::NV_METERING_AREAS[] = "nv-metering-areas";

// Value for color correction
const char NvCameraParameters::NV_COLOR_CORRECTION[] = "nv-color-correction";

// Value for satuation
const char NvCameraParameters::NV_SATURATION[] = "nv-saturation";

// Value for contrast
const char NvCameraParameters::NV_CONTRAST[] = "nv-contrast";

// Value for edge enhancement
const char NvCameraParameters::NV_EDGE_ENHANCEMENT[] = "nv-edge-enhancement";

// Vaule for exposure time
const char NvCameraParameters::NV_EXPOSURE_TIME[] = "nv-exposure-time";

// Value for picture iso
const char NvCameraParameters::NV_PICTURE_ISO[] = "nv-picture-iso";

// Value for focus position
const char NvCameraParameters::NV_FOCUS_POSITION[] = "nv-focus-position";

// Value for video speed
const char NvCameraParameters::NV_VIDEO_SPEED[] = "nv-video-speed";
// Value for autowhitebalance lock
const char NvCameraParameters::NV_AUTOWHITEBALANCE_LOCK[] = "nv-auto-whitebalance-lock";

// Value for autoexposure lock
const char NvCameraParameters::NV_AUTOEXPOSURE_LOCK[] = "nv-auto-exposure-lock";
// Value for sensor capture rate
const char NvCameraParameters::NV_SENSOR_CAPTURE_RATE[] = "nv-sensor-capture-rate";

// Value for per-resolution video capabilities
const char NvCameraParameters::NV_CAPABILITY_FOR_VIDEO_SIZE[] = "nv-capabilities-for-video-size";

// Generic suffix used to query capabilities for parameters
const char NvCameraParameters::NV_SUPPORTED_VALUES_SUFFIX[] = "-values";

// Value for auto rotation mode
const char NvCameraParameters::NV_AUTO_ROTATION[] = "nv-auto-rotation";
// Value for stereo mode
const char NvCameraParameters::NV_STEREO_MODE[] = "nv-stereo-mode";

NvCameraParameters::NvCameraParameters()
                  : CameraParameters()
{
}

NvCameraParameters::~NvCameraParameters()
{
}

}
