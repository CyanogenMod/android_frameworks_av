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

// playback track
class Track : public TrackBase, public VolumeProvider {
public:
                        Track(  PlaybackThread *thread,
                                const sp<Client>& client,
                                audio_stream_type_t streamType,
                                uint32_t sampleRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                size_t frameCount,
                                void *buffer,
                                const sp<IMemory>& sharedBuffer,
                                audio_session_t sessionId,
                                int uid,
                                audio_output_flags_t flags,
                                track_type type);
    virtual             ~Track();
    virtual status_t    initCheck() const;

    static  void        appendDumpHeader(String8& result);
            void        dump(char* buffer, size_t size, bool active);
    virtual status_t    start(AudioSystem::sync_event_t event =
                                    AudioSystem::SYNC_EVENT_NONE,
                             audio_session_t triggerSession = AUDIO_SESSION_NONE);
    virtual void        stop();
            void        pause();

            void        flush();
            void        destroy();
            int         name() const { return mName; }

    virtual uint32_t    sampleRate() const;

            audio_stream_type_t streamType() const {
                return mStreamType;
            }
            bool        isOffloaded() const
                                { return (mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0; }
            bool        isDirect() const { return (mFlags & AUDIO_OUTPUT_FLAG_DIRECT) != 0; }
            status_t    setParameters(const String8& keyValuePairs);
            status_t    attachAuxEffect(int EffectId);
            void        setAuxBuffer(int EffectId, int32_t *buffer);
            int32_t     *auxBuffer() const { return mAuxBuffer; }
            void        setMainBuffer(int16_t *buffer) { mMainBuffer = buffer; }
            int16_t     *mainBuffer() const { return mMainBuffer; }
            int         auxEffectId() const { return mAuxEffectId; }
    virtual status_t    getTimestamp(AudioTimestamp& timestamp);
            void        signal();

// implement FastMixerState::VolumeProvider interface
    virtual gain_minifloat_packed_t getVolumeLR();

    virtual status_t    setSyncEvent(const sp<SyncEvent>& event);

    virtual bool        isFastTrack() const { return (mFlags & AUDIO_OUTPUT_FLAG_FAST) != 0; }

protected:
    // for numerous
    friend class PlaybackThread;
    friend class MixerThread;
    friend class DirectOutputThread;
    friend class OffloadThread;

                        Track(const Track&);
                        Track& operator = (const Track&);

    // AudioBufferProvider interface
    virtual status_t getNextBuffer(AudioBufferProvider::Buffer* buffer);
    // releaseBuffer() not overridden

    // ExtendedAudioBufferProvider interface
    virtual size_t framesReady() const;
    virtual int64_t framesReleased() const;
    virtual void onTimestamp(const ExtendedTimestamp &timestamp);

    bool isPausing() const { return mState == PAUSING; }
    bool isPaused() const { return mState == PAUSED; }
    bool isResuming() const { return mState == RESUMING; }
    bool isReady() const;
    void setPaused() { mState = PAUSED; }
    void reset();
    bool isFlushPending() const { return mFlushHwPending; }
    void flushAck();
    bool isResumePending();
    void resumeAck();
    void updateTrackFrameInfo(int64_t trackFramesReleased, int64_t sinkFramesWritten,
            const ExtendedTimestamp &timeStamp);

    sp<IMemory> sharedBuffer() const { return mSharedBuffer; }

    // framesWritten is cumulative, never reset, and is shared all tracks
    // audioHalFrames is derived from output latency
    // FIXME parameters not needed, could get them from the thread
    bool presentationComplete(int64_t framesWritten, size_t audioHalFrames);
    void signalClientFlag(int32_t flag);

public:
    void triggerEvents(AudioSystem::sync_event_t type);
    void invalidate();
    void disable();

    bool isInvalid() const { return mIsInvalid; }
    int fastIndex() const { return mFastIndex; }

protected:

    // FILLED state is used for suppressing volume ramp at begin of playing
    enum {FS_INVALID, FS_FILLING, FS_FILLED, FS_ACTIVE};
    mutable uint8_t     mFillingUpStatus;
    int8_t              mRetryCount;

    // see comment at AudioFlinger::PlaybackThread::Track::~Track for why this can't be const
    sp<IMemory>         mSharedBuffer;

    bool                mResetDone;
    const audio_stream_type_t mStreamType;
    int                 mName;      // track name on the normal mixer,
                                    // allocated statically at track creation time,
                                    // and is even allocated (though unused) for fast tracks
                                    // FIXME don't allocate track name for fast tracks
    int16_t             *mMainBuffer;
    int32_t             *mAuxBuffer;
    int                 mAuxEffectId;
    bool                mHasVolumeController;
    size_t              mPresentationCompleteFrames; // number of frames written to the
                                    // audio HAL when this track will be fully rendered
                                    // zero means not monitoring

    // access these three variables only when holding thread lock.
    LinearMap<int64_t> mFrameMap;           // track frame to server frame mapping

    ExtendedTimestamp  mSinkTimestamp;

private:
    // The following fields are only for fast tracks, and should be in a subclass
    int                 mFastIndex; // index within FastMixerState::mFastTracks[];
                                    // either mFastIndex == -1 if not isFastTrack()
                                    // or 0 < mFastIndex < FastMixerState::kMaxFast because
                                    // index 0 is reserved for normal mixer's submix;
                                    // index is allocated statically at track creation time
                                    // but the slot is only used if track is active
    FastTrackUnderruns  mObservedUnderruns; // Most recently observed value of
                                    // mFastMixerDumpState.mTracks[mFastIndex].mUnderruns
    volatile float      mCachedVolume;  // combined master volume and stream type volume;
                                        // 'volatile' means accessed without lock or
                                        // barrier, but is read/written atomically
    bool                mIsInvalid; // non-resettable latch, set by invalidate()
    AudioTrackServerProxy*  mAudioTrackServerProxy;
    bool                mResumeToStopping; // track was paused in stopping state.
    bool                mFlushHwPending; // track requests for thread flush
    audio_output_flags_t mFlags;
};  // end of Track


// playback track, used by DuplicatingThread
class OutputTrack : public Track {
public:

