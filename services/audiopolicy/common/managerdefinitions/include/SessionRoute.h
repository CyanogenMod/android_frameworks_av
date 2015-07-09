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

#pragma once

#include <system/audio.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>

namespace android {

class DeviceDescriptor;

class SessionRoute : public RefBase
{
public:
    // For Input (Source) routes, use STREAM_TYPE_NA ("NA" = "not applicable)for the
    // streamType argument
    static const audio_stream_type_t STREAM_TYPE_NA = AUDIO_STREAM_DEFAULT;

    // For Output (Sink) routes, use SOURCE_TYPE_NA ("NA" = "not applicable") for the
    // source argument

    static const audio_source_t SOURCE_TYPE_NA = AUDIO_SOURCE_DEFAULT;

    SessionRoute(audio_session_t session,
                 audio_stream_type_t streamType,
                 audio_source_t source,
                 sp<DeviceDescriptor> deviceDescriptor,
                 uid_t uid)
        : mUid(uid),
          mSession(session),
          mDeviceDescriptor(deviceDescriptor),
          mRefCount(0),
          mActivityCount(0),
          mChanged(false),
          mStreamType(streamType),
          mSource(source)
    {}

    void log(const char* prefix);

    bool isActive() {
        return (mDeviceDescriptor != 0) && (mChanged || (mActivityCount > 0));
    }

    uid_t                       mUid;
    audio_session_t             mSession;
    sp<DeviceDescriptor>        mDeviceDescriptor;

    // "reference" counting
    int                         mRefCount;      // +/- on references
    int                         mActivityCount; // +/- on start/stop
    bool                        mChanged;
    // for outputs
    const audio_stream_type_t   mStreamType;
    // for inputs
    const audio_source_t        mSource;
};

class SessionRouteMap: public KeyedVector<audio_session_t, sp<SessionRoute> >
{
public:
    // These constants identify the SessionRoutMap as holding EITHER input routes,
    // or output routes.  An error will occur if an attempt is made to add a SessionRoute
    // object with mStreamType == STREAM_TYPE_NA (i.e. an input SessionRoute) to a
    // SessionRoutMap that is marked for output (i.e. mMapType == SESSION_ROUTE_MAP_OUTPUT)
    // and similarly  for output SessionRoutes and Input SessionRouteMaps.
    typedef enum
    {
        MAPTYPE_INPUT = 0,
        MAPTYPE_OUTPUT = 1
    } session_route_map_type_t;

    SessionRouteMap(session_route_map_type_t mapType) :
        mMapType(mapType)
    {}

    bool hasRoute(audio_session_t session);

    void removeRoute(audio_session_t session);

    int incRouteActivity(audio_session_t session);
    int decRouteActivity(audio_session_t session);
    bool hasRouteChanged(audio_session_t session); // also clears the changed flag
    void log(const char* caption);

    // Specify an Output(Sink) route by passing SessionRoute::SOURCE_TYPE_NA in the
    // source argument.
    // Specify an Input(Source) rout by passing SessionRoute::AUDIO_STREAM_DEFAULT
    // in the streamType argument.
    void addRoute(audio_session_t session,
                  audio_stream_type_t streamType,
                  audio_source_t source,
                  sp<DeviceDescriptor> deviceDescriptor,
                  uid_t uid);

private:
    // Used to mark a SessionRoute as for either inputs (mMapType == kSessionRouteMap_Input)
    // or outputs (mMapType == kSessionRouteMap_Output)
    const session_route_map_type_t mMapType;
};

}; // namespace android
