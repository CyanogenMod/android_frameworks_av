/*
 * Copyright (C) 2009 The Android Open Source Project
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

#define LOG_TAG "MediaBufferGroup"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>

namespace android {

MediaBufferGroup::MediaBufferGroup(size_t growthLimit) :
    mGrowthLimit(growthLimit) {
}

MediaBufferGroup::~MediaBufferGroup() {
    for (MediaBuffer *buffer : mBuffers) {
        buffer->resolvePendingRelease();
        // If we don't release it, perhaps noone will release it.
        LOG_ALWAYS_FATAL_IF(buffer->refcount() != 0,
                "buffer refcount %p = %d != 0", buffer, buffer->refcount());
        // actually delete it.
        buffer->setObserver(nullptr);
        buffer->release();
    }
}

void MediaBufferGroup::add_buffer(MediaBuffer *buffer) {
    Mutex::Autolock autoLock(mLock);

    buffer->setObserver(this);
    mBuffers.emplace_back(buffer);
    // optionally: mGrowthLimit = max(mGrowthLimit, mBuffers.size());
}

void MediaBufferGroup::gc(size_t freeBuffers) {
    Mutex::Autolock autoLock(mLock);

    size_t freeCount = 0;
    for (auto it = mBuffers.begin(); it != mBuffers.end(); ) {
        (*it)->resolvePendingRelease();
        if ((*it)->isDeadObject()) {
            // The MediaBuffer has been deleted, why is it in the MediaBufferGroup?
            LOG_ALWAYS_FATAL("buffer(%p) has dead object with refcount %d",
                    (*it), (*it)->refcount());
        } else if ((*it)->refcount() == 0 && ++freeCount > freeBuffers) {
            (*it)->setObserver(nullptr);
            (*it)->release();
            it = mBuffers.erase(it);
        } else {
            ++it;
        }
    }
}

status_t MediaBufferGroup::acquire_buffer(
        MediaBuffer **out, bool nonBlocking, size_t requestedSize) {
    Mutex::Autolock autoLock(mLock);
    for (;;) {
        size_t smallest = requestedSize;
        MediaBuffer *buffer = nullptr;
        auto free = mBuffers.end();
        for (auto it = mBuffers.begin(); it != mBuffers.end(); ++it) {
            (*it)->resolvePendingRelease();
            if ((*it)->refcount() == 0) {
                const size_t size = (*it)->size();
                if (size >= requestedSize) {
                    buffer = *it;
                    break;
                }
                if (size < smallest) {
                    smallest = size; // always free the smallest buf
                    free = it;
                }
            }
        }
        if (buffer == nullptr
                && (free != mBuffers.end() || mBuffers.size() < mGrowthLimit)) {
            // We alloc before we free so failure leaves group unchanged.
            const size_t allocateSize = requestedSize < SIZE_MAX / 3 * 2 /* NB: ordering */ ?
                    requestedSize * 3 / 2 : requestedSize;
            buffer = new MediaBuffer(allocateSize);
            if (buffer->data() == nullptr) {
                ALOGE("Allocation failure for size %zu", allocateSize);
                delete buffer; // Invalid alloc, prefer not to call release.
                buffer = nullptr;
            } else {
                buffer->setObserver(this);
                if (free != mBuffers.end()) {
                    ALOGV("reallocate buffer, requested size %zu vs available %zu",
                            requestedSize, (*free)->size());
                    (*free)->setObserver(nullptr);
                    (*free)->release();
                    *free = buffer; // in-place replace
                } else {
                    ALOGV("allocate buffer, requested size %zu", requestedSize);
                    mBuffers.emplace_back(buffer);
                }
            }
        }
        if (buffer != nullptr) {
            buffer->add_ref();
            buffer->reset();
            *out = buffer;
            return OK;
        }
        if (nonBlocking) {
            *out = nullptr;
            return WOULD_BLOCK;
        }
        // All buffers are in use, block until one of them is returned.
        mCondition.wait(mLock);
    }
    // Never gets here.
}

void MediaBufferGroup::signalBufferReturned(MediaBuffer *) {
    mCondition.signal();
}

}  // namespace android
