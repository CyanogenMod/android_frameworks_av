/*
 * Copyright (c) 2012, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
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

#ifndef ANDROID_IDIRECTTRACK_H
#define ANDROID_IDIRECTTRACK_H

#include <stdint.h>
#include <sys/types.h>

#include <utils/RefBase.h>
#include <utils/Errors.h>
#include <binder/IInterface.h>
#include <binder/IMemory.h>


namespace android {

// ----------------------------------------------------------------------------

class IDirectTrack : public IInterface
{
public:
    DECLARE_META_INTERFACE(DirectTrack);

    /* After it's created the track is not active. Call start() to
     * make it active. If set, the callback will start being called.
     */
    virtual status_t    start() = 0;

    /* Stop a track. If set, the callback will cease being called and
     * obtainBuffer will return an error. Buffers that are already released
     * will be processed, unless flush() is called.
     */
    virtual void        stop() = 0;

    /* flush a stopped track. All pending buffers are discarded.
     * This function has no effect if the track is not stoped.
     */
    virtual void        flush() = 0;

    /* mute or unmutes this track.
     * While mutted, the callback, if set, is still called.
     */
    virtual void        mute(bool) = 0;

    /* Pause a track. If set, the callback will cease being called and
     * obtainBuffer will return an error. Buffers that are already released
     * will be processed, unless flush() is called.
     */
    virtual void        pause() = 0;

    /* set volume for both left and right channels.
     */
    virtual void        setVolume(float l, float r) = 0;

    virtual ssize_t     write(const void*, size_t) =  0;

    virtual int64_t     getTimeStamp() =  0;
};

// ----------------------------------------------------------------------------

class BnDirectTrack : public BnInterface<IDirectTrack>
{
public:
    virtual status_t    onTransact( uint32_t code,
                                    const Parcel& data,
                                    Parcel* reply,
                                    uint32_t flags = 0);
};

// ----------------------------------------------------------------------------

}; // namespace android

#endif // ANDROID_IAUDIOTRACK_H
