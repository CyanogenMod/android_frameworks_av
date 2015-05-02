/*
 * Copyright 2015 The Android Open Source Project
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

#ifndef PERSISTENT_SURFACE_H_

#define PERSISTENT_SURFACE_H_

#include <gui/IGraphicBufferProducer.h>
#include <gui/IGraphicBufferConsumer.h>
#include <media/stagefright/foundation/ABase.h>

namespace android {

struct PersistentSurface : public RefBase {
    PersistentSurface(
            const sp<IGraphicBufferProducer>& bufferProducer,
            const sp<IGraphicBufferConsumer>& bufferConsumer) :
        mBufferProducer(bufferProducer),
        mBufferConsumer(bufferConsumer) { }

    sp<IGraphicBufferProducer> getBufferProducer() const {
        return mBufferProducer;
    }

    sp<IGraphicBufferConsumer> getBufferConsumer() const {
        return mBufferConsumer;
    }

private:
    const sp<IGraphicBufferProducer> mBufferProducer;
    const sp<IGraphicBufferConsumer> mBufferConsumer;

    DISALLOW_EVIL_CONSTRUCTORS(PersistentSurface);
};

}  // namespace android

#endif  // PERSISTENT_SURFACE_H_
