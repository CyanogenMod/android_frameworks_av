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
* Class structure based upon CameraParameters in CameraParameters.h:
* Copyright (C) 2009 The Android Open Source Project
*/

#include <camera/CameraParameters.h>

namespace android {

class NvCameraParameters : public CameraParameters
{
public:
    NvCameraParameters();
    NvCameraParameters(const String8 &params) { unflatten(params); }
    ~NvCameraParameters();

    // Number of Buffers for negative shutter lag
    static const char NV_NSL_NUM_BUFFERS[];

    // Skip count at NSL burst mode
    static const char NV_NSL_SKIP_COUNT[];

    // Picture count at NSL burst mode
    static const char NV_NSL_BURST_PICTURE_COUNT[];

    // Skip count at burst mode
    static const char NV_SKIP_COUNT[];

    // Picture count at burst mode
    static const char NV_BURST_PICTURE_COUNT[];

    // Raw dump flag
    static const char NV_RAW_DUMP_FLAG[];


    // Focus areas: (left,top,right,bottom)
    static const char NV_FOCUS_AREAS[];

    // Metering areas: (left,top,right,bottom)
    static const char NV_METERING_AREAS[];

    // Color correction: 16 floats array of 4x4 matrix
    static const char NV_COLOR_CORRECTION[];

    // Satuation
    static const char NV_SATURATION[];

    // Contrast
    static const char NV_CONTRAST[];

    // Edge enhancement
    static const char NV_EDGE_ENHANCEMENT[];

    // Exposure time: microsecond precision
    static const char NV_EXPOSURE_TIME[];

    // Picture iso
    static const char NV_PICTURE_ISO[];

    // Focus position
    static const char NV_FOCUS_POSITION[];
    // Video speed
    static const char NV_VIDEO_SPEED[];

    // Sensor capture rate
    static const char NV_SENSOR_CAPTURE_RATE[];

    // Per-resolution video capabilities

    // Autowhitebalance lock
    static const char NV_AUTOWHITEBALANCE_LOCK[];

    // Autoexposure lock
    static const char NV_AUTOEXPOSURE_LOCK[];

    static const char NV_CAPABILITY_FOR_VIDEO_SIZE[];

    // Generic suffix used to query capabilities for parameters
    static const char NV_SUPPORTED_VALUES_SUFFIX[];

    // Auto Rotation mode for image
    static const char NV_AUTO_ROTATION[];
    // Stereo mode
    static const char NV_STEREO_MODE[];

};

}
