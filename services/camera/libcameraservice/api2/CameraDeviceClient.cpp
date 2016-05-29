/*
 * Copyright (C) 2013 The Android Open Source Project
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

#define LOG_TAG "CameraDeviceClient"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <cutils/properties.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <gui/Surface.h>
#include <camera/camera2/CaptureRequest.h>
#include <camera/CameraUtils.h>

#include "common/CameraDeviceBase.h"
#include "api2/CameraDeviceClient.h"

// Convenience methods for constructing binder::Status objects for error returns

#define STATUS_ERROR(errorCode, errorString) \
    binder::Status::fromServiceSpecificError(errorCode, \
            String8::format("%s:%d: %s", __FUNCTION__, __LINE__, errorString))

#define STATUS_ERROR_FMT(errorCode, errorString, ...) \
    binder::Status::fromServiceSpecificError(errorCode, \
            String8::format("%s:%d: " errorString, __FUNCTION__, __LINE__, \
                    __VA_ARGS__))

namespace android {
using namespace camera2;

CameraDeviceClientBase::CameraDeviceClientBase(
        const sp<CameraService>& cameraService,
        const sp<hardware::camera2::ICameraDeviceCallbacks>& remoteCallback,
        const String16& clientPackageName,
        int cameraId,
        int cameraFacing,
        int clientPid,
        uid_t clientUid,
        int servicePid) :
    BasicClient(cameraService,
            IInterface::asBinder(remoteCallback),
            clientPackageName,
            cameraId,
            cameraFacing,
            clientPid,
            clientUid,
            servicePid),
    mRemoteCallback(remoteCallback) {
}

// Interface used by CameraService

CameraDeviceClient::CameraDeviceClient(const sp<CameraService>& cameraService,
        const sp<hardware::camera2::ICameraDeviceCallbacks>& remoteCallback,
        const String16& clientPackageName,
        int cameraId,
        int cameraFacing,
        int clientPid,
        uid_t clientUid,
        int servicePid) :
    Camera2ClientBase(cameraService, remoteCallback, clientPackageName,
                cameraId, cameraFacing, clientPid, clientUid, servicePid),
    mInputStream(),
    mStreamingRequestId(REQUEST_ID_NONE),
    mRequestIdCounter(0) {

    ATRACE_CALL();
    ALOGI("CameraDeviceClient %d: Opened", cameraId);
}

status_t CameraDeviceClient::initialize(CameraModule *module)
{
    ATRACE_CALL();
    status_t res;

    res = Camera2ClientBase::initialize(module);
    if (res != OK) {
        return res;
    }

    String8 threadName;
    mFrameProcessor = new FrameProcessorBase(mDevice);
    threadName = String8::format("CDU-%d-FrameProc", mCameraId);
    mFrameProcessor->run(threadName.string());

    mFrameProcessor->registerListener(FRAME_PROCESSOR_LISTENER_MIN_ID,
                                      FRAME_PROCESSOR_LISTENER_MAX_ID,
                                      /*listener*/this,
                                      /*sendPartials*/true);

    return OK;
}

CameraDeviceClient::~CameraDeviceClient() {
}

binder::Status CameraDeviceClient::submitRequest(
        const hardware::camera2::CaptureRequest& request,
        bool streaming,
        /*out*/
        hardware::camera2::utils::SubmitInfo *submitInfo) {
    std::vector<hardware::camera2::CaptureRequest> requestList = { request };
    return submitRequestList(requestList, streaming, submitInfo);
}

