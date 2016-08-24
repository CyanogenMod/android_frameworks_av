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

#define LOG_TAG "LockWatch"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include "LockWatch.h"

namespace android {

void LockWatch::onFirstRef()
{
    run("lock watch", ANDROID_PRIORITY_URGENT_AUDIO);
}

bool LockWatch::threadLoop()
{
    while (!exitPending()) {
        // we neglect previous lock time effect on period
        usleep(mPeriodMs * 1000);
        if (mLock.timedLock(ms2ns(mTimeOutMs)) != NO_ERROR) {
            // FIXME: Current implementation of timedLock uses CLOCK_REALTIME which
            // increments even during CPU suspend.  Check twice to be sure.
            if (mLock.timedLock(ms2ns(mTimeOutMs)) != NO_ERROR) {
                LOG_ALWAYS_FATAL("LockWatch timeout for: %s", mTag.string());
            }
        }
        mLock.unlock();
    }
    return false;
}

}   // namespace android

