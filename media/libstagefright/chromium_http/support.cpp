/*
 * Copyright (C) 2011 The Android Open Source Project
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
#define LOG_TAG "ChromiumHTTPDataSourceSupport"
#include <utils/Log.h>

#include <media/stagefright/foundation/AString.h>

#include "support.h"

#include "android/net/android_network_library_impl.h"
#include "base/logging.h"
#include "base/threading/thread.h"
#include "net/base/cert_verifier.h"
#include "net/base/cookie_monster.h"
#include "net/base/host_resolver.h"
#include "net/base/ssl_config_service.h"
#include "net/http/http_auth_handler_factory.h"
#include "net/http/http_cache.h"
#include "net/proxy/proxy_config_service_android.h"

#include "include/ChromiumHTTPDataSource.h"
#include <arpa/inet.h>
#include <binder/Parcel.h>
#include <cutils/log.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>
#include <string>

#include <utils/Errors.h>
#include <binder/IInterface.h>
#include <binder/IServiceManager.h>

namespace android {

// must be kept in sync with interface defined in IAudioService.aidl
class IAudioService : public IInterface
{
public:
    DECLARE_META_INTERFACE(AudioService);

    virtual int verifyX509CertChain(
            const std::vector<std::string>& cert_chain,
            const std::string& hostname,
            const std::string& auth_type) = 0;
};

class BpAudioService : public BpInterface<IAudioService>
{
public:
    BpAudioService(const sp<IBinder>& impl)
        : BpInterface<IAudioService>(impl)
    {
    }

    virtual int verifyX509CertChain(
            const std::vector<std::string>& cert_chain,
            const std::string& hostname,
            const std::string& auth_type)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioService::getInterfaceDescriptor());

        // The vector of std::string we get isn't really a vector of strings,
        // but rather a vector of binary certificate data. If we try to pass
        // it to Java language code as a string, it ends up mangled on the other
        // side, so send them as bytes instead.
        // Since we can't send an array of byte arrays, send a single array,
        // which will be split out by the recipient.

        int numcerts = cert_chain.size();
        data.writeInt32(numcerts);
        size_t total = 0;
        for (int i = 0; i < numcerts; i++) {
            total += cert_chain[i].size();
        }
        size_t bytesize = total + numcerts * 4;
        uint8_t *bytes = (uint8_t*) malloc(bytesize);
        if (!bytes) {
            return 5; // SSL_INVALID
        }
        ALOGV("%d certs: %d -> %d", numcerts, total, bytesize);

        int offset = 0;
        for (int i = 0; i < numcerts; i++) {
            int32_t certsize = cert_chain[i].size();
            // store this in a known order, which just happens to match the default
            // byte order of a java ByteBuffer
            int32_t bigsize = htonl(certsize);
            ALOGV("cert %d, size %d", i, certsize);
            memcpy(bytes + offset, &bigsize, sizeof(bigsize));
            offset += sizeof(bigsize);
            memcpy(bytes + offset, cert_chain[i].data(), certsize);
            offset += certsize;
        }
        data.writeByteArray(bytesize, bytes);
        free(bytes);
        data.writeString16(String16(hostname.c_str()));
        data.writeString16(String16(auth_type.c_str()));

        int32_t result;
        if (remote()->transact(IBinder::FIRST_CALL_TRANSACTION, data, &reply) != NO_ERROR
                || reply.readExceptionCode() < 0 || reply.readInt32(&result) != NO_ERROR) {
            return 5; // SSL_INVALID;
        }
        return result;
    }

};

IMPLEMENT_META_INTERFACE(AudioService, "android.media.IAudioService");


static Mutex gNetworkThreadLock;
static base::Thread *gNetworkThread = NULL;
static scoped_refptr<SfRequestContext> gReqContext;
static scoped_ptr<net::NetworkChangeNotifier> gNetworkChangeNotifier;

bool logMessageHandler(
        int severity,
        const char* file,
        int line,
        size_t message_start,
        const std::string& str) {
    int androidSeverity = ANDROID_LOG_VERBOSE;
    switch(severity) {
    case logging::LOG_FATAL:
        androidSeverity = ANDROID_LOG_FATAL;
        break;
    case logging::LOG_ERROR_REPORT:
    case logging::LOG_ERROR:
        androidSeverity = ANDROID_LOG_ERROR;
        break;
    case logging::LOG_WARNING:
        androidSeverity = ANDROID_LOG_WARN;
        break;
    default:
        androidSeverity = ANDROID_LOG_VERBOSE;
        break;
    }
    android_printLog(androidSeverity, "chromium-libstagefright",
                    "%s:%d: %s", file, line, str.c_str());
    return false;
}

struct AutoPrioritySaver {
    AutoPrioritySaver()
        : mTID(androidGetTid()),
          mPrevPriority(androidGetThreadPriority(mTID)) {
        androidSetThreadPriority(mTID, ANDROID_PRIORITY_NORMAL);
    }

    ~AutoPrioritySaver() {
        androidSetThreadPriority(mTID, mPrevPriority);
    }

private:
    pid_t mTID;
    int mPrevPriority;

    DISALLOW_EVIL_CONSTRUCTORS(AutoPrioritySaver);
};

static void InitializeNetworkThreadIfNecessary() {
    Mutex::Autolock autoLock(gNetworkThreadLock);

    if (gNetworkThread == NULL) {
        // Make sure any threads spawned by the chromium framework are
        // running at normal priority instead of inheriting this thread's.
        AutoPrioritySaver saver;

        gNetworkThread = new base::Thread("network");
        base::Thread::Options options;
        options.message_loop_type = MessageLoop::TYPE_IO;
        CHECK(gNetworkThread->StartWithOptions(options));

        gReqContext = new SfRequestContext;

        gNetworkChangeNotifier.reset(net::NetworkChangeNotifier::Create());

        net::AndroidNetworkLibrary::RegisterSharedInstance(
                new SfNetworkLibrary);
        logging::SetLogMessageHandler(logMessageHandler);
    }
}

static void MY_LOGI(const char *s) {
    LOG_PRI(ANDROID_LOG_INFO, LOG_TAG, "%s", s);
}

static void MY_LOGV(const char *s) {
#if !defined(LOG_NDEBUG) || LOG_NDEBUG == 0
    LOG_PRI(ANDROID_LOG_VERBOSE, LOG_TAG, "%s", s);
#endif
}

SfNetLog::SfNetLog()
    : mNextID(1) {
}

void SfNetLog::AddEntry(
        EventType type,
        const base::TimeTicks &time,
        const Source &source,
        EventPhase phase,
        EventParameters *params) {
#if 0
    MY_LOGI(StringPrintf(
                "AddEntry time=%s type=%s source=%s phase=%s\n",
                TickCountToString(time).c_str(),
                EventTypeToString(type),
                SourceTypeToString(source.type),
                EventPhaseToString(phase)).c_str());
#endif
}

uint32 SfNetLog::NextID() {
    return mNextID++;
}

net::NetLog::LogLevel SfNetLog::GetLogLevel() const {
    return LOG_BASIC;
}

////////////////////////////////////////////////////////////////////////////////

SfRequestContext::SfRequestContext() {
    mUserAgent = MakeUserAgent().c_str();

    set_net_log(new SfNetLog());

    set_host_resolver(
        net::CreateSystemHostResolver(
                net::HostResolver::kDefaultParallelism,
                NULL /* resolver_proc */,
                net_log()));

    set_ssl_config_service(
        net::SSLConfigService::CreateSystemSSLConfigService());

    mProxyConfigService = new net::ProxyConfigServiceAndroid;

    set_proxy_service(net::ProxyService::CreateWithoutProxyResolver(
        mProxyConfigService, net_log()));

    set_http_transaction_factory(new net::HttpCache(
            host_resolver(),
            new net::CertVerifier(),
            dnsrr_resolver(),
            dns_cert_checker(),
            proxy_service(),
            ssl_config_service(),
            net::HttpAuthHandlerFactory::CreateDefault(host_resolver()),
            network_delegate(),
            net_log(),
            NULL));  // backend_factory

    set_cookie_store(new net::CookieMonster(NULL, NULL));
}

