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

#define LOG_TAG "APM::VolumeCurve"
//#define LOG_NDEBUG 0

#include "VolumeCurve.h"
#include "TypeConverter.h"

namespace android {

float VolumeCurve::volIndexToDb(int indexInUi, int volIndexMin, int volIndexMax) const
{
    ALOG_ASSERT(!mCurvePoints.isEmpty(), "Invalid volume curve");

    size_t nbCurvePoints = mCurvePoints.size();
    // the volume index in the UI is relative to the min and max volume indices for this stream
    int nbSteps = 1 + mCurvePoints[nbCurvePoints - 1].mIndex - mCurvePoints[0].mIndex;
    int volIdx = (nbSteps * (indexInUi - volIndexMin)) / (volIndexMax - volIndexMin);

    // Where would this volume index been inserted in the curve point
    size_t indexInUiPosition = mCurvePoints.orderOf(CurvePoint(volIdx, 0));
    if (indexInUiPosition >= nbCurvePoints) {
        //use last point of table
        return mCurvePoints[nbCurvePoints - 1].mAttenuationInMb / 100.0f;
    }
    if (indexInUiPosition == 0) {
        if (indexInUiPosition != mCurvePoints[0].mIndex) {
            return VOLUME_MIN_DB; // out of bounds
        }
        return mCurvePoints[0].mAttenuationInMb / 100.0f;
    }
    // linear interpolation in the attenuation table in dB
    float decibels = (mCurvePoints[indexInUiPosition - 1].mAttenuationInMb / 100.0f) +
            ((float)(volIdx - mCurvePoints[indexInUiPosition - 1].mIndex)) *
                ( ((mCurvePoints[indexInUiPosition].mAttenuationInMb / 100.0f) -
                        (mCurvePoints[indexInUiPosition - 1].mAttenuationInMb / 100.0f)) /
                    ((float)(mCurvePoints[indexInUiPosition].mIndex -
                            mCurvePoints[indexInUiPosition - 1].mIndex)) );

    ALOGV("VOLUME mDeviceCategory %d, mStreamType %d vol index=[%d %d %d], dB=[%.1f %.1f %.1f]",
            mDeviceCategory, mStreamType,
            mCurvePoints[indexInUiPosition - 1].mIndex, volIdx,
            mCurvePoints[indexInUiPosition].mIndex,
            ((float)mCurvePoints[indexInUiPosition - 1].mAttenuationInMb / 100.0f), decibels,
            ((float)mCurvePoints[indexInUiPosition].mAttenuationInMb / 100.0f));

    return decibels;
}

void VolumeCurve::dump(int fd) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, SIZE, " {");
    result.append(buffer);
    for (size_t i = 0; i < mCurvePoints.size(); i++) {
        snprintf(buffer, SIZE, "(%3d, %5d)",
                 mCurvePoints[i].mIndex, mCurvePoints[i].mAttenuationInMb);
        result.append(buffer);
        result.append(i == (mCurvePoints.size() - 1) ? " }\n" : ", ");
    }
    write(fd, result.string(), result.size());
}

void VolumeCurvesForStream::dump(int fd, int spaces = 0, bool curvePoints) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    if (!curvePoints) {
        snprintf(buffer, SIZE, "%s         %02d         %02d         ",
                 mCanBeMuted ? "true " : "false", mIndexMin, mIndexMax);
        result.append(buffer);
        for (size_t i = 0; i < mIndexCur.size(); i++) {
            snprintf(buffer, SIZE, "%04x : %02d, ", mIndexCur.keyAt(i), mIndexCur.valueAt(i));
            result.append(buffer);
        }
        result.append("\n");
        write(fd, result.string(), result.size());
        return;
    }

    for (size_t i = 0; i < size(); i++) {
        std::string deviceCatLiteral;
        DeviceCategoryConverter::toString(keyAt(i), deviceCatLiteral);
        snprintf(buffer, SIZE, "%*s %s :",
                 spaces, "", deviceCatLiteral.c_str());
        write(fd, buffer, strlen(buffer));
        valueAt(i)->dump(fd);
    }
    result.append("\n");
    write(fd, result.string(), result.size());
}

status_t VolumeCurvesCollection::dump(int fd) const
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
    snprintf(buffer, SIZE, "\nVolume Curves for Use Cases (aka Stream types) dump:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < size(); i++) {
        std::string streamTypeLiteral;
        StreamTypeConverter::toString(keyAt(i), streamTypeLiteral);
        snprintf(buffer, SIZE,
                 " %s (%02zu): Curve points for device category (index, attenuation in millibel)\n",
                 streamTypeLiteral.c_str(), i);
        write(fd, buffer, strlen(buffer));
        valueAt(i).dump(fd, 2, true);
    }

    return NO_ERROR;
}

}; // namespace android
