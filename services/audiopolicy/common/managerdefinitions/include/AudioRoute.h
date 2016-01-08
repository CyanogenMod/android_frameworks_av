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

#include "AudioCollections.h"
#include <utils/String8.h>
#include <utils/Vector.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>

namespace android
{

class AudioPort;
class DeviceDescriptor;

typedef enum {
    AUDIO_ROUTE_MUX = 0,
    AUDIO_ROUTE_MIX = 1
} audio_route_type_t;

class AudioRoute  : public virtual RefBase
{
public:
    AudioRoute(audio_route_type_t type) : mType(type) {}

    void setSources(const AudioPortVector &sources) { mSources = sources; }
    const AudioPortVector &getSources() const { return mSources; }

    void setSink(const sp<AudioPort> &sink) { mSink = sink; }
    const sp<AudioPort> &getSink() const { return mSink; }

    audio_route_type_t getType() const { return mType; }

    void dump(int fd, int spaces) const;

private:
    AudioPortVector mSources;
    sp<AudioPort> mSink;
    audio_route_type_t mType;

};

}; // namespace android
