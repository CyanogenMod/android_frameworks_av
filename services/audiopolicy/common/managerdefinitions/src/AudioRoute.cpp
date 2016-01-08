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

#define LOG_TAG "APM::AudioRoute"
//#define LOG_NDEBUG 0

#include "AudioRoute.h"
#include "HwModule.h"
#include "AudioGain.h"

namespace android
{

void AudioRoute::dump(int fd, int spaces) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%*s- Type: %s\n", spaces, "", mType == AUDIO_ROUTE_MUX ? "Mux" : "Mix");
    result.append(buffer);

    snprintf(buffer, SIZE, "%*s- Sink: %s\n", spaces, "", mSink->getTagName().string());
    result.append(buffer);

    if (mSources.size() != 0) {
        snprintf(buffer, SIZE, "%*s- Sources: \n", spaces, "");
        result.append(buffer);
        for (size_t i = 0; i < mSources.size(); i++) {
            snprintf(buffer, SIZE, "%*s%s \n", spaces + 4, "", mSources[i]->getTagName().string());
            result.append(buffer);
        }
    }
    result.append("\n");

    write(fd, result.string(), result.size());
}

}
