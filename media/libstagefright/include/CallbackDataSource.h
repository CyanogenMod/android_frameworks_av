/*
 * Copyright 2015 The Android Open Source Project
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

#ifndef ANDROID_CALLBACKDATASOURCE_H
#define ANDROID_CALLBACKDATASOURCE_H

#include <media/stagefright/DataSource.h>
#include <media/stagefright/foundation/ADebug.h>

namespace android {

class IDataSource;
class IMemory;

// A stagefright DataSource that wraps a binder IDataSource. It's a "Callback"
// DataSource because it calls back to the IDataSource for data.
class CallbackDataSource : public DataSource {
public:
    CallbackDataSource(const sp<IDataSource>& iDataSource);
    virtual ~CallbackDataSource();

    // DataSource implementation.
    virtual status_t initCheck() const;
    virtual ssize_t readAt(off64_t offset, void *data, size_t size);
    virtual status_t getSize(off64_t *size);

private:
    sp<IDataSource> mIDataSource;
    sp<IMemory> mMemory;

    DISALLOW_EVIL_CONSTRUCTORS(CallbackDataSource);
};

}; // namespace android

#endif // ANDROID_CALLBACKDATASOURCE_H
