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

#ifndef ANDROID_AUDIO_FAST_THREAD_H
#define ANDROID_AUDIO_FAST_THREAD_H

#include <utils/Thread.h>

namespace android {

// FastThread is the common abstract base class of FastMixer and FastCapture
class FastThread : public Thread {

public:
            FastThread() : Thread(false /*canCallJava*/) { }
    virtual ~FastThread() { }

protected:
    virtual bool threadLoop() = 0;

};  // class FastThread

}   // android

#endif  // ANDROID_AUDIO_FAST_THREAD_H
