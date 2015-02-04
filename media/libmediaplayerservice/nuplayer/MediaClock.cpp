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

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaClock"
#include <utils/Log.h>

#include "MediaClock.h"

#include <media/stagefright/foundation/ALooper.h>

namespace android {

// Maximum time change between two updates.
static const int64_t kMaxAnchorFluctuationUs = 1000ll;

MediaClock::MediaClock()
    : mAnchorTimeMediaUs(-1),
      mAnchorTimeRealUs(-1),
      mMaxTimeMediaUs(INT64_MAX),
      mStartingTimeMediaUs(-1),
      mPaused(false) {
}

MediaClock::~MediaClock() {
}

void MediaClock::setStartingTimeMedia(int64_t startingTimeMediaUs) {
    Mutex::Autolock autoLock(mLock);
    mStartingTimeMediaUs = startingTimeMediaUs;
}

void MediaClock::clearAnchor() {
    Mutex::Autolock autoLock(mLock);
    mAnchorTimeMediaUs = -1;
    mAnchorTimeRealUs = -1;
}

void MediaClock::updateAnchor(
        int64_t anchorTimeMediaUs,
        int64_t anchorTimeRealUs,
        int64_t maxTimeMediaUs) {
    if (anchorTimeMediaUs < 0 || anchorTimeRealUs < 0) {
        ALOGW("reject anchor time since it is negative.");
        return;
    }

    int64_t nowUs = ALooper::GetNowUs();
    int64_t nowMediaUs = anchorTimeMediaUs + nowUs - anchorTimeRealUs;
    if (nowMediaUs < 0) {
        ALOGW("reject anchor time since it leads to negative media time.");
        return;
    }

    Mutex::Autolock autoLock(mLock);
    mAnchorTimeRealUs = nowUs;
    mAnchorTimeMediaUs = nowMediaUs;
    mMaxTimeMediaUs = maxTimeMediaUs;
}

void MediaClock::updateMaxTimeMedia(int64_t maxTimeMediaUs) {
    Mutex::Autolock autoLock(mLock);
    mMaxTimeMediaUs = maxTimeMediaUs;
}

void MediaClock::pause() {
    Mutex::Autolock autoLock(mLock);
    if (mPaused) {
        return;
    }

    mPaused = true;
    if (mAnchorTimeRealUs == -1) {
        return;
    }

    int64_t nowUs = ALooper::GetNowUs();
    mAnchorTimeMediaUs += nowUs - mAnchorTimeRealUs;
    if (mAnchorTimeMediaUs < 0) {
        ALOGW("anchor time should not be negative, set to 0.");
        mAnchorTimeMediaUs = 0;
    }
    mAnchorTimeRealUs = nowUs;
}

void MediaClock::resume() {
    Mutex::Autolock autoLock(mLock);
    if (!mPaused) {
        return;
    }

    mPaused = false;
    if (mAnchorTimeRealUs == -1) {
        return;
    }

    mAnchorTimeRealUs = ALooper::GetNowUs();
}

int64_t MediaClock::getTimeMedia(int64_t realUs, bool allowPastMaxTime) {
    Mutex::Autolock autoLock(mLock);
    if (mAnchorTimeRealUs == -1) {
        return -1ll;
    }

    if (mPaused) {
        realUs = mAnchorTimeRealUs;
    }
    int64_t currentMediaUs = mAnchorTimeMediaUs + realUs - mAnchorTimeRealUs;
    if (currentMediaUs > mMaxTimeMediaUs && !allowPastMaxTime) {
        currentMediaUs = mMaxTimeMediaUs;
    }
    if (currentMediaUs < mStartingTimeMediaUs) {
        currentMediaUs = mStartingTimeMediaUs;
    }
    if (currentMediaUs < 0) {
        currentMediaUs = 0;
    }
    return currentMediaUs;
}

}  // namespace android
