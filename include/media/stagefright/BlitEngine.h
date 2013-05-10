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

#ifndef BLITENGINE_H_

#define BLITENGINE_H_

#include <ui/GraphicBuffer.h>
#include <media/stagefright/MediaBuffer.h>

namespace android {

class BlitEngine {

    public:
        BlitEngine(size_t foo1, size_t *foo2, int foo3);
};

class C2D_BlitEngine {
    
    public:
        C2D_BlitEngine(size_t foo1, size_t* foo2, int foo3, bool foo4);

        virtual ~C2D_BlitEngine();
        virtual void do_rotate(void* foo1, sp<GraphicBuffer> buffer);
        virtual void do_rotate(MediaBuffer *buffer, bool foo2);
};

}

#endif // BLITENGINE_H_