binder::Status CameraDeviceClient::submitRequestList(
        const std::vector<hardware::camera2::CaptureRequest>& requests,
        bool streaming,
        /*out*/
        hardware::camera2::utils::SubmitInfo *submitInfo) {
    ATRACE_CALL();
    ALOGV("%s-start of function. Request list size %zu", __FUNCTION__, requests.size());

    binder::Status res = binder::Status::ok();
    status_t err;
    if ( !(res = checkPidStatus(__FUNCTION__) ).isOk()) {
        return res;
    }

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    if (requests.empty()) {
        ALOGE("%s: Camera %d: Sent null request. Rejecting request.",
              __FUNCTION__, mCameraId);
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, "Empty request list");
    }

    List<const CameraMetadata> metadataRequestList;
    submitInfo->mRequestId = mRequestIdCounter;
    uint32_t loopCounter = 0;

    for (auto&& request: requests) {
        if (request.mIsReprocess) {
            if (!mInputStream.configured) {
                ALOGE("%s: Camera %d: no input stream is configured.", __FUNCTION__, mCameraId);
                return STATUS_ERROR_FMT(CameraService::ERROR_ILLEGAL_ARGUMENT,
                        "No input configured for camera %d but request is for reprocessing",
                        mCameraId);
            } else if (streaming) {
                ALOGE("%s: Camera %d: streaming reprocess requests not supported.", __FUNCTION__,
                        mCameraId);
                return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT,
                        "Repeating reprocess requests not supported");
            }
        }

        CameraMetadata metadata(request.mMetadata);
        if (metadata.isEmpty()) {
            ALOGE("%s: Camera %d: Sent empty metadata packet. Rejecting request.",
                   __FUNCTION__, mCameraId);
            return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT,
                    "Request settings are empty");
        } else if (request.mSurfaceList.isEmpty()) {
            ALOGE("%s: Camera %d: Requests must have at least one surface target. "
                    "Rejecting request.", __FUNCTION__, mCameraId);
            return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT,
                    "Request has no output targets");
        }

        if (!enforceRequestPermissions(metadata)) {
            // Callee logs
            return STATUS_ERROR(CameraService::ERROR_PERMISSION_DENIED,
                    "Caller does not have permission to change restricted controls");
        }

        /**
         * Write in the output stream IDs which we calculate from
         * the capture request's list of surface targets
         */
        Vector<int32_t> outputStreamIds;
        outputStreamIds.setCapacity(request.mSurfaceList.size());
        for (sp<Surface> surface : request.mSurfaceList) {
            if (surface == 0) continue;

            sp<IGraphicBufferProducer> gbp = surface->getIGraphicBufferProducer();
            int idx = mStreamMap.indexOfKey(IInterface::asBinder(gbp));

            // Trying to submit request with surface that wasn't created
            if (idx == NAME_NOT_FOUND) {
                ALOGE("%s: Camera %d: Tried to submit a request with a surface that"
                        " we have not called createStream on",
                        __FUNCTION__, mCameraId);
                return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT,
                        "Request targets Surface that is not part of current capture session");
            }

            int streamId = mStreamMap.valueAt(idx);
            outputStreamIds.push_back(streamId);
            ALOGV("%s: Camera %d: Appending output stream %d to request",
                    __FUNCTION__, mCameraId, streamId);
        }

        metadata.update(ANDROID_REQUEST_OUTPUT_STREAMS, &outputStreamIds[0],
                        outputStreamIds.size());

        if (request.mIsReprocess) {
            metadata.update(ANDROID_REQUEST_INPUT_STREAMS, &mInputStream.id, 1);
        }

        metadata.update(ANDROID_REQUEST_ID, &(submitInfo->mRequestId), /*size*/1);
        loopCounter++; // loopCounter starts from 1
        ALOGV("%s: Camera %d: Creating request with ID %d (%d of %zu)",
              __FUNCTION__, mCameraId, submitInfo->mRequestId, loopCounter, requests.size());

        metadataRequestList.push_back(metadata);
    }
    mRequestIdCounter++;

    if (streaming) {
        err = mDevice->setStreamingRequestList(metadataRequestList, &(submitInfo->mLastFrameNumber));
        if (err != OK) {
            String8 msg = String8::format(
                "Camera %d:  Got error %s (%d) after trying to set streaming request",
                mCameraId, strerror(-err), err);
            ALOGE("%s: %s", __FUNCTION__, msg.string());
            res = STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION,
                    msg.string());
        } else {
            Mutex::Autolock idLock(mStreamingRequestIdLock);
            mStreamingRequestId = submitInfo->mRequestId;
        }
    } else {
        err = mDevice->captureList(metadataRequestList, &(submitInfo->mLastFrameNumber));
        if (err != OK) {
            String8 msg = String8::format(
                "Camera %d: Got error %s (%d) after trying to submit capture request",
                mCameraId, strerror(-err), err);
            ALOGE("%s: %s", __FUNCTION__, msg.string());
            res = STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION,
                    msg.string());
        }
        ALOGV("%s: requestId = %d ", __FUNCTION__, submitInfo->mRequestId);
    }

    ALOGV("%s: Camera %d: End of function", __FUNCTION__, mCameraId);
    return res;
}

binder::Status CameraDeviceClient::cancelRequest(
        int requestId,
        /*out*/
        int64_t* lastFrameNumber) {
    ATRACE_CALL();
    ALOGV("%s, requestId = %d", __FUNCTION__, requestId);

    status_t err;
    binder::Status res;

    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    Mutex::Autolock idLock(mStreamingRequestIdLock);
    if (mStreamingRequestId != requestId) {
        String8 msg = String8::format("Camera %d: Canceling request ID %d doesn't match "
                "current request ID %d", mCameraId, requestId, mStreamingRequestId);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.string());
    }

    err = mDevice->clearStreamingRequest(lastFrameNumber);

    if (err == OK) {
        ALOGV("%s: Camera %d: Successfully cleared streaming request",
              __FUNCTION__, mCameraId);
        mStreamingRequestId = REQUEST_ID_NONE;
    } else {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error clearing streaming request: %s (%d)",
                mCameraId, strerror(-err), err);
    }

    return res;
}

binder::Status CameraDeviceClient::beginConfigure() {
    // TODO: Implement this.
    ALOGV("%s: Not implemented yet.", __FUNCTION__);
    return binder::Status::ok();
}

