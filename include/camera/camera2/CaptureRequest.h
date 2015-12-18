/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_PHOTOGRAPHY_CAPTUREREQUEST_H
#define ANDROID_HARDWARE_PHOTOGRAPHY_CAPTUREREQUEST_H

#include <utils/RefBase.h>
#include <utils/Vector.h>
#include <binder/Parcelable.h>
#include <camera/CameraMetadata.h>

namespace android {

class Surface;

namespace hardware {
namespace camera2 {

struct CaptureRequest : public Parcelable {
    CameraMetadata          mMetadata;
    Vector<sp<Surface> >    mSurfaceList;
    bool                    mIsReprocess;

    /**
     * Keep impl up-to-date with CaptureRequest.java in frameworks/base
     */
    status_t                readFromParcel(const Parcel* parcel) override;
    status_t                writeToParcel(Parcel* parcel) const override;
};

} // namespace camera2
} // namespace hardware

struct CaptureRequest :
        public RefBase, public hardware::camera2::CaptureRequest {
  public:
    // Same as android::hardware::camera2::CaptureRequest, except that you can
    // put this in an sp<>
};

} // namespace android

#endif
