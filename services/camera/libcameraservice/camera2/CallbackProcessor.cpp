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
#include <gui/Surface.h>
#include "../CameraDeviceBase.h"
#include "../Camera2Client.h"

#define ALIGN(x, mask) ( ((x) + (mask) - 1) & ~((mask) - 1) )

namespace android {
namespace camera2 {

CallbackProcessor::CallbackProcessor(sp<Camera2Client> client):
        Thread(false),
        mClient(client),
        mDevice(client->getCameraDevice()),
        mId(client->getCameraId()),
        mCallbackAvailable(false),
        mCallbackToApp(false),
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

status_t CallbackProcessor::setCallbackWindow(
        sp<ANativeWindow> callbackWindow) {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock l(mInputMutex);

    sp<Camera2Client> client = mClient.promote();
    if (client == 0) return OK;
    sp<CameraDeviceBase> device = client->getCameraDevice();

    // If the window is changing, clear out stream if it already exists
    if (mCallbackWindow != callbackWindow && mCallbackStreamId != NO_STREAM) {
        res = device->deleteStream(mCallbackStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to delete old stream "
                    "for callbacks: %s (%d)", __FUNCTION__,
                    client->getCameraId(), strerror(-res), res);
            return res;
        }
        mCallbackStreamId = NO_STREAM;
        mCallbackConsumer.clear();
    }
    mCallbackWindow = callbackWindow;
    mCallbackToApp = (mCallbackWindow != NULL);

    return OK;
}

status_t CallbackProcessor::updateStream(const Parameters &params) {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock l(mInputMutex);

    sp<CameraDeviceBase> device = mDevice.promote();
    if (device == 0) {
        ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    // If possible, use the flexible YUV format
    int32_t callbackFormat = params.previewFormat;
    if (mCallbackToApp) {
        // TODO: etalvala: This should use the flexible YUV format as well, but
        // need to reconcile HAL2/HAL3 requirements.
        callbackFormat = HAL_PIXEL_FORMAT_YV12;
    } else if(params.fastInfo.useFlexibleYuv &&
            (params.previewFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP ||
             params.previewFormat == HAL_PIXEL_FORMAT_YV12) ) {
        callbackFormat = HAL_PIXEL_FORMAT_YCbCr_420_888;
    }

    if (!mCallbackToApp && mCallbackConsumer == 0) {
        // Create CPU buffer queue endpoint, since app hasn't given us one
        mCallbackConsumer = new CpuConsumer(kCallbackHeapCount);
        mCallbackConsumer->setFrameAvailableListener(this);
        mCallbackConsumer->setName(String8("Camera2Client::CallbackConsumer"));
        mCallbackWindow = new Surface(
            mCallbackConsumer->getProducerInterface());
    }

    if (mCallbackStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight, currentFormat;
        res = device->getStreamInfo(mCallbackStreamId,
                &currentWidth, &currentHeight, &currentFormat);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying callback output stream info: "
                    "%s (%d)", __FUNCTION__, mId,
                    strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.previewWidth ||
                currentHeight != (uint32_t)params.previewHeight ||
                currentFormat != (uint32_t)callbackFormat) {
            // Since size should only change while preview is not running,
            // assuming that all existing use of old callback stream is
            // completed.
            ALOGV("%s: Camera %d: Deleting stream %d since the buffer "
                    "parameters changed", __FUNCTION__, mId, mCallbackStreamId);
            res = device->deleteStream(mCallbackStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for callbacks: %s (%d)", __FUNCTION__,
                        mId, strerror(-res), res);
                return res;
            }
            mCallbackStreamId = NO_STREAM;
        }
    }

    if (mCallbackStreamId == NO_STREAM) {
        ALOGV("Creating callback stream: %d x %d, format 0x%x, API format 0x%x",
                params.previewWidth, params.previewHeight,
                callbackFormat, params.previewFormat);
        res = device->createStream(mCallbackWindow,
                params.previewWidth, params.previewHeight,
                callbackFormat, 0, &mCallbackStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create output stream for callbacks: "
                    "%s (%d)", __FUNCTION__, mId,
                    strerror(-res), res);
            return res;
        }
    }

    return OK;
}

