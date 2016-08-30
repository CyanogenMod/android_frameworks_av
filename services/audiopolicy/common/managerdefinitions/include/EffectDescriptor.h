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

#include <RoutingStrategy.h>
#include <hardware/audio_effect.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>
#include <utils/Thread.h>

namespace android {


class EffectDescriptor : public RefBase
{
public:
    status_t dump(int fd);

    int mIo;                // io the effect is attached to
    routing_strategy mStrategy; // routing strategy the effect is associated to
    int mSession;               // audio session the effect is on
    effect_descriptor_t mDesc;  // effect descriptor
    bool mEnabled;              // enabled state: CPU load being used or not
};

class EffectDescriptorCollection : public KeyedVector<int, sp<EffectDescriptor> >
{
public:
    EffectDescriptorCollection();

    status_t registerEffect(const effect_descriptor_t *desc, audio_io_handle_t io,
                            uint32_t strategy, int session, int id);
    status_t unregisterEffect(int id);
    status_t setEffectEnabled(int id, bool enabled);
    uint32_t getMaxEffectsCpuLoad() const;
    uint32_t getMaxEffectsMemory() const;
    bool isNonOffloadableEffectEnabled();

    status_t dump(int fd);

private:
    status_t setEffectEnabled(const sp<EffectDescriptor> &effectDesc, bool enabled);

    uint32_t mTotalEffectsCpuLoad; // current CPU load used by effects (in MIPS)
    uint32_t mTotalEffectsMemory;  // current memory used by effects (in KB)
    uint32_t mTotalEffectsMemoryMaxUsed; // maximum memory used by effects (in KB)

    /**
     * Maximum CPU load allocated to audio effects in 0.1 MIPS (ARMv5TE, 0 WS memory) units
     */
    static const uint32_t MAX_EFFECTS_CPU_LOAD = 1000;
    /**
     * Maximum memory allocated to audio effects in KB
     */
    static const uint32_t MAX_EFFECTS_MEMORY = 512;

    Mutex mLock;
};

}; // namespace android