binder::Status CameraDeviceClient::endConfigure(bool isConstrainedHighSpeed) {
    ALOGV("%s: ending configure (%d input stream, %zu output streams)",
            __FUNCTION__, mInputStream.configured ? 1 : 0, mStreamMap.size());

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    // Sanitize the high speed session against necessary capability bit.
    if (isConstrainedHighSpeed) {
        CameraMetadata staticInfo = mDevice->info();
        camera_metadata_entry_t entry = staticInfo.find(ANDROID_REQUEST_AVAILABLE_CAPABILITIES);
        bool isConstrainedHighSpeedSupported = false;
        for(size_t i = 0; i < entry.count; ++i) {
            uint8_t capability = entry.data.u8[i];
            if (capability == ANDROID_REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO) {
                isConstrainedHighSpeedSupported = true;
                break;
            }
        }
        if (!isConstrainedHighSpeedSupported) {
            String8 msg = String8::format(
                "Camera %d: Try to create a constrained high speed configuration on a device"
                " that doesn't support it.", mCameraId);
            ALOGE("%s: %s", __FUNCTION__, msg.string());
            return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT,
                    msg.string());
        }
    }

    status_t err = mDevice->configureStreams(isConstrainedHighSpeed);
    if (err == BAD_VALUE) {
        String8 msg = String8::format("Camera %d: Unsupported set of inputs/outputs provided",
                mCameraId);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        res = STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.string());
    } else if (err != OK) {
        String8 msg = String8::format("Camera %d: Error configuring streams: %s (%d)",
                mCameraId, strerror(-err), err);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        res = STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.string());
    }

    return res;
}

binder::Status CameraDeviceClient::deleteStream(int streamId) {
    ATRACE_CALL();
    ALOGV("%s (streamId = 0x%x)", __FUNCTION__, streamId);

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    bool isInput = false;
    ssize_t index = NAME_NOT_FOUND;
    ssize_t dIndex = NAME_NOT_FOUND;

    if (mInputStream.configured && mInputStream.id == streamId) {
        isInput = true;
    } else {
        // Guard against trying to delete non-created streams
        for (size_t i = 0; i < mStreamMap.size(); ++i) {
            if (streamId == mStreamMap.valueAt(i)) {
                index = i;
                break;
            }
        }

        if (index == NAME_NOT_FOUND) {
            // See if this stream is one of the deferred streams.
            for (size_t i = 0; i < mDeferredStreams.size(); ++i) {
                if (streamId == mDeferredStreams[i]) {
                    dIndex = i;
                    break;
                }
            }
            if (dIndex == NAME_NOT_FOUND) {
                String8 msg = String8::format("Camera %d: Invalid stream ID (%d) specified, no such"
                        " stream created yet", mCameraId, streamId);
                ALOGW("%s: %s", __FUNCTION__, msg.string());
                return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.string());
            }
        }
    }

    // Also returns BAD_VALUE if stream ID was not valid
    status_t err = mDevice->deleteStream(streamId);

    if (err != OK) {
        String8 msg = String8::format("Camera %d: Unexpected error %s (%d) when deleting stream %d",
                mCameraId, strerror(-err), err, streamId);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        res = STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.string());
    } else {
        if (isInput) {
            mInputStream.configured = false;
        } else if (index != NAME_NOT_FOUND) {
            mStreamMap.removeItemsAt(index);
        } else {
            mDeferredStreams.removeItemsAt(dIndex);
        }
    }

    return res;
}

