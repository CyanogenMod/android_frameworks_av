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

#ifndef MEDIA_BUFFER_H_

#define MEDIA_BUFFER_H_

#include <atomic>
#include <list>
#include <media/stagefright/foundation/MediaBufferBase.h>

#include <pthread.h>

#include <binder/MemoryDealer.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>

namespace android {

struct ABuffer;
class GraphicBuffer;
class MediaBuffer;
class MediaBufferObserver;
class MetaData;

class MediaBufferObserver {
public:
    MediaBufferObserver() {}
    virtual ~MediaBufferObserver() {}

    virtual void signalBufferReturned(MediaBuffer *buffer) = 0;

private:
    MediaBufferObserver(const MediaBufferObserver &);
    MediaBufferObserver &operator=(const MediaBufferObserver &);
};

class MediaBuffer : public MediaBufferBase {
public:
    // allocations larger than or equal to this will use shared memory.
    static const size_t kSharedMemThreshold = 64 * 1024;

    // The underlying data remains the responsibility of the caller!
    MediaBuffer(void *data, size_t size);

    MediaBuffer(size_t size);

    MediaBuffer(const sp<GraphicBuffer>& graphicBuffer);

    MediaBuffer(const sp<ABuffer> &buffer);

    MediaBuffer(const sp<IMemory> &mem) :
        MediaBuffer((uint8_t *)mem->pointer() + sizeof(SharedControl), mem->size()) {
        // delegate and override mMemory
        mMemory = mem;
    }

    // Decrements the reference count and returns the buffer to its
    // associated MediaBufferGroup if the reference count drops to 0.
    virtual void release();

    // Increments the reference count.
    virtual void add_ref();

    void *data() const;
    size_t size() const;

    size_t range_offset() const;
    size_t range_length() const;

    void set_range(size_t offset, size_t length);

    sp<GraphicBuffer> graphicBuffer() const;

    sp<MetaData> meta_data();

    // Clears meta data and resets the range to the full extent.
    void reset();

    void setObserver(MediaBufferObserver *group);

    // Returns a clone of this MediaBuffer increasing its reference count.
    // The clone references the same data but has its own range and
    // MetaData.
    MediaBuffer *clone();

    int refcount() const;

    bool isDeadObject() const {
        return isDeadObject(mMemory);
    }

    static bool isDeadObject(const sp<IMemory> &memory) {
        if (memory.get() == nullptr || memory->pointer() == nullptr) return false;
        return reinterpret_cast<SharedControl *>(memory->pointer())->isDeadObject();
    }

    // Sticky on enabling of shared memory MediaBuffers. By default we don't use
    // shared memory for MediaBuffers, but we enable this for those processes
    // that export MediaBuffers.
    static void useSharedMemory() {
        std::atomic_store_explicit(
                &mUseSharedMemory, (int_least32_t)1, std::memory_order_seq_cst);
    }

protected:
    // MediaBuffer remote releases are handled through a
    // pending release count variable stored in a SharedControl block
    // at the start of the IMemory.

    // Returns old value of pending release count.
    inline int32_t addPendingRelease(int32_t value) {
        return getSharedControl()->addPendingRelease(value);
    }

    // Issues all pending releases (works in parallel).
    // Assumes there is a MediaBufferObserver.
    inline void resolvePendingRelease() {
        if (mMemory.get() == nullptr) return;
        while (addPendingRelease(-1) > 0) {
            release();
        }
        addPendingRelease(1);
    }

    // true if MediaBuffer is observed (part of a MediaBufferGroup).
    inline bool isObserved() const {
        return mObserver != nullptr;
    }

    virtual ~MediaBuffer();

    sp<IMemory> mMemory;

private:
    friend class MediaBufferGroup;
    friend class OMXDecoder;
    friend class BnMediaSource;
    friend class BpMediaSource;

    // For use by OMXDecoder, reference count must be 1, drop reference
    // count to 0 without signalling the observer.
    void claim();

    MediaBufferObserver *mObserver;
    int mRefCount;

    void *mData;
    size_t mSize, mRangeOffset, mRangeLength;
    sp<GraphicBuffer> mGraphicBuffer;
    sp<ABuffer> mBuffer;

    bool mOwnsData;

    sp<MetaData> mMetaData;

    MediaBuffer *mOriginal;

    static std::atomic_int_least32_t mUseSharedMemory;

    MediaBuffer(const MediaBuffer &);
    MediaBuffer &operator=(const MediaBuffer &);

    // SharedControl block at the start of IMemory.
    struct SharedControl {
        enum {
            FLAG_DEAD_OBJECT = (1 << 0),
        };

        // returns old value
        inline int32_t addPendingRelease(int32_t value) {
            return std::atomic_fetch_add_explicit(
                    &mPendingRelease, (int_least32_t)value, std::memory_order_seq_cst);
        }

        inline int32_t getPendingRelease() const {
            return std::atomic_load_explicit(&mPendingRelease, std::memory_order_seq_cst);
        }

        inline void setPendingRelease(int32_t value) {
            std::atomic_store_explicit(
                    &mPendingRelease, (int_least32_t)value, std::memory_order_seq_cst);
        }

        inline bool isDeadObject() const {
            return (std::atomic_load_explicit(
                    &mFlags, std::memory_order_seq_cst) & FLAG_DEAD_OBJECT) != 0;
        }

        inline void setDeadObject() {
            (void)std::atomic_fetch_or_explicit(
                    &mFlags, (int_least32_t)FLAG_DEAD_OBJECT, std::memory_order_seq_cst);
        }

        inline void clear() {
            std::atomic_store_explicit(
                    &mFlags, (int_least32_t)0, std::memory_order_seq_cst);
            std::atomic_store_explicit(
                    &mPendingRelease, (int_least32_t)0, std::memory_order_seq_cst);
        }

    private:
        // Caution: atomic_int_fast32_t is 64 bits on LP64.
        std::atomic_int_least32_t mFlags;
        std::atomic_int_least32_t mPendingRelease;
        int32_t unused[6]; // additional buffer space
    };

    inline SharedControl *getSharedControl() const {
         return reinterpret_cast<SharedControl *>(mMemory->pointer());
     }
};

}  // namespace android

#endif  // MEDIA_BUFFER_H_
