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

#define LOG_TAG "Camera2-CallbackProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>

#include "CallbackProcessor.h"
#include <gui/SurfaceTextureClient.h>
#include "../Camera2Device.h"
#include "../Camera2Client.h"


namespace android {
namespace camera2 {

CallbackProcessor::CallbackProcessor(wp<Camera2Client> client):
        Thread(false),
        mClient(client),
        mCallbackAvailable(false),
        mCallbackStreamId(NO_STREAM) {
}

CallbackProcessor::~CallbackProcessor() {
    ALOGV("%s: Exit", __FUNCTION__);
    deleteStream();
}

void CallbackProcessor::onFrameAvailable() {
    Mutex::Autolock l(mInputMutex);
    if (!mCallbackAvailable) {
        mCallbackAvailable = true;
        mCallbackAvailableSignal.signal();
    }
}

status_t CallbackProcessor::updateStream(const Parameters &params) {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock l(mInputMutex);

    sp<Camera2Client> client = mClient.promote();
    if (client == 0) return OK;
    sp<Camera2Device> device = client->getCameraDevice();

    if (mCallbackConsumer == 0) {
        // Create CPU buffer queue endpoint
        mCallbackConsumer = new CpuConsumer(kCallbackHeapCount);
        mCallbackConsumer->setFrameAvailableListener(this);
        mCallbackConsumer->setName(String8("Camera2Client::CallbackConsumer"));
        mCallbackWindow = new SurfaceTextureClient(
            mCallbackConsumer->getProducerInterface());
    }

    if (mCallbackStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight, currentFormat;
        res = device->getStreamInfo(mCallbackStreamId,
                &currentWidth, &currentHeight, &currentFormat);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying callback output stream info: "
                    "%s (%d)", __FUNCTION__, client->getCameraId(),
                    strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.previewWidth ||
                currentHeight != (uint32_t)params.previewHeight ||
                currentFormat != (uint32_t)params.previewFormat) {
            // Since size should only change while preview is not running,
            // assuming that all existing use of old callback stream is
            // completed.
            ALOGV("%s: Camera %d: Deleting stream %d since the buffer dimensions changed",
                __FUNCTION__, client->getCameraId(), mCallbackStreamId);
            res = device->deleteStream(mCallbackStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for callbacks: %s (%d)", __FUNCTION__, client->getCameraId(),
                        strerror(-res), res);
                return res;
            }
            mCallbackStreamId = NO_STREAM;
        }
    }

    if (mCallbackStreamId == NO_STREAM) {
        ALOGV("Creating callback stream: %d %d format 0x%x",
                params.previewWidth, params.previewHeight,
                params.previewFormat);
        res = device->createStream(mCallbackWindow,
                params.previewWidth, params.previewHeight,
                params.previewFormat, 0, &mCallbackStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create output stream for callbacks: "
                    "%s (%d)", __FUNCTION__, client->getCameraId(),
                    strerror(-res), res);
            return res;
        }
    }

    return OK;
}

status_t CallbackProcessor::deleteStream() {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock l(mInputMutex);

    if (mCallbackStreamId != NO_STREAM) {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return OK;
        sp<Camera2Device> device = client->getCameraDevice();

        device->deleteStream(mCallbackStreamId);

        mCallbackHeap.clear();
        mCallbackWindow.clear();
        mCallbackConsumer.clear();

        mCallbackStreamId = NO_STREAM;
    }
    return OK;
}

int CallbackProcessor::getStreamId() const {
    Mutex::Autolock l(mInputMutex);
    return mCallbackStreamId;
}

void CallbackProcessor::dump(int fd, const Vector<String16>& args) const {
}

bool CallbackProcessor::threadLoop() {
    status_t res;

    {
        Mutex::Autolock l(mInputMutex);
        while (!mCallbackAvailable) {
            res = mCallbackAvailableSignal.waitRelative(mInputMutex,
                    kWaitDuration);
            if (res == TIMED_OUT) return true;
        }
        mCallbackAvailable = false;
    }

    do {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return false;
        res = processNewCallback(client);
    } while (res == OK);

    return true;
}