const std::string &SfRequestContext::GetUserAgent(const GURL &url) const {
    return mUserAgent;
}

status_t SfRequestContext::updateProxyConfig(
        const char *host, int32_t port, const char *exclusionList) {
    Mutex::Autolock autoLock(mProxyConfigLock);

    if (host == NULL || *host == '\0') {
        MY_LOGV("updateProxyConfig NULL");

        std::string proxy;
        std::string exList;
        mProxyConfigService->UpdateProxySettings(proxy, exList);
    } else {
#if !defined(LOG_NDEBUG) || LOG_NDEBUG == 0
        LOG_PRI(ANDROID_LOG_VERBOSE, LOG_TAG,
                "updateProxyConfig %s:%d, exclude '%s'",
                host, port, exclusionList);
#endif

        std::string proxy = StringPrintf("%s:%d", host, port).c_str();
        std::string exList = exclusionList;
        mProxyConfigService->UpdateProxySettings(proxy, exList);
    }

    return OK;
}

////////////////////////////////////////////////////////////////////////////////

SfNetworkLibrary::SfNetworkLibrary() {}

SfNetworkLibrary::VerifyResult SfNetworkLibrary::VerifyX509CertChain(
        const std::vector<std::string>& cert_chain,
        const std::string& hostname,
        const std::string& auth_type) {

    sp<IBinder> binder =
        defaultServiceManager()->checkService(String16("audio"));
    if (binder == 0) {
        ALOGW("Thread cannot connect to the audio service");
    } else {
        sp<IAudioService> service = interface_cast<IAudioService>(binder);
        int code = service->verifyX509CertChain(cert_chain, hostname, auth_type);
        ALOGV("verified: %d", code);
        if (code == -1) {
            return VERIFY_OK;
        } else if (code == 2) { // SSL_IDMISMATCH
            return VERIFY_BAD_HOSTNAME;
        } else if (code == 3) { // SSL_UNTRUSTED
            return VERIFY_NO_TRUSTED_ROOT;
        }
    }
    return VERIFY_INVOCATION_ERROR;
}

