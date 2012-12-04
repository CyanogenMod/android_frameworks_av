/*
 * Copyright (C) 2007 The Android Open Source Project
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

#ifndef ANDROID_AUDIO_TRACK_SHARED_H
#define ANDROID_AUDIO_TRACK_SHARED_H

#include <stdint.h>
#include <sys/types.h>

#include <utils/threads.h>
#include <utils/Log.h>

namespace android {

// ----------------------------------------------------------------------------

// Maximum cumulated timeout milliseconds before restarting audioflinger thread
#define MAX_STARTUP_TIMEOUT_MS  3000    // Longer timeout period at startup to cope with A2DP
                                        // init time
#define MAX_RUN_TIMEOUT_MS      1000
#define WAIT_PERIOD_MS          10

#define CBLK_UNDERRUN   0x01 // set: underrun (out) or overrrun (in), clear: no underrun or overrun
#define CBLK_FORCEREADY 0x02 // set: track is considered ready immediately by AudioFlinger,
                             // clear: track is ready when buffer full
#define CBLK_INVALID    0x04 // track buffer invalidated by AudioFlinger, need to re-create
#define CBLK_DISABLED   0x08 // track disabled by AudioFlinger due to underrun, need to re-start

struct AudioTrackSharedStreaming {
    // similar to NBAIO MonoPipe
    volatile int32_t mFront;
    volatile int32_t mRear;
};

// future
struct AudioTrackSharedStatic {
    int mReserved;
};

// ----------------------------------------------------------------------------

// Important: do not add any virtual methods, including ~
struct audio_track_cblk_t
{
                friend class Proxy;
                friend class AudioTrackClientProxy;
                friend class AudioRecordClientProxy;
                friend class ServerProxy;

    // The data members are grouped so that members accessed frequently and in the same context
    // are in the same line of data cache.
                Mutex       lock;           // sizeof(int)
                Condition   cv;             // sizeof(int)

                // next 4 are offsets within "buffers"
    volatile    uint32_t    user;
    volatile    uint32_t    server;
                uint32_t    userBase;
                uint32_t    serverBase;

                int         mPad1;          // unused, but preserves cache line alignment

                size_t      frameCount_;    // used during creation to pass actual track buffer size
                                            // from AudioFlinger to client, and not referenced again
                                            // FIXME remove here and replace by createTrack() in/out parameter
                                            // renamed to "_" to detect incorrect use

                // Cache line boundary (32 bytes)

                uint32_t    loopStart;
                uint32_t    loopEnd;        // read-only for server, read/write for client
                int         loopCount;      // read/write for client

                // Channel volumes are fixed point U4.12, so 0x1000 means 1.0.
                // Left channel is in [0:15], right channel is in [16:31].
                // Always read and write the combined pair atomically.
                // For AudioTrack only, not used by AudioRecord.
private:
                uint32_t    mVolumeLR;

                uint32_t    mSampleRate;    // AudioTrack only: client's requested sample rate in Hz
                                            // or 0 == default. Write-only client, read-only server.

                uint8_t     mPad2;           // unused

public:
                // read-only for client, server writes once at initialization and is then read-only
                uint8_t     mName;           // normal tracks: track name, fast tracks: track index

                // used by client only
                uint16_t    bufferTimeoutMs; // Maximum cumulated timeout before restarting
                                             // audioflinger

                uint16_t    waitTimeMs;      // Cumulated wait time, used by client only
private:
                // client write-only, server read-only
                uint16_t    mSendLevel;      // Fixed point U4.12 so 0x1000 means 1.0
public:
    volatile    int32_t     flags;

                // Cache line boundary (32 bytes)

#if 0
                union {
                    AudioTrackSharedStreaming   mStreaming;
                    AudioTrackSharedStatic      mStatic;
                    int                         mAlign[8];
                } u;

                // Cache line boundary (32 bytes)
#endif

                // Since the control block is always located in shared memory, this constructor
                // is only used for placement new().  It is never used for regular new() or stack.
                            audio_track_cblk_t();

private:
                // if there is a shared buffer, "buffers" is the value of pointer() for the shared
                // buffer, otherwise "buffers" points immediately after the control block
                void*       buffer(void *buffers, uint32_t frameSize, size_t offset) const;

                bool        tryLock();

                // isOut == true means AudioTrack, isOut == false means AudioRecord
                bool        stepServer(size_t stepCount, size_t frameCount, bool isOut);
                uint32_t    stepUser(size_t stepCount, size_t frameCount, bool isOut);
                uint32_t    framesAvailable(size_t frameCount, bool isOut);
                uint32_t    framesAvailable_l(size_t frameCount, bool isOut);
                uint32_t    framesReady(bool isOut);
};

// ----------------------------------------------------------------------------

// Proxy for shared memory control block, to isolate callers from needing to know the details.
// There is exactly one ClientProxy and one ServerProxy per shared memory control block.
// The proxies are located in normal memory, and are not multi-thread safe within a given side.
class Proxy {
protected:
    Proxy(audio_track_cblk_t* cblk, void *buffers, size_t frameCount, size_t frameSize)
        : mCblk(cblk), mBuffers(buffers), mFrameCount(frameCount), mFrameSize(frameSize) { }
    virtual ~Proxy() { }

public:
    void*   buffer(size_t offset) const {
        return mCblk->buffer(mBuffers, mFrameSize, offset);
    }

protected:
    // These refer to shared memory, and are virtual addresses with respect to the current process.
    // They may have different virtual addresses within the other process.
    audio_track_cblk_t* const   mCblk;          // the control block
    void* const                 mBuffers;       // starting address of buffers

    const size_t                mFrameCount;    // not necessarily a power of 2
    const size_t                mFrameSize;     // in bytes
#if 0
    const size_t                mFrameCountP2;  // mFrameCount rounded to power of 2, streaming mode
#endif

};

// ----------------------------------------------------------------------------

// Proxy seen by AudioTrack client and AudioRecord client
class ClientProxy : public Proxy {
protected:
    ClientProxy(audio_track_cblk_t* cblk, void *buffers, size_t frameCount, size_t frameSize)
        : Proxy(cblk, buffers, frameCount, frameSize) { }
    virtual ~ClientProxy() { }
};

// ----------------------------------------------------------------------------

// Proxy used by AudioTrack client, which also includes AudioFlinger::PlaybackThread::OutputTrack
class AudioTrackClientProxy : public ClientProxy {
public:
    AudioTrackClientProxy(audio_track_cblk_t* cblk, void *buffers, size_t frameCount, size_t frameSize)
        : ClientProxy(cblk, buffers, frameCount, frameSize) { }
    virtual ~AudioTrackClientProxy() { }

    // No barriers on the following operations, so the ordering of loads/stores
    // with respect to other parameters is UNPREDICTABLE. That's considered safe.

    // caller must limit to 0.0 <= sendLevel <= 1.0
    void        setSendLevel(float sendLevel) {
        mCblk->mSendLevel = uint16_t(sendLevel * 0x1000);
    }

    // caller must limit to 0 <= volumeLR <= 0x10001000
    void        setVolumeLR(uint32_t volumeLR) {
        mCblk->mVolumeLR = volumeLR;
    }

    void        setSampleRate(uint32_t sampleRate) {
        mCblk->mSampleRate = sampleRate;
    }

    // called by:
    //   PlaybackThread::OutputTrack::write
    //   AudioTrack::createTrack_l
    //   AudioTrack::releaseBuffer
    //   AudioTrack::reload
    //   AudioTrack::restoreTrack_l (2 places)
    size_t      stepUser(size_t stepCount) {
        return mCblk->stepUser(stepCount, mFrameCount, true /*isOut*/);
    }

    // called by AudioTrack::obtainBuffer and AudioTrack::processBuffer
    size_t      framesAvailable() {
        return mCblk->framesAvailable(mFrameCount, true /*isOut*/);
    }

    // called by AudioTrack::obtainBuffer and PlaybackThread::OutputTrack::obtainBuffer
    // FIXME remove this API since it assumes a lock that should be invisible to caller
    size_t      framesAvailable_l() {
        return mCblk->framesAvailable_l(mFrameCount, true /*isOut*/);
    }

};