binder::Status CameraDeviceClient::createStream(
        const hardware::camera2::params::OutputConfiguration &outputConfiguration,
        /*out*/
        int32_t* newStreamId) {
    ATRACE_CALL();

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    sp<IGraphicBufferProducer> bufferProducer = outputConfiguration.getGraphicBufferProducer();
    bool deferredConsumer = bufferProducer == NULL;
    int surfaceType = outputConfiguration.getSurfaceType();
    bool validSurfaceType = ((surfaceType == OutputConfiguration::SURFACE_TYPE_SURFACE_VIEW) ||
            (surfaceType == OutputConfiguration::SURFACE_TYPE_SURFACE_TEXTURE));
    if (deferredConsumer && !validSurfaceType) {
        ALOGE("%s: Target surface is invalid: bufferProducer = %p, surfaceType = %d.",
                __FUNCTION__, bufferProducer.get(), surfaceType);
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, "Target Surface is invalid");
    }

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    int width, height, format;
    int32_t consumerUsage;
    android_dataspace dataSpace;
    status_t err;

    // Create stream for deferred surface case.
    if (deferredConsumer) {
        return createDeferredSurfaceStreamLocked(outputConfiguration, newStreamId);
    }

    // Don't create multiple streams for the same target surface
    {
        ssize_t index = mStreamMap.indexOfKey(IInterface::asBinder(bufferProducer));
        if (index != NAME_NOT_FOUND) {
            String8 msg = String8::format("Camera %d: Surface already has a stream created for it "
                    "(ID %zd)", mCameraId, index);
            ALOGW("%s: %s", __FUNCTION__, msg.string());
            return STATUS_ERROR(CameraService::ERROR_ALREADY_EXISTS, msg.string());
        }
    }

    // HACK b/10949105
    // Query consumer usage bits to set async operation mode for
    // GLConsumer using controlledByApp parameter.
    bool useAsync = false;
    if ((err = bufferProducer->query(NATIVE_WINDOW_CONSUMER_USAGE_BITS,
            &consumerUsage)) != OK) {
        String8 msg = String8::format("Camera %d: Failed to query Surface consumer usage: %s (%d)",
                mCameraId, strerror(-err), err);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.string());
    }
    if (consumerUsage & GraphicBuffer::USAGE_HW_TEXTURE) {
        ALOGW("%s: Camera %d with consumer usage flag: 0x%x: Forcing asynchronous mode for stream",
                __FUNCTION__, mCameraId, consumerUsage);
        useAsync = true;
    }

    int32_t disallowedFlags = GraphicBuffer::USAGE_HW_VIDEO_ENCODER |
                              GRALLOC_USAGE_RENDERSCRIPT;
    int32_t allowedFlags = GraphicBuffer::USAGE_SW_READ_MASK |
                           GraphicBuffer::USAGE_HW_TEXTURE |
                           GraphicBuffer::USAGE_HW_COMPOSER;
    bool flexibleConsumer = (consumerUsage & disallowedFlags) == 0 &&
            (consumerUsage & allowedFlags) != 0;

    sp<IBinder> binder = IInterface::asBinder(bufferProducer);
    sp<Surface> surface = new Surface(bufferProducer, useAsync);
    ANativeWindow *anw = surface.get();

    if ((err = anw->query(anw, NATIVE_WINDOW_WIDTH, &width)) != OK) {
        String8 msg = String8::format("Camera %d: Failed to query Surface width: %s (%d)",
                mCameraId, strerror(-err), err);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.string());
    }
    if ((err = anw->query(anw, NATIVE_WINDOW_HEIGHT, &height)) != OK) {
        String8 msg = String8::format("Camera %d: Failed to query Surface height: %s (%d)",
                mCameraId, strerror(-err), err);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.string());
    }
    if ((err = anw->query(anw, NATIVE_WINDOW_FORMAT, &format)) != OK) {
        String8 msg = String8::format("Camera %d: Failed to query Surface format: %s (%d)",
                mCameraId, strerror(-err), err);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.string());
    }
    if ((err = anw->query(anw, NATIVE_WINDOW_DEFAULT_DATASPACE,
                            reinterpret_cast<int*>(&dataSpace))) != OK) {
        String8 msg = String8::format("Camera %d: Failed to query Surface dataspace: %s (%d)",
                mCameraId, strerror(-err), err);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.string());
    }

    // FIXME: remove this override since the default format should be
    //       IMPLEMENTATION_DEFINED. b/9487482
    if (format >= HAL_PIXEL_FORMAT_RGBA_8888 &&
        format <= HAL_PIXEL_FORMAT_BGRA_8888) {
        ALOGW("%s: Camera %d: Overriding format %#x to IMPLEMENTATION_DEFINED",
              __FUNCTION__, mCameraId, format);
        format = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
    }

    // Round dimensions to the nearest dimensions available for this format
    if (flexibleConsumer && !CameraDeviceClient::roundBufferDimensionNearest(width, height,
            format, dataSpace, mDevice->info(), /*out*/&width, /*out*/&height)) {
        String8 msg = String8::format("Camera %d: No supported stream configurations with "
                "format %#x defined, failed to create output stream", mCameraId, format);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.string());
    }

    int streamId = camera3::CAMERA3_STREAM_ID_INVALID;
    err = mDevice->createStream(surface, width, height, format, dataSpace,
            static_cast<camera3_stream_rotation_t>(outputConfiguration.getRotation()),
            &streamId, outputConfiguration.getSurfaceSetID());

    if (err != OK) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error creating output stream (%d x %d, fmt %x, dataSpace %x): %s (%d)",
                mCameraId, width, height, format, dataSpace, strerror(-err), err);
    } else {
        mStreamMap.add(binder, streamId);

        ALOGV("%s: Camera %d: Successfully created a new stream ID %d for output surface"
                " (%d x %d) with format 0x%x.",
              __FUNCTION__, mCameraId, streamId, width, height, format);

        // Set transform flags to ensure preview to be rotated correctly.
        res = setStreamTransformLocked(streamId);

        *newStreamId = streamId;
    }

    return res;
}

binder::Status CameraDeviceClient::createDeferredSurfaceStreamLocked(
        const hardware::camera2::params::OutputConfiguration &outputConfiguration,
        /*out*/
        int* newStreamId) {
    int width, height, format, surfaceType;
    int32_t consumerUsage;
    android_dataspace dataSpace;
    status_t err;
    binder::Status res;

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    // Infer the surface info for deferred surface stream creation.
    width = outputConfiguration.getWidth();
    height = outputConfiguration.getHeight();
    surfaceType = outputConfiguration.getSurfaceType();
    format = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
    dataSpace = android_dataspace_t::HAL_DATASPACE_UNKNOWN;
    // Hardcode consumer usage flags: SurfaceView--0x900, SurfaceTexture--0x100.
    consumerUsage = GraphicBuffer::USAGE_HW_TEXTURE;
    if (surfaceType == OutputConfiguration::SURFACE_TYPE_SURFACE_VIEW) {
        consumerUsage |= GraphicBuffer::USAGE_HW_COMPOSER;
    }
    int streamId = camera3::CAMERA3_STREAM_ID_INVALID;
    err = mDevice->createStream(/*surface*/nullptr, width, height, format, dataSpace,
            static_cast<camera3_stream_rotation_t>(outputConfiguration.getRotation()),
            &streamId, outputConfiguration.getSurfaceSetID(), consumerUsage);

    if (err != OK) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error creating output stream (%d x %d, fmt %x, dataSpace %x): %s (%d)",
                mCameraId, width, height, format, dataSpace, strerror(-err), err);
    } else {
        // Can not add streamId to mStreamMap here, as the surface is deferred. Add it to
        // a separate list to track. Once the deferred surface is set, this id will be
        // relocated to mStreamMap.
        mDeferredStreams.push_back(streamId);

        ALOGV("%s: Camera %d: Successfully created a new stream ID %d for a deferred surface"
                " (%d x %d) stream with format 0x%x.",
              __FUNCTION__, mCameraId, streamId, width, height, format);

        // Set transform flags to ensure preview to be rotated correctly.
        res = setStreamTransformLocked(streamId);

        *newStreamId = streamId;
    }
    return res;
}

