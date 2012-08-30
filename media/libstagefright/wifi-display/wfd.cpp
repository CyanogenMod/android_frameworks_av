/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "wfd"
#include <utils/Log.h>

#define SUPPORT_SINK    0

#if SUPPORT_SINK
#include "sink/WifiDisplaySink.h"
#endif

#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <media/IMediaPlayerService.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/foundation/ADebug.h>

namespace android {

static void enableDisableRemoteDisplay(const char *iface) {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.player"));

    sp<IMediaPlayerService> service =
        interface_cast<IMediaPlayerService>(binder);

    CHECK(service.get() != NULL);

    service->enableRemoteDisplay(iface);
}

}  // namespace android

static void usage(const char *me) {
    fprintf(stderr,
            "usage:\n"
#if SUPPORT_SINK
            "           %s -c host[:port]\tconnect to wifi source\n"
            "           -u uri        \tconnect to an rtsp uri\n"
#endif
            "           -e ip[:port]       \tenable remote display\n"
            "           -d            \tdisable remote display\n",
            me);
}

int main(int argc, char **argv) {
    using namespace android;

    ProcessState::self()->startThreadPool();

    DataSource::RegisterDefaultSniffers();

    AString connectToHost;
    int32_t connectToPort = -1;
    AString uri;

    int res;
    while ((res = getopt(argc, argv, "hc:l:u:e:d")) >= 0) {
        switch (res) {
#if SUPPORT_SINK
            case 'c':
            {
                const char *colonPos = strrchr(optarg, ':');

                if (colonPos == NULL) {
                    connectToHost = optarg;
                    connectToPort = WifiDisplaySource::kWifiDisplayDefaultPort;
                } else {
                    connectToHost.setTo(optarg, colonPos - optarg);

                    char *end;
                    connectToPort = strtol(colonPos + 1, &end, 10);

                    if (*end != '\0' || end == colonPos + 1
                            || connectToPort < 1 || connectToPort > 65535) {
                        fprintf(stderr, "Illegal port specified.\n");
                        exit(1);
                    }
                }
                break;
            }

            case 'u':
            {
                uri = optarg;
                break;
            }
#endif

            case 'e':
            {
                enableDisableRemoteDisplay(optarg);
                exit(0);
                break;
            }

            case 'd':
            {
                enableDisableRemoteDisplay(NULL);
                exit(0);
                break;
            }

            case '?':
            case 'h':
            default:
                usage(argv[0]);
                exit(1);
        }
    }

#if SUPPORT_SINK
    if (connectToPort < 0 && uri.empty()) {
        fprintf(stderr,
                "You need to select either source host or uri.\n");

        exit(1);
    }

    if (connectToPort >= 0 && !uri.empty()) {
        fprintf(stderr,
                "You need to either connect to a wfd host or an rtsp url, "
                "not both.\n");
        exit(1);
    }

    sp<ANetworkSession> session = new ANetworkSession;
    session->start();

    sp<ALooper> looper = new ALooper;

    sp<WifiDisplaySink> sink = new WifiDisplaySink(session);
    looper->registerHandler(sink);

    if (connectToPort >= 0) {
        sink->start(connectToHost.c_str(), connectToPort);
    } else {
        sink->start(uri.c_str());
    }

    looper->start(true /* runOnCallingThread */);
#endif

    return 0;
}
