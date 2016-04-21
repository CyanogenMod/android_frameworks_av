/*
 * Copyright (C) 2009 The Android Open Source Project
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

#ifndef DATA_SOURCE_H_

#define DATA_SOURCE_H_

#include <sys/types.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaErrors.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/List.h>
#include <utils/RefBase.h>
#include <utils/threads.h>
#include <drm/DrmManagerClient.h>
#include <media/stagefright/foundation/AMessage.h>

namespace android {

struct AString;
class  IDataSource;
struct IMediaHTTPService;
class String8;
struct HTTPBase;

class DataSource : public RefBase {
public:
    enum Flags {
        kWantsPrefetching      = 1,
        kStreamedFromLocalHost = 2,
        kIsCachingDataSource   = 4,
        kIsHTTPBasedSource     = 8,
    };

    static sp<DataSource> CreateFromURI(
            const sp<IMediaHTTPService> &httpService,
            const char *uri,
            const KeyedVector<String8, String8> *headers = NULL,
            String8 *contentType = NULL,
            HTTPBase *httpSource = NULL,
            bool useExtendedCache = false);

    static sp<DataSource> CreateMediaHTTP(const sp<IMediaHTTPService> &httpService);
    static sp<DataSource> CreateFromIDataSource(const sp<IDataSource> &source);

    DataSource() : mMeta(new AMessage) {}

    virtual status_t initCheck() const = 0;

    // Returns the number of bytes read, or -1 on failure. It's not an error if
    // this returns zero; it just means the given offset is equal to, or
    // beyond, the end of the source.
    virtual ssize_t readAt(off64_t offset, void *data, size_t size) = 0;

    // Convenience methods:
    bool getUInt16(off64_t offset, uint16_t *x);
    bool getUInt24(off64_t offset, uint32_t *x); // 3 byte int, returned as a 32-bit int
    bool getUInt32(off64_t offset, uint32_t *x);
    bool getUInt64(off64_t offset, uint64_t *x);

    // Reads in "count" entries of type T into vector *x.
    // Returns true if "count" entries can be read.
    // If fewer than "count" entries can be read, return false. In this case,
    // the output vector *x will still have those entries that were read. Call
    // x->size() to obtain the number of entries read.
    // The optional parameter chunkSize specifies how many entries should be
    // read from the data source at one time into a temporary buffer. Increasing
    // chunkSize can improve the performance at the cost of extra memory usage.
    // The default value for chunkSize is set to read at least 4k bytes at a
    // time, depending on sizeof(T).
    template <typename T>
    bool getVector(off64_t offset, Vector<T>* x, size_t count,
                   size_t chunkSize = (4095 / sizeof(T)) + 1);

    // May return ERROR_UNSUPPORTED.
    virtual status_t getSize(off64_t *size);

    virtual uint32_t flags() {
        return 0;
    }

    virtual status_t reconnectAtOffset(off64_t offset) {
        return ERROR_UNSUPPORTED;
    }

    ////////////////////////////////////////////////////////////////////////////

    bool sniff(String8 *mimeType, float *confidence, sp<AMessage> *meta);

    // The sniffer can optionally fill in "meta" with an AMessage containing
    // a dictionary of values that helps the corresponding extractor initialize
    // its state without duplicating effort already exerted by the sniffer.
    typedef bool (*SnifferFunc)(
            const sp<DataSource> &source, String8 *mimeType,
            float *confidence, sp<AMessage> *meta);

    static void RegisterDefaultSniffers();

    // for DRM
    virtual sp<DecryptHandle> DrmInitialization(const char *mime = NULL) {
        return NULL;
    }
    virtual void getDrmInfo(sp<DecryptHandle> &handle, DrmManagerClient **client) {};

    virtual String8 getUri() {
        return String8();
    }

    virtual String8 getMIMEType() const;

    virtual sp<AMessage> meta() { return mMeta; }

protected:
    virtual ~DataSource() {}

private:
    sp<AMessage> mMeta;

    static Mutex gSnifferMutex;
    static List<SnifferFunc> gSniffers;
    static List<SnifferFunc> gExtraSniffers;
    static bool gSniffersRegistered;

    static void RegisterSniffer_l(SnifferFunc func);
    static void RegisterSnifferPlugin();

    DataSource(const DataSource &);
    DataSource &operator=(const DataSource &);
};

template <typename T>
bool DataSource::getVector(off64_t offset, Vector<T>* x, size_t count,
                           size_t chunkSize)
{
    x->clear();
    if (chunkSize == 0) {
        return false;
    }
    if (count == 0) {
        return true;
    }

    T tmp[chunkSize];
    ssize_t numBytesRead;
    size_t numBytesPerChunk = chunkSize * sizeof(T);
    size_t i;

    for (i = 0; i + chunkSize < count; i += chunkSize) {
        // This loops is executed when more than chunkSize records need to be
        // read.
        numBytesRead = this->readAt(offset, (void*)&tmp, numBytesPerChunk);
        if (numBytesRead == -1) { // If readAt() returns -1, there is an error.
            return false;
        }
        if (numBytesRead < numBytesPerChunk) {
            // This case is triggered when the stream ends before the whole
            // chunk is read.
            x->appendArray(tmp, (size_t)numBytesRead / sizeof(T));
            return false;
        }
        x->appendArray(tmp, chunkSize);
        offset += numBytesPerChunk;
    }

    // There are (count - i) more records to read.
    // Right now, (count - i) <= chunkSize.
    // We do the same thing as above, but with chunkSize replaced by count - i.
    numBytesRead = this->readAt(offset, (void*)&tmp, (count - i) * sizeof(T));
    if (numBytesRead == -1) {
        return false;
    }
    x->appendArray(tmp, (size_t)numBytesRead / sizeof(T));
    return x->size() == count;
}

}  // namespace android

#endif  // DATA_SOURCE_H_
