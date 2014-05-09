/*
**
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "IAudioRecord"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <stdint.h>
#include <sys/types.h>

#include <binder/Parcel.h>

#include <media/IAudioRecord.h>

namespace android {

enum {
    UNUSED_WAS_GET_CBLK = IBinder::FIRST_CALL_TRANSACTION,
    START,
    STOP
};

class BpAudioRecord : public BpInterface<IAudioRecord>
{
public:
    BpAudioRecord(const sp<IBinder>& impl)
        : BpInterface<IAudioRecord>(impl)
    {
    }

    virtual status_t start(int /*AudioSystem::sync_event_t*/ event, int triggerSession)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioRecord::getInterfaceDescriptor());
        data.writeInt32(event);
        data.writeInt32(triggerSession);
        status_t status = remote()->transact(START, data, &reply);
        if (status == NO_ERROR) {
            status = reply.readInt32();
        } else {
            ALOGW("start() error: %s", strerror(-status));
        }
        return status;
    }

    virtual void stop()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioRecord::getInterfaceDescriptor());
        remote()->transact(STOP, data, &reply);
    }

};

IMPLEMENT_META_INTERFACE(AudioRecord, "android.media.IAudioRecord");

// ----------------------------------------------------------------------

status_t BnAudioRecord::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {
        case START: {
            CHECK_INTERFACE(IAudioRecord, data, reply);
            int /*AudioSystem::sync_event_t*/ event = data.readInt32();
            int triggerSession = data.readInt32();
            reply->writeInt32(start(event, triggerSession));
            return NO_ERROR;
        } break;
        case STOP: {
            CHECK_INTERFACE(IAudioRecord, data, reply);
            stop();
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

}; // namespace android