binder::Status CameraDeviceClient::setStreamTransformLocked(int streamId) {
    int32_t transform = 0;
    status_t err;
    binder::Status res;

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    err = getRotationTransformLocked(&transform);
    if (err != OK) {
        // Error logged by getRotationTransformLocked.
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION,
                "Unable to calculate rotation transform for new stream");
    }

    err = mDevice->setStreamTransform(streamId, transform);
    if (err != OK) {
        String8 msg = String8::format("Failed to set stream transform (stream id %d)",
                streamId);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.string());
    }

    return res;
}

binder::Status CameraDeviceClient::createInputStream(
        int width, int height, int format,
        /*out*/
        int32_t* newStreamId) {

    ATRACE_CALL();
    ALOGV("%s (w = %d, h = %d, f = 0x%x)", __FUNCTION__, width, height, format);

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    if (mInputStream.configured) {
        String8 msg = String8::format("Camera %d: Already has an input stream "
                "configured (ID %zd)", mCameraId, mInputStream.id);
        ALOGE("%s: %s", __FUNCTION__, msg.string() );
        return STATUS_ERROR(CameraService::ERROR_ALREADY_EXISTS, msg.string());
    }

    int streamId = -1;
    status_t err = mDevice->createInputStream(width, height, format, &streamId);
    if (err == OK) {
        mInputStream.configured = true;
        mInputStream.width = width;
        mInputStream.height = height;
        mInputStream.format = format;
        mInputStream.id = streamId;

        ALOGV("%s: Camera %d: Successfully created a new input stream ID %d",
                __FUNCTION__, mCameraId, streamId);

        *newStreamId = streamId;
    } else {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error creating new input stream: %s (%d)", mCameraId,
                strerror(-err), err);
    }

    return res;
}

binder::Status CameraDeviceClient::getInputSurface(/*out*/ view::Surface *inputSurface) {

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    if (inputSurface == NULL) {
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, "Null input surface");
    }

    Mutex::Autolock icl(mBinderSerializationLock);
    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }
    sp<IGraphicBufferProducer> producer;
    status_t err = mDevice->getInputBufferProducer(&producer);
    if (err != OK) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error getting input Surface: %s (%d)",
                mCameraId, strerror(-err), err);
    } else {
        inputSurface->name = String16("CameraInput");
        inputSurface->graphicBufferProducer = producer;
    }
    return res;
}

bool CameraDeviceClient::roundBufferDimensionNearest(int32_t width, int32_t height,
        int32_t format, android_dataspace dataSpace, const CameraMetadata& info,
        /*out*/int32_t* outWidth, /*out*/int32_t* outHeight) {

    camera_metadata_ro_entry streamConfigs =
            (dataSpace == HAL_DATASPACE_DEPTH) ?
            info.find(ANDROID_DEPTH_AVAILABLE_DEPTH_STREAM_CONFIGURATIONS) :
            info.find(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);

    int32_t bestWidth = -1;
    int32_t bestHeight = -1;

    // Iterate through listed stream configurations and find the one with the smallest euclidean
    // distance from the given dimensions for the given format.
    for (size_t i = 0; i < streamConfigs.count; i += 4) {
        int32_t fmt = streamConfigs.data.i32[i];
        int32_t w = streamConfigs.data.i32[i + 1];
        int32_t h = streamConfigs.data.i32[i + 2];

        // Ignore input/output type for now
        if (fmt == format) {
            if (w == width && h == height) {
                bestWidth = width;
                bestHeight = height;
                break;
            } else if (w <= ROUNDING_WIDTH_CAP && (bestWidth == -1 ||
                    CameraDeviceClient::euclidDistSquare(w, h, width, height) <
                    CameraDeviceClient::euclidDistSquare(bestWidth, bestHeight, width, height))) {
                bestWidth = w;
                bestHeight = h;
            }
        }
    }

    if (bestWidth == -1) {
        // Return false if no configurations for this format were listed
        return false;
    }

    // Set the outputs to the closet width/height
    if (outWidth != NULL) {
        *outWidth = bestWidth;
    }
    if (outHeight != NULL) {
        *outHeight = bestHeight;
    }

    // Return true if at least one configuration for this format was listed
    return true;
}

int64_t CameraDeviceClient::euclidDistSquare(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int64_t d0 = x0 - x1;
    int64_t d1 = y0 - y1;
    return d0 * d0 + d1 * d1;
}

