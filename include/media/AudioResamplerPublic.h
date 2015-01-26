/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ANDROID_AUDIO_RESAMPLER_PUBLIC_H
#define ANDROID_AUDIO_RESAMPLER_PUBLIC_H

// AUDIO_RESAMPLER_DOWN_RATIO_MAX is the maximum ratio between the original
// audio sample rate and the target rate when downsampling,
// as permitted in the audio framework, e.g. AudioTrack and AudioFlinger.
// In practice, it is not recommended to downsample more than 6:1
// for best audio quality, even though the audio framework permits a larger
// downsampling ratio.
// TODO: replace with an API
#define AUDIO_RESAMPLER_DOWN_RATIO_MAX 256

// Returns the source frames needed to resample to destination frames.  This is not a precise
// value and depends on the resampler (and possibly how it handles rounding internally).
// Nevertheless, this should be an upper bound on the requirements of the resampler.
// If srcSampleRate and dstSampleRate are equal, then it returns destination frames, which
// may not be true if the resampler is asynchronous.
static inline size_t sourceFramesNeeded(
        uint32_t srcSampleRate, size_t dstFramesRequired, uint32_t dstSampleRate) {
    // +1 for rounding - always do this even if matched ratio (resampler may use phases not ratio)
    // +1 for additional sample needed for interpolation
    return srcSampleRate == dstSampleRate ? dstFramesRequired :
            size_t((uint64_t)dstFramesRequired * srcSampleRate / dstSampleRate + 1 + 1);
}

#endif // ANDROID_AUDIO_RESAMPLER_PUBLIC_H
