/*
 * Copyright (C) 2014 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaFilter"

#include <inttypes.h>
#include <utils/Trace.h>

#include <binder/MemoryDealer.h>

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>

#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaFilter.h>

#include "IntrinsicBlurFilter.h"
#include "SaturationFilter.h"
#include "ZeroFilter.h"

namespace android {

// parameter: number of input and output buffers
static const size_t kBufferCountActual = 4;

MediaFilter::MediaFilter()
    : mState(UNINITIALIZED),
      mGeneration(0) {
}

MediaFilter::~MediaFilter() {
}

//////////////////// PUBLIC FUNCTIONS //////////////////////////////////////////

void MediaFilter::setNotificationMessage(const sp<AMessage> &msg) {
    mNotify = msg;
}

void MediaFilter::initiateAllocateComponent(const sp<AMessage> &msg) {
    msg->setWhat(kWhatAllocateComponent);
    msg->setTarget(id());
    msg->post();
}

void MediaFilter::initiateConfigureComponent(const sp<AMessage> &msg) {
    msg->setWhat(kWhatConfigureComponent);
    msg->setTarget(id());
    msg->post();
}

void MediaFilter::initiateCreateInputSurface() {
    (new AMessage(kWhatCreateInputSurface, id()))->post();
}

void MediaFilter::initiateStart() {
    (new AMessage(kWhatStart, id()))->post();
}

void MediaFilter::initiateShutdown(bool keepComponentAllocated) {
    sp<AMessage> msg = new AMessage(kWhatShutdown, id());
    msg->setInt32("keepComponentAllocated", keepComponentAllocated);
    msg->post();
}

void MediaFilter::signalFlush() {
    (new AMessage(kWhatFlush, id()))->post();
}

void MediaFilter::signalResume() {
    (new AMessage(kWhatResume, id()))->post();
}

// nothing to do
void MediaFilter::signalRequestIDRFrame() {
    return;
}

void MediaFilter::signalSetParameters(const sp<AMessage> &params) {
    sp<AMessage> msg = new AMessage(kWhatSetParameters, id());
    msg->setMessage("params", params);
    msg->post();
}

void MediaFilter::signalEndOfInputStream() {
    (new AMessage(kWhatSignalEndOfInputStream, id()))->post();
}

void MediaFilter::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatAllocateComponent:
        {
            onAllocateComponent(msg);
            break;
        }
        case kWhatConfigureComponent:
        {
            onConfigureComponent(msg);
            break;
        }
        case kWhatStart:
        {
            onStart();
            break;
        }
        case kWhatProcessBuffers:
        {
            processBuffers();
            break;
        }
        case kWhatInputBufferFilled:
        {
            onInputBufferFilled(msg);
            break;
        }
        case kWhatOutputBufferDrained:
        {
            onOutputBufferDrained(msg);
            break;
        }
        case kWhatShutdown:
        {
            onShutdown(msg);
            break;
        }
        case kWhatFlush:
        {
            onFlush();
            break;
        }
        case kWhatResume:
        {
            // nothing to do
            break;
        }
        case kWhatSetParameters:
        {
            onSetParameters(msg);
            break;
        }
        default:
        {
            ALOGE("Message not handled:\n%s", msg->debugString().c_str());
            break;
        }
    }
}

//////////////////// PORT DESCRIPTION //////////////////////////////////////////

MediaFilter::PortDescription::PortDescription() {
}

void MediaFilter::PortDescription::addBuffer(
        IOMX::buffer_id id, const sp<ABuffer> &buffer) {
    mBufferIDs.push_back(id);
    mBuffers.push_back(buffer);
}

size_t MediaFilter::PortDescription::countBuffers() {
    return mBufferIDs.size();
}

IOMX::buffer_id MediaFilter::PortDescription::bufferIDAt(size_t index) const {
    return mBufferIDs.itemAt(index);
}

sp<ABuffer> MediaFilter::PortDescription::bufferAt(size_t index) const {
    return mBuffers.itemAt(index);
}

//////////////////// HELPER FUNCTIONS //////////////////////////////////////////

void MediaFilter::signalProcessBuffers() {
    (new AMessage(kWhatProcessBuffers, id()))->post();
}

void MediaFilter::signalError(status_t error) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatError);
    notify->setInt32("err", error);
    notify->post();
}

status_t MediaFilter::allocateBuffersOnPort(OMX_U32 portIndex) {
    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);
    const bool isInput = portIndex == kPortIndexInput;
    const size_t bufferSize = isInput ? mMaxInputSize : mMaxOutputSize;

    CHECK(mDealer[portIndex] == NULL);
    CHECK(mBuffers[portIndex].isEmpty());

    ALOGV("Allocating %zu buffers of size %zu on %s port",
            kBufferCountActual, bufferSize,
            isInput ? "input" : "output");

    size_t totalSize = kBufferCountActual * bufferSize;

    mDealer[portIndex] = new MemoryDealer(totalSize, "MediaFilter");

    for (size_t i = 0; i < kBufferCountActual; ++i) {
        sp<IMemory> mem = mDealer[portIndex]->allocate(bufferSize);
        CHECK(mem.get() != NULL);

        BufferInfo info;
        info.mStatus = BufferInfo::OWNED_BY_US;
        info.mBufferID = i;
        info.mGeneration = mGeneration;
        info.mOutputFlags = 0;
        info.mData = new ABuffer(mem->pointer(), bufferSize);
        info.mData->meta()->setInt64("timeUs", 0);

        mBuffers[portIndex].push_back(info);

        if (!isInput) {
            mAvailableOutputBuffers.push(
                    &mBuffers[portIndex].editItemAt(i));
        }
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatBuffersAllocated);

    notify->setInt32("portIndex", portIndex);

    sp<PortDescription> desc = new PortDescription;

    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        const BufferInfo &info = mBuffers[portIndex][i];

        desc->addBuffer(info.mBufferID, info.mData);
    }

    notify->setObject("portDesc", desc);
    notify->post();

    return OK;
}

MediaFilter::BufferInfo* MediaFilter::findBufferByID(
        uint32_t portIndex, IOMX::buffer_id bufferID,
        ssize_t *index) {
    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        BufferInfo *info = &mBuffers[portIndex].editItemAt(i);

        if (info->mBufferID == bufferID) {
            if (index != NULL) {
                *index = i;
            }
            return info;
        }
    }

    TRESPASS();

    return NULL;
}

void MediaFilter::postFillThisBuffer(BufferInfo *info) {
    if (mPortEOS[kPortIndexInput]) {
        return;
    }

    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_US);

    info->mGeneration = mGeneration;

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatFillThisBuffer);
    notify->setInt32("buffer-id", info->mBufferID);

    info->mData->meta()->clear();
    notify->setBuffer("buffer", info->mData);

    sp<AMessage> reply = new AMessage(kWhatInputBufferFilled, id());
    reply->setInt32("buffer-id", info->mBufferID);

    notify->setMessage("reply", reply);

    notify->post();

    info->mStatus = BufferInfo::OWNED_BY_UPSTREAM;
}

void MediaFilter::postDrainThisBuffer(BufferInfo *info) {
    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_US);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatDrainThisBuffer);
    notify->setInt32("buffer-id", info->mBufferID);
    notify->setInt32("flags", info->mOutputFlags);
    notify->setBuffer("buffer", info->mData);

    sp<AMessage> reply = new AMessage(kWhatOutputBufferDrained, id());
    reply->setInt32("buffer-id", info->mBufferID);

    notify->setMessage("reply", reply);

    notify->post();

    info->mStatus = BufferInfo::OWNED_BY_UPSTREAM;
}

void MediaFilter::postEOS() {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatEOS);
    notify->setInt32("err", ERROR_END_OF_STREAM);
    notify->post();

    ALOGV("Sent kWhatEOS.");
}

void MediaFilter::sendFormatChange() {
    sp<AMessage> notify = mNotify->dup();

    notify->setInt32("what", kWhatOutputFormatChanged);

    AString mime;
    CHECK(mOutputFormat->findString("mime", &mime));
    notify->setString("mime", mime.c_str());

    notify->setInt32("stride", mStride);
    notify->setInt32("slice-height", mSliceHeight);
    notify->setInt32("color-format", mColorFormatOut);
    notify->setRect("crop", 0, 0, mStride - 1, mSliceHeight - 1);
    notify->setInt32("width", mWidth);
    notify->setInt32("height", mHeight);

    notify->post();
}

void MediaFilter::requestFillEmptyInput() {
    if (mPortEOS[kPortIndexInput]) {
        return;
    }

    for (size_t i = 0; i < mBuffers[kPortIndexInput].size(); ++i) {
        BufferInfo *info = &mBuffers[kPortIndexInput].editItemAt(i);

        if (info->mStatus == BufferInfo::OWNED_BY_US) {
            postFillThisBuffer(info);
        }
    }
}

void MediaFilter::processBuffers() {
    if (mAvailableInputBuffers.empty() || mAvailableOutputBuffers.empty()) {
        ALOGV("Skipping process (buffers unavailable)");
        return;
    }

    if (mPortEOS[kPortIndexOutput]) {
        // TODO notify caller of queueInput error when it is supported
        // in MediaCodec
        ALOGW("Tried to process a buffer after EOS.");
        return;
    }

    BufferInfo *inputInfo = mAvailableInputBuffers[0];
    mAvailableInputBuffers.removeAt(0);
    BufferInfo *outputInfo = mAvailableOutputBuffers[0];
    mAvailableOutputBuffers.removeAt(0);

    status_t err;
    err = mFilter->processBuffers(inputInfo->mData, outputInfo->mData);
    if (err != (status_t)OK) {
        outputInfo->mData->meta()->setInt32("err", err);
    }

    int64_t timeUs;
    CHECK(inputInfo->mData->meta()->findInt64("timeUs", &timeUs));
    outputInfo->mData->meta()->setInt64("timeUs", timeUs);
    outputInfo->mOutputFlags = 0;
    int32_t eos = 0;
    if (inputInfo->mData->meta()->findInt32("eos", &eos) && eos != 0) {
        outputInfo->mOutputFlags |= OMX_BUFFERFLAG_EOS;
        mPortEOS[kPortIndexOutput] = true;
        outputInfo->mData->meta()->setInt32("eos", eos);
        postEOS();
        ALOGV("Output stream saw EOS.");
    }

    ALOGV("Processed input buffer %u [%zu], output buffer %u [%zu]",
                inputInfo->mBufferID, inputInfo->mData->size(),
                outputInfo->mBufferID, outputInfo->mData->size());

    postFillThisBuffer(inputInfo);
    postDrainThisBuffer(outputInfo);

    // prevent any corner case where buffers could get stuck in queue
    signalProcessBuffers();
}

void MediaFilter::onAllocateComponent(const sp<AMessage> &msg) {
    CHECK_EQ(mState, UNINITIALIZED);

    CHECK(msg->findString("componentName", &mComponentName));
    const char* name = mComponentName.c_str();
    if (!strcasecmp(name, "android.filter.zerofilter")) {
        mFilter = new ZeroFilter;
    } else if (!strcasecmp(name, "android.filter.saturation")) {
        mFilter = new SaturationFilter;
    } else if (!strcasecmp(name, "android.filter.intrinsicblur")) {
        mFilter = new IntrinsicBlurFilter;
    } else {
        ALOGE("Unrecognized filter name: %s", name);
        signalError(NAME_NOT_FOUND);
        return;
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatComponentAllocated);
    // HACK - need "OMX.google" to use MediaCodec's software renderer
    notify->setString("componentName", "OMX.google.MediaFilter");
    notify->post();
    mState = INITIALIZED;
    ALOGV("Handled kWhatAllocateComponent.");
}

void MediaFilter::onConfigureComponent(const sp<AMessage> &msg) {
    // TODO: generalize to allow audio filters as well as video

    CHECK_EQ(mState, INITIALIZED);

    // get params - at least mime, width & height
    AString mime;
    CHECK(msg->findString("mime", &mime));
    if (strcasecmp(mime.c_str(), MEDIA_MIMETYPE_VIDEO_RAW)) {
        ALOGE("Bad mime: %s", mime.c_str());
        signalError(BAD_VALUE);
        return;
    }

    CHECK(msg->findInt32("width", &mWidth));
    CHECK(msg->findInt32("height", &mHeight));
    if (!msg->findInt32("stride", &mStride)) {
        mStride = mWidth;
    }
    if (!msg->findInt32("slice-height", &mSliceHeight)) {
        mSliceHeight = mHeight;
    }

    mMaxInputSize = mWidth * mHeight * 4;   // room for ARGB8888
    int32_t maxInputSize;
    if (msg->findInt32("max-input-size", &maxInputSize)
            && (size_t)maxInputSize > mMaxInputSize) {
        mMaxInputSize = maxInputSize;
    }

    if (!msg->findInt32("color-format", &mColorFormatIn)) {
        // default to OMX_COLOR_Format32bitARGB8888
        mColorFormatIn = OMX_COLOR_Format32bitARGB8888;
    }
    mColorFormatOut = mColorFormatIn;
    mMaxOutputSize = mWidth * mHeight * 4;  // room for ARGB8888

    status_t err;
    err = mFilter->configure(
            mWidth, mHeight, mStride, mSliceHeight, mColorFormatIn);
    if (err != (status_t)OK) {
        ALOGE("Failed to configure filter component, err %d", err);
        signalError(err);
        return;
    }

    mInputFormat = new AMessage();
    mInputFormat->setString("mime", mime.c_str());
    mInputFormat->setInt32("stride", mStride);
    mInputFormat->setInt32("slice-height", mSliceHeight);
    mInputFormat->setInt32("color-format", mColorFormatIn);
    mInputFormat->setRect("crop", 0, 0, mStride, mSliceHeight);
    mInputFormat->setInt32("width", mWidth);
    mInputFormat->setInt32("height", mHeight);

    mOutputFormat = new AMessage();
    mOutputFormat->setString("mime", mime.c_str());
    mOutputFormat->setInt32("stride", mStride);
    mOutputFormat->setInt32("slice-height", mSliceHeight);
    mOutputFormat->setInt32("color-format", mColorFormatOut);
    mOutputFormat->setRect("crop", 0, 0, mStride, mSliceHeight);
    mOutputFormat->setInt32("width", mWidth);
    mOutputFormat->setInt32("height", mHeight);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatComponentConfigured);
    notify->setString("componentName", "MediaFilter");
    notify->setMessage("input-format", mInputFormat);
    notify->setMessage("output-format", mOutputFormat);
    notify->post();
    mState = CONFIGURED;
    ALOGV("Handled kWhatConfigureComponent.");

    sendFormatChange();
}

void MediaFilter::onStart() {
    CHECK_EQ(mState, CONFIGURED);

    allocateBuffersOnPort(kPortIndexInput);

    allocateBuffersOnPort(kPortIndexOutput);

    status_t err = mFilter->start();
    if (err != (status_t)OK) {
        ALOGE("Failed to start filter component, err %d", err);
        signalError(err);
        return;
    }

    mPortEOS[kPortIndexInput] = false;
    mPortEOS[kPortIndexOutput] = false;
    mInputEOSResult = OK;
    mState = STARTED;

    requestFillEmptyInput();
    ALOGV("Handled kWhatStart.");
}

void MediaFilter::onInputBufferFilled(const sp<AMessage> &msg) {
    IOMX::buffer_id bufferID;
    CHECK(msg->findInt32("buffer-id", (int32_t*)&bufferID));
    BufferInfo *info = findBufferByID(kPortIndexInput, bufferID);

    if (mState != STARTED) {
        // we're not running, so we'll just keep that buffer...
        info->mStatus = BufferInfo::OWNED_BY_US;
        return;
    }

    if (info->mGeneration != mGeneration) {
        // buffer is stale (taken before a flush/shutdown) - repost it
        CHECK_EQ(info->mStatus, BufferInfo::OWNED_BY_US);
        postFillThisBuffer(info);
    }

    CHECK_EQ(info->mStatus, BufferInfo::OWNED_BY_UPSTREAM);
    info->mStatus = BufferInfo::OWNED_BY_US;

    sp<ABuffer> buffer;
    int32_t err = OK;
    bool eos = false;

    if (!msg->findBuffer("buffer", &buffer)) {
        // these are unfilled buffers returned by client
        CHECK(msg->findInt32("err", &err));

        if (err == OK) {
            // buffers with no errors are returned on MediaCodec.flush
            ALOGV("saw unfilled buffer (MediaCodec.flush)");
            return;
        } else {
            ALOGV("saw error %d instead of an input buffer", err);
            eos = true;
        }

        buffer.clear();
    }

    int32_t isCSD;
    if (buffer != NULL && buffer->meta()->findInt32("csd", &isCSD)
            && isCSD != 0) {
        // ignore codec-specific data buffers
        ALOGW("MediaFilter received a codec-specific data buffer");
        postFillThisBuffer(info);
        return;
    }

    int32_t tmp;
    if (buffer != NULL && buffer->meta()->findInt32("eos", &tmp) && tmp) {
        eos = true;
        err = ERROR_END_OF_STREAM;
    }

    mAvailableInputBuffers.push_back(info);
    processBuffers();

    if (eos) {
        mPortEOS[kPortIndexInput] = true;
        mInputEOSResult = err;
    }

    ALOGV("Handled kWhatInputBufferFilled. [ID %u]",
            bufferID);
}

void MediaFilter::onOutputBufferDrained(const sp<AMessage> &msg) {
    IOMX::buffer_id bufferID;
    CHECK(msg->findInt32("buffer-id",(int32_t*) &bufferID));
    BufferInfo *info = findBufferByID(kPortIndexOutput, bufferID);

    if (mState != STARTED) {
        // we're not running, so we'll just keep that buffer...
        info->mStatus = BufferInfo::OWNED_BY_US;
        return;
    }

    CHECK_EQ(info->mStatus, BufferInfo::OWNED_BY_UPSTREAM);
    info->mStatus = BufferInfo::OWNED_BY_US;

    mAvailableOutputBuffers.push_back(info);

    processBuffers();

    ALOGV("Handled kWhatOutputBufferDrained. [ID %u]",
            bufferID);
}

void MediaFilter::onShutdown(const sp<AMessage> &msg) {
    mGeneration++;

    if (mState != UNINITIALIZED) {
        mFilter->reset();
    }

    int32_t keepComponentAllocated;
    CHECK(msg->findInt32("keepComponentAllocated", &keepComponentAllocated));
    if (!keepComponentAllocated || mState == UNINITIALIZED) {
        mState = UNINITIALIZED;
    } else {
        mState = INITIALIZED;
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatShutdownCompleted);
    notify->post();
}

void MediaFilter::onFlush() {
    mGeneration++;

    for (size_t i = 0; i < mBuffers[kPortIndexInput].size(); ++i) {
        BufferInfo *info = &mBuffers[kPortIndexInput].editItemAt(i);
        info->mStatus = BufferInfo::OWNED_BY_US;
    }
    for (size_t i = 0; i < mBuffers[kPortIndexOutput].size(); ++i) {
        BufferInfo *info = &mBuffers[kPortIndexOutput].editItemAt(i);
        info->mStatus = BufferInfo::OWNED_BY_US;
    }

    mPortEOS[kPortIndexInput] = false;
    mPortEOS[kPortIndexOutput] = false;
    mInputEOSResult = OK;

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatFlushCompleted);
    notify->post();

    requestFillEmptyInput();
}

void MediaFilter::onSetParameters(const sp<AMessage> &msg) {
    CHECK(mState != STARTED);

    status_t err = mFilter->setParameters(msg);
    if (err != (status_t)OK) {
        ALOGE("setParameters returned err %d", err);
    }
}

}   // namespace android