////////////////////////////////////////////////////////////////////////////////

SfDelegate::SfDelegate()
    : mOwner(NULL),
      mURLRequest(NULL),
      mReadBuffer(new net::IOBufferWithSize(8192)),
      mNumBytesRead(0),
      mNumBytesTotal(0),
      mDataDestination(NULL),
      mAtEOS(false) {
    InitializeNetworkThreadIfNecessary();
}

SfDelegate::~SfDelegate() {
    CHECK(mURLRequest == NULL);
}

// static
status_t SfDelegate::UpdateProxyConfig(
        const char *host, int32_t port, const char *exclusionList) {
    InitializeNetworkThreadIfNecessary();

    return gReqContext->updateProxyConfig(host, port, exclusionList);
}

void SfDelegate::setOwner(ChromiumHTTPDataSource *owner) {
    mOwner = owner;
}

void SfDelegate::setUID(uid_t uid) {
    gReqContext->setUID(uid);
}

bool SfDelegate::getUID(uid_t *uid) const {
    return gReqContext->getUID(uid);
}

void SfDelegate::OnReceivedRedirect(
            net::URLRequest *request, const GURL &new_url, bool *defer_redirect) {
    MY_LOGV("OnReceivedRedirect");
}

void SfDelegate::OnAuthRequired(
            net::URLRequest *request, net::AuthChallengeInfo *auth_info) {
    MY_LOGV("OnAuthRequired");

    inherited::OnAuthRequired(request, auth_info);
}

