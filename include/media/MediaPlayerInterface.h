/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Not a Contribution.
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

#ifndef ANDROID_MEDIAPLAYERINTERFACE_H
#define ANDROID_MEDIAPLAYERINTERFACE_H

#ifdef __cplusplus

#include <sys/types.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/String8.h>
#include <utils/RefBase.h>

#include <media/mediaplayer.h>
#include <media/AudioSystem.h>
#include <media/AudioTimestamp.h>
#include <media/Metadata.h>

// Fwd decl to make sure everyone agrees that the scope of struct sockaddr_in is
// global, and not in android::
struct sockaddr_in;

namespace android {

class Parcel;
class Surface;
class IGraphicBufferProducer;

template<typename T> class SortedVector;

enum player_type {
    PV_PLAYER = 1,
    SONIVOX_PLAYER = 2,
    STAGEFRIGHT_PLAYER = 3,
    NU_PLAYER = 4,
    // Test players are available only in the 'test' and 'eng' builds.
    // The shared library with the test player is passed passed as an
    // argument to the 'test:' url in the setDataSource call.
    TEST_PLAYER = 5,
    DASH_PLAYER = 6,
};


#define DEFAULT_AUDIOSINK_BUFFERCOUNT 4
#define DEFAULT_AUDIOSINK_BUFFERSIZE 1200
#define DEFAULT_AUDIOSINK_SAMPLERATE 44100

// when the channel mask isn't known, use the channel count to derive a mask in AudioSink::open()
#define CHANNEL_MASK_USE_CHANNEL_ORDER 0

// duration below which we do not allow deep audio buffering
#define AUDIO_SINK_MIN_DEEP_BUFFER_DURATION_US 5000000

// callback mechanism for passing messages to MediaPlayer object
typedef void (*notify_callback_f)(void* cookie,
        int msg, int ext1, int ext2, const Parcel *obj);

// abstract base class - use MediaPlayerInterface
class MediaPlayerBase : public RefBase
{
public:
    // AudioSink: abstraction layer for audio output
    class AudioSink : public RefBase {
    public:
        enum cb_event_t {
            CB_EVENT_FILL_BUFFER,   // Request to write more data to buffer.
            CB_EVENT_STREAM_END,    // Sent after all the buffers queued in AF and HW are played
                                    // back (after stop is called)
            CB_EVENT_TEAR_DOWN,      // The AudioTrack was invalidated due to use case change:
                                    // Need to re-evaluate offloading options
#ifdef QCOM_DIRECTTRACK
            CB_EVENT_UNDERRUN,
            CB_EVENT_HW_FAIL
#endif
        };

        // Callback returns the number of bytes actually written to the buffer.
        typedef size_t (*AudioCallback)(
                AudioSink *audioSink, void *buffer, size_t size, void *cookie,
                        cb_event_t event);

        virtual             ~AudioSink() {}
        virtual bool        ready() const = 0; // audio output is open and ready
        virtual bool        realtime() const = 0; // audio output is real-time output
        virtual ssize_t     bufferSize() const = 0;
        virtual ssize_t     frameCount() const = 0;
        virtual ssize_t     channelCount() const = 0;
        virtual ssize_t     frameSize() const = 0;
        virtual uint32_t    latency() const = 0;
#ifdef QCOM_DIRECTTRACK
        virtual audio_stream_type_t    streamType() const {return AUDIO_STREAM_DEFAULT;}
#endif
        virtual float       msecsPerFrame() const = 0;
        virtual status_t    getPosition(uint32_t *position) const = 0;
        virtual status_t    getTimestamp(AudioTimestamp &ts) const = 0;
        virtual status_t    getFramesWritten(uint32_t *frameswritten) const = 0;
        virtual int         getSessionId() const = 0;
        virtual audio_stream_type_t getAudioStreamType() const = 0;
        virtual uint32_t    getSampleRate() const = 0;

        // If no callback is specified, use the "write" API below to submit
        // audio data.
        virtual status_t    open(
                uint32_t sampleRate, int channelCount, audio_channel_mask_t channelMask,
                audio_format_t format=AUDIO_FORMAT_PCM_16_BIT,
                int bufferCount=DEFAULT_AUDIOSINK_BUFFERCOUNT,
                AudioCallback cb = NULL,
                void *cookie = NULL,
                audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE,
                const audio_offload_info_t *offloadInfo = NULL) = 0;

        virtual status_t    start() = 0;
        virtual ssize_t     write(const void* buffer, size_t size) = 0;
        virtual void        stop() = 0;
        virtual void        flush() = 0;
        virtual void        pause() = 0;
        virtual void        close() = 0;

        virtual status_t    setPlaybackRatePermille(int32_t rate) { return INVALID_OPERATION; }
        virtual bool        needsTrailingPadding() { return true; }
        virtual status_t    setParameters(const String8& keyValuePairs) { return NO_ERROR; };
        virtual String8     getParameters(const String8& keys) { return String8::empty(); };

#ifdef QCOM_DIRECTTRACK
        virtual ssize_t     sampleRate() const {return 0;};
        virtual status_t    getTimeStamp(uint64_t *tstamp) {return 0;};
#endif
    };

