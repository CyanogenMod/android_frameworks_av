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

#define LOG_TAG "IMediaExtractorService"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <stdint.h>
#include <sys/types.h>
#include <binder/Parcel.h>
#include <media/IMediaExtractorService.h>

namespace android {

enum {
    MAKE_EXTRACTOR = IBinder::FIRST_CALL_TRANSACTION
};

class BpMediaExtractorService : public BpInterface<IMediaExtractorService>
{
public:
    BpMediaExtractorService(const sp<IBinder>& impl)
        : BpInterface<IMediaExtractorService>(impl)
    {
    }

    virtual sp<IMediaExtractor> makeExtractor(const sp<IDataSource> &source, const char *mime) {
        Parcel data, reply;
        data.writeInterfaceToken(IMediaExtractorService::getInterfaceDescriptor());
        data.writeStrongBinder(IInterface::asBinder(source));
        if (mime != NULL) {
            data.writeCString(mime);
        }
        status_t ret = remote()->transact(MAKE_EXTRACTOR, data, &reply);
        if (ret == NO_ERROR) {
            return interface_cast<IMediaExtractor>(reply.readStrongBinder());
        }
        return NULL;
    }

};

IMPLEMENT_META_INTERFACE(MediaExtractorService, "android.media.IMediaExtractorService");

// ----------------------------------------------------------------------

status_t BnMediaExtractorService::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {

        case MAKE_EXTRACTOR: {
            CHECK_INTERFACE(IMediaExtractorService, data, reply);
            sp<IBinder> b;
            status_t ret = data.readStrongBinder(&b);
            if (ret != NO_ERROR || b == NULL) {
                ALOGE("Error reading source from parcel");
                return ret;
            }
            // If we make an extractor through Binder, enabled shared memory
            // for MediaBuffers for this process.
            MediaBuffer::useSharedMemory();
            sp<IDataSource> source = interface_cast<IDataSource>(b);
            const char *mime = data.readCString();
            sp<IMediaExtractor> ex = makeExtractor(source, mime);
            reply->writeStrongBinder(IInterface::asBinder(ex));
            return NO_ERROR;
        }
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

} // namespace android
