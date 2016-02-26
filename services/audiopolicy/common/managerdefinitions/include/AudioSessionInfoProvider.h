/*
 * Copyright (C) 2016 The Android Open Source Project
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

namespace android {

/**
 * Interface for input descriptors to implement so dependent audio sessions can query information
 * about their context
 */
class AudioSessionInfoProvider
{
public:
    virtual ~AudioSessionInfoProvider() {};

    virtual audio_config_base_t getConfig() const = 0;

    virtual audio_patch_handle_t getPatchHandle() const = 0;

};

class AudioSessionInfoUpdateListener
{
public:
    virtual ~AudioSessionInfoUpdateListener() {};

    virtual void onSessionInfoUpdate() const = 0;;
};

} // namespace android
