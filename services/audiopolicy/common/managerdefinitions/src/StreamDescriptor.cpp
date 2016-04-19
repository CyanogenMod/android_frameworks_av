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

#define LOG_TAG "APM::Volumes"
//#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include "StreamDescriptor.h"
#include "Gains.h"
#include "policy.h"
#include <utils/Log.h>
#include <utils/String8.h>

namespace android {

// --- StreamDescriptor class implementation

StreamDescriptor::StreamDescriptor()
    :   mIndexMin(0), mIndexMax(1), mCanBeMuted(true)
{
    // Initialize the current stream's index to mIndexMax so volume isn't 0 in
    // cases where the Java layer doesn't call into the audio policy service to
    // set the default volume.
    mIndexCur.add(AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME, mIndexMax);
}

int StreamDescriptor::getVolumeIndex(audio_devices_t device) const
{
    device = Volume::getDeviceForVolume(device);
    // there is always a valid entry for AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME
    if (mIndexCur.indexOfKey(device) < 0) {
        device = AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME;
    }
    return mIndexCur.valueFor(device);
}

void StreamDescriptor::clearCurrentVolumeIndex()
{
    mIndexCur.clear();
}

void StreamDescriptor::addCurrentVolumeIndex(audio_devices_t device, int index)
{
    mIndexCur.add(device, index);
}

void StreamDescriptor::setVolumeIndexMin(int volIndexMin)
{
    mIndexMin = volIndexMin;
}

void StreamDescriptor::setVolumeIndexMax(int volIndexMax)
{
    mIndexMax = volIndexMax;
}

void StreamDescriptor::setVolumeCurvePoint(device_category deviceCategory,
                                           const VolumeCurvePoint *point)
{
    mVolumeCurve[deviceCategory] = point;
}

void StreamDescriptor::dump(int fd) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%s         %02d         %02d         ",
             mCanBeMuted ? "true " : "false", mIndexMin, mIndexMax);
    result.append(buffer);
    for (size_t i = 0; i < mIndexCur.size(); i++) {
        snprintf(buffer, SIZE, "%04x : %02d, ",
                 mIndexCur.keyAt(i),
                 mIndexCur.valueAt(i));
        result.append(buffer);
    }
    result.append("\n");

    write(fd, result.string(), result.size());
}

StreamDescriptorCollection::StreamDescriptorCollection()
{
    for (size_t stream = 0 ; stream < AUDIO_STREAM_CNT; stream++) {
        add(static_cast<audio_stream_type_t>(stream), StreamDescriptor());
    }
}

bool StreamDescriptorCollection::canBeMuted(audio_stream_type_t stream)
{
    return valueAt(stream).canBeMuted();
}

void StreamDescriptorCollection::clearCurrentVolumeIndex(audio_stream_type_t stream)
{
    editValueAt(stream).clearCurrentVolumeIndex();
}

void StreamDescriptorCollection::addCurrentVolumeIndex(audio_stream_type_t stream,
                                                       audio_devices_t device, int index)
{
    editValueAt(stream).addCurrentVolumeIndex(device, index);
}

void StreamDescriptorCollection::setVolumeCurvePoint(audio_stream_type_t stream,
                                                     device_category deviceCategory,
                                                     const VolumeCurvePoint *point)
{
    editValueAt(stream).setVolumeCurvePoint(deviceCategory, point);
}

const VolumeCurvePoint *StreamDescriptorCollection::getVolumeCurvePoint(audio_stream_type_t stream,
                                                                        device_category deviceCategory) const
{
    return valueAt(stream).getVolumeCurvePoint(deviceCategory);
}

void StreamDescriptorCollection::setVolumeIndexMin(audio_stream_type_t stream,int volIndexMin)
{
    return editValueAt(stream).setVolumeIndexMin(volIndexMin);
}

