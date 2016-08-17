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


#ifndef LOCK_WATCH_H
#define LOCK_WATCH_H

#include <utils/String8.h>
#include <utils/Thread.h>

namespace android {

// periodically checks if a mutex can be acquired and kill process otherwise
class LockWatch : public Thread {

public:
    static const uint32_t DEFAULT_PERIOD_MS = 10000; // 10 seconds default check period
    static const uint32_t DEFAULT_TIMEOUT_MS = 3000; // 3 seconds default lock timeout

    LockWatch(Mutex& lock, const String8& tag = String8(""),
            uint32_t periodMs = DEFAULT_PERIOD_MS, uint32_t timeoutMs = DEFAULT_TIMEOUT_MS)
        : Thread(false /*canCallJava*/),
          mLock(lock), mTag(tag), mPeriodMs(periodMs), mTimeOutMs(timeoutMs) {}

    virtual         ~LockWatch() { }

    // RefBase
    virtual void    onFirstRef();

private:
    // Thread
    virtual bool    threadLoop();

    Mutex&          mLock;          // monitored mutex
    String8         mTag;           // tag
    uint32_t        mPeriodMs;      // check period in milliseconds
    uint32_t        mTimeOutMs;     // mutex lock timeout in milliseconds
};

}   // namespace android

#endif  // LOCK_WATCH_H
