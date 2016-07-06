/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <binder/AppOpsManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/PermissionCache.h>
#include <private/android_filesystem_config.h>
#include "ServiceUtilities.h"

/* When performing permission checks we do not use permission cache for
 * runtime permissions (protection level dangerous) as they may change at
 * runtime. All other permissions (protection level normal and dangerous)
 * can be cached as they never change. Of course all permission checked
 * here are platform defined.
 */

namespace android {

// Not valid until initialized by AudioFlinger constructor.  It would have to be
// re-initialized if the process containing AudioFlinger service forks (which it doesn't).
pid_t getpid_cached;

bool recordingAllowed(const String16& opPackageName) {
    // Note: We are getting the UID from the calling IPC thread state because all
    // clients that perform recording create AudioRecord in their own processes
    // and the system does not create AudioRecord objects on behalf of apps. This
    // differs from playback where in some situations the system recreates AudioTrack
    // instances associated with a client's MediaPlayer on behalf of this client.
    // In the latter case we have to store the client UID and pass in along for
    // security checks.

    if (getpid_cached == IPCThreadState::self()->getCallingPid()) return true;
    static const String16 sRecordAudio("android.permission.RECORD_AUDIO");

    // IMPORTANT: Don't use PermissionCache - a runtime permission and may change.
    const bool ok = checkCallingPermission(sRecordAudio);
    if (!ok) {
        ALOGE("Request requires android.permission.RECORD_AUDIO");
        return false;
    }

    const uid_t uid = IPCThreadState::self()->getCallingUid();

    // To permit command-line native tests
    if (uid == AID_ROOT) return true;

    String16 checkedOpPackageName = opPackageName;

    // In some cases the calling code has no access to the package it runs under.
    // For example, code using the wilhelm framework's OpenSL-ES APIs. In this
    // case we will get the packages for the calling UID and pick the first one
    // for attributing the app op. This will work correctly for runtime permissions
    // as for legacy apps we will toggle the app op for all packages in the UID.
    // The caveat is that the operation may be attributed to the wrong package and
    // stats based on app ops may be slightly off.
    if (checkedOpPackageName.size() <= 0) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder = sm->getService(String16("permission"));
        if (binder == 0) {
            ALOGE("Cannot get permission service");
            return false;
        }

        sp<IPermissionController> permCtrl = interface_cast<IPermissionController>(binder);
        Vector<String16> packages;

        permCtrl->getPackagesForUid(uid, packages);

        if (packages.isEmpty()) {
            ALOGE("No packages for calling UID");
            return false;
        }
        checkedOpPackageName = packages[0];
    }

    AppOpsManager appOps;
    if (appOps.noteOp(AppOpsManager::OP_RECORD_AUDIO, uid, checkedOpPackageName)
            != AppOpsManager::MODE_ALLOWED) {
        ALOGE("Request denied by app op OP_RECORD_AUDIO");
        return false;
    }

    return true;
}

bool captureAudioOutputAllowed() {
    if (getpid_cached == IPCThreadState::self()->getCallingPid()) return true;
    static const String16 sCaptureAudioOutput("android.permission.CAPTURE_AUDIO_OUTPUT");
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkCallingPermission(sCaptureAudioOutput);
    if (!ok) ALOGE("Request requires android.permission.CAPTURE_AUDIO_OUTPUT");
    return ok;
}

bool accessFmRadioAllowed() {
    static const String16 sAccessFmRadio("android.permission.ACCESS_FM_RADIO");
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkCallingPermission(sAccessFmRadio);
    if (!ok) ALOGE("Request requires android.permission.ACCESS_FM_RADIO");
    return ok;
}

bool captureHotwordAllowed() {
    static const String16 sCaptureHotwordAllowed("android.permission.CAPTURE_AUDIO_HOTWORD");
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkCallingPermission(sCaptureHotwordAllowed);
    if (!ok) ALOGE("android.permission.CAPTURE_AUDIO_HOTWORD");
    return ok;
}

bool settingsAllowed() {
    if (getpid_cached == IPCThreadState::self()->getCallingPid()) return true;
    static const String16 sAudioSettings("android.permission.MODIFY_AUDIO_SETTINGS");
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkCallingPermission(sAudioSettings);
    if (!ok) ALOGE("Request requires android.permission.MODIFY_AUDIO_SETTINGS");
    return ok;
}

bool modifyAudioRoutingAllowed() {
    static const String16 sModifyAudioRoutingAllowed("android.permission.MODIFY_AUDIO_ROUTING");
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkCallingPermission(sModifyAudioRoutingAllowed);
    if (!ok) ALOGE("android.permission.MODIFY_AUDIO_ROUTING");
    return ok;
}

bool dumpAllowed() {
    // don't optimize for same pid, since mediaserver never dumps itself
    static const String16 sDump("android.permission.DUMP");
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkCallingPermission(sDump);
    // convention is for caller to dump an error message to fd instead of logging here
    //if (!ok) ALOGE("Request requires android.permission.DUMP");
    return ok;
}

} // namespace android