void StreamDescriptorCollection::setVolumeIndexMax(audio_stream_type_t stream,int volIndexMax)
{
    return editValueAt(stream).setVolumeIndexMax(volIndexMax);
}

float StreamDescriptorCollection::volIndexToDb(audio_stream_type_t stream, device_category category,
                                               int indexInUi) const
{
    const StreamDescriptor &streamDesc = valueAt(stream);
    return Gains::volIndexToDb(streamDesc.getVolumeCurvePoint(category),
                               streamDesc.getVolumeIndexMin(), streamDesc.getVolumeIndexMax(),
                               indexInUi);
}

status_t StreamDescriptorCollection::initStreamVolume(audio_stream_type_t stream,
                                                      int indexMin, int indexMax)
{
    ALOGV("initStreamVolume() stream %d, min %d, max %d", stream , indexMin, indexMax);
    if (indexMin < 0 || indexMin >= indexMax) {
        ALOGW("initStreamVolume() invalid index limits for stream %d, min %d, max %d",
              stream , indexMin, indexMax);
        return BAD_VALUE;
    }
    setVolumeIndexMin(stream, indexMin);
    setVolumeIndexMax(stream, indexMax);
    return NO_ERROR;
}

void StreamDescriptorCollection::initializeVolumeCurves(bool isSpeakerDrcEnabled)
{
    for (int i = 0; i < AUDIO_STREAM_CNT; i++) {
        for (int j = 0; j < DEVICE_CATEGORY_CNT; j++) {
            setVolumeCurvePoint(static_cast<audio_stream_type_t>(i),
                                static_cast<device_category>(j),
                                Gains::sVolumeProfiles[i][j]);
        }
    }

    // Check availability of DRC on speaker path: if available, override some of the speaker curves
    if (isSpeakerDrcEnabled) {
        setVolumeCurvePoint(AUDIO_STREAM_SYSTEM, DEVICE_CATEGORY_SPEAKER,
                            Gains::sDefaultSystemVolumeCurveDrc);
        setVolumeCurvePoint(AUDIO_STREAM_RING, DEVICE_CATEGORY_SPEAKER,
                            Gains::sSpeakerSonificationVolumeCurveDrc);
        setVolumeCurvePoint(AUDIO_STREAM_ALARM, DEVICE_CATEGORY_SPEAKER,
                            Gains::sSpeakerSonificationVolumeCurveDrc);
        setVolumeCurvePoint(AUDIO_STREAM_NOTIFICATION, DEVICE_CATEGORY_SPEAKER,
                            Gains::sSpeakerSonificationVolumeCurveDrc);
        setVolumeCurvePoint(AUDIO_STREAM_MUSIC, DEVICE_CATEGORY_SPEAKER,
                            Gains::sSpeakerMediaVolumeCurveDrc);
        setVolumeCurvePoint(AUDIO_STREAM_ACCESSIBILITY, DEVICE_CATEGORY_SPEAKER,
                            Gains::sSpeakerMediaVolumeCurveDrc);
    }
}

void StreamDescriptorCollection::switchVolumeCurve(audio_stream_type_t streamSrc,
                                                   audio_stream_type_t streamDst)
{
    for (int j = 0; j < DEVICE_CATEGORY_CNT; j++) {
        setVolumeCurvePoint(streamDst, static_cast<device_category>(j),
                            Gains::sVolumeProfiles[streamSrc][j]);
    }
}

status_t StreamDescriptorCollection::dump(int fd) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];

    snprintf(buffer, SIZE, "\nStreams dump:\n");
    write(fd, buffer, strlen(buffer));
    snprintf(buffer, SIZE,
             " Stream  Can be muted  Index Min  Index Max  Index Cur [device : index]...\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < size(); i++) {
        snprintf(buffer, SIZE, " %02zu      ", i);
        write(fd, buffer, strlen(buffer));
        valueAt(i).dump(fd);
    }

    return NO_ERROR;
}

}; // namespace android
