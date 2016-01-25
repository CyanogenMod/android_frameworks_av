/*
 * Copyright 2016 The Android Open Source Project
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
#define LOG_TAG "Camera3-BufferManager"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include <gui/ISurfaceComposer.h>
#include <private/gui/ComposerService.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include "utils/CameraTraces.h"
#include "Camera3BufferManager.h"

namespace android {

namespace camera3 {

Camera3BufferManager::Camera3BufferManager(const sp<IGraphicBufferAlloc>& allocator) :
        mAllocator(allocator) {
    if (allocator == NULL) {
        sp<ISurfaceComposer> composer(ComposerService::getComposerService());
        mAllocator = composer->createGraphicBufferAlloc();
        if (mAllocator == NULL) {
            ALOGE("createGraphicBufferAlloc failed");
        }
    }
}

Camera3BufferManager::~Camera3BufferManager() {
}

status_t Camera3BufferManager::registerStream(const StreamInfo& streamInfo) {
    ATRACE_CALL();

    int streamId = streamInfo.streamId;
    int streamSetId = streamInfo.streamSetId;

    if (streamId == CAMERA3_STREAM_ID_INVALID || streamSetId == CAMERA3_STREAM_SET_ID_INVALID) {
        ALOGE("%s: Stream id (%d) or stream set id (%d) is invalid",
                __FUNCTION__, streamId, streamSetId);
        return BAD_VALUE;
    }
    if (streamInfo.totalBufferCount > kMaxBufferCount || streamInfo.totalBufferCount == 0) {
        ALOGE("%s: Stream id (%d) with stream set id (%d) total buffer count %zu is invalid",
                __FUNCTION__, streamId, streamSetId, streamInfo.totalBufferCount);
        return BAD_VALUE;
    }
    if (!streamInfo.isConfigured) {
        ALOGE("%s: Stream (%d) is not configured", __FUNCTION__, streamId);
        return BAD_VALUE;
    }

    // For Gralloc v1, try to allocate a buffer and see if it is successful, otherwise, stream
    // buffer sharing for this newly added stream is not supported. For Gralloc v0, we don't
    // need check this, as the buffers are not really shared between streams, the buffers are
    // allocated for each stream individually, the allocation failure will be checked in
    // getBufferForStream() call.
    if (mGrallocVersion > HARDWARE_DEVICE_API_VERSION(0,1)) {
        // TODO: To be implemented.

        // In case allocation fails, return invalid operation
        return INVALID_OPERATION;
    }

    Mutex::Autolock l(mLock);
    if (mAllocator == NULL) {
        ALOGE("%s: allocator is NULL, buffer manager is bad state.", __FUNCTION__);
        return INVALID_OPERATION;
    }

    // Check if this stream was registered with different stream set ID, if so, error out.
    for (size_t i = 0; i < mStreamSetMap.size(); i++) {
        ssize_t streamIdx = mStreamSetMap[i].streamInfoMap.indexOfKey(streamId);
        if (streamIdx != NAME_NOT_FOUND &&
            mStreamSetMap[i].streamInfoMap[streamIdx].streamSetId != streamInfo.streamSetId) {
            ALOGE("%s: It is illegal to register the same stream id with different stream set",
                    __FUNCTION__);
            return BAD_VALUE;
        }
    }
    // Check if there is an existing stream set registered; if not, create one; otherwise, add this
    // stream info to the existing stream set entry.
    ssize_t setIdx = mStreamSetMap.indexOfKey(streamSetId);
    if (setIdx == NAME_NOT_FOUND) {
        ALOGV("%s: stream set %d is not registered to stream set map yet, create it.",
                __FUNCTION__, streamSetId);
        // Create stream info map, then add to mStreamsetMap.
        StreamSet newStreamSet;
        setIdx = mStreamSetMap.add(streamSetId, newStreamSet);
    }
    // Update stream set map and water mark.
    StreamSet& currentStreamSet = mStreamSetMap.editValueAt(setIdx);
    ssize_t streamIdx = currentStreamSet.streamInfoMap.indexOfKey(streamId);
    if (streamIdx != NAME_NOT_FOUND) {
        ALOGW("%s: stream %d was already registered with stream set %d",
                __FUNCTION__, streamId, streamSetId);
        return OK;
    }
    currentStreamSet.streamInfoMap.add(streamId, streamInfo);
    currentStreamSet.handoutBufferCountMap.add(streamId, 0);

    // The watermark should be the max of buffer count of each stream inside a stream set.
    if (streamInfo.totalBufferCount > currentStreamSet.allocatedBufferWaterMark) {
       currentStreamSet.allocatedBufferWaterMark = streamInfo.totalBufferCount;
    }

    return OK;
}

status_t Camera3BufferManager::unregisterStream(int streamId, int streamSetId) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    ALOGV("%s: unregister stream %d with stream set %d", __FUNCTION__,
            streamId, streamSetId);
    if (mAllocator == NULL) {
        ALOGE("%s: allocator is NULL, buffer manager is bad state.", __FUNCTION__);
        return INVALID_OPERATION;
    }

    if (!checkIfStreamRegisteredLocked(streamId, streamSetId)){
        ALOGE("%s: stream %d with set id %d wasn't properly registered to this buffer manager!",
                __FUNCTION__, streamId, streamSetId);
        return BAD_VALUE;
    }

    // De-list all the buffers associated with this stream first.
    StreamSet& currentSet = mStreamSetMap.editValueFor(streamSetId);
    BufferList& freeBufs = currentSet.freeBuffers;
    BufferCountMap& handOutBufferCounts = currentSet.handoutBufferCountMap;
    InfoMap& infoMap = currentSet.streamInfoMap;
    removeBuffersFromBufferListLocked(freeBufs, streamId);
    handOutBufferCounts.removeItem(streamId);

    // Remove the stream info from info map and recalculate the buffer count water mark.
    infoMap.removeItem(streamId);
    currentSet.allocatedBufferWaterMark = 0;
    for (size_t i = 0; i < infoMap.size(); i++) {
        if (infoMap[i].totalBufferCount > currentSet.allocatedBufferWaterMark) {
            currentSet.allocatedBufferWaterMark = infoMap[i].totalBufferCount;
        }
    }

    // Remove this stream set if all its streams have been removed.
    if (freeBufs.size() == 0 && handOutBufferCounts.size() == 0 && infoMap.size() == 0) {
        mStreamSetMap.removeItem(streamSetId);
    }

    return OK;
}

status_t Camera3BufferManager::getBufferForStream(int streamId, int streamSetId,
        sp<GraphicBuffer>* gb, int* fenceFd) {
    ATRACE_CALL();

    Mutex::Autolock l(mLock);
    ALOGV("%s: get buffer for stream %d with stream set %d", __FUNCTION__,
            streamId, streamSetId);
    if (mAllocator == NULL) {
        ALOGE("%s: allocator is NULL, buffer manager is bad state.", __FUNCTION__);
        return INVALID_OPERATION;
    }

    if (!checkIfStreamRegisteredLocked(streamId, streamSetId)) {
        ALOGE("%s: stream %d is not registered with stream set %d yet!!!",
                __FUNCTION__, streamId, streamSetId);
        return BAD_VALUE;
    }

    StreamSet &streamSet = mStreamSetMap.editValueFor(streamSetId);
    GraphicBufferEntry buffer =
            getFirstBufferFromBufferListLocked(streamSet.freeBuffers, streamId);

    if (mGrallocVersion < HARDWARE_DEVICE_API_VERSION(1,0)) {
        // Allocate one if there is no free buffer available.
        if (buffer.graphicBuffer == nullptr) {
            const StreamInfo& info = streamSet.streamInfoMap.valueFor(streamId);
            status_t res = OK;
            buffer.fenceFd = -1;
            buffer.graphicBuffer = mAllocator->createGraphicBuffer(
                    info.width, info.height, info.format, info.combinedUsage, &res);
            ALOGV("%s: allocate a new graphic buffer %p with handle %p",
                    __FUNCTION__, buffer.graphicBuffer.get(), buffer.graphicBuffer->handle);
            if (res != OK) {
                ALOGE("%s: graphic buffer allocation failed: (error %d %s) ",
                        __FUNCTION__, res, strerror(-res));
                return res;
            }
        }

        // Increase the hand-out buffer count for tracking purpose.
        BufferCountMap& handOutBufferCounts = streamSet.handoutBufferCountMap;
        size_t& bufferCount = handOutBufferCounts.editValueFor(streamId);
        bufferCount++;

        *gb = buffer.graphicBuffer;
        *fenceFd = buffer.fenceFd;
        ALOGV("%s: get buffer (%p) with handle (%p).",
                __FUNCTION__, buffer.graphicBuffer.get(), buffer.graphicBuffer->handle);

        // Proactively free buffers for other streams if the current number of allocated buffers
        // exceeds the water mark. This only for Gralloc V1, for V2, this logic can also be handled
        // in returnBufferForStream() if we want to free buffer more quickly.
        // TODO: probably should find out all the inactive stream IDs, and free the firstly found
        // buffers for them.
        StreamId firstOtherStreamId = CAMERA3_STREAM_ID_INVALID;
        if (streamSet.streamInfoMap.size() > 1) {
            for (size_t i = 0; i < streamSet.streamInfoMap.size(); i++) {
                firstOtherStreamId = streamSet.streamInfoMap[i].streamId;
                if (firstOtherStreamId != streamId) {
                    break;
                }
            }
            if (firstOtherStreamId == CAMERA3_STREAM_ID_INVALID) {
                return OK;
            }

            // This will drop the reference to one free buffer, which will effectively free one
            // buffer (from the free buffer list) for the inactive streams.
            size_t totalAllocatedBufferCount = streamSet.freeBuffers.size();
            for (size_t i = 0; i < streamSet.handoutBufferCountMap.size(); i++) {
                totalAllocatedBufferCount += streamSet.handoutBufferCountMap[i];
            }
            if (totalAllocatedBufferCount > streamSet.allocatedBufferWaterMark) {
                getFirstBufferFromBufferListLocked(streamSet.freeBuffers, firstOtherStreamId);
            }
        }
    } else {
        // TODO: implement this.
        return BAD_VALUE;
    }

    return OK;
}

status_t Camera3BufferManager::returnBufferForStream(int streamId,
        int streamSetId, const sp<GraphicBuffer>& buffer, int fenceFd) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    ALOGV_IF(buffer != 0, "%s: return buffer (%p) with handle (%p) for stream %d and stream set %d",
            __FUNCTION__, buffer.get(), buffer->handle, streamId, streamSetId);
    if (mAllocator == NULL) {
        ALOGE("%s: allocator is NULL, buffer manager is bad state.", __FUNCTION__);
        return INVALID_OPERATION;
    }

    if (!checkIfStreamRegisteredLocked(streamId, streamSetId)){
        ALOGV("%s: returning buffer for an already unregistered stream (stream %d with set id %d),"
                "buffer will be dropped right away!", __FUNCTION__, streamId, streamSetId);
        return OK;
    }

    if (mGrallocVersion < HARDWARE_DEVICE_API_VERSION(1,0)) {
        // Add to the freeBuffer list.
        StreamSet& streamSet = mStreamSetMap.editValueFor(streamSetId);
        if (buffer != 0) {
            BufferEntry entry;
            entry.add(streamId, GraphicBufferEntry(buffer, fenceFd));
            status_t res = addBufferToBufferListLocked(streamSet.freeBuffers, entry);
            if (res != OK) {
                ALOGE("%s: add buffer to free buffer list failed", __FUNCTION__);
                return res;
            }
        }

        // Update the hand-out buffer count for this buffer.
        BufferCountMap& handOutBufferCounts = streamSet.handoutBufferCountMap;
        size_t& bufferCount = handOutBufferCounts.editValueFor(streamId);
        bufferCount--;
    } else {
        // TODO: implement this.
        return BAD_VALUE;
    }

    return OK;
}

void Camera3BufferManager::dump(int fd, const Vector<String16>& args) const {
    Mutex::Autolock l(mLock);

    (void) args;
    String8 lines;
    lines.appendFormat("      Total stream sets: %zu\n", mStreamSetMap.size());
    for (size_t i = 0; i < mStreamSetMap.size(); i++) {
        lines.appendFormat("        Stream set %d has below streams:\n", mStreamSetMap.keyAt(i));
        for (size_t j = 0; j < mStreamSetMap[i].streamInfoMap.size(); j++) {
            lines.appendFormat("          Stream %d\n", mStreamSetMap[i].streamInfoMap[j].streamId);
        }
        lines.appendFormat("          Stream set buffer count water mark: %zu\n",
                mStreamSetMap[i].allocatedBufferWaterMark);
        lines.appendFormat("          Handout buffer counts:\n");
        for (size_t m = 0; m < mStreamSetMap[i].handoutBufferCountMap.size(); m++) {
            int streamId = mStreamSetMap[i].handoutBufferCountMap.keyAt(m);
            size_t bufferCount = mStreamSetMap[i].handoutBufferCountMap.valueAt(m);
            lines.appendFormat("            stream id: %d, buffer count: %zu.\n",
                    streamId, bufferCount);
        }

        lines.appendFormat("          Free buffer count: %zu\n",
                mStreamSetMap[i].freeBuffers.size());
        for (auto& bufEntry : mStreamSetMap[i].freeBuffers) {
            for (size_t m = 0; m < bufEntry.size(); m++) {
                const sp<GraphicBuffer>& buffer = bufEntry.valueAt(m).graphicBuffer;
                int streamId = bufEntry.keyAt(m);
                lines.appendFormat("            stream id: %d, buffer: %p, handle: %p.\n",
                        streamId, buffer.get(), buffer->handle);
            }
        }
    }
    write(fd, lines.string(), lines.size());
}

bool Camera3BufferManager::checkIfStreamRegisteredLocked(int streamId, int streamSetId) const {
    ssize_t setIdx = mStreamSetMap.indexOfKey(streamSetId);
    if (setIdx == NAME_NOT_FOUND) {
        ALOGV("%s: stream set %d is not registered to stream set map yet!",
                __FUNCTION__, streamSetId);
        return false;
    }

    ssize_t streamIdx = mStreamSetMap.valueAt(setIdx).streamInfoMap.indexOfKey(streamId);
    if (streamIdx == NAME_NOT_FOUND) {
        ALOGV("%s: stream %d is not registered to stream info map yet!", __FUNCTION__, streamId);
        return false;
    }

    size_t bufferWaterMark = mStreamSetMap[setIdx].allocatedBufferWaterMark;
    if (bufferWaterMark == 0 || bufferWaterMark > kMaxBufferCount) {
        ALOGW("%s: stream %d with stream set %d is not registered correctly to stream set map,"
                " as the water mark (%zu) is wrong!",
                __FUNCTION__, streamId, streamSetId, bufferWaterMark);
        return false;
    }

    return true;
}

status_t Camera3BufferManager::addBufferToBufferListLocked(BufferList& bufList,
        const BufferEntry& buffer) {
    // TODO: need add some sanity check here.
    bufList.push_back(buffer);

    return OK;
}

status_t Camera3BufferManager::removeBuffersFromBufferListLocked(BufferList& bufferList,
        int streamId) {
    BufferList::iterator i = bufferList.begin();
    while (i != bufferList.end()) {
        ssize_t idx = i->indexOfKey(streamId);
        if (idx != NAME_NOT_FOUND) {
            i->removeItem(streamId);
            if (i->isEmpty()) {
                i = bufferList.erase(i);
            }
            break;
        } else {
            i++;
        }
    }

    ALOGW_IF(i == bufferList.end(), "%s: Unable to find buffers for stream %d",
            __FUNCTION__, streamId);

    return OK;
}

Camera3BufferManager::GraphicBufferEntry Camera3BufferManager::getFirstBufferFromBufferListLocked(
        BufferList& buffers, int streamId) {
    // Try to get the first buffer from the free buffer list if there is one.
    GraphicBufferEntry entry;
    BufferList::iterator i = buffers.begin();
    while (i != buffers.end()) {
        ssize_t idx = i->indexOfKey(streamId);
        if (idx != NAME_NOT_FOUND) {
            entry = GraphicBufferEntry(i->valueAt(idx));
            i = buffers.erase(i);
            break;
        } else {
            i++;
        }
    }

    ALOGV_IF(entry.graphicBuffer == 0, "%s: Unable to find free buffer for stream %d",
            __FUNCTION__, streamId);
    return entry;
}

} // namespace camera3
} // namespace android
