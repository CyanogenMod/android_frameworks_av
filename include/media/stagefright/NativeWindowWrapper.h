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

#ifndef NATIVE_WINDOW_WRAPPER_H_

#define NATIVE_WINDOW_WRAPPER_H_

#include <gui/Surface.h>

namespace android {

// Surface derives from ANativeWindow which derives from multiple
// base classes, in order to carry it in AMessages, we'll temporarily wrap it
// into a NativeWindowWrapper.

struct NativeWindowWrapper : RefBase {
    NativeWindowWrapper(
            const sp<Surface> &surfaceTextureClient) :
        mSurfaceTextureClient(surfaceTextureClient) { }

    sp<ANativeWindow> getNativeWindow() const {
        return mSurfaceTextureClient;
    }

    sp<Surface> getSurfaceTextureClient() const {
        return mSurfaceTextureClient;
    }

private:
    const sp<Surface> mSurfaceTextureClient;

    DISALLOW_EVIL_CONSTRUCTORS(NativeWindowWrapper);
};

}  // namespace android

#endif  // NATIVE_WINDOW_WRAPPER_H_
