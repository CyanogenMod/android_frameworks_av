/*
 * Copyright (C) 2011 NXP Software
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

#ifndef PREVIEW_RENDERER_H_

#define PREVIEW_RENDERER_H_

#include <media/stagefright/ColorConverter.h>
#include <utils/RefBase.h>
#include <ui/android_native_buffer.h>
#include <ui/GraphicBufferMapper.h>
#include "SoftwareRenderer.h"


namespace android {

class Surface;

class PreviewRenderer {
public:

static PreviewRenderer* CreatePreviewRenderer (OMX_COLOR_FORMATTYPE colorFormat,
        const sp<Surface> &surface,
        size_t displayWidth, size_t displayHeight,
        size_t decodedWidth, size_t decodedHeight,
        int32_t rotationDegrees);

    ~PreviewRenderer();

    void render(
            const void *data, size_t size, void *platformPrivate);

    void getBufferYV12(uint8_t **data, size_t *stride);

    void renderYV12();

    static size_t ALIGN(size_t x, size_t alignment) {
        return (x + alignment - 1) & ~(alignment - 1);
    }

private:
    PreviewRenderer(
            OMX_COLOR_FORMATTYPE colorFormat,
            const sp<Surface> &surface,
            size_t displayWidth, size_t displayHeight,
            size_t decodedWidth, size_t decodedHeight,
            int32_t rotationDegrees);
    enum YUVMode {
        None,
        YUV420ToYUV420sp,
        YUV420spToYUV420sp,
    };

    OMX_COLOR_FORMATTYPE mColorFormat;
    ColorConverter *mConverter;
    YUVMode mYUVMode;
    sp<Surface> mSurface;
    size_t mDisplayWidth, mDisplayHeight;
    size_t mDecodedWidth, mDecodedHeight;

    ANativeWindowBuffer *mBuf;

    PreviewRenderer(const PreviewRenderer &);
    PreviewRenderer &operator=(const PreviewRenderer &);
};

}  // namespace android

#endif  // PREVIEW_RENDERER_H_
