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

#define LOG_TAG "APM::AudioSourceDescriptor"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/String8.h>
#include <media/AudioPolicyHelper.h>
#include <HwModule.h>
#include <AudioGain.h>
#include <AudioSourceDescriptor.h>
#include <DeviceDescriptor.h>
#include <IOProfile.h>
#include <AudioOutputDescriptor.h>

namespace android {

status_t AudioSourceDescriptor::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "mStream: %d\n", audio_attributes_to_stream_type(&mAttributes));
    result.append(buffer);
    snprintf(buffer, SIZE, "mDevice:\n");
    result.append(buffer);
    write(fd, result.string(), result.size());
    mDevice->dump(fd, 2 , 0);
    return NO_ERROR;
}


status_t AudioSourceCollection::dump(int fd) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];

    snprintf(buffer, SIZE, "\nAudio sources dump:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < size(); i++) {
        snprintf(buffer, SIZE, "- Source %d dump:\n", keyAt(i));
        write(fd, buffer, strlen(buffer));
        valueAt(i)->dump(fd);
    }

    return NO_ERROR;
}

}; //namespace android
