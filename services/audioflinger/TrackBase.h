/*
**
** Copyright 2012, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef INCLUDING_FROM_AUDIOFLINGER_H
    #error This header file should only be included from AudioFlinger.h
#endif

// base for record and playback
class TrackBase : public ExtendedAudioBufferProvider, public RefBase {

public:
    enum track_state {
        IDLE,
        FLUSHED,
        STOPPED,
        // next 2 states are currently used for fast tracks
        // and offloaded tracks only
        STOPPING_1,     // waiting for first underrun
        STOPPING_2,     // waiting for presentation complete
        RESUMING,
        ACTIVE,
        PAUSING,
        PAUSED,
        STARTING_1,     // for RecordTrack only
        STARTING_2,     // for RecordTrack only
    };

    // where to allocate the data buffer
    enum alloc_type {
        ALLOC_CBLK,     // allocate immediately after control block
        ALLOC_READONLY, // allocate from a separate read-only heap per thread
        ALLOC_PIPE,     // do not allocate; use the pipe buffer
        ALLOC_LOCAL,    // allocate a local buffer
        ALLOC_NONE,     // do not allocate:use the buffer passed to TrackBase constructor
    };

    enum track_type {
        TYPE_DEFAULT,
        TYPE_OUTPUT,
        TYPE_PATCH,
    };

                        TrackBase(ThreadBase *thread,
                                const sp<Client>& client,
                                uint32_t sampleRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                size_t frameCount,
                                void *buffer,
                                audio_session_t sessionId,
                                int uid,
                                bool isOut,
                                alloc_type alloc = ALLOC_CBLK,
                                track_type type = TYPE_DEFAULT);
    virtual             ~TrackBase();
    virtual status_t    initCheck() const;

    virtual status_t    start(AudioSystem::sync_event_t event,
                             audio_session_t triggerSession) = 0;
    virtual void        stop() = 0;
            sp<IMemory> getCblk() const { return mCblkMemory; }
            audio_track_cblk_t* cblk() const { return mCblk; }
            audio_session_t sessionId() const { return mSessionId; }
            int         uid() const { return mUid; }
    virtual status_t    setSyncEvent(const sp<SyncEvent>& event);

            sp<IMemory> getBuffers() const { return mBufferMemory; }
            void*       buffer() const { return mBuffer; }
    virtual bool        isFastTrack() const = 0;
            bool        isOutputTrack() const { return (mType == TYPE_OUTPUT); }
            bool        isPatchTrack() const { return (mType == TYPE_PATCH); }
            bool        isExternalTrack() const { return !isOutputTrack() && !isPatchTrack(); }

protected:
                        TrackBase(const TrackBase&);
                        TrackBase& operator = (const TrackBase&);

    // AudioBufferProvider interface
    virtual status_t getNextBuffer(AudioBufferProvider::Buffer* buffer) = 0;
    virtual void releaseBuffer(AudioBufferProvider::Buffer* buffer);

    // ExtendedAudioBufferProvider interface is only needed for Track,
    // but putting it in TrackBase avoids the complexity of virtual inheritance
    virtual size_t  framesReady() const { return SIZE_MAX; }

    audio_format_t format() const { return mFormat; }

    uint32_t channelCount() const { return mChannelCount; }

    audio_channel_mask_t channelMask() const { return mChannelMask; }

    virtual uint32_t sampleRate() const { return mSampleRate; }

    bool isStopped() const {
        return (mState == STOPPED || mState == FLUSHED);
    }

    // for fast tracks and offloaded tracks only
    bool isStopping() const {
        return mState == STOPPING_1 || mState == STOPPING_2;
    }
    bool isStopping_1() const {
        return mState == STOPPING_1;
    }
    bool isStopping_2() const {
        return mState == STOPPING_2;
    }

    bool isTerminated() const {
        return mTerminated;
    }

    void terminate() {
        mTerminated = true;
    }

    bool isOut() const { return mIsOut; }
                                    // true for Track, false for RecordTrack,
                                    // this could be a track type if needed later

    const wp<ThreadBase> mThread;
    /*const*/ sp<Client> mClient;   // see explanation at ~TrackBase() why not const
    sp<IMemory>         mCblkMemory;
    audio_track_cblk_t* mCblk;
    sp<IMemory>         mBufferMemory;  // currently non-0 for fast RecordTrack only
    void*               mBuffer;    // start of track buffer, typically in shared memory
                                    // except for OutputTrack when it is in local memory
    // we don't really need a lock for these
    track_state         mState;
    const uint32_t      mSampleRate;    // initial sample rate only; for tracks which
                        // support dynamic rates, the current value is in control block
    const audio_format_t mFormat;
    const audio_channel_mask_t mChannelMask;
    const uint32_t      mChannelCount;
    const size_t        mFrameSize; // AudioFlinger's view of frame size in shared memory,
                                    // where for AudioTrack (but not AudioRecord),
                                    // 8-bit PCM samples are stored as 16-bit
    const size_t        mFrameCount;// size of track buffer given at createTrack() or
                                    // openRecord(), and then adjusted as needed

    const audio_session_t mSessionId;
    int                 mUid;
    Vector < sp<SyncEvent> >mSyncEvents;
    const bool          mIsOut;
    ServerProxy*        mServerProxy;
    const int           mId;
    sp<NBAIO_Sink>      mTeeSink;
    sp<NBAIO_Source>    mTeeSource;
    bool                mTerminated;
    track_type          mType;      // must be one of TYPE_DEFAULT, TYPE_OUTPUT, TYPE_PATCH ...
    audio_io_handle_t   mThreadIoHandle; // I/O handle of the thread the track is attached to
};

// PatchProxyBufferProvider interface is implemented by PatchTrack and PatchRecord.
// it provides buffer access methods that map those of a ClientProxy (see AudioTrackShared.h)
class PatchProxyBufferProvider
{
public:

    virtual ~PatchProxyBufferProvider() {}

    virtual status_t    obtainBuffer(Proxy::Buffer* buffer,
                                     const struct timespec *requested = NULL) = 0;
    virtual void        releaseBuffer(Proxy::Buffer* buffer) = 0;
};
