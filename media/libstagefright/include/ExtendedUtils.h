/*Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
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
#ifndef QC_UTILS_H_
#define QC_UTILS_H_

#include <utils/StrongPointer.h>
#include <media/Metadata.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/MediaCodecList.h>

#include <media/MediaRecorderBase.h>
#include <media/stagefright/MediaExtractor.h>
#include <camera/CameraParameters.h>
#include <OMX_Video.h>
#include <android/native_window.h>

#include <gui/DisplayEventReceiver.h>
#include <utils/Looper.h>

#define MIN_BITERATE_AAC 24000
#define MAX_BITERATE_AAC 320000

namespace android {

/*
 * This class is a placeholder for utility methods for
 * QC specific changes
 */
struct ExtendedUtils {

    /*
     * This class is a placeholder for set of methods used
     * to enable HFR (High Frame Rate) Recording
     *
     * HFR is a slow-motion recording feature where framerate
     * is increased at capture, but file is composed to play
     * back at normal rate, giving a net result of slow-motion.
     * If HFR factor = N
     *   framerate (at capture and encoder) = N * actual value
     *   timeStamps (at composition) = N * actual value
     *   bitrate = N * actual value
     *      (as the encoder still gets actual timestamps)
     */
    struct HFR {
        // set kKeyHFR when 'video-hfr' paramater is enabled
        static void setHFRIfEnabled(
                const CameraParameters& params, sp<MetaData> &meta);

        // recalculate fileduration when hfr is enabled
        static status_t initializeHFR(
                sp<MetaData> &meta, sp<MetaData> &enc_meta,
                int64_t &maxFileDurationUs, video_encoder videoEncoder);

        // Copy HFR params (bitrate,framerate) from output to
        // to input format, if HFR is enabled
        static void copyHFRParams(
                const sp<MetaData> &inputFormat,
                sp<MetaData> &outputFormat);

        // Adjust clip timescale for authoring, if HFR is enabled
        static int32_t getHFRRatio(
                const sp<MetaData> &meta);

        private:
        // Query supported capabilities from target-specific profiles
        static int32_t getHFRCapabilities(
                video_encoder codec,
                int& maxHFRWidth, int& maxHFRHeight, int& maxHFRFps,
                int& maxBitrate);
    };

    /*
     * This class is a placeholder for methods to override
     * default heaviour based on shell properties set
     */
    struct ShellProp {
        // check if shell property to disable audio is set
        static bool isAudioDisabled(bool isEncoder);

        //helper function to set encoding profiles
        static void setEncoderProfile(video_encoder &videoEncoder,
                int32_t &videoEncoderProfile);

        static bool isSmoothStreamingEnabled();

        static int64_t getMaxAVSyncLateMargin();

        static bool isCustomAVSyncEnabled();

        static bool isMpeg4DPSupportedByHardware();
    };

    //set B frames for MPEG4
    static void setBFrames(OMX_VIDEO_PARAM_MPEG4TYPE &mpeg4type, int32_t &numBFrames,
            const char* componentName);

    //set B frames for H264
    static void setBFrames(OMX_VIDEO_PARAM_AVCTYPE &h264type, int32_t &numBFrames,
            int32_t iFramesInterval, int32_t frameRate, const char* componentName);

    static bool UseQCHWAACEncoder(audio_encoder Encoder, int32_t Channel,
            int32_t BitRate, int32_t SampleRate);

    static sp<MediaExtractor> MediaExtractor_CreateIfNeeded(
            sp<MediaExtractor> defaultExt, const sp<DataSource> &source,
            const char *mime);

    //helper function to add media codecs with specific quirks
    static void helper_addMediaCodec(Vector<MediaCodecList::CodecInfo> &mCodecInfos,
                                     KeyedVector<AString, size_t> &mTypes,
                                     bool encoder, const char *name,
                                     const char *type, uint32_t quirks);

    //helper function to calculate the value of quirks from strings
    static uint32_t helper_getCodecSpecificQuirks(KeyedVector<AString, size_t> &mCodecQuirks,
                                                  Vector<AString> quirks);

    static bool isAVCProfileSupported(int32_t profile);

    //notify stride change to ANW
    static void updateNativeWindowBufferGeometry(ANativeWindow* anw,
            OMX_U32 width, OMX_U32 height, OMX_COLOR_FORMATTYPE colorFormat);

    static bool checkIsThumbNailMode(const uint32_t flags, char* componentName);

    //helper function for MPEG4 Extractor to check for AC3/EAC3 contents
    static void helper_Mpeg4ExtractorCheckAC3EAC3(MediaBuffer *buffer, sp<MetaData> &format,
                                                   size_t size);

    static int32_t getEncoderTypeFlags();

    static void prefetchSecurePool(int fd);

    static void prefetchSecurePool(const char *uri);

    static void prefetchSecurePool();

    static void createSecurePool();

    static void drainSecurePool();

    //helper function to parse rtp port range form system property
    static void parseRtpPortRangeFromSystemProperty(unsigned *start, unsigned *end);

    static void updateOutputBitWidth(sp<MetaData> format, bool isOffload);
};

class VSyncLocker : public RefBase {
private:
    //Number of frames profiled for calculating fps
    static const int kMaxProfileCount = 60;

    enum SyncState {
        PROFILE_FPS,
        ENABLE_SYNC,
        BLOCK_SYNC,
    };

    volatile bool mExitVsyncEvent;
    Looper *mLooper;
    SyncState mSyncState;
    int64_t mStartTime;
    int mProfileCount;

    pthread_t mThread;
    Mutex mVsyncLock;
    Condition mVSyncCondition;
    DisplayEventReceiver mDisplayEventReceiver;

    virtual void updateSyncState();
    virtual void waitOnVSync();

public:
    static bool isSyncRenderEnabled();
    static int receiver(int fd, int events, void *context);
    static void *ThreadWrapper(void *context);

    virtual void resetProfile();
    virtual void blockSync();
    virtual void blockOnVSync();
    virtual void start();
    void VSyncEvent();
    void signalVSync();

    virtual ~VSyncLocker();
    VSyncLocker();
};

}
#endif  //QC_UTILS_H_
