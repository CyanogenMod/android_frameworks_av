/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ANDROID_AUDIO_TIMESTAMP_H
#define ANDROID_AUDIO_TIMESTAMP_H

#include <string>
#include <sstream>
#include <time.h>

namespace android {

class AudioTimestamp {
public:
    AudioTimestamp() : mPosition(0) {
        mTime.tv_sec = 0;
        mTime.tv_nsec = 0;
    }
    // FIXME change type to match android.media.AudioTrack
    uint32_t        mPosition; // a frame position in AudioTrack::getPosition() units
    struct timespec mTime;     // corresponding CLOCK_MONOTONIC when frame is expected to present
};

struct ExtendedTimestamp {
    enum Location {
        LOCATION_CLIENT,   // timestamp of last read frame from client-server track buffer
        LOCATION_SERVER,   // timestamp of newest frame from client-server track buffer
        LOCATION_KERNEL,   // timestamp of newest frame in the kernel (alsa) buffer.
        LOCATION_MAX       // for sizing arrays only
    };

    // This needs to be kept in sync with android.media.AudioTimestamp
    enum Timebase {
        TIMEBASE_MONOTONIC,  // Clock monotonic offset (generally 0)
        TIMEBASE_BOOTTIME,
        TIMEBASE_MAX,
    };

    ExtendedTimestamp() {
        clear();
    }

    // mPosition is expressed in frame units.
    // It is generally nonnegative, though we keep this signed for
    // to potentially express algorithmic latency at the start of the stream
    // and to prevent unintentional unsigned integer underflow.
    int64_t mPosition[LOCATION_MAX];

    // mTimeNs is in nanoseconds for the default timebase, monotonic.
    // If this value is -1, then both time and position are invalid.
    // If this value is 0, then the time is not valid but the position is valid.
    int64_t mTimeNs[LOCATION_MAX];

    // mTimebaseOffset is the offset in ns from monotonic when the
    // timestamp was taken.  This may vary due to suspend time
    // or NTP adjustment.
    int64_t mTimebaseOffset[TIMEBASE_MAX];

    void clear() {
        memset(mPosition, 0, sizeof(mPosition)); // actually not necessary if time is -1
        for (int i = 0; i < LOCATION_MAX; ++i) {
            mTimeNs[i] = -1;
        }
        memset(mTimebaseOffset, 0, sizeof(mTimebaseOffset));
    }

    // Returns the best timestamp as judged from the closest-to-hw stage in the
    // pipeline with a valid timestamp.
    int getBestTimestamp(int64_t *position, int64_t *time, int timebase) {
        if (position == nullptr || time == nullptr
                || timebase < 0 || timebase >= TIMEBASE_MAX) {
            return BAD_VALUE;
        }
        // look for the closest-to-hw stage in the pipeline with a valid timestamp.
        // We omit LOCATION_CLIENT as we prefer at least LOCATION_SERVER based accuracy
        // when getting the best timestamp.
        for (int i = LOCATION_MAX - 1; i >= LOCATION_SERVER; --i) {
            if (mTimeNs[i] > 0) {
                *position = mPosition[i];
                *time = mTimeNs[i] + mTimebaseOffset[timebase];
                return OK;
            }
        }
        return INVALID_OPERATION;
    }

    // convert fields to a printable string
    std::string toString() {
        std::stringstream ss;

        ss << "BOOTTIME offset " << mTimebaseOffset[TIMEBASE_BOOTTIME] << "\n";
        for (int i = 0; i < LOCATION_MAX; ++i) {
            ss << "ExtendedTimestamp[" << i << "]  position: "
                    << mPosition[i] << "  time: "  << mTimeNs[i] << "\n";
        }
        return ss.str();
    }
    // TODO:
    // Consider adding buffer status:
    // size, available, algorithmic latency
};

}   // namespace

#endif  // ANDROID_AUDIO_TIMESTAMP_H
