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

#define LOG_TAG "Camera2Client::FrameProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>

#include "FrameProcessor.h"
#include "../Camera2Device.h"
#include "../Camera2Client.h"

namespace android {
namespace camera2 {

FrameProcessor::FrameProcessor(wp<Camera2Client> client):
        Thread(false), mClient(client) {
}

FrameProcessor::~FrameProcessor() {
    ALOGV("%s: Exit", __FUNCTION__);
}

status_t FrameProcessor::registerListener(int32_t id,
        wp<FilteredListener> listener) {
    Mutex::Autolock l(mInputMutex);
    ALOGV("%s: Registering listener for frame id %d",
            __FUNCTION__, id);
    return mListeners.replaceValueFor(id, listener);
}

status_t FrameProcessor::removeListener(int32_t id) {
    Mutex::Autolock l(mInputMutex);
    return mListeners.removeItem(id);
}

void FrameProcessor::dump(int fd, const Vector<String16>& args) {
    String8 result("    Latest received frame:\n");
    write(fd, result.string(), result.size());
    mLastFrame.dump(fd, 2, 6);
}

bool FrameProcessor::threadLoop() {
    status_t res;

    sp<Camera2Device> device;
    {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return false;
        device = client->getCameraDevice();
        if (device == 0) return false;
    }

    res = device->waitForNextFrame(kWaitDuration);
    if (res == OK) {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return false;
        processNewFrames(client);
    } else if (res != TIMED_OUT) {
        ALOGE("Camera2Client::FrameProcessor: Error waiting for new "
                "frames: %s (%d)", strerror(-res), res);
    }

    return true;
}

void FrameProcessor::processNewFrames(sp<Camera2Client> &client) {
    status_t res;
    ATRACE_CALL();
    CameraMetadata frame;
    while ( (res = client->getCameraDevice()->getNextFrame(&frame)) == OK) {
        camera_metadata_entry_t entry;

        entry = frame.find(ANDROID_REQUEST_FRAME_COUNT);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: Error reading frame number",
                    __FUNCTION__, client->getCameraId());
            break;
        }

        res = processFaceDetect(frame, client);
        if (res != OK) break;

        // Must be last - listener can take ownership of frame
        res = processListener(frame, client);
        if (res != OK) break;

        if (!frame.isEmpty()) {
            mLastFrame.acquire(frame);
        }
    }
    if (res != NOT_ENOUGH_DATA) {
        ALOGE("%s: Camera %d: Error getting next frame: %s (%d)",
                __FUNCTION__, client->getCameraId(), strerror(-res), res);
        return;
    }

    return;
}

status_t FrameProcessor::processListener(CameraMetadata &frame,
        sp<Camera2Client> &client) {
    status_t res;
    ATRACE_CALL();
    camera_metadata_entry_t entry;

    entry = frame.find(ANDROID_REQUEST_ID);
    if (entry.count == 0) {
        ALOGE("%s: Camera %d: Error reading frame id",
                __FUNCTION__, client->getCameraId());
        return BAD_VALUE;
    }
    int32_t frameId = entry.data.i32[0];
    ALOGV("%s: Got frame with ID %d", __FUNCTION__, frameId);

    sp<FilteredListener> listener;
    {
        Mutex::Autolock l(mInputMutex);
        ssize_t listenerIndex = mListeners.indexOfKey(frameId);
        if (listenerIndex != NAME_NOT_FOUND) {
            listener = mListeners[listenerIndex].promote();
            if (listener == 0) {
                mListeners.removeItemsAt(listenerIndex, 1);
            }
        }
    }

    if (listener != 0) {
        listener->onFrameAvailable(frameId, frame);
    }
    return OK;
}

