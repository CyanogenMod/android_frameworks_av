/*
**
** Copyright 2015, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "OutputConfiguration"
//#define LOG_NDEBUG 0

#include <utils/Log.h>

#include <camera/camera2/OutputConfiguration.h>
#include <gui/Surface.h>
#include <binder/Parcel.h>

namespace android {


const int OutputConfiguration::INVALID_ROTATION = -1;
const int OutputConfiguration::INVALID_SET_ID = -1;

sp<IGraphicBufferProducer> OutputConfiguration::getGraphicBufferProducer() const {
    return mGbp;
}

int OutputConfiguration::getRotation() const {
    return mRotation;
}

int OutputConfiguration::getSurfaceSetID() const {
    return mSurfaceSetID;
}

int OutputConfiguration::getSurfaceType() const {
    return mSurfaceType;
}

int OutputConfiguration::getWidth() const {
    return mWidth;
}

int OutputConfiguration::getHeight() const {
    return mHeight;
}

OutputConfiguration::OutputConfiguration() :
        mRotation(INVALID_ROTATION),
        mSurfaceSetID(INVALID_SET_ID),
        mSurfaceType(SURFACE_TYPE_UNKNOWN),
        mWidth(0),
        mHeight(0) {
}

OutputConfiguration::OutputConfiguration(const Parcel& parcel) :
        mRotation(INVALID_ROTATION),
        mSurfaceSetID(INVALID_SET_ID) {
    readFromParcel(&parcel);
}

status_t OutputConfiguration::readFromParcel(const Parcel* parcel) {
    status_t err = OK;
    int rotation = 0;

    if (parcel == nullptr) return BAD_VALUE;

    if ((err = parcel->readInt32(&rotation)) != OK) {
        ALOGE("%s: Failed to read rotation from parcel", __FUNCTION__);
        return err;
    }

    int setID = INVALID_SET_ID;
    if ((err = parcel->readInt32(&setID)) != OK) {
        ALOGE("%s: Failed to read surface set ID from parcel", __FUNCTION__);
        return err;
    }

    int surfaceType = SURFACE_TYPE_UNKNOWN;
    if ((err = parcel->readInt32(&surfaceType)) != OK) {
        ALOGE("%s: Failed to read surface type from parcel", __FUNCTION__);
        return err;
    }

    int width = 0;
    if ((err = parcel->readInt32(&width)) != OK) {
        ALOGE("%s: Failed to read surface width from parcel", __FUNCTION__);
        return err;
    }

    int height = 0;
    if ((err = parcel->readInt32(&height)) != OK) {
        ALOGE("%s: Failed to read surface height from parcel", __FUNCTION__);
        return err;
    }

    view::Surface surfaceShim;
    if ((err = surfaceShim.readFromParcel(parcel)) != OK) {
        // Read surface failure for deferred surface configuration is expected.
        if (surfaceType == SURFACE_TYPE_SURFACE_VIEW ||
                surfaceType == SURFACE_TYPE_SURFACE_TEXTURE) {
            ALOGV("%s: Get null surface from a deferred surface configuration (%dx%d)",
                    __FUNCTION__, width, height);
            err = OK;
        } else {
            ALOGE("%s: Failed to read surface from parcel", __FUNCTION__);
            return err;
        }
    }

    mGbp = surfaceShim.graphicBufferProducer;
    mRotation = rotation;
    mSurfaceSetID = setID;
    mSurfaceType = surfaceType;
    mWidth = width;
    mHeight = height;

    ALOGV("%s: OutputConfiguration: bp = %p, name = %s, rotation = %d, setId = %d,"
            "surfaceType = %d", __FUNCTION__, mGbp.get(), String8(surfaceShim.name).string(),
            mRotation, mSurfaceSetID, mSurfaceType);

    return err;
}

OutputConfiguration::OutputConfiguration(sp<IGraphicBufferProducer>& gbp, int rotation,
        int surfaceSetID) {
    mGbp = gbp;
    mRotation = rotation;
    mSurfaceSetID = surfaceSetID;
}

status_t OutputConfiguration::writeToParcel(Parcel* parcel) const {

    if (parcel == nullptr) return BAD_VALUE;
    status_t err = OK;

    err = parcel->writeInt32(mRotation);
    if (err != OK) return err;

    err = parcel->writeInt32(mSurfaceSetID);
    if (err != OK) return err;

    err = parcel->writeInt32(mSurfaceType);
    if (err != OK) return err;

    err = parcel->writeInt32(mWidth);
    if (err != OK) return err;

    err = parcel->writeInt32(mHeight);
    if (err != OK) return err;

    view::Surface surfaceShim;
    surfaceShim.name = String16("unknown_name"); // name of surface
    surfaceShim.graphicBufferProducer = mGbp;

    err = surfaceShim.writeToParcel(parcel);
    if (err != OK) return err;

    return OK;
}

}; // namespace android
