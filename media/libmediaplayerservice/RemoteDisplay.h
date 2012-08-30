/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef REMOTE_DISPLAY_H_

#define REMOTE_DISPLAY_H_

#include <media/stagefright/foundation/ABase.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>

namespace android {

struct ALooper;
struct ANetworkSession;
struct WifiDisplaySource;

struct RemoteDisplay : public RefBase {
    RemoteDisplay();

    status_t start(const char *iface);
    status_t stop();

protected:
    virtual ~RemoteDisplay();

private:
    status_t mInitCheck;

    sp<ALooper> mNetLooper;
    sp<ALooper> mLooper;
    sp<ANetworkSession> mNetSession;
    sp<WifiDisplaySource> mSource;

    DISALLOW_EVIL_CONSTRUCTORS(RemoteDisplay);
};

}  // namespace android

#endif  // REMOTE_DISPLAY_H_

