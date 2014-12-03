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

#ifndef _DTSUtils_h
#define _DTSUtils_h

#include <media/IOMX.h>

namespace android {

struct DTSUtils
{
    template<class T>
    static void InitOMXParams(T *params) {
        params->nSize = sizeof(T);
        params->nVersion.s.nVersionMajor = 1;
        params->nVersion.s.nVersionMinor = 0;
        params->nVersion.s.nRevision = 0;
        params->nVersion.s.nStep = 0;
    }

    static status_t setupDecoder(sp<IOMX> omx, IOMX::node_id node, int32_t sampleRate);
};

} // namespace android

#endif