// Create a request object from a template.
binder::Status CameraDeviceClient::createDefaultRequest(int templateId,
        /*out*/
        hardware::camera2::impl::CameraMetadataNative* request)
{
    ATRACE_CALL();
    ALOGV("%s (templateId = 0x%x)", __FUNCTION__, templateId);

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    CameraMetadata metadata;
    status_t err;
    if ( (err = mDevice->createDefaultRequest(templateId, &metadata) ) == OK &&
        request != NULL) {

        request->swap(metadata);
    } else if (err == BAD_VALUE) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_ILLEGAL_ARGUMENT,
                "Camera %d: Template ID %d is invalid or not supported: %s (%d)",
                mCameraId, templateId, strerror(-err), err);

    } else {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error creating default request for template %d: %s (%d)",
                mCameraId, templateId, strerror(-err), err);
    }
    return res;
}

binder::Status CameraDeviceClient::getCameraInfo(
        /*out*/
        hardware::camera2::impl::CameraMetadataNative* info)
{
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    binder::Status res;

    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    if (info != NULL) {
        *info = mDevice->info(); // static camera metadata
        // TODO: merge with device-specific camera metadata
    }

    return res;
}

binder::Status CameraDeviceClient::waitUntilIdle()
{
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    // FIXME: Also need check repeating burst.
    Mutex::Autolock idLock(mStreamingRequestIdLock);
    if (mStreamingRequestId != REQUEST_ID_NONE) {
        String8 msg = String8::format(
            "Camera %d: Try to waitUntilIdle when there are active streaming requests",
            mCameraId);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.string());
    }
    status_t err = mDevice->waitUntilDrained();
    if (err != OK) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error waiting to drain: %s (%d)",
                mCameraId, strerror(-err), err);
    }
    ALOGV("%s Done", __FUNCTION__);
    return res;
}

binder::Status CameraDeviceClient::flush(
        /*out*/
        int64_t* lastFrameNumber) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    Mutex::Autolock idLock(mStreamingRequestIdLock);
    mStreamingRequestId = REQUEST_ID_NONE;
    status_t err = mDevice->flush(lastFrameNumber);
    if (err != OK) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error flushing device: %s (%d)", mCameraId, strerror(-err), err);
    }
    return res;
}

binder::Status CameraDeviceClient::prepare(int streamId) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    // Guard against trying to prepare non-created streams
    ssize_t index = NAME_NOT_FOUND;
    for (size_t i = 0; i < mStreamMap.size(); ++i) {
        if (streamId == mStreamMap.valueAt(i)) {
            index = i;
            break;
        }
    }

    if (index == NAME_NOT_FOUND) {
        String8 msg = String8::format("Camera %d: Invalid stream ID (%d) specified, no stream "
              "with that ID exists", mCameraId, streamId);
        ALOGW("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.string());
    }

    // Also returns BAD_VALUE if stream ID was not valid, or stream already
    // has been used
    status_t err = mDevice->prepare(streamId);
    if (err == BAD_VALUE) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_ILLEGAL_ARGUMENT,
                "Camera %d: Stream %d has already been used, and cannot be prepared",
                mCameraId, streamId);
    } else if (err != OK) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error preparing stream %d: %s (%d)", mCameraId, streamId,
                strerror(-err), err);
    }
    return res;
}

binder::Status CameraDeviceClient::prepare2(int maxCount, int streamId) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    // Guard against trying to prepare non-created streams
    ssize_t index = NAME_NOT_FOUND;
    for (size_t i = 0; i < mStreamMap.size(); ++i) {
        if (streamId == mStreamMap.valueAt(i)) {
            index = i;
            break;
        }
    }

    if (index == NAME_NOT_FOUND) {
        String8 msg = String8::format("Camera %d: Invalid stream ID (%d) specified, no stream "
              "with that ID exists", mCameraId, streamId);
        ALOGW("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.string());
    }

    if (maxCount <= 0) {
        String8 msg = String8::format("Camera %d: maxCount (%d) must be greater than 0",
                mCameraId, maxCount);
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.string());
    }

    // Also returns BAD_VALUE if stream ID was not valid, or stream already
    // has been used
    status_t err = mDevice->prepare(maxCount, streamId);
    if (err == BAD_VALUE) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_ILLEGAL_ARGUMENT,
                "Camera %d: Stream %d has already been used, and cannot be prepared",
                mCameraId, streamId);
    } else if (err != OK) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error preparing stream %d: %s (%d)", mCameraId, streamId,
                strerror(-err), err);
    }

    return res;
}

binder::Status CameraDeviceClient::tearDown(int streamId) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    // Guard against trying to prepare non-created streams
    ssize_t index = NAME_NOT_FOUND;
    for (size_t i = 0; i < mStreamMap.size(); ++i) {
        if (streamId == mStreamMap.valueAt(i)) {
            index = i;
            break;
        }
    }

    if (index == NAME_NOT_FOUND) {
        String8 msg = String8::format("Camera %d: Invalid stream ID (%d) specified, no stream "
              "with that ID exists", mCameraId, streamId);
        ALOGW("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.string());
    }

    // Also returns BAD_VALUE if stream ID was not valid or if the stream is in
    // use
    status_t err = mDevice->tearDown(streamId);
    if (err == BAD_VALUE) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_ILLEGAL_ARGUMENT,
                "Camera %d: Stream %d is still in use, cannot be torn down",
                mCameraId, streamId);
    } else if (err != OK) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error tearing down stream %d: %s (%d)", mCameraId, streamId,
                strerror(-err), err);
    }

    return res;
}

