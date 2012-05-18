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

#define LOG_TAG "Camera2Client"
//#define LOG_NDEBUG 0

#include <cutils/properties.h>
#include <gui/SurfaceTextureClient.h>
#include <gui/Surface.h>

#include "Camera2Client.h"

namespace android {

#define ALOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define ALOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);

#define ALOG1_ENTRY {       \
    int callingPid = getCallingPid(); \
    ALOG1("%s: E (pid %d, id %d) ", __FUNCTION__, \
            callingPid, mCameraId);      \
}

static int getCallingPid() {
    return IPCThreadState::self()->getCallingPid();
}

static int getCallingUid() {
    return IPCThreadState::self()->getCallingUid();
}

// Interface used by CameraService

Camera2Client::Camera2Client(const sp<CameraService>& cameraService,
        const sp<ICameraClient>& cameraClient,
        const sp<Camera2Device>& device,
        int cameraId,
        int cameraFacing,
        int clientPid):
        Client(cameraService, cameraClient,
                cameraId, cameraFacing, clientPid) {
    int callingPid = getCallingPid();
    ALOG1("%s: E (pid %d, id %d)", __FUNCTION__, callingPid, cameraId);
    ALOG1_ENTRY;

}

Camera2Client::~Camera2Client() {
}

status_t Camera2Client::dump(int fd, const Vector<String16>& args) {
    return BAD_VALUE;
}

// ICamera interface

void Camera2Client::disconnect() {
    CameraService::Client::disconnect();
}

status_t Camera2Client::connect(const sp<ICameraClient>& client) {
    return BAD_VALUE;
}

status_t Camera2Client::lock() {
    return BAD_VALUE;
}

status_t Camera2Client::unlock() {
    return BAD_VALUE;
}

status_t Camera2Client::setPreviewDisplay(const sp<Surface>& surface) {
    return BAD_VALUE;
}

status_t Camera2Client::setPreviewTexture(const sp<ISurfaceTexture>& surfaceTexture) {
    return BAD_VALUE;
}

void Camera2Client::setPreviewCallbackFlag(int flag) {

}

status_t Camera2Client::startPreview() {
    return BAD_VALUE;
}

void Camera2Client::stopPreview() {

}

bool Camera2Client::previewEnabled() {
    return false;
}

status_t Camera2Client::storeMetaDataInBuffers(bool enabled) {
    return BAD_VALUE;
}

status_t Camera2Client::startRecording() {
    return BAD_VALUE;
}

void Camera2Client::stopRecording() {
}

bool Camera2Client::recordingEnabled() {
    return BAD_VALUE;
}

void Camera2Client::releaseRecordingFrame(const sp<IMemory>& mem) {

}

status_t Camera2Client::autoFocus() {
    return BAD_VALUE;
}

status_t Camera2Client::cancelAutoFocus() {
    return BAD_VALUE;
}

status_t Camera2Client::takePicture(int msgType) {
    return BAD_VALUE;
}

status_t Camera2Client::setParameters(const String8& params) {
    return BAD_VALUE;
}
String8 Camera2Client::getParameters() const {
    return String8();
}

status_t Camera2Client::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2) {
    return BAD_VALUE;
}


} // namespace android
