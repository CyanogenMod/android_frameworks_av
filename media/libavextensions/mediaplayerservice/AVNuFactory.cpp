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

//#define LOG_NDEBUG 0
#define LOG_TAG "AVNuFactory"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>

#include <nuplayer/NuPlayer.h>
#include <nuplayer/NuPlayerDecoderBase.h>
#include <nuplayer/NuPlayerDecoderPassThrough.h>
#include <nuplayer/NuPlayerDecoder.h>
#include <nuplayer/NuPlayerCCDecoder.h>
#include <gui/Surface.h>
#include <nuplayer/NuPlayerSource.h>
#include <nuplayer/NuPlayerRenderer.h>

#include "common/ExtensionsLoader.hpp"
#include "mediaplayerservice/AVNuExtensions.h"

namespace android {

sp<NuPlayer> AVNuFactory::createNuPlayer(pid_t pid) {
    return new NuPlayer(pid);
}

sp<NuPlayer::DecoderBase> AVNuFactory::createPassThruDecoder(
            const sp<AMessage> &notify,
            const sp<NuPlayer::Source> &source,
            const sp<NuPlayer::Renderer> &renderer) {
    return new NuPlayer::DecoderPassThrough(notify, source, renderer);
}

sp<NuPlayer::DecoderBase> AVNuFactory::createDecoder(
            const sp<AMessage> &notify,
            const sp<NuPlayer::Source> &source,
            pid_t pid,
            const sp<NuPlayer::Renderer> &renderer) {
    return new NuPlayer::Decoder(notify, source, pid, renderer);
}

sp<NuPlayer::Renderer> AVNuFactory::createRenderer(
            const sp<MediaPlayerBase::AudioSink> &sink,
            const sp<AMessage> &notify,
            uint32_t flags) {
    return new NuPlayer::Renderer(sink, notify, flags);
}

// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVNuFactory::AVNuFactory() {
}

AVNuFactory::~AVNuFactory() {
}

//static
AVNuFactory *AVNuFactory::sInst =
        ExtensionsLoader<AVNuFactory>::createInstance("createExtendedNuFactory");

} //namespace android

