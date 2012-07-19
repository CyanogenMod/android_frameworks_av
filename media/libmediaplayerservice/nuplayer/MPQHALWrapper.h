/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (c) 2012, Code Aurora Forum. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
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


#ifndef MPQ_HAL_WRAPPER_H_

#define MPQ_HAL_WRAPPER_H_

#include <media/stagefright/foundation/AHandler.h>
#include "NuPlayerRenderer.h"
#include "NuPlayer.h"

#include <media/IAudioFlinger.h>
#include <binder/IServiceManager.h>

namespace android {

struct ABuffer;

struct NuPlayer::MPQHALWrapper : public AHandler {

    enum {
        kWhatFillThisBuffer      = 'fill',
        kWhatDrainThisBuffer     = 'drai',
        kWhatEOS                 = 'eos ',
        kWhatShutdownCompleted   = 'scom',
        kWhatFlushCompleted      = 'fcom',
        kWhatOutputFormatChanged = 'outC',
        kWhatError               = 'erro',
    };

    MPQHALWrapper(const sp<MediaPlayerBase::AudioSink> &sink, sp<Renderer> mRenderer);

    void setNotificationMessage(const sp<AMessage> &msg);
    void initiateSetup(const sp<AMessage> &msg);
    void signalFlush();
    void signalResume();
    void initiateShutdown();


protected:
    virtual ~MPQHALWrapper();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatSetup                   = 'setu',
        kWhatOMXMessage              = 'omx ',
        kWhatInputBufferFilled       = 'inpF',
        kWhatOutputBufferDrained     = 'outD',
        kWhatShutdown                = 'shut',
        kWhatFlush                   = 'flus',
        kWhatResume                  = 'resm',
        kWhatDrainDeferredMessages   = 'drai',
    };


    sp<AMessage> mNotify;
    sp<ALooper> mCodecLooper;

//================ Audio HAL Related Changes ================================
    sp<MediaPlayerBase::AudioSink> mAudioSink;


    //Structure to recieve the BT notification from the flinger.
    class MPQHALWrapperClient: public IBinder::DeathRecipient, public BnAudioFlingerClient {
        public:
            MPQHALWrapperClient(void *obj);

            NuPlayer::MPQHALWrapper *pBaseClass;
            // DeathRecipient
            virtual void binderDied(const wp<IBinder>& who);

            // IAudioFlingerClient
            // indicate a change in the configuration of an output or input: keeps the cached
            // values for output/input parameters upto date in client process
            virtual void ioConfigChanged(int event, audio_io_handle_t ioHandle, void *param2);

            friend class MPQHALWrapper;
    };

    //Audio Flinger related variables
    sp<IAudioFlinger> mAudioFlinger;
    sp<MPQHALWrapperClient> mAudioFlingerClient;
    Mutex mAudioFlingerLock;
    friend class MPQHALWrapperClient;
    int mAudioFormat;

    //Audio Parameters
    int mSampleRate;
    int32_t mNumChannels;
    int32_t mChannelMask;
    audio_stream_out_t* mPCMStream;
    uint32_t mInputBufferSize;
    bool mA2DPEnabled;
    sp<ABuffer> mLocalBuf;
    sp<Renderer> mRenderer;
    sp<ABuffer> mAudioBuffer;

    status_t getAudioFlinger();
//===================================================================
    void onSetup(const sp<AMessage> &msg);
    void onShutDown(const sp<AMessage> &msg);
    void onFlush(const sp<AMessage> &msg);
    void onInputBufferFilled(const sp<AMessage> &msg);
    void onOutputBufferDrained(const sp<AMessage> &msg);
    void postFillThisBuffer();
    void postAudioChangeEvent();
    void writeDataToAudioRenderer(sp<ABuffer> buffer);
    static size_t postEOS(MediaPlayerBase::AudioSink *audioSink,void *data, size_t size, void *me);
    DISALLOW_EVIL_CONSTRUCTORS(MPQHALWrapper);
};

}  // namespace android

#endif  // MPQ_HAL_WRAPPER_H_
