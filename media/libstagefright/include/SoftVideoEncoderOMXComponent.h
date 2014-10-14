/*
 * Copyright 2014 The Android Open Source Project
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

#ifndef SOFT_VIDEO_ENCODER_OMX_COMPONENT_H_

#define SOFT_VIDEO_ENCODER_OMX_COMPONENT_H_

#include "SimpleSoftOMXComponent.h"
#include <system/window.h>

struct hw_module_t;

namespace android {

struct SoftVideoEncoderOMXComponent : public SimpleSoftOMXComponent {
    SoftVideoEncoderOMXComponent(
            const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

protected:
    static void ConvertFlexYUVToPlanar(
            uint8_t *dst, size_t dstStride, size_t dstVStride,
            struct android_ycbcr *ycbcr, int32_t width, int32_t height);

    static void ConvertYUV420SemiPlanarToYUV420Planar(
            const uint8_t *inYVU, uint8_t* outYUV, int32_t width, int32_t height);

    static void ConvertRGB32ToPlanar(
        uint8_t *dstY, size_t dstStride, size_t dstVStride,
        const uint8_t *src, size_t width, size_t height, size_t srcStride,
        bool bgr);

    const uint8_t *extractGraphicBuffer(
            uint8_t *dst, size_t dstSize, const uint8_t *src, size_t srcSize,
            size_t width, size_t height) const;

    virtual OMX_ERRORTYPE getExtensionIndex(const char *name, OMX_INDEXTYPE *index);

    enum {
        kInputPortIndex = 0,
        kOutputPortIndex = 1,
    };

private:
    mutable const hw_module_t *mGrallocModule;

    DISALLOW_EVIL_CONSTRUCTORS(SoftVideoEncoderOMXComponent);
};

}  // namespace android

#endif  // SOFT_VIDEO_ENCODER_OMX_COMPONENT_H_
