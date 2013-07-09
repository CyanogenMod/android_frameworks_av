/*Copyright (c) 2013, The Linux Foundation. All rights reserved.
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
#ifndef RESOURCEMANAGER_H_
#define RESOURCEMANAGER_H_

#include <utils/StrongPointer.h>
#include <media/Metadata.h>
#include <media/stagefright/MediaSource.h>
#include <media/MediaRecorderBase.h>

#include <media/stagefright/AudioPlayer.h>
#include <media/stagefright/foundation/AMessage.h>

namespace android {

struct ResourceManager {

    struct AudioConcurrencyInfo {

        static status_t resetParameter(String8 &useCase, bool &useCaseFlag,
                uint32_t codecFlags = 0);

        static status_t setNonCodecParameter(String8 &useCase, bool &flag,
                uint32_t codecFlags = 0, const char * mime = NULL);

        static status_t findUseCaseAndSetParameter(const char *mime,
             const char *componentName, bool isDecoder,
             String8 &useCase, bool &useCaseFlag, uint32_t codecFlags = 0);

        static status_t updateConcurrencyParam(const sp<AMessage> &msg,
                String8 &useCase, bool &useCaseFlag);

        static status_t updateConcurrencyParam(String8 &useCase,
                bool &useCaseFlag, bool pause, uint32_t codecFlag = 0);

        static void  setULLStream(
               const sp<MediaPlayerBase::AudioSink> &audioSink, uint32_t &flags);
private:

        static status_t setParameter(String8 useCase, bool value);

        static void findUseCase(const char *mime, const char *componentName,
            bool isDecoder, String8 &useCase);

        static void modifyUseCaseMetaData(String8 &useCaseDst, bool &flagDst,
            String8 useCase, bool flag);

        static Mutex mLock;
   };

    static bool isLPAPlayback(const sp<MediaSource> &audioTrack,
        const sp<MediaSource> &videoSource, const AudioPlayer * audioPlayer,
        const sp<MediaPlayerBase::AudioSink> &audioSink,
        int64_t &durationUs, String8 &useCase,
        bool &useCaseFlag);

    static bool isStreamMusic(const sp<MediaPlayerBase::AudioSink> &audioSink);
};

}
#endif  //RESOURCEMANAGER_H_