status_t CallbackProcessor::deleteStream() {
    ATRACE_CALL();
    sp<CameraDeviceBase> device;

    {
        Mutex::Autolock l(mInputMutex);

        if (mCallbackStreamId == NO_STREAM) {
            return OK;
        }
        device = mDevice.promote();
        if (device == 0) {
            ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
            return INVALID_OPERATION;
        }
    }
    device->deleteStream(mCallbackStreamId);

    {
        Mutex::Autolock l(mInputMutex);

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

void CallbackProcessor::dump(int /*fd*/, const Vector<String16>& /*args*/) const {
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
        if (client == 0) {
            res = discardNewCallback();
        } else {
            res = processNewCallback(client);
        }
    } while (res == OK);

    return true;
}

status_t CallbackProcessor::discardNewCallback() {
    ATRACE_CALL();
    status_t res;
    CpuConsumer::LockedBuffer imgBuffer;
    res = mCallbackConsumer->lockNextBuffer(&imgBuffer);
    if (res != OK) {
        if (res != BAD_VALUE) {
            ALOGE("%s: Camera %d: Error receiving next callback buffer: "
                    "%s (%d)", __FUNCTION__, mId, strerror(-res), res);
        }
        return res;
    }
    mCallbackConsumer->unlockBuffer(imgBuffer);
    return OK;
}

status_t CallbackProcessor::processNewCallback(sp<Camera2Client> &client) {
    ATRACE_CALL();
    status_t res;

    sp<Camera2Heap> callbackHeap;
    size_t heapIdx;

    CpuConsumer::LockedBuffer imgBuffer;
    ALOGV("%s: Getting buffer", __FUNCTION__);
    res = mCallbackConsumer->lockNextBuffer(&imgBuffer);
    if (res != OK) {
        if (res != BAD_VALUE) {
            ALOGE("%s: Camera %d: Error receiving next callback buffer: "
                    "%s (%d)", __FUNCTION__, mId, strerror(-res), res);
        }
        return res;
    }
    ALOGV("%s: Camera %d: Preview callback available", __FUNCTION__,
            mId);

    bool useFlexibleYuv = false;
    int32_t previewFormat = 0;
    {
        SharedParameters::Lock l(client->getParameters());

        if ( l.mParameters.state != Parameters::PREVIEW
                && l.mParameters.state != Parameters::RECORD
                && l.mParameters.state != Parameters::VIDEO_SNAPSHOT) {
            ALOGV("%s: Camera %d: No longer streaming",
                    __FUNCTION__, mId);
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

        previewFormat = l.mParameters.previewFormat;
        useFlexibleYuv = l.mParameters.fastInfo.useFlexibleYuv &&
                (previewFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP ||
                 previewFormat == HAL_PIXEL_FORMAT_YV12);

        int32_t expectedFormat = useFlexibleYuv ?
                HAL_PIXEL_FORMAT_YCbCr_420_888 : previewFormat;

        if (imgBuffer.format != expectedFormat) {
            ALOGE("%s: Camera %d: Unexpected format for callback: "
                    "0x%x, expected 0x%x", __FUNCTION__, mId,
                    imgBuffer.format, expectedFormat);
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

    uint32_t destYStride = 0;
    uint32_t destCStride = 0;
    if (useFlexibleYuv) {
        if (previewFormat == HAL_PIXEL_FORMAT_YV12) {
            // Strides must align to 16 for YV12
            destYStride = ALIGN(imgBuffer.width, 16);
            destCStride = ALIGN(destYStride / 2, 16);
        } else {
            // No padding for NV21
            ALOG_ASSERT(previewFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP,
                    "Unexpected preview format 0x%x", previewFormat);
            destYStride = imgBuffer.width;
            destCStride = destYStride / 2;
        }
    } else {
        destYStride = imgBuffer.stride;
        // don't care about cStride
    }

    size_t bufferSize = Camera2Client::calculateBufferSize(
            imgBuffer.width, imgBuffer.height,
            previewFormat, destYStride);
    size_t currentBufferSize = (mCallbackHeap == 0) ?
            0 : (mCallbackHeap->mHeap->getSize() / kCallbackHeapCount);
    if (bufferSize != currentBufferSize) {
        mCallbackHeap.clear();
        mCallbackHeap = new Camera2Heap(bufferSize, kCallbackHeapCount,
                "Camera2Client::CallbackHeap");
        if (mCallbackHeap->mHeap->getSize() == 0) {
            ALOGE("%s: Camera %d: Unable to allocate memory for callbacks",
                    __FUNCTION__, mId);
            mCallbackConsumer->unlockBuffer(imgBuffer);
            return INVALID_OPERATION;
        }

        mCallbackHeapHead = 0;
        mCallbackHeapFree = kCallbackHeapCount;
    }

    if (mCallbackHeapFree == 0) {
        ALOGE("%s: Camera %d: No free callback buffers, dropping frame",
                __FUNCTION__, mId);
        mCallbackConsumer->unlockBuffer(imgBuffer);
        return OK;
    }

    heapIdx = mCallbackHeapHead;

    mCallbackHeapHead = (mCallbackHeapHead + 1) & kCallbackHeapCount;
    mCallbackHeapFree--;

    // TODO: Get rid of this copy by passing the gralloc queue all the way
    // to app

    ssize_t offset;
    size_t size;
    sp<IMemoryHeap> heap =
            mCallbackHeap->mBuffers[heapIdx]->getMemory(&offset,
                    &size);
    uint8_t *data = (uint8_t*)heap->getBase() + offset;

    if (!useFlexibleYuv) {
        // Can just memcpy when HAL format matches API format
        memcpy(data, imgBuffer.data, bufferSize);
    } else {
        res = convertFromFlexibleYuv(previewFormat, data, imgBuffer,
                destYStride, destCStride);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't convert between 0x%x and 0x%x formats!",
                    __FUNCTION__, mId, imgBuffer.format, previewFormat);
            mCallbackConsumer->unlockBuffer(imgBuffer);
            return BAD_VALUE;
        }
    }

    ALOGV("%s: Freeing buffer", __FUNCTION__);
    mCallbackConsumer->unlockBuffer(imgBuffer);

    // Call outside parameter lock to allow re-entrancy from notification
    {
        Camera2Client::SharedCameraCallbacks::Lock
            l(client->mSharedCameraCallbacks);
        if (l.mRemoteCallback != 0) {
            ALOGV("%s: Camera %d: Invoking client data callback",
                    __FUNCTION__, mId);
            l.mRemoteCallback->dataCallback(CAMERA_MSG_PREVIEW_FRAME,
                    mCallbackHeap->mBuffers[heapIdx], NULL);
        }
    }

    // Only increment free if we're still using the same heap
    mCallbackHeapFree++;

    ALOGV("%s: exit", __FUNCTION__);

    return OK;
}

status_t CallbackProcessor::convertFromFlexibleYuv(int32_t previewFormat,
        uint8_t *dst,
        const CpuConsumer::LockedBuffer &src,
        uint32_t dstYStride,
        uint32_t dstCStride) const {

    if (previewFormat != HAL_PIXEL_FORMAT_YCrCb_420_SP &&
            previewFormat != HAL_PIXEL_FORMAT_YV12) {
        ALOGE("%s: Camera %d: Unexpected preview format when using "
                "flexible YUV: 0x%x", __FUNCTION__, mId, previewFormat);
        return INVALID_OPERATION;
    }

    // Copy Y plane, adjusting for stride
    const uint8_t *ySrc = src.data;
    uint8_t *yDst = dst;
    for (size_t row = 0; row < src.height; row++) {
        memcpy(yDst, ySrc, src.width);
        ySrc += src.stride;
        yDst += dstYStride;
    }

    // Copy/swizzle chroma planes, 4:2:0 subsampling
    const uint8_t *uSrc = src.dataCb;
    const uint8_t *vSrc = src.dataCr;
    size_t chromaHeight = src.height / 2;
    size_t chromaWidth = src.width / 2;
    ssize_t chromaGap = src.chromaStride -
            (chromaWidth * src.chromaStep);
    size_t dstChromaGap = dstCStride - chromaWidth;

    if (previewFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP) {
        // NV21
        uint8_t *vuDst = yDst;
        for (size_t row = 0; row < chromaHeight; row++) {
            for (size_t col = 0; col < chromaWidth; col++) {
                *(vuDst++) = *vSrc;
                *(vuDst++) = *uSrc;
                vSrc += src.chromaStep;
                uSrc += src.chromaStep;
            }
            vSrc += chromaGap;
            uSrc += chromaGap;
        }
    } else {
        // YV12
        ALOG_ASSERT(previewFormat == HAL_PIXEL_FORMAT_YV12,
                "Unexpected preview format 0x%x", previewFormat);
        uint8_t *vDst = yDst;
        uint8_t *uDst = yDst + chromaHeight * dstCStride;
        for (size_t row = 0; row < chromaHeight; row++) {
            for (size_t col = 0; col < chromaWidth; col++) {
                *(vDst++) = *vSrc;
                *(uDst++) = *uSrc;
                vSrc += src.chromaStep;
                uSrc += src.chromaStep;
            }
            vSrc += chromaGap;
            uSrc += chromaGap;
            vDst += dstChromaGap;
            uDst += dstChromaGap;
        }
    }

    return OK;
}

}; // namespace camera2
}; // namespace android
