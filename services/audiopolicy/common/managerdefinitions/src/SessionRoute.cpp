/*
 * Copyright (C) 2015 The Android Open Source Project
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

#define LOG_TAG "APM::SessionRoute"
//#define LOG_NDEBUG 0

#include "SessionRoute.h"
#include "HwModule.h"
#include "AudioGain.h"
#include "DeviceDescriptor.h"
#include <utils/Log.h>

namespace android {

// --- SessionRoute class implementation
void SessionRoute::log(const char* prefix)
{
    ALOGI("%s[SessionRoute strm:0x%X, src:%d, sess:0x%X, dev:0x%X refs:%d act:%d",
          prefix, mStreamType, mSource, mSession,
          mDeviceDescriptor != 0 ? mDeviceDescriptor->type() : AUDIO_DEVICE_NONE,
          mRefCount, mActivityCount);
}

// --- SessionRouteMap class implementation
bool SessionRouteMap::hasRoute(audio_session_t session)
{
    return indexOfKey(session) >= 0 && valueFor(session)->mDeviceDescriptor != 0;
}

bool SessionRouteMap::hasRouteChanged(audio_session_t session)
{
    if (indexOfKey(session) >= 0) {
        if (valueFor(session)->mChanged) {
            valueFor(session)->mChanged = false;
            return true;
        }
    }
    return false;
}

void SessionRouteMap::removeRoute(audio_session_t session)
{
    sp<SessionRoute> route = indexOfKey(session) >= 0 ? valueFor(session) : 0;
    if (route != 0) {
        ALOG_ASSERT(route->mRefCount > 0);
        --route->mRefCount;
        if (route->mRefCount <= 0) {
            removeItem(session);
        }
    }
}

int SessionRouteMap::incRouteActivity(audio_session_t session)
{
    sp<SessionRoute> route = indexOfKey(session) >= 0 ? valueFor(session) : 0;
    return route != 0 ? ++(route->mActivityCount) : -1;
}

int SessionRouteMap::decRouteActivity(audio_session_t session)
{
    sp<SessionRoute> route = indexOfKey(session) >= 0 ? valueFor(session) : 0;
    if (route != 0 && route->mActivityCount > 0) {
        return --(route->mActivityCount);
    } else {
        return -1;
    }
}

void SessionRouteMap::log(const char* caption)
{
    ALOGI("%s ----", caption);
    for(size_t index = 0; index < size(); index++) {
        valueAt(index)->log("  ");
    }
}

void SessionRouteMap::addRoute(audio_session_t session,
                               audio_stream_type_t streamType,
                               audio_source_t source,
                               sp<DeviceDescriptor> descriptor,
                               uid_t uid)
{
    if (mMapType == MAPTYPE_INPUT && streamType != SessionRoute::STREAM_TYPE_NA) {
        ALOGE("Adding Output Route to InputRouteMap");
        return;
    } else if (mMapType == MAPTYPE_OUTPUT && source != SessionRoute::SOURCE_TYPE_NA) {
        ALOGE("Adding Input Route to OutputRouteMap");
        return;
    }

    sp<SessionRoute> route = indexOfKey(session) >= 0 ? valueFor(session) : 0;

    if (route != 0) {
        if (((route->mDeviceDescriptor == 0) && (descriptor != 0)) ||
                ((route->mDeviceDescriptor != 0) &&
                 ((descriptor == 0) || (!route->mDeviceDescriptor->equals(descriptor))))) {
            route->mChanged = true;
        }
        route->mRefCount++;
        route->mDeviceDescriptor = descriptor;
    } else {
        route = new SessionRoute(session, streamType, source, descriptor, uid);
        route->mRefCount++;
        add(session, route);
        if (descriptor != 0) {
            route->mChanged = true;
        }
    }
}

} // namespace android
