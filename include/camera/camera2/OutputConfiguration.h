/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_CAMERA2_OUTPUTCONFIGURATION_H
#define ANDROID_HARDWARE_CAMERA2_OUTPUTCONFIGURATION_H

#include <gui/IGraphicBufferProducer.h>
#include <binder/Parcelable.h>

namespace android {

class Surface;

namespace hardware {
namespace camera2 {
namespace params {

class OutputConfiguration : public android::Parcelable {
public:

    static const int INVALID_ROTATION;
    static const int INVALID_SET_ID;
    enum SurfaceType{
        SURFACE_TYPE_UNKNOWN = -1,
        SURFACE_TYPE_SURFACE_VIEW = 0,
        SURFACE_TYPE_SURFACE_TEXTURE = 1
    };
    sp<IGraphicBufferProducer> getGraphicBufferProducer() const;
    int                        getRotation() const;
    int                        getSurfaceSetID() const;
    int                        getSurfaceType() const;
    int                        getWidth() const;
    int                        getHeight() const;
    /**
     * Keep impl up-to-date with OutputConfiguration.java in frameworks/base
     */
    virtual status_t           writeToParcel(Parcel* parcel) const override;

    virtual status_t           readFromParcel(const Parcel* parcel) override;

    // getGraphicBufferProducer will be NULL
    // getRotation will be INVALID_ROTATION
    // getSurfaceSetID will be INVALID_SET_ID
    OutputConfiguration();

    // getGraphicBufferProducer will be NULL if error occurred
    // getRotation will be INVALID_ROTATION if error occurred
    // getSurfaceSetID will be INVALID_SET_ID if error occurred
    OutputConfiguration(const Parcel& parcel);

    OutputConfiguration(sp<IGraphicBufferProducer>& gbp, int rotation,
            int surfaceSetID = INVALID_SET_ID);

    bool operator == (const OutputConfiguration& other) const {
        return (mGbp == other.mGbp &&
                mRotation == other.mRotation &&
                mSurfaceSetID == other.mSurfaceSetID &&
                mSurfaceType == other.mSurfaceType &&
                mWidth == other.mWidth &&
                mHeight == other.mHeight);
    }
    bool operator != (const OutputConfiguration& other) const {
        return !(*this == other);
    }
    bool operator < (const OutputConfiguration& other) const {
        if (*this == other) return false;
        if (mGbp != other.mGbp) return mGbp < other.mGbp;
        if (mSurfaceSetID != other.mSurfaceSetID) {
            return mSurfaceSetID < other.mSurfaceSetID;
        }
        if (mSurfaceType != other.mSurfaceType) {
            return mSurfaceType < other.mSurfaceType;
        }
        if (mWidth != other.mWidth) {
            return mWidth < other.mWidth;
        }
        if (mHeight != other.mHeight) {
            return mHeight < other.mHeight;
        }

        return mRotation < other.mRotation;
    }
    bool operator > (const OutputConfiguration& other) const {
        return (*this != other && !(*this < other));
    }

private:
    sp<IGraphicBufferProducer> mGbp;
    int                        mRotation;
    int                        mSurfaceSetID;
    int                        mSurfaceType;
    int                        mWidth;
    int                        mHeight;
    // helper function
    static String16 readMaybeEmptyString16(const Parcel* parcel);
};
} // namespace params
} // namespace camera2
} // namespace hardware


using hardware::camera2::params::OutputConfiguration;

}; // namespace android

#endif