binder::Status CameraDeviceClient::setDeferredConfiguration(int32_t streamId,
        const hardware::camera2::params::OutputConfiguration &outputConfiguration) {
    ATRACE_CALL();

    binder::Status res;
    if (!(res = checkPidStatus(__FUNCTION__)).isOk()) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    sp<IGraphicBufferProducer> bufferProducer = outputConfiguration.getGraphicBufferProducer();

    // Client code should guarantee that the surface is from SurfaceView or SurfaceTexture.
    if (bufferProducer == NULL) {
        ALOGE("%s: bufferProducer must not be null", __FUNCTION__);
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, "Target Surface is invalid");
    }
    // Check if this stram id is one of the deferred streams
    ssize_t index = NAME_NOT_FOUND;
    for (size_t i = 0; i < mDeferredStreams.size(); i++) {
        if (streamId == mDeferredStreams[i]) {
            index = i;
            break;
        }
    }
    if (index == NAME_NOT_FOUND) {
        String8 msg = String8::format("Camera %d: deferred surface is set to a unknown stream"
                "(ID %d)", mCameraId, streamId);
        ALOGW("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.string());
    }

    if (!mDevice.get()) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED, "Camera device no longer alive");
    }

    // Don't create multiple streams for the same target surface
    {
        ssize_t index = mStreamMap.indexOfKey(IInterface::asBinder(bufferProducer));
        if (index != NAME_NOT_FOUND) {
            String8 msg = String8::format("Camera %d: Surface already has a stream created "
                    " for it (ID %zd)", mCameraId, index);
            ALOGW("%s: %s", __FUNCTION__, msg.string());
            return STATUS_ERROR(CameraService::ERROR_ALREADY_EXISTS, msg.string());
        }
    }

    status_t err;

    // Always set to async, as we know the deferred surface is for preview streaming.
    sp<Surface> consumerSurface = new Surface(bufferProducer, /*useAsync*/true);

    // Finish the deferred stream configuration with the surface.
    err = mDevice->setConsumerSurface(streamId, consumerSurface);
    if (err == OK) {
        sp<IBinder> binder = IInterface::asBinder(bufferProducer);
        mStreamMap.add(binder, streamId);
        mDeferredStreams.removeItemsAt(index);
    } else if (err == NO_INIT) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_ILLEGAL_ARGUMENT,
                "Camera %d: Deferred surface is invalid: %s (%d)",
                mCameraId, strerror(-err), err);
    } else {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %d: Error setting output stream deferred surface: %s (%d)",
                mCameraId, strerror(-err), err);
    }

    return res;
}

status_t CameraDeviceClient::dump(int fd, const Vector<String16>& args) {
    return BasicClient::dump(fd, args);
}

status_t CameraDeviceClient::dumpClient(int fd, const Vector<String16>& args) {
    String8 result;
    result.appendFormat("CameraDeviceClient[%d] (%p) dump:\n",
            mCameraId,
            (getRemoteCallback() != NULL ?
                    IInterface::asBinder(getRemoteCallback()).get() : NULL) );
    result.appendFormat("  Current client UID %u\n", mClientUid);

    result.append("  State:\n");
    result.appendFormat("    Request ID counter: %d\n", mRequestIdCounter);
    if (mInputStream.configured) {
        result.appendFormat("    Current input stream ID: %d\n",
                    mInputStream.id);
    } else {
        result.append("    No input stream configured.\n");
    }
    if (!mStreamMap.isEmpty()) {
        result.append("    Current output stream IDs:\n");
        for (size_t i = 0; i < mStreamMap.size(); i++) {
            result.appendFormat("      Stream %d\n", mStreamMap.valueAt(i));
        }
    } else if (!mDeferredStreams.isEmpty()) {
        result.append("    Current deferred surface output stream IDs:\n");
        for (auto& streamId : mDeferredStreams) {
            result.appendFormat("      Stream %d\n", streamId);
        }
    } else {
        result.append("    No output streams configured.\n");
    }
    write(fd, result.string(), result.size());
    // TODO: print dynamic/request section from most recent requests
    mFrameProcessor->dump(fd, args);

    return dumpDevice(fd, args);
}

void CameraDeviceClient::notifyError(int32_t errorCode,
                                     const CaptureResultExtras& resultExtras) {
    // Thread safe. Don't bother locking.
    sp<hardware::camera2::ICameraDeviceCallbacks> remoteCb = getRemoteCallback();

    if (remoteCb != 0) {
        remoteCb->onDeviceError(errorCode, resultExtras);
    }
}

void CameraDeviceClient::notifyRepeatingRequestError(long lastFrameNumber) {
    sp<hardware::camera2::ICameraDeviceCallbacks> remoteCb = getRemoteCallback();

    if (remoteCb != 0) {
        remoteCb->onRepeatingRequestError(lastFrameNumber);
    }

    Mutex::Autolock idLock(mStreamingRequestIdLock);
    mStreamingRequestId = REQUEST_ID_NONE;
}

