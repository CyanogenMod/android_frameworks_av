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

MediaBufferGroup::MediaBufferGroup()
    : mFirstBuffer(NULL),
      mLastBuffer(NULL) {
}

MediaBufferGroup::~MediaBufferGroup() {
    MediaBuffer *next;
    for (MediaBuffer *buffer = mFirstBuffer; buffer != NULL;
         buffer = next) {
        next = buffer->nextBuffer();

        CHECK_EQ(buffer->refcount(), 0);

        buffer->setObserver(NULL);
        buffer->release();
    }
}

void MediaBufferGroup::add_buffer(MediaBuffer *buffer) {
    Mutex::Autolock autoLock(mLock);

    buffer->setObserver(this);

    if (mLastBuffer) {
        mLastBuffer->setNextBuffer(buffer);
    } else {
        mFirstBuffer = buffer;
    }

    mLastBuffer = buffer;
}

status_t MediaBufferGroup::acquire_buffer(
        MediaBuffer **out, bool nonBlocking, size_t requestedSize) {
    Mutex::Autolock autoLock(mLock);

    for (;;) {
        MediaBuffer *freeBuffer = NULL;
        MediaBuffer *freeBufferPrevious = NULL;
        MediaBuffer *buffer = NULL;
        MediaBuffer *bufferPrevious = NULL;
        size_t smallest = requestedSize;
        for (buffer = mFirstBuffer;
             buffer != NULL; buffer = buffer->nextBuffer()) {
            if (buffer->refcount() == 0) {
               if (buffer->size() >= requestedSize) {
                   break;
               } else if (buffer->size() < smallest) {
                   freeBuffer = buffer;
                   freeBufferPrevious = bufferPrevious;
               }
            }
            bufferPrevious = buffer;
        }

        if (buffer == NULL && freeBuffer != NULL) {
            ALOGV("allocate new buffer, requested size %zu vs available %zu",
                    requestedSize, freeBuffer->size());
            size_t allocateSize = requestedSize;
            if (requestedSize < SIZE_MAX / 3) {
                allocateSize = requestedSize * 3 / 2;
            }
            MediaBuffer *newBuffer = new MediaBuffer(allocateSize);
            newBuffer->setObserver(this);
            if (freeBuffer == mFirstBuffer) {
                mFirstBuffer = newBuffer;
            }
            if (freeBuffer == mLastBuffer) {
                mLastBuffer = newBuffer;
            }
            newBuffer->setNextBuffer(freeBuffer->nextBuffer());
            if (freeBufferPrevious != NULL) {
                freeBufferPrevious->setNextBuffer(newBuffer);
            }
            freeBuffer->setObserver(NULL);
            freeBuffer->release();

            buffer = newBuffer;
        }

        if (buffer != NULL) {
            buffer->add_ref();
            buffer->reset();

            *out = buffer;
            goto exit;
        }

        if (nonBlocking) {
            *out = NULL;
            return WOULD_BLOCK;
        }

        // All buffers are in use. Block until one of them is returned to us.
        mCondition.wait(mLock);
    }

exit:
    return OK;
}

void MediaBufferGroup::signalBufferReturned(MediaBuffer *) {
    Mutex::Autolock autoLock(mLock);
    mCondition.signal();
}

}  // namespace android
