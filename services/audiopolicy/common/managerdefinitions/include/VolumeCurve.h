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

#pragma once

#include "IVolumeCurvesCollection.h"
#include <policy.h>
#include <hardware/audio.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/SortedVector.h>
#include <utils/KeyedVector.h>
#include <system/audio.h>
#include <cutils/config_utils.h>
#include <string>
#include <utility>

namespace android {

struct CurvePoint
{
    CurvePoint() {}
    CurvePoint(int index, int attenuationInMb) :
        mIndex(index), mAttenuationInMb(attenuationInMb) {}
    uint32_t mIndex;
    int mAttenuationInMb;
};

inline bool operator< (const CurvePoint &lhs, const CurvePoint &rhs)
{
    return lhs.mIndex < rhs.mIndex;
}

// A volume curve for a given use case and device category
// It contains of list of points of this curve expressing the attenuation in Millibels for
// a given volume index from 0 to 100
class VolumeCurve : public RefBase
{
public:
    VolumeCurve(device_category device, audio_stream_type_t stream) :
        mDeviceCategory(device), mStreamType(stream) {}

    device_category getDeviceCategory() const { return mDeviceCategory; }
    audio_stream_type_t getStreamType() const { return mStreamType; }

    void add(const CurvePoint &point) { mCurvePoints.add(point); }

    float volIndexToDb(int indexInUi, int volIndexMin, int volIndexMax) const;

    void dump(int fd) const;

private:
    SortedVector<CurvePoint> mCurvePoints;
    device_category mDeviceCategory;
    audio_stream_type_t mStreamType;
};

// Volume Curves for a given use case indexed by device category
class VolumeCurvesForStream : public KeyedVector<device_category, sp<VolumeCurve> >
{
public:
    VolumeCurvesForStream() : mIndexMin(0), mIndexMax(1), mCanBeMuted(true)
    {
        mIndexCur.add(AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME, 0);
    }

    sp<VolumeCurve> getCurvesFor(device_category device) const
    {
        if (indexOfKey(device) < 0) {
            return 0;
        }
        return valueFor(device);
    }

    int getVolumeIndex(audio_devices_t device) const
    {
        device = Volume::getDeviceForVolume(device);
        // there is always a valid entry for AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME
        if (mIndexCur.indexOfKey(device) < 0) {
            device = AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME;
        }
        return mIndexCur.valueFor(device);
    }

    bool canBeMuted() const { return mCanBeMuted; }
    void clearCurrentVolumeIndex() { mIndexCur.clear(); }
    void addCurrentVolumeIndex(audio_devices_t device, int index) { mIndexCur.add(device, index); }

    void setVolumeIndexMin(int volIndexMin) { mIndexMin = volIndexMin; }
    int getVolumeIndexMin() const { return mIndexMin; }

    void setVolumeIndexMax(int volIndexMax) { mIndexMax = volIndexMax; }
    int getVolumeIndexMax() const { return mIndexMax; }

    bool hasVolumeIndexForDevice(audio_devices_t device) const
    {
        device = Volume::getDeviceForVolume(device);
        return mIndexCur.indexOfKey(device) >= 0;
    }

    const sp<VolumeCurve> getOriginVolumeCurve(device_category deviceCategory) const
    {
        ALOG_ASSERT(mOriginVolumeCurves.indexOfKey(deviceCategory) >= 0, "Invalid device category");
        return mOriginVolumeCurves.valueFor(deviceCategory);
    }
    void setVolumeCurve(device_category deviceCategory, const sp<VolumeCurve> &volumeCurve)
    {
        ALOG_ASSERT(indexOfKey(deviceCategory) >= 0, "Invalid device category for Volume Curve");
        replaceValueFor(deviceCategory, volumeCurve);
    }

    ssize_t add(const sp<VolumeCurve> &volumeCurve)
    {
        device_category deviceCategory = volumeCurve->getDeviceCategory();
        ssize_t index = indexOfKey(deviceCategory);
        if (index < 0) {
            // Keep track of original Volume Curves per device category in order to switch curves.
            mOriginVolumeCurves.add(deviceCategory, volumeCurve);
            return KeyedVector::add(deviceCategory, volumeCurve);
        }
        return index;
    }

