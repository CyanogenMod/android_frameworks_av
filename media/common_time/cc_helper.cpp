/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <stdint.h>

#include <common_time/cc_helper.h>
#include <common_time/ICommonClock.h>
#include <utils/threads.h>

namespace android {

Mutex CCHelper::lock_;
sp<ICommonClock> CCHelper::common_clock_;
sp<ICommonClockListener> CCHelper::common_clock_listener_;
uint32_t CCHelper::ref_count_ = 0;

bool CCHelper::verifyClock_l() {
    bool ret = false;

    if (common_clock_ == NULL) {
        common_clock_ = ICommonClock::getInstance();
        if (common_clock_ == NULL)
            goto bailout;
    }

    if (ref_count_ > 0) {
        if (common_clock_listener_ == NULL) {
            common_clock_listener_ = new CommonClockListener();
            if (common_clock_listener_ == NULL)
                goto bailout;

            if (OK != common_clock_->registerListener(common_clock_listener_))
                goto bailout;
        }
    }

    ret = true;

bailout:
    if (!ret) {
        common_clock_listener_ = NULL;
        common_clock_ = NULL;
    }
    return ret;
}

CCHelper::CCHelper() {
    Mutex::Autolock lock(&lock_);
    ref_count_++;
    verifyClock_l();
}

CCHelper::~CCHelper() {
    Mutex::Autolock lock(&lock_);

    assert(ref_count_ > 0);
    ref_count_--;

    // If we were the last CCHelper instance in the system, and we had
    // previously register a listener, unregister it now so that the common time
    // service has the chance to go into auto-disabled mode.
    if (!ref_count_ &&
       (common_clock_ != NULL) &&
       (common_clock_listener_ != NULL)) {
        common_clock_->unregisterListener(common_clock_listener_);
        common_clock_listener_ = NULL;
    }
}

void CCHelper::CommonClockListener::onTimelineChanged(uint64_t timelineID) {
    // do nothing; listener is only really used as a token so the server can
    // find out when clients die.
}

// Helper methods which attempts to make calls to the common time binder
// service.  If the first attempt fails with DEAD_OBJECT, the helpers will
// attempt to make a connection to the service again (assuming that the process
// hosting the service had crashed and the client proxy we are holding is dead)
// If the second attempt fails, or no connection can be made, the we let the
// error propagate up the stack and let the caller deal with the situation as
// best they can.
#define CCHELPER_METHOD(decl, call)                 \
    status_t CCHelper::decl {                       \
        Mutex::Autolock lock(&lock_);               \
                                                    \
        if (!verifyClock_l())                       \
            return DEAD_OBJECT;                     \
                                                    \
        status_t status = common_clock_->call;      \
        if (DEAD_OBJECT == status) {                \
            if (!verifyClock_l())                   \
                return DEAD_OBJECT;                 \
            status = common_clock_->call;           \
        }                                           \
                                                    \
        return status;                              \
    }

#define VERIFY_CLOCK()

CCHELPER_METHOD(isCommonTimeValid(bool* valid, uint32_t* timelineID),
                isCommonTimeValid(valid, timelineID))
CCHELPER_METHOD(commonTimeToLocalTime(int64_t commonTime, int64_t* localTime),
                commonTimeToLocalTime(commonTime, localTime))
CCHELPER_METHOD(localTimeToCommonTime(int64_t localTime, int64_t* commonTime),
                localTimeToCommonTime(localTime, commonTime))
CCHELPER_METHOD(getCommonTime(int64_t* commonTime),
                getCommonTime(commonTime))
CCHELPER_METHOD(getCommonFreq(uint64_t* freq),
                getCommonFreq(freq))
CCHELPER_METHOD(getLocalTime(int64_t* localTime),
                getLocalTime(localTime))
CCHELPER_METHOD(getLocalFreq(uint64_t* freq),
                getLocalFreq(freq))

}  // namespace android
