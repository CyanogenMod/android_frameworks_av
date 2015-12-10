/*
 * Copyright (c) 2013 - 2015, The Linux Foundation. All rights reserved.
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

#ifndef _AV_MEDIA_EXTENSIONS_H_
#define _AV_MEDIA_EXTENSIONS_H_

#include <common/AVExtensionsCommon.h>
#include <hardware/audio.h>
#include <media/AudioTrack.h>
#include <audio_utils/format.h>

namespace android {

class MediaRecorder;
class Parcel;
/*
 * Common delegate to the classes in libstagefright
 */
struct AVMediaUtils {

    virtual bool AudioTrackIsPcmOffloaded(const audio_format_t format) {
        return audio_is_offload_pcm(format);
    }
    virtual status_t AudioTrackGetPosition(AudioTrack* /*track*/,
            uint32_t* /*position*/) {
        return NO_INIT;
    }

    virtual status_t AudioTrackGetTimestamp(AudioTrack* /*track*/,
            AudioTimestamp* /*timestamp*/) {
        return NO_INIT;
    }

    virtual size_t AudioTrackGetOffloadFrameCount(size_t frameCount) {
        return frameCount;
    }

    virtual bool AudioTrackIsTrackOffloaded(audio_io_handle_t /*output*/) {
        return false;
    }

    virtual sp<MediaRecorder> createMediaRecorder(const String16& opPackageName);
    virtual void writeCustomData(
            Parcel * /* reply */, void * /* buffer_data */) {}
    virtual void readCustomData(
            const Parcel * /* reply */, void ** /*buffer_data */ ) {}
    virtual void closeFileDescriptor(void * /* buffer_ptr */) {}
    // ----- NO TRESSPASSING BEYOND THIS LINE ------
    DECLARE_LOADABLE_SINGLETON(AVMediaUtils);
};

} //namespace android

#endif //_AV_MEDIA_EXTENSIONS_H_