// ----------------------------------------------------------------------------

// Proxy used by AudioRecord client
class AudioRecordClientProxy : public ClientProxy {
public:
    AudioRecordClientProxy(audio_track_cblk_t* cblk, void *buffers, size_t frameCount, size_t frameSize)
        : ClientProxy(cblk, buffers, frameCount, frameSize) { }
    ~AudioRecordClientProxy() { }

    // called by AudioRecord::releaseBuffer
    size_t      stepUser(size_t stepCount) {
        return mCblk->stepUser(stepCount, mFrameCount, false /*isOut*/);
    }

    // called by AudioRecord::processBuffer
    size_t      framesAvailable() {
        return mCblk->framesAvailable(mFrameCount, false /*isOut*/);
    }

    // called by AudioRecord::obtainBuffer
    size_t      framesReady() {
        return mCblk->framesReady(false /*isOut*/);
    }

};

// ----------------------------------------------------------------------------

// Proxy used by AudioFlinger server
class ServerProxy : public Proxy {
public:
    ServerProxy(audio_track_cblk_t* cblk, void *buffers, size_t frameCount, size_t frameSize, bool isOut)
        : Proxy(cblk, buffers, frameCount, frameSize), mIsOut(isOut) { }
    virtual ~ServerProxy() { }

    // for AudioTrack and AudioRecord
    bool        step(size_t stepCount) { return mCblk->stepServer(stepCount, mFrameCount, mIsOut); }

    // return value of these methods must be validated by the caller
    uint32_t    getSampleRate() const { return mCblk->mSampleRate; }
    uint16_t    getSendLevel_U4_12() const { return mCblk->mSendLevel; }
    uint32_t    getVolumeLR() const { return mCblk->mVolumeLR; }

    // for AudioTrack only
    size_t      framesReady() {
        ALOG_ASSERT(mIsOut);
        return mCblk->framesReady(true);
    }

    // for AudioRecord only, called by RecordThread::RecordTrack::getNextBuffer
    // FIXME remove this API since it assumes a lock that should be invisible to caller
    size_t      framesAvailableIn_l() {
        ALOG_ASSERT(!mIsOut);
        return mCblk->framesAvailable_l(mFrameCount, false);
    }

private:
    const bool  mIsOut;     // true for AudioTrack, false for AudioRecord

};

// ----------------------------------------------------------------------------

}; // namespace android

#endif // ANDROID_AUDIO_TRACK_SHARED_H
