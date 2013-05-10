/*
 * Copyright (C) 2013 The CyanogenMod Project
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

#define LOG_NDEBUG 0
#define LOG_TAG "BlitEngine"
#include <utils/Log.h>

#include "media/stagefright/BlitEngine.h"

namespace android {

    BlitEngine::BlitEngine(size_t foo1, size_t *foo2, int foo3) {
        ALOGE("%s", __func__);
    }

    C2D_BlitEngine::C2D_BlitEngine(size_t foo1, size_t* foo2, int foo3, bool foo4) {
        ALOGE("%s", __func__);
    }

    C2D_BlitEngine::~C2D_BlitEngine() {
        ALOGE("%s", __func__);
    }

    void C2D_BlitEngine::do_rotate(void* foo1, sp<GraphicBuffer> buffer) {
        ALOGE("%s", __func__);
    }

    void C2D_BlitEngine::do_rotate(MediaBuffer *buffer, bool foo2) {
        ALOGE("%s", __func__);
    }

}