                        MediaPlayerBase() : mCookie(0), mNotify(0) {}
    virtual             ~MediaPlayerBase() {}
    virtual status_t    initCheck() = 0;
    virtual bool        hardwareOutput() = 0;

    virtual status_t    setUID(uid_t uid) {
        return INVALID_OPERATION;
    }

    virtual status_t    setDataSource(
            const sp<IMediaHTTPService> &httpService,
            const char *url,
            const KeyedVector<String8, String8> *headers = NULL) = 0;

    virtual status_t    setDataSource(int fd, int64_t offset, int64_t length) = 0;

    virtual status_t    setDataSource(const sp<IStreamSource> &source) {
        return INVALID_OPERATION;
    }

    // pass the buffered IGraphicBufferProducer to the media player service
    virtual status_t    setVideoSurfaceTexture(
                                const sp<IGraphicBufferProducer>& bufferProducer) = 0;

    virtual status_t    prepare() = 0;
    virtual status_t    prepareAsync() = 0;
    virtual status_t    start() = 0;
    virtual status_t    stop() = 0;
    virtual status_t    pause() = 0;
    virtual bool        isPlaying() = 0;
    virtual status_t    seekTo(int msec) = 0;
    virtual status_t    getCurrentPosition(int *msec) = 0;
    virtual status_t    getDuration(int *msec) = 0;
    virtual status_t    reset() = 0;
    virtual status_t    setLooping(int loop) = 0;
    virtual player_type playerType() = 0;
    virtual status_t    setParameter(int key, const Parcel &request) = 0;
    virtual status_t    getParameter(int key, Parcel *reply) = 0;

    // default no-op implementation of optional extensions
    virtual status_t setRetransmitEndpoint(const struct sockaddr_in* endpoint) {
        return INVALID_OPERATION;
    }
    virtual status_t getRetransmitEndpoint(struct sockaddr_in* endpoint) {
        return INVALID_OPERATION;
    }
    virtual status_t setNextPlayer(const sp<MediaPlayerBase>& next) {
        return OK;
    }

    // Invoke a generic method on the player by using opaque parcels
    // for the request and reply.
    //
    // @param request Parcel that is positioned at the start of the
    //                data sent by the java layer.
    // @param[out] reply Parcel to hold the reply data. Cannot be null.
    // @return OK if the call was successful.
    virtual status_t    invoke(const Parcel& request, Parcel *reply) = 0;

    // The Client in the MetadataPlayerService calls this method on
    // the native player to retrieve all or a subset of metadata.
    //
    // @param ids SortedList of metadata ID to be fetch. If empty, all
    //            the known metadata should be returned.
    // @param[inout] records Parcel where the player appends its metadata.
    // @return OK if the call was successful.
    virtual status_t    getMetadata(const media::Metadata::Filter& ids,
                                    Parcel *records) {
        return INVALID_OPERATION;
    };

    void        setNotifyCallback(
            void* cookie, notify_callback_f notifyFunc) {
        Mutex::Autolock autoLock(mNotifyLock);
        mCookie = cookie; mNotify = notifyFunc;
    }

    void        sendEvent(int msg, int ext1=0, int ext2=0,
                          const Parcel *obj=NULL) {
        notify_callback_f notifyCB;
        void* cookie;
        {
            Mutex::Autolock autoLock(mNotifyLock);
            notifyCB = mNotify;
            cookie = mCookie;
        }

        if (notifyCB) notifyCB(cookie, msg, ext1, ext2, obj);
    }

    virtual status_t dump(int fd, const Vector<String16> &args) const {
        return INVALID_OPERATION;
    }

    virtual status_t suspend() {
        return INVALID_OPERATION;
    }

    virtual status_t resume() {
        return INVALID_OPERATION;
    }

private:
    friend class MediaPlayerService;

    Mutex               mNotifyLock;
    void*               mCookie;
    notify_callback_f   mNotify;
};

// Implement this class for media players that use the AudioFlinger software mixer
class MediaPlayerInterface : public MediaPlayerBase
{
public:
    virtual             ~MediaPlayerInterface() { }
    virtual bool        hardwareOutput() { return false; }
    virtual void        setAudioSink(const sp<AudioSink>& audioSink) { mAudioSink = audioSink; }
protected:
    sp<AudioSink>       mAudioSink;
};

// Implement this class for media players that output audio directly to hardware
class MediaPlayerHWInterface : public MediaPlayerBase
{
public:
    virtual             ~MediaPlayerHWInterface() {}
    virtual bool        hardwareOutput() { return true; }
    virtual status_t    setVolume(float leftVolume, float rightVolume) = 0;
    virtual status_t    setAudioStreamType(audio_stream_type_t streamType) = 0;
};

}; // namespace android

#endif // __cplusplus


#endif // ANDROID_MEDIAPLAYERINTERFACE_H
