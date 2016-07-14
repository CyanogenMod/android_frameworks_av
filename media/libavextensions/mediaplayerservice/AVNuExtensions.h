/*
 * Copyright (c) 2013 - 2016, The Linux Foundation. All rights reserved.
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
#ifndef _AV_NU_EXTENSIONS_H_
#define _AV_NU_EXTENSIONS_H_

#include <common/AVExtensionsCommon.h>

namespace android {

struct NuPlayer;
/*
 * Factory to create extended NuPlayer objects
 */
struct AVNuFactory {
    virtual sp<NuPlayer> createNuPlayer(pid_t pid);

    virtual sp<NuPlayer::DecoderBase> createPassThruDecoder(
            const sp<AMessage> &notify,
            const sp<NuPlayer::Source> &source,
            const sp<NuPlayer::Renderer> &renderer);

    virtual sp<NuPlayer::DecoderBase> createDecoder(
            const sp<AMessage> &notify,
            const sp<NuPlayer::Source> &source,
            pid_t pid,
            const sp<NuPlayer::Renderer> &renderer);

    virtual sp<NuPlayer::Renderer> createRenderer(
            const sp<MediaPlayerBase::AudioSink> &sink,
            const sp<AMessage> &notify,
            uint32_t flags);

    // ----- NO TRESSPASSING BEYOND THIS LINE ------
    DECLARE_LOADABLE_SINGLETON(AVNuFactory);
};

/*
 * Common delegate to the classes in NuPlayer
 */
struct AVNuUtils {
    virtual bool isVorbisFormat(const sp<MetaData> &);

    virtual void printFileName(int fd);
    virtual bool dropCorruptFrame();
    virtual void addFlagsInMeta(const sp<ABuffer> &buffer, int32_t flags, bool isAudio);
    virtual void checkFormatChange(bool *formatChange, const sp<ABuffer> &accessUnit);
    virtual void overWriteAudioOutputFormat(sp <AMessage> &dst, const sp <AMessage> &src);
    virtual bool pcmOffloadException(const sp<AMessage> &);
    virtual audio_format_t getPCMFormat(const sp<AMessage> &);
    virtual void setCodecOutputFormat(const sp<AMessage> &);
    virtual bool isByteStreamModeEnabled(const sp<MetaData> &);

    // ----- NO TRESSPASSING BEYOND THIS LINE ------
    DECLARE_LOADABLE_SINGLETON(AVNuUtils);
};

}

#endif // _AV_EXTENSIONS__H_
