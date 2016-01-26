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
#include <utils/KeyedVector.h>
#include <utils/StrongPointer.h>
#include <utils/SortedVector.h>
#include <hardware/audio.h>

namespace android {

// stream descriptor used for volume control
class StreamDescriptor
{
public:
    StreamDescriptor();

    int getVolumeIndex(audio_devices_t device) const;
    bool canBeMuted() const { return mCanBeMuted; }
    void clearCurrentVolumeIndex();
    void addCurrentVolumeIndex(audio_devices_t device, int index);
    int getVolumeIndexMin() const { return mIndexMin; }
    int getVolumeIndexMax() const { return mIndexMax; }
    void setVolumeIndexMin(int volIndexMin);
    void setVolumeIndexMax(int volIndexMax);

    void dump(int fd) const;

    void setVolumeCurvePoint(device_category deviceCategory, const VolumeCurvePoint *point);
    const VolumeCurvePoint *getVolumeCurvePoint(device_category deviceCategory) const
    {
        return mVolumeCurve[deviceCategory];
    }

private:
    const VolumeCurvePoint *mVolumeCurve[DEVICE_CATEGORY_CNT];
    KeyedVector<audio_devices_t, int> mIndexCur; /**< current volume index per device. */
    int mIndexMin; /**< min volume index. */
    int mIndexMax; /**< max volume index. */
    bool mCanBeMuted; /**< true is the stream can be muted. */
};

/**
 * stream descriptors collection for volume control
 */
class StreamDescriptorCollection : public DefaultKeyedVector<audio_stream_type_t, StreamDescriptor>,
                                   public IVolumeCurvesCollection
{
public:
    StreamDescriptorCollection();

    virtual void clearCurrentVolumeIndex(audio_stream_type_t stream);
    virtual void addCurrentVolumeIndex(audio_stream_type_t stream, audio_devices_t device,
                                       int index);
    virtual bool canBeMuted(audio_stream_type_t stream);
    virtual int getVolumeIndexMin(audio_stream_type_t stream) const
    {
        return valueFor(stream).getVolumeIndexMin();
    }
    virtual int getVolumeIndex(audio_stream_type_t stream, audio_devices_t device)
    {
        return valueFor(stream).getVolumeIndex(device);
    }
    virtual int getVolumeIndexMax(audio_stream_type_t stream) const
    {
        return valueFor(stream).getVolumeIndexMax();
    }
    virtual float volIndexToDb(audio_stream_type_t stream, device_category device,
                               int indexInUi) const;
    virtual status_t initStreamVolume(audio_stream_type_t stream, int indexMin, int indexMax);
    virtual void initializeVolumeCurves(bool isSpeakerDrcEnabled);
    virtual void switchVolumeCurve(audio_stream_type_t streamSrc, audio_stream_type_t streamDst);

    virtual status_t dump(int fd) const;

private:
    void setVolumeCurvePoint(audio_stream_type_t stream, device_category deviceCategory,
                             const VolumeCurvePoint *point);
    const VolumeCurvePoint *getVolumeCurvePoint(audio_stream_type_t stream,
                                                device_category deviceCategory) const;
    void setVolumeIndexMin(audio_stream_type_t stream,int volIndexMin);
    void setVolumeIndexMax(audio_stream_type_t stream,int volIndexMax);
};

}; // namespace android
