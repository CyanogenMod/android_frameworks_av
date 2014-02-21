/*
 * Copyright (C) 2013 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaHTTP"
#include <utils/Log.h>

#include <media/stagefright/MediaHTTP.h>

#include <binder/IServiceManager.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/Utils.h>

#include <media/IMediaHTTPConnection.h>

namespace android {

MediaHTTP::MediaHTTP(const sp<IMediaHTTPConnection> &conn)
    : mInitCheck(NO_INIT),
      mHTTPConnection(conn),
      mCachedSizeValid(false),
      mCachedSize(0ll),
      mDrmManagerClient(NULL) {
    mInitCheck = OK;
}

MediaHTTP::~MediaHTTP() {
    clearDRMState_l();
}

status_t MediaHTTP::connect(
        const char *uri,
        const KeyedVector<String8, String8> *headers,
        off64_t /* offset */) {
    if (mInitCheck != OK) {
        return mInitCheck;
    }

    KeyedVector<String8, String8> extHeaders;
    if (headers != NULL) {
        extHeaders = *headers;
    }
    extHeaders.add(String8("User-Agent"), String8(MakeUserAgent().c_str()));

    bool success = mHTTPConnection->connect(uri, &extHeaders);

    mLastHeaders = extHeaders;
    mLastURI = uri;

    mCachedSizeValid = false;

    return success ? OK : UNKNOWN_ERROR;
}

void MediaHTTP::disconnect() {
    if (mInitCheck != OK) {
        return;
    }

    mHTTPConnection->disconnect();
}

status_t MediaHTTP::initCheck() const {
    return mInitCheck;
}

ssize_t MediaHTTP::readAt(off64_t offset, void *data, size_t size) {
    if (mInitCheck != OK) {
        return mInitCheck;
    }

    int64_t startTimeUs = ALooper::GetNowUs();

    size_t numBytesRead = 0;
    while (numBytesRead < size) {
        size_t copy = size - numBytesRead;

        if (copy > 64 * 1024) {
            // limit the buffer sizes transferred across binder boundaries
            // to avoid spurious transaction failures.
            copy = 64 * 1024;
        }

        ssize_t n = mHTTPConnection->readAt(
                offset + numBytesRead, (uint8_t *)data + numBytesRead, copy);

        if (n < 0) {
            return n;
        } else if (n == 0) {
            break;
        }

        numBytesRead += n;
    }

    int64_t delayUs = ALooper::GetNowUs() - startTimeUs;

    addBandwidthMeasurement(numBytesRead, delayUs);

    return numBytesRead;
}

status_t MediaHTTP::getSize(off64_t *size) {
    if (mInitCheck != OK) {
        return mInitCheck;
    }

    // Caching the returned size so that it stays valid even after a
    // disconnect. NuCachedSource2 relies on this.

    if (!mCachedSizeValid) {
        mCachedSize = mHTTPConnection->getSize();
        mCachedSizeValid = true;
    }

    *size = mCachedSize;

    return *size < 0 ? *size : OK;
}

uint32_t MediaHTTP::flags() {
    return kWantsPrefetching | kIsHTTPBasedSource;
}

status_t MediaHTTP::reconnectAtOffset(off64_t offset) {
    return connect(mLastURI.c_str(), &mLastHeaders, offset);
}

// DRM...

sp<DecryptHandle> MediaHTTP::DrmInitialization(const char* mime) {
    if (mDrmManagerClient == NULL) {
        mDrmManagerClient = new DrmManagerClient();
    }

    if (mDrmManagerClient == NULL) {
        return NULL;
    }

    if (mDecryptHandle == NULL) {
        mDecryptHandle = mDrmManagerClient->openDecryptSession(
                String8(mLastURI.c_str()), mime);
    }

    if (mDecryptHandle == NULL) {
        delete mDrmManagerClient;
        mDrmManagerClient = NULL;
    }

    return mDecryptHandle;
}

void MediaHTTP::getDrmInfo(
        sp<DecryptHandle> &handle, DrmManagerClient **client) {
    handle = mDecryptHandle;
    *client = mDrmManagerClient;
}

String8 MediaHTTP::getUri() {
    String8 uri;
    if (OK == mHTTPConnection->getUri(&uri)) {
        return uri;
    }
    return String8(mLastURI.c_str());
}

String8 MediaHTTP::getMIMEType() const {
    if (mInitCheck != OK) {
        return String8("application/octet-stream");
    }

    String8 mimeType;
    status_t err = mHTTPConnection->getMIMEType(&mimeType);

    if (err != OK) {
        return String8("application/octet-stream");
    }

    return mimeType;
}

void MediaHTTP::clearDRMState_l() {
    if (mDecryptHandle != NULL) {
        // To release mDecryptHandle
        CHECK(mDrmManagerClient);
        mDrmManagerClient->closeDecryptSession(mDecryptHandle);
        mDecryptHandle = NULL;
    }
}

}  // namespace android
