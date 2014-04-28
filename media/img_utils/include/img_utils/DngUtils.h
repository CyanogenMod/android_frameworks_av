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

#ifndef IMG_UTILS_DNG_UTILS_H
#define IMG_UTILS_DNG_UTILS_H

#include <img_utils/ByteArrayOutput.h>
#include <img_utils/EndianUtils.h>

#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/RefBase.h>

#include <cutils/compiler.h>
#include <stdint.h>

namespace android {
namespace img_utils {

#define NELEMS(x) ((int) (sizeof(x) / sizeof((x)[0])))

/**
 * Utility class for building values for the OpcodeList tags specified
 * in the Adobe DNG 1.4 spec.
 */
class ANDROID_API OpcodeListBuilder : public LightRefBase<OpcodeListBuilder> {
    public:
        enum CfaLayout {
            CFA_RGGB = 0,
            CFA_GRBG,
            CFA_GBRG,
            CFA_BGGR,
        };

        OpcodeListBuilder();
        virtual ~OpcodeListBuilder();

        /**
         * Get the total size of this opcode list in bytes.
         */
        virtual size_t getSize() const;

        /**
         * Get the number of opcodes defined in this list.
         */
        virtual uint32_t getCount() const;

        /**
         * Write the opcode list into the given buffer.  This buffer
         * must be able to hold at least as many elements as returned
         * by calling the getSize() method.
         *
         * Returns OK on success, or a negative error code.
         */
        virtual status_t buildOpList(/*out*/ uint8_t* buf) const;

        /**
         * Add GainMap opcode(s) for the given metadata parameters.  The given
         * CFA layout must match the layout of the shading map passed into the
         * lensShadingMap parameter.
         *
         * Returns OK on success, or a negative error code.
         */
        virtual status_t addGainMapsForMetadata(uint32_t lsmWidth,
                                                uint32_t lsmHeight,
                                                uint32_t activeAreaTop,
                                                uint32_t activeAreaLeft,
                                                uint32_t activeAreaBottom,
                                                uint32_t activeAreaRight,
                                                CfaLayout cfa,
                                                const float* lensShadingMap);


        /**
         * Add a GainMap opcode with the given fields.  The mapGains array
         * must have mapPointsV * mapPointsH * mapPlanes elements.
         *
         * Returns OK on success, or a negative error code.
         */
        virtual status_t addGainMap(uint32_t top,
                                    uint32_t left,
                                    uint32_t bottom,
                                    uint32_t right,
                                    uint32_t plane,
                                    uint32_t planes,
                                    uint32_t rowPitch,
                                    uint32_t colPitch,
                                    uint32_t mapPointsV,
                                    uint32_t mapPointsH,
                                    double mapSpacingV,
                                    double mapSpacingH,
                                    double mapOriginV,
                                    double mapOriginH,
                                    uint32_t mapPlanes,
                                    const float* mapGains);

        // TODO: Add other Opcode methods
    protected:
        static const uint32_t FLAG_OPTIONAL = 0x1u;
        static const uint32_t FLAG_OPTIONAL_FOR_PREVIEW = 0x2u;

        enum {
            GAIN_MAP_ID = 9,
            LSM_R_IND = 0,
            LSM_GE_IND = 1,
            LSM_GO_IND = 2,
            LSM_B_IND = 3,
        };

        uint32_t mCount;
        ByteArrayOutput mOpList;
        EndianOutput mEndianOut;

};

} /*namespace img_utils*/
} /*namespace android*/

#endif /*IMG_UTILS_DNG_UTILS_H*/