    float volIndexToDb(device_category deviceCat, int indexInUi) const
    {
        return getCurvesFor(deviceCat)->volIndexToDb(indexInUi, mIndexMin, mIndexMax);
    }

    void dump(int fd, int spaces, bool curvePoints = false) const;

private:
    KeyedVector<device_category, sp<VolumeCurve> > mOriginVolumeCurves;
    KeyedVector<audio_devices_t, int> mIndexCur; /**< current volume index per device. */
    int mIndexMin; /**< min volume index. */
    int mIndexMax; /**< max volume index. */
    bool mCanBeMuted; /**< true is the stream can be muted. */
};

// Collection of Volume Curves indexed by use case
class VolumeCurvesCollection : public KeyedVector<audio_stream_type_t, VolumeCurvesForStream>,
                               public IVolumeCurvesCollection
{
public:
    VolumeCurvesCollection()
    {
        // Create an empty collection of curves
        for (ssize_t i = 0 ; i < AUDIO_STREAM_CNT; i++) {
            audio_stream_type_t stream = static_cast<audio_stream_type_t>(i);
            KeyedVector::add(stream, VolumeCurvesForStream());
        }
    }

    // Once XML has been parsed, must be call first to sanity check table and initialize indexes
    virtual status_t initStreamVolume(audio_stream_type_t stream, int indexMin, int indexMax)
    {
        editValueAt(stream).setVolumeIndexMin(indexMin);
        editValueAt(stream).setVolumeIndexMax(indexMax);
        return NO_ERROR;
    }
    virtual void clearCurrentVolumeIndex(audio_stream_type_t stream)
    {
        editCurvesFor(stream).clearCurrentVolumeIndex();
    }
    virtual void addCurrentVolumeIndex(audio_stream_type_t stream, audio_devices_t device, int index)
    {
        editCurvesFor(stream).addCurrentVolumeIndex(device, index);
    }
    virtual bool canBeMuted(audio_stream_type_t stream) { return getCurvesFor(stream).canBeMuted(); }

    virtual int getVolumeIndexMin(audio_stream_type_t stream) const
    {
        return getCurvesFor(stream).getVolumeIndexMin();
    }
    virtual int getVolumeIndexMax(audio_stream_type_t stream) const
    {
        return getCurvesFor(stream).getVolumeIndexMax();
    }
    virtual int getVolumeIndex(audio_stream_type_t stream, audio_devices_t device)
    {
        return getCurvesFor(stream).getVolumeIndex(device);
    }
    virtual void switchVolumeCurve(audio_stream_type_t streamSrc, audio_stream_type_t streamDst)
    {
        const VolumeCurvesForStream &sourceCurves = getCurvesFor(streamSrc);
        VolumeCurvesForStream &dstCurves = editCurvesFor(streamDst);
        ALOG_ASSERT(sourceCurves.size() == dstCurves.size(), "device category not aligned");
        for (size_t index = 0; index < sourceCurves.size(); index++) {
            device_category cat = sourceCurves.keyAt(index);
            dstCurves.setVolumeCurve(cat, sourceCurves.getOriginVolumeCurve(cat));
        }
    }
    virtual float volIndexToDb(audio_stream_type_t stream, device_category cat, int indexInUi) const
    {
        return getCurvesFor(stream).volIndexToDb(cat, indexInUi);
    }
    virtual bool hasVolumeIndexForDevice(audio_stream_type_t stream,
                                         audio_devices_t device) const
    {
        return getCurvesFor(stream).hasVolumeIndexForDevice(device);
    }

    virtual status_t dump(int fd) const;

    ssize_t add(const sp<VolumeCurve> &volumeCurve)
    {
        audio_stream_type_t streamType = volumeCurve->getStreamType();
        return editCurvesFor(streamType).add(volumeCurve);
    }
    VolumeCurvesForStream &editCurvesFor(audio_stream_type_t stream)
    {
        ALOG_ASSERT(indexOfKey(stream) >= 0, "Invalid stream type for Volume Curve");
        return editValueAt(stream);
    }
    const VolumeCurvesForStream &getCurvesFor(audio_stream_type_t stream) const
    {
        ALOG_ASSERT(indexOfKey(stream) >= 0, "Invalid stream type for Volume Curve");
        return valueFor(stream);
    }
};

}; // namespace android
