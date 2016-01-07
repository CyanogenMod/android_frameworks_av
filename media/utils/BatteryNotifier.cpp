/*
 * Copyright 2015, The Android Open Source Project
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

#define LOG_TAG "BatteryNotifier"
//#define LOG_NDEBUG 0

#include "include/mediautils/BatteryNotifier.h"

#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <private/android_filesystem_config.h>

namespace android {

void BatteryNotifier::DeathNotifier::binderDied(const wp<IBinder>& /*who*/) {
    BatteryNotifier::getInstance().onBatteryStatServiceDied();
}

BatteryNotifier::BatteryNotifier() : mVideoRefCount(0), mAudioRefCount(0) {}

BatteryNotifier::~BatteryNotifier() {
    Mutex::Autolock _l(mLock);
    if (mDeathNotifier != nullptr) {
        IInterface::asBinder(mBatteryStatService)->unlinkToDeath(mDeathNotifier);
    }
}

void BatteryNotifier::noteStartVideo() {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();
    if (mVideoRefCount == 0 && batteryService != nullptr) {
        batteryService->noteStartVideo(AID_MEDIA);
    }
    mVideoRefCount++;
}

void BatteryNotifier::noteStopVideo() {
    Mutex::Autolock _l(mLock);
    if (mVideoRefCount == 0) {
        ALOGW("%s: video refcount is broken.", __FUNCTION__);
        return;
    }

    sp<IBatteryStats> batteryService = getBatteryService_l();

    mVideoRefCount--;
    if (mVideoRefCount == 0 && batteryService != nullptr) {
        batteryService->noteStopVideo(AID_MEDIA);
    }
}

void BatteryNotifier::noteResetVideo() {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();
    mVideoRefCount = 0;
    if (batteryService != nullptr) {
        batteryService->noteResetAudio();
    }
}

void BatteryNotifier::noteStartAudio() {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();
    if (mAudioRefCount == 0 && batteryService != nullptr) {
        batteryService->noteStartAudio(AID_MEDIA);
    }
    mAudioRefCount++;
}

void BatteryNotifier::noteStopAudio() {
    Mutex::Autolock _l(mLock);
    if (mAudioRefCount == 0) {
        ALOGW("%s: audio refcount is broken.", __FUNCTION__);
        return;
    }

    sp<IBatteryStats> batteryService = getBatteryService_l();

    mAudioRefCount--;
    if (mAudioRefCount == 0 && batteryService != nullptr) {
        batteryService->noteStopAudio(AID_MEDIA);
    }
}

void BatteryNotifier::noteResetAudio() {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();
    mAudioRefCount = 0;
    if (batteryService != nullptr) {
        batteryService->noteResetAudio();
    }
}

void BatteryNotifier::noteFlashlightOn(const String8& id, int uid) {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();

    std::pair<String8, int> k = std::make_pair(id, uid);
    if (!mFlashlightState[k]) {
        mFlashlightState[k] = true;
        if (batteryService != nullptr) {
            batteryService->noteFlashlightOn(uid);
        }
    }
}

void BatteryNotifier::noteFlashlightOff(const String8& id, int uid) {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();

    std::pair<String8, int> k = std::make_pair(id, uid);
    if (mFlashlightState[k]) {
        mFlashlightState[k] = false;
        if (batteryService != nullptr) {
            batteryService->noteFlashlightOff(uid);
        }
    }
}

void BatteryNotifier::noteResetFlashlight() {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();
    mFlashlightState.clear();
    if (batteryService != nullptr) {
        batteryService->noteResetFlashlight();
    }
}

void BatteryNotifier::noteStartCamera(const String8& id, int uid) {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();
    std::pair<String8, int> k = std::make_pair(id, uid);
    if (!mCameraState[k]) {
        mCameraState[k] = true;
        if (batteryService != nullptr) {
            batteryService->noteStartCamera(uid);
        }
    }
}

void BatteryNotifier::noteStopCamera(const String8& id, int uid) {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();
    std::pair<String8, int> k = std::make_pair(id, uid);
    if (mCameraState[k]) {
        mCameraState[k] = false;
        if (batteryService != nullptr) {
            batteryService->noteStopCamera(uid);
        }
    }
}

void BatteryNotifier::noteResetCamera() {
    Mutex::Autolock _l(mLock);
    sp<IBatteryStats> batteryService = getBatteryService_l();
    mCameraState.clear();
    if (batteryService != nullptr) {
        batteryService->noteResetCamera();
    }
}

void BatteryNotifier::onBatteryStatServiceDied() {
    Mutex::Autolock _l(mLock);
    mBatteryStatService.clear();
    mDeathNotifier.clear();
    // Do not reset mVideoRefCount and mAudioRefCount here. The ref
    // counting is independent of the battery service availability.
    // We need this if battery service becomes available after media
    // started.

}

sp<IBatteryStats> BatteryNotifier::getBatteryService_l() {
    if (mBatteryStatService != nullptr) {
        return mBatteryStatService;
    }
    // Get battery service from service manager
    const sp<IServiceManager> sm(defaultServiceManager());
    if (sm != nullptr) {
        const String16 name("batterystats");
        mBatteryStatService = interface_cast<IBatteryStats>(sm->checkService(name));
        if (mBatteryStatService == nullptr) {
            ALOGE("batterystats service unavailable!");
            return nullptr;
        }

        mDeathNotifier = new DeathNotifier();
        IInterface::asBinder(mBatteryStatService)->linkToDeath(mDeathNotifier);

        // Notify start now if media already started
        if (mVideoRefCount > 0) {
            mBatteryStatService->noteStartVideo(AID_MEDIA);
        }
        if (mAudioRefCount > 0) {
            mBatteryStatService->noteStartAudio(AID_MEDIA);
        }
    }
    return mBatteryStatService;
}

ANDROID_SINGLETON_STATIC_INSTANCE(BatteryNotifier);

}  // namespace android
