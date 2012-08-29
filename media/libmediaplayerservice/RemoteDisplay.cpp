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

#include "RemoteDisplay.h"

#include "ANetworkSession.h"
#include "source/WifiDisplaySource.h"

namespace android {

RemoteDisplay::RemoteDisplay()
    : mInitCheck(NO_INIT),
      mLooper(new ALooper),
      mNetSession(new ANetworkSession),
      mSource(new WifiDisplaySource(mNetSession)) {
    mLooper->registerHandler(mSource);
}

RemoteDisplay::~RemoteDisplay() {
}

status_t RemoteDisplay::start() {
    mNetSession->start();
    mLooper->start();

    // XXX replace with 8554 for bcom dongle (it doesn't respect the
    // default port or the one advertised in the wfd IE).
    mSource->start(WifiDisplaySource::kWifiDisplayDefaultPort);

    return OK;
}

status_t RemoteDisplay::stop() {
    mSource->stop();

    mLooper->stop();
    mNetSession->stop();

    return OK;
}

}  // namespace android

