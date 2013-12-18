/*Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef PREFETCH_SOURCE_H_
#define PREFETCH_SOURCE_H_

#include <sys/types.h>
#include <utils/RefBase.h>
#include <utils/Vector.h>

#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaErrors.h>

namespace android {

static const size_t kDefaultAudioPrefetchBufferSize = (32 * 1024);
static const size_t kDefaultVideoPrefetchBufferSize = (4 * 1024 * 1024);
static const size_t kNumAudioPrefetchBuffers = 4;
static const size_t kNumVideoPrefetchBuffers = 8;

class MediaBuffer;
class MetaData;

class PrefetchSource : public MediaSource {
public:
    enum {
        MODE_AGGREGATE      = 0x01,
        MODE_FRAME_BY_FRAME = 0x02
    };

    enum {
        STATE_STOPPED  = 0x00,
        STATE_STARTING = 0x01,
        STATE_RUNNING  = 0x02,
        STATE_STOPPING = 0x03
    };

    PrefetchSource(sp<MediaSource> source, uint32_t mode, const char *id);

    static bool isPrefetchEnabled();

    /* Inherited from MediaSource API */
    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();
    virtual status_t read(MediaBuffer **buffer, const ReadOptions *options = NULL);
    virtual status_t setBuffers(const Vector<MediaBuffer *> &buffers);

    /* Thread-safe queue that assumes a 1-producer, 1-consumer model.
     * Synchronization is intentionally kept minimal to improve performance. */
    class SyncQueue : public MediaBufferObserver {
    public:
        SyncQueue (int size);
        ~SyncQueue ();

        /* Inherited from MediaBufferObserver API */
        /* Callback triggered when a buffer observed by this queue is released.
         * Adds the buffer back to the queue. */
        virtual void signalBufferReturned(MediaBuffer *buf) {
            put(buf);
        }

        /* Adds a buffer to the queue & registers as the buffer's observer */
        status_t add (MediaBuffer *buf);

        /* Adds a buffer to the queue */
        status_t put (MediaBuffer *buf);

        /* Gets a buffer from the queue, blocking if no buffer is available */
        status_t get (MediaBuffer **buf);

        /* Wakes any blocked threads that are waiting for an available buffer */
        void wake();

        /* Sets blocking behavior of get() (default behavior is blocking, or
         * 'true'). Setting to non-blocking ensures wakeup synchronization when
         * flushing the queue. */
        void setBlocking(bool blocking);

        /* Sets name of the queue to be used in logs. Names may be up to 31
         * characters long. */
        void setName(const char *name);

        /* Returns true if the queue is empty. If multiple threads are active,
         * the value of mCount may be timing-dependent. */
        inline bool empty () const {
            return (mCount <= 0);
        }

        /* Returns true if the queue is completely filled. If the consumer
         * is active in another thread, the value of mCount may be
         * timing-dependent. */
        inline bool full () const {
            return (mCount == mSize);
        }

        /* Returns the current number of buffers contained by the queue. The
         * value of mCount may be timing-dependent if either producer or
         * consumer is active in another thread. */
        inline int count () const {
            return mCount;
        }

        private:
            static const nsecs_t TIMEOUT_NS = 5000000000;
            const int mSize;
            MediaBuffer **mList;
            volatile int mCount; // atomic counter
            int mReadIndex;
            int mWriteIndex;
            bool mBlocking;
            Mutex mLock;
            Condition mCondition;
            char mName[32];
    };

protected:
    virtual ~PrefetchSource();

private:
    /* Helper function called by prefetching thread to read from source */
    status_t readInternal(MediaBuffer **buffer);

    /* Returns all buffers to the available buffer queue */
    void flushFilledBuffers();

    /* Copies buffer metadata to destination buffer */
    void copyMetaData(MediaBuffer *srcBuffer,MediaBuffer* dstBuffer);

    /* Wrapper for pthread_create */
    static void *ThreadWrapper(void *me);

    /* Main prefetching loop */
    void prefetchThread();

    /* Starts the prefetching thread */
    void startThread();

    /* Stops the prefetching thread blocking on pthread_join until the
     * prefetching thread has completed. */
    void stopThread();

    sp<MediaSource> mSource;
    bool mSourceStarted;
    MediaBuffer *mBuffer;
    size_t mRemnantOffset;

    SyncQueue *mAvailBufferQueue;
    SyncQueue *mFilledBufferQueue;

    uint32_t mMode;
    volatile int mState; // atomic state
    int64_t mSeekTimeUs;
    ReadOptions::SeekMode mSeekMode;
    bool mReachedEos;
    pthread_t mThread;
    char mId[32];
};

};

#endif //PREFETCH_SOURCE_H_