    class Buffer : public AudioBufferProvider::Buffer {
    public:
        void *mBuffer;
    };

                        OutputTrack(PlaybackThread *thread,
                                DuplicatingThread *sourceThread,
                                uint32_t sampleRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                size_t frameCount,
                                int uid);
    virtual             ~OutputTrack();

    virtual status_t    start(AudioSystem::sync_event_t event =
                                    AudioSystem::SYNC_EVENT_NONE,
                             audio_session_t triggerSession = AUDIO_SESSION_NONE);
    virtual void        stop();
            bool        write(void* data, uint32_t frames);
            bool        bufferQueueEmpty() const { return mBufferQueue.size() == 0; }
            bool        isActive() const { return mActive; }
    const wp<ThreadBase>& thread() const { return mThread; }

private:

    status_t            obtainBuffer(AudioBufferProvider::Buffer* buffer,
                                     uint32_t waitTimeMs);
    void                clearBufferQueue();

    void                restartIfDisabled();

    // Maximum number of pending buffers allocated by OutputTrack::write()
    static const uint8_t kMaxOverFlowBuffers = 10;

    Vector < Buffer* >          mBufferQueue;
    AudioBufferProvider::Buffer mOutBuffer;
    bool                        mActive;
    DuplicatingThread* const mSourceThread; // for waitTimeMs() in write()
    AudioTrackClientProxy*      mClientProxy;
};  // end of OutputTrack

// playback track, used by PatchPanel
class PatchTrack : public Track, public PatchProxyBufferProvider {
public:

                        PatchTrack(PlaybackThread *playbackThread,
                                   audio_stream_type_t streamType,
                                   uint32_t sampleRate,
                                   audio_channel_mask_t channelMask,
                                   audio_format_t format,
                                   size_t frameCount,
                                   void *buffer,
                                   audio_output_flags_t flags);
    virtual             ~PatchTrack();

    virtual status_t    start(AudioSystem::sync_event_t event =
                                    AudioSystem::SYNC_EVENT_NONE,
                             audio_session_t triggerSession = AUDIO_SESSION_NONE);

    // AudioBufferProvider interface
    virtual status_t getNextBuffer(AudioBufferProvider::Buffer* buffer);
    virtual void releaseBuffer(AudioBufferProvider::Buffer* buffer);

    // PatchProxyBufferProvider interface
    virtual status_t    obtainBuffer(Proxy::Buffer* buffer,
                                     const struct timespec *timeOut = NULL);
    virtual void        releaseBuffer(Proxy::Buffer* buffer);

            void setPeerProxy(PatchProxyBufferProvider *proxy) { mPeerProxy = proxy; }

private:
            void restartIfDisabled();

    sp<ClientProxy>             mProxy;
    PatchProxyBufferProvider*   mPeerProxy;
    struct timespec             mPeerTimeout;
};  // end of PatchTrack
