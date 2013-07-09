
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
#ifndef TRACKUTILS_H_
#define TRACKUTILS_H_

#include <system/audio.h>
#include <sys/types.h>
#include <utils/Errors.h>
#include <sys/resource.h>

namespace android {

struct TrackUtils {

    static void setFastFlag(audio_stream_type_t &streamType,
            audio_output_flags_t &flags);

    static void  isClientLivesLocally(bool &livesLocally);

    static bool SetConcurrencyParameterForRemotePlaybackSession(
            audio_stream_type_t &streamType, audio_format_t &format,
            audio_output_flags_t &flags,  bool active);

    static bool SetConcurrencyParameterForRemoteRecordSession(
        audio_source_t inputSource, audio_format_t format, uint32_t samplerate, uint32_t channels, bool active);

    static status_t setParameterForConcurrency(String8 useCase,
        bool value);

    static Mutex mLock;

};
}
#endif //TRACKUTILS_H_
