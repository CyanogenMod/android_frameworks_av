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

#ifndef MEDIA_CLOCK_H_

#define MEDIA_CLOCK_H_

#include <media/stagefright/foundation/ABase.h>
#include <utils/Mutex.h>
#include <utils/RefBase.h>

namespace android {

struct AMessage;

struct MediaClock : public RefBase {
    MediaClock();

    void setStartingTimeMedia(int64_t startingTimeMediaUs);

    void clearAnchor();
    // It's highly recommended to use timestamp of just rendered frame as
    // anchor time, especially in paused state. Such restriction will be
    // required when dynamic playback rate is supported in the future.
    void updateAnchor(
        int64_t anchorTimeMediaUs,
        int64_t anchorTimeRealUs,
        int64_t maxTimeMediaUs = INT64_MAX);

    void updateMaxTimeMedia(int64_t maxTimeMediaUs);

    void pause();
    void resume();

    int64_t getTimeMedia(int64_t realUs, bool allowPastMaxTime = false);

protected:
    virtual ~MediaClock();

private:
    Mutex mLock;

    int64_t mAnchorTimeMediaUs;
    int64_t mAnchorTimeRealUs;
    int64_t mMaxTimeMediaUs;
    int64_t mStartingTimeMediaUs;

    bool mPaused;

    DISALLOW_EVIL_CONSTRUCTORS(MediaClock);
};

}  // namespace android

#endif  // MEDIA_CLOCK_H_
