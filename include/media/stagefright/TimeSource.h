/*
 * Copyright (C) 2009 The Android Open Source Project
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

#ifndef TIME_SOURCE_H_

#define TIME_SOURCE_H_

#include <stdint.h>

namespace android {

class TimeSource {
public:
    TimeSource() {}
    virtual ~TimeSource() {}

    virtual int64_t getRealTimeUs() = 0;

private:
    TimeSource(const TimeSource &);
    TimeSource &operator=(const TimeSource &);
};

class SystemTimeSource : public TimeSource {
public:
    SystemTimeSource();

    virtual int64_t getRealTimeUs();

private:
    int64_t mStartTimeUs;
};

}  // namespace android

#endif  // TIME_SOURCE_H_
