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

//#define LOG_NDEBUG 0
#define LOG_TAG "VideoFrameScheduler"
#include <utils/Log.h>
#define ATRACE_TAG ATRACE_TAG_VIDEO
#include <utils/Trace.h>

#include <sys/time.h>

#include <binder/IServiceManager.h>
#include <gui/ISurfaceComposer.h>
#include <ui/DisplayStatInfo.h>

#include <media/stagefright/foundation/ADebug.h>

#include "VideoFrameScheduler.h"

namespace android {

static const nsecs_t kNanosIn1s = 1000000000;

/* ======================================================================= */
/*                             Frame Scheduler                             */
/* ======================================================================= */

static const nsecs_t kDefaultVsyncPeriod = kNanosIn1s / 60;  // 60Hz
static const nsecs_t kVsyncRefreshPeriod = kNanosIn1s;       // 1 sec

VideoFrameScheduler::VideoFrameScheduler()
    : mVsyncTime(0),
      mVsyncPeriod(0),
      mVsyncRefreshAt(0) {
}

void VideoFrameScheduler::updateVsync() {
    mVsyncRefreshAt = systemTime(SYSTEM_TIME_MONOTONIC) + kVsyncRefreshPeriod;
    mVsyncPeriod = 0;
    mVsyncTime = 0;

    // TODO: schedule frames for the destination surface
    // For now, surface flinger only schedules frames on the primary display
    if (mComposer == NULL) {
        String16 name("SurfaceFlinger");
        sp<IServiceManager> sm = defaultServiceManager();
        mComposer = interface_cast<ISurfaceComposer>(sm->checkService(name));
    }
    if (mComposer != NULL) {
        DisplayStatInfo stats;
        status_t res = mComposer->getDisplayStats(NULL /* display */, &stats);
        if (res == OK) {
            ALOGV("vsync time:%lld period:%lld",
                    (long long)stats.vsyncTime, (long long)stats.vsyncPeriod);
            mVsyncTime = stats.vsyncTime;
            mVsyncPeriod = stats.vsyncPeriod;
        } else {
            ALOGW("getDisplayStats returned %d", res);
        }
    } else {
        ALOGW("could not get surface mComposer service");
    }
}

void VideoFrameScheduler::init() {
    updateVsync();
}

nsecs_t VideoFrameScheduler::getVsyncPeriod() {
    if (mVsyncPeriod > 0) {
        return mVsyncPeriod;
    }
    return kDefaultVsyncPeriod;
}

nsecs_t VideoFrameScheduler::schedule(nsecs_t renderTime) {
    nsecs_t origRenderTime = renderTime;

    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    if (now >= mVsyncRefreshAt) {
        updateVsync();
    }

    // without VSYNC info, there is nothing to do
    if (mVsyncPeriod == 0) {
        ALOGV("no vsync: render=%lld", (long long)renderTime);
        return renderTime;
    }

    // ensure vsync time is well before (corrected) render time
    if (mVsyncTime > renderTime - 4 * mVsyncPeriod) {
        mVsyncTime -=
            ((mVsyncTime - renderTime) / mVsyncPeriod + 5) * mVsyncPeriod;
    }

    // Video presentation takes place at the VSYNC _after_ renderTime.  Adjust renderTime
    // so this effectively becomes a rounding operation (to the _closest_ VSYNC.)
    renderTime -= mVsyncPeriod / 2;

    // align rendertime to the center between VSYNC edges
    renderTime -= (renderTime - mVsyncTime) % mVsyncPeriod;
    renderTime += mVsyncPeriod / 2;
    ALOGV("adjusting render: %lld => %lld", (long long)origRenderTime, (long long)renderTime);
    ATRACE_INT("FRAME_FLIP_IN(ms)", (renderTime - now) / 1000000);
    return renderTime;
}

void VideoFrameScheduler::release() {
    mComposer.clear();
}

VideoFrameScheduler::~VideoFrameScheduler() {
    release();
}

} // namespace android

