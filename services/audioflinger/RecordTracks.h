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

// record track
class RecordTrack : public TrackBase {
public:
                        RecordTrack(RecordThread *thread,
                                const sp<Client>& client,
                                uint32_t sampleRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                size_t frameCount,
                                int sessionId,
                                int uid);
    virtual             ~RecordTrack();

    virtual status_t    start(AudioSystem::sync_event_t event, int triggerSession);
    virtual void        stop();

            void        destroy();

            void        invalidate();
            // clear the buffer overflow flag
            void        clearOverflow() { mOverflow = false; }
            // set the buffer overflow flag and return previous value
            bool        setOverflow() { bool tmp = mOverflow; mOverflow = true;
                                                return tmp; }

    static  void        appendDumpHeader(String8& result);
            void        dump(char* buffer, size_t size, bool active);

            void        handleSyncStartEvent(const sp<SyncEvent>& event);
            void        clearSyncStartEvent();

private:
    friend class AudioFlinger;  // for mState

                        RecordTrack(const RecordTrack&);
                        RecordTrack& operator = (const RecordTrack&);

    // AudioBufferProvider interface
    virtual status_t getNextBuffer(AudioBufferProvider::Buffer* buffer,
                                   int64_t pts = kInvalidPTS);
    // releaseBuffer() not overridden

    bool                mOverflow;  // overflow on most recent attempt to fill client buffer

           // updated by RecordThread::readInputParameters_l()
            AudioResampler                      *mResampler;

            // interleaved stereo pairs of fixed-point Q4.27
            int32_t                             *mRsmpOutBuffer;
            // current allocated frame count for the above, which may be larger than needed
            size_t                              mRsmpOutFrameCount;

            size_t                              mRsmpInUnrel;   // unreleased frames remaining from
                                                                // most recent getNextBuffer
                                                                // for debug only

            // rolling counter that is never cleared
            int32_t                             mRsmpInFront;   // next available frame

            AudioBufferProvider::Buffer mSink;  // references client's buffer sink in shared memory

            // sync event triggering actual audio capture. Frames read before this event will
            // be dropped and therefore not read by the application.
            sp<SyncEvent>                       mSyncStartEvent;

            // number of captured frames to drop after the start sync event has been received.
            // when < 0, maximum frames to drop before starting capture even if sync event is
            // not received
            ssize_t                             mFramesToDrop;

            // used by resampler to find source frames
            ResamplerBufferProvider *mResamplerBufferProvider;
};
