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

#ifndef ANDROID_AUDIO_FAST_THREAD_STATE_H
#define ANDROID_AUDIO_FAST_THREAD_STATE_H

#include <stdint.h>
#include <media/nbaio/NBLog.h>

namespace android {

// Represents a single state of a FastThread
struct FastThreadState {
                FastThreadState();
    /*virtual*/ ~FastThreadState();

    typedef uint32_t Command;
    static const Command
        INITIAL = 0,            // used only for the initial state
        HOT_IDLE = 1,           // do nothing
        COLD_IDLE = 2,          // wait for the futex
        IDLE = 3,               // either HOT_IDLE or COLD_IDLE
        EXIT = 4;               // exit from thread
        // additional values defined per subclass
    Command     mCommand;

    int32_t*    mColdFutexAddr; // for COLD_IDLE only, pointer to the associated futex
    unsigned    mColdGen;       // increment when COLD_IDLE is requested so it's only performed once

    NBLog::Writer* mNBLogWriter; // non-blocking logger
};  // struct FastThreadState

}   // android

#endif  // ANDROID_AUDIO_FAST_THREAD_STATE_H