status_t CallbackProcessor::processNewCallback(sp<Camera2Client> &client) {
    ATRACE_CALL();
    status_t res;

    int callbackHeapId;
    sp<Camera2Heap> callbackHeap;
    size_t heapIdx;

    CpuConsumer::LockedBuffer imgBuffer;
    ALOGV("%s: Getting buffer", __FUNCTION__);
    res = mCallbackConsumer->lockNextBuffer(&imgBuffer);
    if (res != OK) {
        if (res != BAD_VALUE) {
            ALOGE("%s: Camera %d: Error receiving next callback buffer: "
                    "%s (%d)", __FUNCTION__, client->getCameraId(), strerror(-res), res);
        }
        return res;
    }
    ALOGV("%s: Camera %d: Preview callback available", __FUNCTION__,
            client->getCameraId());

    {
        SharedParameters::Lock l(client->getParameters());

        if ( l.mParameters.state != Parameters::PREVIEW
                && l.mParameters.state != Parameters::RECORD
                && l.mParameters.state != Parameters::VIDEO_SNAPSHOT) {
            ALOGV("%s: Camera %d: No longer streaming",
                    __FUNCTION__, client->getCameraId());
            mCallbackConsumer->unlockBuffer(imgBuffer);
            return OK;
        }

        if (! (l.mParameters.previewCallbackFlags &
                CAMERA_FRAME_CALLBACK_FLAG_ENABLE_MASK) ) {
            ALOGV("%s: No longer enabled, dropping", __FUNCTION__);
            mCallbackConsumer->unlockBuffer(imgBuffer);
            return OK;
        }
        if ((l.mParameters.previewCallbackFlags &
                        CAMERA_FRAME_CALLBACK_FLAG_ONE_SHOT_MASK) &&
                !l.mParameters.previewCallbackOneShot) {
            ALOGV("%s: One shot mode, already sent, dropping", __FUNCTION__);
            mCallbackConsumer->unlockBuffer(imgBuffer);
            return OK;
        }

        if (imgBuffer.format != l.mParameters.previewFormat) {
            ALOGE("%s: Camera %d: Unexpected format for callback: "
                    "%x, expected %x", __FUNCTION__, client->getCameraId(),
                    imgBuffer.format, l.mParameters.previewFormat);
            mCallbackConsumer->unlockBuffer(imgBuffer);
            return INVALID_OPERATION;
        }

        // In one-shot mode, stop sending callbacks after the first one
        if (l.mParameters.previewCallbackFlags &
                CAMERA_FRAME_CALLBACK_FLAG_ONE_SHOT_MASK) {
            ALOGV("%s: clearing oneshot", __FUNCTION__);
            l.mParameters.previewCallbackOneShot = false;
        }
    }

    size_t bufferSize = Camera2Client::calculateBufferSize(
            imgBuffer.width, imgBuffer.height,
            imgBuffer.format, imgBuffer.stride);
    size_t currentBufferSize = (mCallbackHeap == 0) ?
            0 : (mCallbackHeap->mHeap->getSize() / kCallbackHeapCount);
    if (bufferSize != currentBufferSize) {
        mCallbackHeap.clear();
        mCallbackHeap = new Camera2Heap(bufferSize, kCallbackHeapCount,
                "Camera2Client::CallbackHeap");
        if (mCallbackHeap->mHeap->getSize() == 0) {
            ALOGE("%s: Camera %d: Unable to allocate memory for callbacks",
                    __FUNCTION__, client->getCameraId());
            mCallbackConsumer->unlockBuffer(imgBuffer);
            return INVALID_OPERATION;
        }

        mCallbackHeapHead = 0;
        mCallbackHeapFree = kCallbackHeapCount;
    }

    if (mCallbackHeapFree == 0) {
        ALOGE("%s: Camera %d: No free callback buffers, dropping frame",
                __FUNCTION__, client->getCameraId());
        mCallbackConsumer->unlockBuffer(imgBuffer);
        return OK;
    }

    heapIdx = mCallbackHeapHead;

    mCallbackHeapHead = (mCallbackHeapHead + 1) & kCallbackHeapCount;
    mCallbackHeapFree--;

    // TODO: Get rid of this memcpy by passing the gralloc queue all the way
    // to app

    ssize_t offset;
    size_t size;
    sp<IMemoryHeap> heap =
            mCallbackHeap->mBuffers[heapIdx]->getMemory(&offset,
                    &size);
    uint8_t *data = (uint8_t*)heap->getBase() + offset;
    memcpy(data, imgBuffer.data, bufferSize);

    ALOGV("%s: Freeing buffer", __FUNCTION__);
    mCallbackConsumer->unlockBuffer(imgBuffer);

    // Call outside parameter lock to allow re-entrancy from notification
    {
        Camera2Client::SharedCameraClient::Lock l(client->mSharedCameraClient);
        if (l.mCameraClient != 0) {
            ALOGV("%s: Camera %d: Invoking client data callback",
                    __FUNCTION__, client->getCameraId());
            l.mCameraClient->dataCallback(CAMERA_MSG_PREVIEW_FRAME,
                    mCallbackHeap->mBuffers[heapIdx], NULL);
        }
    }

    // Only increment free if we're still using the same heap
    mCallbackHeapFree++;

    ALOGV("%s: exit", __FUNCTION__);

    return OK;
}

}; // namespace camera2
}; // namespace android
