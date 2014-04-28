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

#include <img_utils/DngUtils.h>

namespace android {
namespace img_utils {

OpcodeListBuilder::OpcodeListBuilder() : mOpList(), mEndianOut(&mOpList, BIG) {
    if(mEndianOut.open() != OK) {
        ALOGE("%s: Open failed.", __FUNCTION__);
    }
}

OpcodeListBuilder::~OpcodeListBuilder() {
    if(mEndianOut.close() != OK) {
        ALOGE("%s: Close failed.", __FUNCTION__);
    }
}

size_t OpcodeListBuilder::getSize() const {
    return mOpList.getSize() + sizeof(mCount);
}

uint32_t OpcodeListBuilder::getCount() const {
    return mCount;
}

status_t OpcodeListBuilder::buildOpList(uint8_t* buf) const {
    uint32_t count = convertToBigEndian(mCount);
    memcpy(buf, &count, sizeof(count));
    memcpy(buf + sizeof(count), mOpList.getArray(), mOpList.getSize());
    return OK;
}

status_t OpcodeListBuilder::addGainMapsForMetadata(uint32_t lsmWidth,
                                                   uint32_t lsmHeight,
                                                   uint32_t activeAreaTop,
                                                   uint32_t activeAreaLeft,
                                                   uint32_t activeAreaBottom,
                                                   uint32_t activeAreaRight,
                                                   CfaLayout cfa,
                                                   const float* lensShadingMap) {
    uint32_t activeAreaWidth = activeAreaRight - activeAreaLeft;
    uint32_t activeAreaHeight = activeAreaBottom - activeAreaTop;
    double spacingV = 1.0 / lsmHeight;
    double spacingH = 1.0 / lsmWidth;

    float redMap[lsmWidth * lsmHeight];
    float greenEvenMap[lsmWidth * lsmHeight];
    float greenOddMap[lsmWidth * lsmHeight];
    float blueMap[lsmWidth * lsmHeight];

    size_t lsmMapSize = lsmWidth * lsmHeight * 4;

    // Split lens shading map channels into separate arrays
    size_t j = 0;
    for (size_t i = 0; i < lsmMapSize; i += 4, ++j) {
        redMap[j] = lensShadingMap[i + LSM_R_IND];
        greenEvenMap[j] = lensShadingMap[i + LSM_GE_IND];
        greenOddMap[j] = lensShadingMap[i + LSM_GO_IND];
        blueMap[j] = lensShadingMap[i + LSM_B_IND];
    }

    uint32_t redTop = 0;
    uint32_t redLeft = 0;
    uint32_t greenEvenTop = 0;
    uint32_t greenEvenLeft = 1;
    uint32_t greenOddTop = 1;
    uint32_t greenOddLeft = 0;
    uint32_t blueTop = 1;
    uint32_t blueLeft = 1;

    switch(cfa) {
        case CFA_RGGB:
            redTop = 0;
            redLeft = 0;
            greenEvenTop = 0;
            greenEvenLeft = 1;
            greenOddTop = 1;
            greenOddLeft = 0;
            blueTop = 1;
            blueLeft = 1;
            break;
        case CFA_GRBG:
            redTop = 0;
            redLeft = 1;
            greenEvenTop = 0;
            greenEvenLeft = 0;
            greenOddTop = 1;
            greenOddLeft = 1;
            blueTop = 1;
            blueLeft = 0;
            break;
        case CFA_GBRG:
            redTop = 1;
            redLeft = 0;
            greenEvenTop = 0;
            greenEvenLeft = 0;
            greenOddTop = 1;
            greenOddLeft = 1;
            blueTop = 0;
            blueLeft = 1;
            break;
        case CFA_BGGR:
            redTop = 1;
            redLeft = 1;
            greenEvenTop = 0;
            greenEvenLeft = 1;
            greenOddTop = 1;
            greenOddLeft = 0;
            blueTop = 0;
            blueLeft = 0;
            break;
        default:
            ALOGE("%s: Unknown CFA layout %d", __FUNCTION__, cfa);
            return BAD_VALUE;
    }

    status_t err = addGainMap(/*top*/redTop,
                              /*left*/redLeft,
                              /*bottom*/activeAreaHeight - 1,
                              /*right*/activeAreaWidth - 1,
                              /*plane*/0,
                              /*planes*/1,
                              /*rowPitch*/2,
                              /*colPitch*/2,
                              /*mapPointsV*/lsmHeight,
                              /*mapPointsH*/lsmWidth,
                              /*mapSpacingV*/spacingV,
                              /*mapSpacingH*/spacingH,
                              /*mapOriginV*/0,
                              /*mapOriginH*/0,
                              /*mapPlanes*/1,
                              /*mapGains*/redMap);
    if (err != OK) return err;

    err = addGainMap(/*top*/greenEvenTop,
                     /*left*/greenEvenLeft,
                     /*bottom*/activeAreaHeight - 1,
                     /*right*/activeAreaWidth - 1,
                     /*plane*/0,
                     /*planes*/1,
                     /*rowPitch*/2,
                     /*colPitch*/2,
                     /*mapPointsV*/lsmHeight,
                     /*mapPointsH*/lsmWidth,
                     /*mapSpacingV*/spacingV,
                     /*mapSpacingH*/spacingH,
                     /*mapOriginV*/0,
                     /*mapOriginH*/0,
                     /*mapPlanes*/1,
                     /*mapGains*/greenEvenMap);
    if (err != OK) return err;

    err = addGainMap(/*top*/greenOddTop,
                     /*left*/greenOddLeft,
                     /*bottom*/activeAreaHeight - 1,
                     /*right*/activeAreaWidth - 1,
                     /*plane*/0,
                     /*planes*/1,
                     /*rowPitch*/2,
                     /*colPitch*/2,
                     /*mapPointsV*/lsmHeight,
                     /*mapPointsH*/lsmWidth,
                     /*mapSpacingV*/spacingV,
                     /*mapSpacingH*/spacingH,
                     /*mapOriginV*/0,
                     /*mapOriginH*/0,
                     /*mapPlanes*/1,
                     /*mapGains*/greenOddMap);
    if (err != OK) return err;

    err = addGainMap(/*top*/blueTop,
                     /*left*/blueLeft,
                     /*bottom*/activeAreaHeight - 1,
                     /*right*/activeAreaWidth - 1,
                     /*plane*/0,
                     /*planes*/1,
                     /*rowPitch*/2,
                     /*colPitch*/2,
                     /*mapPointsV*/lsmHeight,
                     /*mapPointsH*/lsmWidth,
                     /*mapSpacingV*/spacingV,
                     /*mapSpacingH*/spacingH,
                     /*mapOriginV*/0,
                     /*mapOriginH*/0,
                     /*mapPlanes*/1,
                     /*mapGains*/blueMap);
    return err;
}

status_t OpcodeListBuilder::addGainMap(uint32_t top,
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
                                       const float* mapGains) {

    uint32_t opcodeId = GAIN_MAP_ID;

    status_t err = mEndianOut.write(&opcodeId, 0, 1);
    if (err != OK) return err;

    uint8_t version[] = {1, 3, 0, 0};
    err = mEndianOut.write(version, 0, NELEMS(version));
    if (err != OK) return err;

    uint32_t flags = FLAG_OPTIONAL | FLAG_OPTIONAL_FOR_PREVIEW;
    err = mEndianOut.write(&flags, 0, 1);
    if (err != OK) return err;

    const uint32_t NUMBER_INT_ARGS = 11;
    const uint32_t NUMBER_DOUBLE_ARGS = 4;

    uint32_t totalSize = NUMBER_INT_ARGS * sizeof(uint32_t) + NUMBER_DOUBLE_ARGS * sizeof(double) +
            mapPointsV * mapPointsH * mapPlanes * sizeof(float);

    err = mEndianOut.write(&totalSize, 0, 1);
    if (err != OK) return err;

    // Batch writes as much as possible
    uint32_t settings1[] = { top,
                            left,
                            bottom,
                            right,
                            plane,
                            planes,
                            rowPitch,
                            colPitch,
                            mapPointsV,
                            mapPointsH };

    err = mEndianOut.write(settings1, 0, NELEMS(settings1));
    if (err != OK) return err;

    double settings2[] = { mapSpacingV,
                          mapSpacingH,
                          mapOriginV,
                          mapOriginH };

    err = mEndianOut.write(settings2, 0, NELEMS(settings2));
    if (err != OK) return err;

    err = mEndianOut.write(&mapPlanes, 0, 1);
    if (err != OK) return err;

    err = mEndianOut.write(mapGains, 0, mapPointsV * mapPointsH * mapPlanes);
    if (err != OK) return err;

    mCount++;

    return OK;
}

} /*namespace img_utils*/
} /*namespace android*/