status_t FrameProcessor::processFaceDetect(const CameraMetadata &frame,
        sp<Camera2Client> &client) {
    status_t res;
    ATRACE_CALL();
    camera_metadata_ro_entry_t entry;
    bool enableFaceDetect;
    int maxFaces;
    {
        SharedParameters::Lock l(client->getParameters());
        enableFaceDetect = l.mParameters.enableFaceDetect;
    }
    entry = frame.find(ANDROID_STATS_FACE_DETECT_MODE);

    // TODO: This should be an error once implementations are compliant
    if (entry.count == 0) {
        return OK;
    }

    uint8_t faceDetectMode = entry.data.u8[0];

    camera_frame_metadata metadata;
    Vector<camera_face_t> faces;
    metadata.number_of_faces = 0;

    if (enableFaceDetect && faceDetectMode != ANDROID_STATS_FACE_DETECTION_OFF) {
        SharedParameters::Lock l(client->getParameters());
        entry = frame.find(ANDROID_STATS_FACE_RECTANGLES);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: Unable to read face rectangles",
                    __FUNCTION__, client->getCameraId());
            return res;
        }
        metadata.number_of_faces = entry.count / 4;
        if (metadata.number_of_faces >
                l.mParameters.fastInfo.maxFaces) {
            ALOGE("%s: Camera %d: More faces than expected! (Got %d, max %d)",
                    __FUNCTION__, client->getCameraId(),
                    metadata.number_of_faces, l.mParameters.fastInfo.maxFaces);
            return res;
        }
        const int32_t *faceRects = entry.data.i32;

        entry = frame.find(ANDROID_STATS_FACE_SCORES);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: Unable to read face scores",
                    __FUNCTION__, client->getCameraId());
            return res;
        }
        const uint8_t *faceScores = entry.data.u8;

        const int32_t *faceLandmarks = NULL;
        const int32_t *faceIds = NULL;

        if (faceDetectMode == ANDROID_STATS_FACE_DETECTION_FULL) {
            entry = frame.find(ANDROID_STATS_FACE_LANDMARKS);
            if (entry.count == 0) {
                ALOGE("%s: Camera %d: Unable to read face landmarks",
                        __FUNCTION__, client->getCameraId());
                return res;
            }
            faceLandmarks = entry.data.i32;

            entry = frame.find(ANDROID_STATS_FACE_IDS);

            if (entry.count == 0) {
                ALOGE("%s: Camera %d: Unable to read face IDs",
                        __FUNCTION__, client->getCameraId());
                return res;
            }
            faceIds = entry.data.i32;
        }

        faces.setCapacity(metadata.number_of_faces);

        for (int i = 0; i < metadata.number_of_faces; i++) {
            camera_face_t face;

            face.rect[0] = l.mParameters.arrayXToNormalized(faceRects[i*4 + 0]);
            face.rect[1] = l.mParameters.arrayYToNormalized(faceRects[i*4 + 1]);
            face.rect[2] = l.mParameters.arrayXToNormalized(faceRects[i*4 + 2]);
            face.rect[3] = l.mParameters.arrayYToNormalized(faceRects[i*4 + 3]);

            face.score = faceScores[i];
            if (faceDetectMode == ANDROID_STATS_FACE_DETECTION_FULL) {
                face.id = faceIds[i];
                face.left_eye[0] =
                        l.mParameters.arrayXToNormalized(faceLandmarks[i*6 + 0]);
                face.left_eye[1] =
                        l.mParameters.arrayYToNormalized(faceLandmarks[i*6 + 1]);
                face.right_eye[0] =
                        l.mParameters.arrayXToNormalized(faceLandmarks[i*6 + 2]);
                face.right_eye[1] =
                        l.mParameters.arrayYToNormalized(faceLandmarks[i*6 + 3]);
                face.mouth[0] =
                        l.mParameters.arrayXToNormalized(faceLandmarks[i*6 + 4]);
                face.mouth[1] =
                        l.mParameters.arrayYToNormalized(faceLandmarks[i*6 + 5]);
            } else {
                face.id = 0;
                face.left_eye[0] = face.left_eye[1] = -2000;
                face.right_eye[0] = face.right_eye[1] = -2000;
                face.mouth[0] = face.mouth[1] = -2000;
            }
            faces.push_back(face);
        }

        metadata.faces = faces.editArray();
    }

    if (metadata.number_of_faces != 0) {
        Camera2Client::SharedCameraClient::Lock l(client->mSharedCameraClient);
        if (l.mCameraClient != NULL) {
            l.mCameraClient->dataCallback(CAMERA_MSG_PREVIEW_METADATA,
                    NULL, &metadata);
        }
    }
    return OK;
}

}; // namespace camera2
}; // namespace android