void CameraDeviceClient::notifyIdle() {
    // Thread safe. Don't bother locking.
    sp<hardware::camera2::ICameraDeviceCallbacks> remoteCb = getRemoteCallback();

    if (remoteCb != 0) {
        remoteCb->onDeviceIdle();
    }
    Camera2ClientBase::notifyIdle();
}

void CameraDeviceClient::notifyShutter(const CaptureResultExtras& resultExtras,
        nsecs_t timestamp) {
    // Thread safe. Don't bother locking.
    sp<hardware::camera2::ICameraDeviceCallbacks> remoteCb = getRemoteCallback();
    if (remoteCb != 0) {
        remoteCb->onCaptureStarted(resultExtras, timestamp);
    }
    Camera2ClientBase::notifyShutter(resultExtras, timestamp);
}

void CameraDeviceClient::notifyPrepared(int streamId) {
    // Thread safe. Don't bother locking.
    sp<hardware::camera2::ICameraDeviceCallbacks> remoteCb = getRemoteCallback();
    if (remoteCb != 0) {
        remoteCb->onPrepared(streamId);
    }
}

void CameraDeviceClient::detachDevice() {
    if (mDevice == 0) return;

    ALOGV("Camera %d: Stopping processors", mCameraId);

    mFrameProcessor->removeListener(FRAME_PROCESSOR_LISTENER_MIN_ID,
                                    FRAME_PROCESSOR_LISTENER_MAX_ID,
                                    /*listener*/this);
    mFrameProcessor->requestExit();
    ALOGV("Camera %d: Waiting for threads", mCameraId);
    mFrameProcessor->join();
    ALOGV("Camera %d: Disconnecting device", mCameraId);

    // WORKAROUND: HAL refuses to disconnect while there's streams in flight
    {
        mDevice->clearStreamingRequest();

        status_t code;
        if ((code = mDevice->waitUntilDrained()) != OK) {
            ALOGE("%s: waitUntilDrained failed with code 0x%x", __FUNCTION__,
                  code);
        }
    }

    Camera2ClientBase::detachDevice();
}

/** Device-related methods */
void CameraDeviceClient::onResultAvailable(const CaptureResult& result) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    // Thread-safe. No lock necessary.
    sp<hardware::camera2::ICameraDeviceCallbacks> remoteCb = mRemoteCallback;
    if (remoteCb != NULL) {
        remoteCb->onResultReceived(result.mMetadata, result.mResultExtras);
    }
}

binder::Status CameraDeviceClient::checkPidStatus(const char* checkLocation) {
    if (mDisconnected) {
        return STATUS_ERROR(CameraService::ERROR_DISCONNECTED,
                "The camera device has been disconnected");
    }
    status_t res = checkPid(checkLocation);
    return (res == OK) ? binder::Status::ok() :
            STATUS_ERROR(CameraService::ERROR_PERMISSION_DENIED,
                    "Attempt to use camera from a different process than original client");
}

// TODO: move to Camera2ClientBase
bool CameraDeviceClient::enforceRequestPermissions(CameraMetadata& metadata) {

    const int pid = IPCThreadState::self()->getCallingPid();
    const int selfPid = getpid();
    camera_metadata_entry_t entry;

    /**
     * Mixin default important security values
     * - android.led.transmit = defaulted ON
     */
    CameraMetadata staticInfo = mDevice->info();
    entry = staticInfo.find(ANDROID_LED_AVAILABLE_LEDS);
    for(size_t i = 0; i < entry.count; ++i) {
        uint8_t led = entry.data.u8[i];

        switch(led) {
            case ANDROID_LED_AVAILABLE_LEDS_TRANSMIT: {
                uint8_t transmitDefault = ANDROID_LED_TRANSMIT_ON;
                if (!metadata.exists(ANDROID_LED_TRANSMIT)) {
                    metadata.update(ANDROID_LED_TRANSMIT,
                                    &transmitDefault, 1);
                }
                break;
            }
        }
    }

    // We can do anything!
    if (pid == selfPid) {
        return true;
    }

    /**
     * Permission check special fields in the request
     * - android.led.transmit = android.permission.CAMERA_DISABLE_TRANSMIT
     */
    entry = metadata.find(ANDROID_LED_TRANSMIT);
    if (entry.count > 0 && entry.data.u8[0] != ANDROID_LED_TRANSMIT_ON) {
        String16 permissionString =
            String16("android.permission.CAMERA_DISABLE_TRANSMIT_LED");
        if (!checkCallingPermission(permissionString)) {
            const int uid = IPCThreadState::self()->getCallingUid();
            ALOGE("Permission Denial: "
                  "can't disable transmit LED pid=%d, uid=%d", pid, uid);
            return false;
        }
    }

    return true;
}

status_t CameraDeviceClient::getRotationTransformLocked(int32_t* transform) {
    ALOGV("%s: begin", __FUNCTION__);

    const CameraMetadata& staticInfo = mDevice->info();
    return CameraUtils::getRotationTransform(staticInfo, transform);
}

} // namespace android
