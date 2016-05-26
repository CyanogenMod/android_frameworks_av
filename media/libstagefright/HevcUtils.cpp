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

//#define LOG_NDEBUG 0
#define LOG_TAG "HevcUtils"

#include <cstring>

#include "include/HevcUtils.h"
#include "include/avc_utils.h"

#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>

namespace android {

static const uint8_t kHevcNalUnitTypes[5] = {
    kHevcNalUnitTypeVps,
    kHevcNalUnitTypeSps,
    kHevcNalUnitTypePps,
    kHevcNalUnitTypePrefixSei,
    kHevcNalUnitTypeSuffixSei,
};

HevcParameterSets::HevcParameterSets() {
}

status_t HevcParameterSets::addNalUnit(const uint8_t* data, size_t size) {
    uint8_t nalUnitType = (data[0] >> 1) & 0x3f;
    status_t err = OK;
    switch (nalUnitType) {
        case 32:  // VPS
            err = parseVps(data + 2, size - 2);
            break;
        case 33:  // SPS
            err = parseSps(data + 2, size - 2);
            break;
        case 34:  // PPS
            err = parsePps(data + 2, size - 2);
            break;
        case 39:  // Prefix SEI
        case 40:  // Suffix SEI
            // Ignore
            break;
        default:
            ALOGE("Unrecognized NAL unit type.");
            return ERROR_MALFORMED;
    }

    if (err != OK) {
        return err;
    }

    sp<ABuffer> buffer = ABuffer::CreateAsCopy(data, size);
    buffer->setInt32Data(nalUnitType);
    mNalUnits.push(buffer);
    return OK;
}

template <typename T>
static bool findParam(uint32_t key, T *param,
        KeyedVector<uint32_t, uint64_t> &params) {
    CHECK(param);
    if (params.indexOfKey(key) < 0) {
        return false;
    }
    *param = (T) params[key];
    return true;
}

bool HevcParameterSets::findParam8(uint32_t key, uint8_t *param) {
    return findParam(key, param, mParams);
}

bool HevcParameterSets::findParam16(uint32_t key, uint16_t *param) {
    return findParam(key, param, mParams);
}

bool HevcParameterSets::findParam32(uint32_t key, uint32_t *param) {
    return findParam(key, param, mParams);
}

bool HevcParameterSets::findParam64(uint32_t key, uint64_t *param) {
    return findParam(key, param, mParams);
}

size_t HevcParameterSets::getNumNalUnitsOfType(uint8_t type) {
    size_t num = 0;
    for (size_t i = 0; i < mNalUnits.size(); ++i) {
        if (getType(i) == type) {
            ++num;
        }
    }
    return num;
}

uint8_t HevcParameterSets::getType(size_t index) {
    CHECK_LT(index, mNalUnits.size());
    return mNalUnits[index]->int32Data();
}

size_t HevcParameterSets::getSize(size_t index) {
    CHECK_LT(index, mNalUnits.size());
    return mNalUnits[index]->size();
}

bool HevcParameterSets::write(size_t index, uint8_t* dest, size_t size) {
    CHECK_LT(index, mNalUnits.size());
    const sp<ABuffer>& nalUnit = mNalUnits[index];
    if (size < nalUnit->size()) {
        ALOGE("dest buffer size too small: %zu vs. %zu to be written",
                size, nalUnit->size());
        return false;
    }
    memcpy(dest, nalUnit->data(), nalUnit->size());
    return true;
}

status_t HevcParameterSets::parseVps(const uint8_t* data, size_t size) {
    // See Rec. ITU-T H.265 v3 (04/2015) Chapter 7.3.2.1 for reference
    NALBitReader reader(data, size);
    // Skip vps_video_parameter_set_id
    reader.skipBits(4);
    // Skip vps_base_layer_internal_flag
    reader.skipBits(1);
    // Skip vps_base_layer_available_flag
    reader.skipBits(1);
    // Skip vps_max_layers_minus_1
    reader.skipBits(6);
    // Skip vps_temporal_id_nesting_flags
    reader.skipBits(1);
    // Skip reserved
    reader.skipBits(16);

    if (reader.atLeastNumBitsLeft(96)) {
        mParams.add(kGeneralProfileSpace, reader.getBits(2));
        mParams.add(kGeneralTierFlag, reader.getBits(1));
        mParams.add(kGeneralProfileIdc, reader.getBits(5));
        mParams.add(kGeneralProfileCompatibilityFlags, reader.getBits(32));
        mParams.add(
                kGeneralConstraintIndicatorFlags,
                ((uint64_t)reader.getBits(16) << 32) | reader.getBits(32));
        mParams.add(kGeneralLevelIdc, reader.getBits(8));
        // 96 bits total for general profile.
    } else {
        reader.skipBits(96);
    }

    return reader.overRead() ? ERROR_MALFORMED : OK;
}

status_t HevcParameterSets::parseSps(const uint8_t* data, size_t size) {
    // See Rec. ITU-T H.265 v3 (04/2015) Chapter 7.3.2.2 for reference
    NALBitReader reader(data, size);
    // Skip sps_video_parameter_set_id
    reader.skipBits(4);
    uint8_t maxSubLayersMinus1 = reader.getBitsWithFallback(3, 0);
    // Skip sps_temporal_id_nesting_flag;
    reader.skipBits(1);
    // Skip general profile
    reader.skipBits(96);
    if (maxSubLayersMinus1 > 0) {
        bool subLayerProfilePresentFlag[8];
        bool subLayerLevelPresentFlag[8];
        for (int i = 0; i < maxSubLayersMinus1; ++i) {
            subLayerProfilePresentFlag[i] = reader.getBitsWithFallback(1, 0);
            subLayerLevelPresentFlag[i] = reader.getBitsWithFallback(1, 0);
        }
        // Skip reserved
        reader.skipBits(2 * (8 - maxSubLayersMinus1));
        for (int i = 0; i < maxSubLayersMinus1; ++i) {
            if (subLayerProfilePresentFlag[i]) {
                // Skip profile
                reader.skipBits(88);
            }
            if (subLayerLevelPresentFlag[i]) {
                // Skip sub_layer_level_idc[i]
                reader.skipBits(8);
            }
        }
    }
    // Skip sps_seq_parameter_set_id
    skipUE(&reader);
    uint8_t chromaFormatIdc = parseUEWithFallback(&reader, 0);
    mParams.add(kChromaFormatIdc, chromaFormatIdc);
    if (chromaFormatIdc == 3) {
        // Skip separate_colour_plane_flag
        reader.skipBits(1);
    }
    // Skip pic_width_in_luma_samples
    skipUE(&reader);
    // Skip pic_height_in_luma_samples
    skipUE(&reader);
    if (reader.getBitsWithFallback(1, 0) /* i.e. conformance_window_flag */) {
        // Skip conf_win_left_offset
        skipUE(&reader);
        // Skip conf_win_right_offset
        skipUE(&reader);
        // Skip conf_win_top_offset
        skipUE(&reader);
        // Skip conf_win_bottom_offset
        skipUE(&reader);
    }
    mParams.add(kBitDepthLumaMinus8, parseUEWithFallback(&reader, 0));
    mParams.add(kBitDepthChromaMinus8, parseUEWithFallback(&reader, 0));

    return reader.overRead() ? ERROR_MALFORMED : OK;
}

status_t HevcParameterSets::parsePps(
        const uint8_t* data __unused, size_t size __unused) {
    return OK;
}

status_t HevcParameterSets::makeHvcc(uint8_t *hvcc, size_t *hvccSize,
        size_t nalSizeLength) {
    if (hvcc == NULL || hvccSize == NULL
            || (nalSizeLength != 4 && nalSizeLength != 2)) {
        return BAD_VALUE;
    }
    // ISO 14496-15: HEVC file format
    size_t size = 23;  // 23 bytes in the header
    size_t numOfArrays = 0;
    const size_t numNalUnits = getNumNalUnits();
    for (size_t i = 0; i < ARRAY_SIZE(kHevcNalUnitTypes); ++i) {
        uint8_t type = kHevcNalUnitTypes[i];
        size_t numNalus = getNumNalUnitsOfType(type);
        if (numNalus == 0) {
            continue;
        }
        ++numOfArrays;
        size += 3;
        for (size_t j = 0; j < numNalUnits; ++j) {
            if (getType(j) != type) {
                continue;
            }
            size += 2 + getSize(j);
        }
    }
    uint8_t generalProfileSpace, generalTierFlag, generalProfileIdc;
    if (!findParam8(kGeneralProfileSpace, &generalProfileSpace)
            || !findParam8(kGeneralTierFlag, &generalTierFlag)
            || !findParam8(kGeneralProfileIdc, &generalProfileIdc)) {
        return ERROR_MALFORMED;
    }
    uint32_t compatibilityFlags;
    uint64_t constraintIdcFlags;
    if (!findParam32(kGeneralProfileCompatibilityFlags, &compatibilityFlags)
            || !findParam64(kGeneralConstraintIndicatorFlags, &constraintIdcFlags)) {
        return ERROR_MALFORMED;
    }
    uint8_t generalLevelIdc;
    if (!findParam8(kGeneralLevelIdc, &generalLevelIdc)) {
        return ERROR_MALFORMED;
    }
    uint8_t chromaFormatIdc, bitDepthLumaMinus8, bitDepthChromaMinus8;
    if (!findParam8(kChromaFormatIdc, &chromaFormatIdc)
            || !findParam8(kBitDepthLumaMinus8, &bitDepthLumaMinus8)
            || !findParam8(kBitDepthChromaMinus8, &bitDepthChromaMinus8)) {
        return ERROR_MALFORMED;
    }
    if (size > *hvccSize) {
        return NO_MEMORY;
    }
    *hvccSize = size;

    uint8_t *header = hvcc;
    header[0] = 1;
    header[1] = (kGeneralProfileSpace << 6) | (kGeneralTierFlag << 5) | kGeneralProfileIdc;
    header[2] = (compatibilityFlags >> 24) & 0xff;
    header[3] = (compatibilityFlags >> 16) & 0xff;
    header[4] = (compatibilityFlags >> 8) & 0xff;
    header[5] = compatibilityFlags & 0xff;
    header[6] = (constraintIdcFlags >> 40) & 0xff;
    header[7] = (constraintIdcFlags >> 32) & 0xff;
    header[8] = (constraintIdcFlags >> 24) & 0xff;
    header[9] = (constraintIdcFlags >> 16) & 0xff;
    header[10] = (constraintIdcFlags >> 8) & 0xff;
    header[11] = constraintIdcFlags & 0xff;
    header[12] = generalLevelIdc;
    // FIXME: parse min_spatial_segmentation_idc.
    header[13] = 0xf0;
    header[14] = 0;
    // FIXME: derive parallelismType properly.
    header[15] = 0xfc;
    header[16] = 0xfc | chromaFormatIdc;
    header[17] = 0xf8 | bitDepthLumaMinus8;
    header[18] = 0xf8 | bitDepthChromaMinus8;
    // FIXME: derive avgFrameRate
    header[19] = 0;
    header[20] = 0;
    // constantFrameRate, numTemporalLayers, temporalIdNested all set to 0.
    header[21] = nalSizeLength - 1;
    header[22] = numOfArrays;
    header += 23;
    for (size_t i = 0; i < ARRAY_SIZE(kHevcNalUnitTypes); ++i) {
        uint8_t type = kHevcNalUnitTypes[i];
        size_t numNalus = getNumNalUnitsOfType(type);
        if (numNalus == 0) {
            continue;
        }
        // array_completeness set to 0.
        header[0] = type;
        header[1] = (numNalus >> 8) & 0xff;
        header[2] = numNalus & 0xff;
        header += 3;
        for (size_t j = 0; j < numNalUnits; ++j) {
            if (getType(j) != type) {
                continue;
            }
            header[0] = (getSize(j) >> 8) & 0xff;
            header[1] = getSize(j) & 0xff;
            if (!write(j, header + 2, size - (header - (uint8_t *)hvcc))) {
                return NO_MEMORY;
            }
            header += (2 + getSize(j));
        }
    }
    CHECK_EQ(header - size, hvcc);

    return OK;
}

}  // namespace android
