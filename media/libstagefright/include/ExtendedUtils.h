/* Copyright (c) 2013 - 2014, The Linux Foundation. All rights reserved.
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
#ifndef EXTENDED_UTILS_H_
#define EXTENDED_UTILS_H_

#include <utils/StrongPointer.h>
#include <media/Metadata.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MPEG4Writer.h>

#include <media/MediaRecorderBase.h>
#include <media/stagefright/MediaExtractor.h>
#include <camera/CameraParameters.h>
#include <OMX_Video.h>
#include <android/native_window.h>

#define MIN_BITERATE_AAC 24000
#define MAX_BITERATE_AAC 192000

#define IPV4 4
#define IPV6 6

namespace android {

/*
 * This class is a placeholder for utility methods for
 * QC specific changes
 */
struct ExtendedUtils {

    /*
     * This class is a placeholder for the set of methods used
     * to enable HFR (High Frame Rate) Recording
     *
     * HFR is a slow-motion recording feature where framerate
     * is increased at capture, but file is composed to play
     * back at normal rate, giving a net result of slow-motion.
     * If HFR factor = N
     *   framerate (at capture and encoder) = N * actual value
     *   bitrate = N * actual value
     *      (as the encoder still gets actual timestamps)
     *   timeStamps (at composition) = actual value
     *   timeScale (at composition) = actual value / N
     *      (when parser re-generates timestamps, they will be
     *       up-scaled by factor N, which results in slow-motion)
     *
     * HSR is a high-framerate recording variant where timestamps
     * are not meddled with, yielding a video mux'ed at captured
     * fps
     */
    struct HFR {
        // set kKeyHFR when 'video-hfr' paramater is enabled
        // or set kKeyHSR when 'video-hsr' paramater is enabled
        static void setHFRIfEnabled(
                const CameraParameters& params, sp<MetaData> &meta);

        // recalculate file-duration when HFR is enabled
        static status_t initializeHFR(
                const sp<MetaData> &meta, sp<AMessage> &format,
                int64_t &maxFileDurationUs, video_encoder videoEncoder);

        static void setHFRRatio(
                sp<MetaData> &meta, const int32_t hfrRatio);

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
     * This class is a placeholder for set of methods used
     * to enable HEVC muxing
     */

    struct HEVCParamSet {
        HEVCParamSet(uint16_t length, const uint8_t *data)
               : mLength(length), mData(data) {}

        uint16_t mLength;
        const uint8_t *mData;
    };

    struct HEVCMuxer {
        static void writeHEVCFtypBox(MPEG4Writer *writer);

        static status_t makeHEVCCodecSpecificData(const uint8_t *data,
                  size_t size, void** codecSpecificData,
                  size_t *codecSpecificDataSize);

        static void beginHEVCBox(MPEG4Writer *writer);

        static void writeHvccBox(MPEG4Writer *writer,
                  void* codecSpecificData, size_t codecSpecificDataSize,
                  bool useNalLengthFour);

        static bool isVideoHEVC(const char* mime);

        static bool getHEVCCodecConfigData(const sp<MetaData> &meta,
                  const void **data, size_t *size);

        private:

        static status_t extractNALRBSPData(const uint8_t *data, size_t size,
                  uint8_t **header, bool *alreadyFilled);

        static status_t parserProfileTierLevel(const uint8_t *data, size_t size,
                  uint8_t **header, bool *alreadyFilled);

        static const uint8_t *parseHEVCParamSet(const uint8_t *data, size_t length,
                  List<HEVCParamSet> &paramSetList, size_t *paramSetLen);

        static size_t parseHEVCCodecSpecificData(const uint8_t *data, size_t size,
                  List<HEVCParamSet> &vidParamSet, List<HEVCParamSet> &seqParamSet,
                  List<HEVCParamSet> &picParamSet );
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
                int32_t &videoEncoderProfile, int32_t &videoEncoderLevel);

        static bool isSmoothStreamingEnabled();

        static int64_t getMaxAVSyncLateMargin();

        //helper function to parse rtp port range form system property
        static void getRtpPortRange(unsigned *start, unsigned *end);
    };

    struct RTSPStream {

        static bool ParseURL_V6(
                AString *host, const char **colonPos);

        // Creates a pair of UDP datagram sockets bound to adjacent ports
        // (the rtpSocket is bound to an even port, the rtcpSocket to the
        // next higher port) for IPV6.
        static void MakePortPair_V6(
                int *rtpSocket, int *rtcpSocket, unsigned *rtpPort);

        // In case we're behind NAT, fire off two UDP packets to the remote
        // rtp/rtcp ports to poke a hole into the firewall for future incoming
        // packets. We're going to send an RR/SDES RTCP packet to both of them.
        static bool pokeAHole_V6(int rtpSocket, int rtcpSocket,
                 const AString &transport, AString &sessionHost);

        private:

        static void bumpSocketBufferSize_V6(int s);

        static bool GetAttribute(const char *s, const char *key, AString *value);

        static void addRR(const sp<ABuffer> &buf);

        static void addSDES(int s, const sp<ABuffer> &buffer);

    };


    static const int32_t kNumBFramesPerPFrame = 1;
    static bool mIsQCHWAACEncoder;

    //set B frames for MPEG4
    static void setBFrames(OMX_VIDEO_PARAM_MPEG4TYPE &mpeg4type,
            const char* componentName);

    //set B frames for H264
    static void setBFrames(OMX_VIDEO_PARAM_AVCTYPE &h264type,
            const int32_t iFramesInterval, const int32_t frameRate,
            const char* componentName);

    static bool UseQCHWAACEncoder(audio_encoder Encoder = AUDIO_ENCODER_DEFAULT, int32_t Channel = 0,
            int32_t BitRate = 0, int32_t SampleRate = 0);

    static sp<MediaExtractor> MediaExtractor_CreateIfNeeded(
            sp<MediaExtractor> defaultExt, const sp<DataSource> &source,
            const char *mime);

    static bool isAVCProfileSupported(int32_t profile);

    //notify stride change to ANW
    static void updateNativeWindowBufferGeometry(ANativeWindow* anw,
            OMX_U32 width, OMX_U32 height, OMX_COLOR_FORMATTYPE colorFormat);

    static bool checkIsThumbNailMode(const uint32_t flags, char* componentName);
    
    static void setArbitraryModeIfInterlaced(const uint8_t *ptr, const sp<MetaData> &meta);

    static int32_t checkIsInterlace(sp<MetaData> &meta);

    static bool isVideoMuxFormatSupported(const char *mime);

    static void printFileName(int fd);
    static void applyPreRotation(
            const CameraParameters& params, sp<MetaData> &meta);

    static bool isAudioAMR(const char* mime);

    static void updateVideoTrackInfoFromESDS_MPEG4Video(sp<MetaData> meta);
    static bool checkDPFromVOLHeader(const uint8_t *ptr, size_t size);
    static bool checkDPFromCodecSpecificData(const uint8_t *ptr, size_t size);

    static bool isPcmOffloadEnabled();

    static bool pcmOffloadException(const char* const mime);
};

}
#endif  //EXTENDED_UTILS_H_
