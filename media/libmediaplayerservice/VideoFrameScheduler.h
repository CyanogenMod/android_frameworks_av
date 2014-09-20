/*
 * Copyright 2014, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VIDEO_FRAME_SCHEDULER_H_
#define VIDEO_FRAME_SCHEDULER_H_

#include <utils/RefBase.h>
#include <utils/Timers.h>

#include <media/stagefright/foundation/ABase.h>

namespace android {

struct ISurfaceComposer;

struct VideoFrameScheduler : public RefBase {
    VideoFrameScheduler();

    // (re)initialize scheduler
    void init();
    // get adjusted nanotime for a video frame render at renderTime
    nsecs_t schedule(nsecs_t renderTime);

    // returns the vsync period for the main display
    nsecs_t getVsyncPeriod();

    void release();

protected:
    virtual ~VideoFrameScheduler();

private:
    void updateVsync();

    nsecs_t mVsyncTime;        // vsync timing from display
    nsecs_t mVsyncPeriod;
    nsecs_t mVsyncRefreshAt;   // next time to refresh timing info

    sp<ISurfaceComposer> mComposer;

    DISALLOW_EVIL_CONSTRUCTORS(VideoFrameScheduler);
};

}  // namespace android

#endif  // VIDEO_FRAME_SCHEDULER_H_