void SfDelegate::OnCertificateRequested(
            net::URLRequest *request, net::SSLCertRequestInfo *cert_request_info) {
    MY_LOGV("OnCertificateRequested");

    inherited::OnCertificateRequested(request, cert_request_info);
}

void SfDelegate::OnSSLCertificateError(
            net::URLRequest *request, int cert_error, net::X509Certificate *cert) {
    fprintf(stderr, "OnSSLCertificateError cert_error=%d\n", cert_error);

    inherited::OnSSLCertificateError(request, cert_error, cert);
}

void SfDelegate::OnGetCookies(net::URLRequest *request, bool blocked_by_policy) {
    MY_LOGV("OnGetCookies");
}

void SfDelegate::OnSetCookie(
        net::URLRequest *request,
        const std::string &cookie_line,
        const net::CookieOptions &options,
        bool blocked_by_policy) {
    MY_LOGV("OnSetCookie");
}

void SfDelegate::OnResponseStarted(net::URLRequest *request) {
    if (request->status().status() != net::URLRequestStatus::SUCCESS) {
        MY_LOGI(StringPrintf(
                    "Request failed with status %d and os_error %d",
                    request->status().status(),
                    request->status().os_error()).c_str());

        delete mURLRequest;
        mURLRequest = NULL;

        mOwner->onConnectionFailed(ERROR_IO);
        return;
    } else if (mRangeRequested && request->GetResponseCode() != 206) {
        MY_LOGI(StringPrintf(
                    "We requested a content range, but server didn't "
                    "support that. (responded with %d)",
                    request->GetResponseCode()).c_str());

        delete mURLRequest;
        mURLRequest = NULL;

        mOwner->onConnectionFailed(-EPIPE);
        return;
    } else if ((request->GetResponseCode() / 100) != 2) {
        MY_LOGI(StringPrintf(
                    "Server responded with http status %d",
                    request->GetResponseCode()).c_str());

        delete mURLRequest;
        mURLRequest = NULL;

        mOwner->onConnectionFailed(ERROR_IO);
        return;
    }

    MY_LOGV("OnResponseStarted");

    std::string headers;
    request->GetAllResponseHeaders(&headers);

    MY_LOGV(StringPrintf("response headers: %s", headers.c_str()).c_str());

    std::string contentType;
    request->GetResponseHeaderByName("Content-Type", &contentType);

    mOwner->onConnectionEstablished(
            request->GetExpectedContentSize(), contentType.c_str());
}

void SfDelegate::OnReadCompleted(net::URLRequest *request, int bytes_read) {
    if (bytes_read == -1) {
        MY_LOGI(StringPrintf(
                    "OnReadCompleted, read failed, status %d",
                    request->status().status()).c_str());

        mOwner->onReadCompleted(ERROR_IO);
        return;
    }

    MY_LOGV(StringPrintf("OnReadCompleted, read %d bytes", bytes_read).c_str());

    if (bytes_read < 0) {
        MY_LOGI(StringPrintf(
                    "Read failed w/ status %d\n",
                    request->status().status()).c_str());

        mOwner->onReadCompleted(ERROR_IO);
        return;
    } else if (bytes_read == 0) {
        mAtEOS = true;
        mOwner->onReadCompleted(mNumBytesRead);
        return;
    }

    CHECK_GT(bytes_read, 0);
    CHECK_LE(mNumBytesRead + bytes_read, mNumBytesTotal);

    memcpy((uint8_t *)mDataDestination + mNumBytesRead,
           mReadBuffer->data(),
           bytes_read);

    mNumBytesRead += bytes_read;

    readMore(request);
}

