/*
**
** Copyright 2015, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef ANDROID_SERVICELOG_H
#define ANDROID_SERVICELOG_H

#include <utils/Errors.h>
#include <utils/String8.h>
#include <utils/threads.h>
#include <utils/Vector.h>

namespace android {

class ServiceLog : public RefBase {
public:
    ServiceLog();
    ServiceLog(size_t maxNum);

    void add(const String8 &log);
    String8 toString() const;

private:
    int mMaxNum;
    mutable Mutex mLock;
    Vector<String8> mLogs;
};

// ----------------------------------------------------------------------------

}; // namespace android

#endif // ANDROID_SERVICELOG_H