void SfDelegate::readMore(net::URLRequest *request) {
    while (mNumBytesRead < mNumBytesTotal) {
        size_t copy = mNumBytesTotal - mNumBytesRead;
        if (copy > mReadBuffer->size()) {
            copy = mReadBuffer->size();
        }

        int n;
        if (request->Read(mReadBuffer, copy, &n)) {
            MY_LOGV(StringPrintf("Read %d bytes directly.", n).c_str());

            CHECK_LE((size_t)n, copy);

            memcpy((uint8_t *)mDataDestination + mNumBytesRead,
                   mReadBuffer->data(),
                   n);

            mNumBytesRead += n;

            if (n == 0) {
                mAtEOS = true;
                break;
            }
        } else {
            MY_LOGV("readMore pending read");

            if (request->status().status() != net::URLRequestStatus::IO_PENDING) {
                MY_LOGI(StringPrintf(
                            "Direct read failed w/ status %d\n",
                            request->status().status()).c_str());

                mOwner->onReadCompleted(ERROR_IO);
                return;
            }

            return;
        }
    }

    mOwner->onReadCompleted(mNumBytesRead);
}

void SfDelegate::initiateConnection(
        const char *uri,
        const KeyedVector<String8, String8> *headers,
        off64_t offset) {
    GURL url(uri);

    MessageLoop *loop = gNetworkThread->message_loop();
    loop->PostTask(
            FROM_HERE,
            NewRunnableFunction(
                &SfDelegate::OnInitiateConnectionWrapper,
                this,
                url,
                headers,
                offset));

}

// static
void SfDelegate::OnInitiateConnectionWrapper(
        SfDelegate *me, GURL url,
        const KeyedVector<String8, String8> *headers,
        off64_t offset) {
    me->onInitiateConnection(url, headers, offset);
}

void SfDelegate::onInitiateConnection(
        const GURL &url,
        const KeyedVector<String8, String8> *extra,
        off64_t offset) {
    CHECK(mURLRequest == NULL);

    mURLRequest = new net::URLRequest(url, this);
    mAtEOS = false;

    mRangeRequested = false;

    if (offset != 0 || extra != NULL) {
        net::HttpRequestHeaders headers =
            mURLRequest->extra_request_headers();

        if (offset != 0) {
            headers.AddHeaderFromString(
                    StringPrintf("Range: bytes=%lld-", offset).c_str());

            mRangeRequested = true;
        }

        if (extra != NULL) {
            for (size_t i = 0; i < extra->size(); ++i) {
                AString s;
                s.append(extra->keyAt(i).string());
                s.append(": ");
                s.append(extra->valueAt(i).string());

                headers.AddHeaderFromString(s.c_str());
            }
        }

        mURLRequest->SetExtraRequestHeaders(headers);
    }

    mURLRequest->set_context(gReqContext);

    mURLRequest->Start();
}

void SfDelegate::initiateDisconnect() {
    MessageLoop *loop = gNetworkThread->message_loop();
    loop->PostTask(
            FROM_HERE,
            NewRunnableFunction(
                &SfDelegate::OnInitiateDisconnectWrapper, this));
}

// static
void SfDelegate::OnInitiateDisconnectWrapper(SfDelegate *me) {
    me->onInitiateDisconnect();
}

void SfDelegate::onInitiateDisconnect() {
    if (mURLRequest == NULL) {
        return;
    }

    mURLRequest->Cancel();

    delete mURLRequest;
    mURLRequest = NULL;

    mOwner->onDisconnectComplete();
}

void SfDelegate::initiateRead(void *data, size_t size) {
    MessageLoop *loop = gNetworkThread->message_loop();
    loop->PostTask(
            FROM_HERE,
            NewRunnableFunction(
                &SfDelegate::OnInitiateReadWrapper, this, data, size));
}

// static
void SfDelegate::OnInitiateReadWrapper(
        SfDelegate *me, void *data, size_t size) {
    me->onInitiateRead(data, size);
}

void SfDelegate::onInitiateRead(void *data, size_t size) {
    CHECK(mURLRequest != NULL);

    mNumBytesRead = 0;
    mNumBytesTotal = size;
    mDataDestination = data;

    if (mAtEOS) {
        mOwner->onReadCompleted(0);
        return;
    }

    readMore(mURLRequest);
}

}  // namespace android

