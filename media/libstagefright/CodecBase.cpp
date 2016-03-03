/*
 * Copyright (C) 2014 The Android Open Source Project
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
#define LOG_TAG "CodecBase"

#include <inttypes.h>

#include <media/stagefright/CodecBase.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALookup.h>

namespace android {

CodecBase::CodecBase() {
}

CodecBase::~CodecBase() {
}

CodecBase::PortDescription::PortDescription() {
}

CodecBase::PortDescription::~PortDescription() {
}

/***************************************** COLOR SUPPORT *****************************************/

// shortcut names for brevity in the following tables
typedef ColorAspects CA;
typedef CodecBase CB;

ALookup<CB::ColorRange, CA::Range> sRanges{
    {
        { CB::kColorRangeLimited, CA::RangeLimited },
        { CB::kColorRangeFull, CA::RangeFull },
        { CB::kColorRangeUnspecified, CA::RangeUnspecified },
    }
};

ALookup<CB::ColorStandard, std::pair<CA::Primaries, CA::MatrixCoeffs>> sStandards {
    {
        { CB::kColorStandardUnspecified,    { CA::PrimariesUnspecified, CA::MatrixUnspecified } },
        { CB::kColorStandardBT709,          { CA::PrimariesBT709_5, CA::MatrixBT709_5 } },
        { CB::kColorStandardBT601_625,      { CA::PrimariesBT601_6_625, CA::MatrixBT601_6 } },
        { CB::kColorStandardBT601_625_Unadjusted,
                                            // this is a really close match
                                            { CA::PrimariesBT601_6_625, CA::MatrixBT709_5 } },
        { CB::kColorStandardBT601_525,      { CA::PrimariesBT601_6_525, CA::MatrixBT601_6 } },
        { CB::kColorStandardBT601_525_Unadjusted,
                                            { CA::PrimariesBT601_6_525, CA::MatrixSMPTE240M } },
        { CB::kColorStandardBT2020,         { CA::PrimariesBT2020, CA::MatrixBT2020 } },
        { CB::kColorStandardBT2020Constant, { CA::PrimariesBT2020, CA::MatrixBT2020Constant } },
        { CB::kColorStandardBT470M,         { CA::PrimariesBT470_6M, CA::MatrixBT470_6M } },
        // NOTE: there is no close match to the matrix used by standard film, chose closest
        { CB::kColorStandardFilm,           { CA::PrimariesGenericFilm, CA::MatrixBT2020 } },
    }
};

ALookup<CB::ColorTransfer, CA::Transfer> sTransfers{
    {
        { CB::kColorTransferUnspecified,    CA::TransferUnspecified },
        { CB::kColorTransferLinear,         CA::TransferLinear },
        { CB::kColorTransferSRGB,           CA::TransferSRGB },
        { CB::kColorTransferSMPTE_170M,     CA::TransferSMPTE170M },
        { CB::kColorTransferGamma22,        CA::TransferGamma22 },
        { CB::kColorTransferGamma28,        CA::TransferGamma28 },
        { CB::kColorTransferST2084,         CA::TransferST2084 },
        { CB::kColorTransferHLG,            CA::TransferHLG },
    }
};

static bool isValid(ColorAspects::Primaries p) {
    return p <= ColorAspects::PrimariesOther;
}

static bool isDefined(ColorAspects::Primaries p) {
    return p <= ColorAspects::PrimariesBT2020;
}

static bool isValid(ColorAspects::MatrixCoeffs c) {
    return c <= ColorAspects::MatrixOther;
}

static bool isDefined(ColorAspects::MatrixCoeffs c) {
    return c <= ColorAspects::MatrixBT2020Constant;
}

//static
int32_t CodecBase::wrapColorAspectsIntoColorStandard(
        ColorAspects::Primaries primaries, ColorAspects::MatrixCoeffs coeffs) {
    ColorStandard res;
    if (sStandards.map(std::make_pair(primaries, coeffs), &res)) {
        return res;
    } else if (!isValid(primaries) || !isValid(coeffs)) {
        return kColorStandardUnspecified;
    }

    // check platform media limits
    uint32_t numPrimaries = ColorAspects::PrimariesBT2020 + 1;
    if (isDefined(primaries) && isDefined(coeffs)) {
        return kColorStandardExtendedStart + primaries + coeffs * numPrimaries;
    } else {
        return kColorStandardVendorStart + primaries + coeffs * 0x100;
    }
}

//static
status_t CodecBase::unwrapColorAspectsFromColorStandard(
        int32_t standard,
        ColorAspects::Primaries *primaries, ColorAspects::MatrixCoeffs *coeffs) {
    std::pair<ColorAspects::Primaries, ColorAspects::MatrixCoeffs> res;
    if (sStandards.map((ColorStandard)standard, &res)) {
        *primaries = res.first;
        *coeffs = res.second;
        return OK;
    }

    int32_t start = kColorStandardExtendedStart;
    int32_t numPrimaries = ColorAspects::PrimariesBT2020 + 1;
    int32_t numCoeffs = ColorAspects::MatrixBT2020Constant + 1;
    if (standard >= (int32_t)kColorStandardVendorStart) {
        start = kColorStandardVendorStart;
        numPrimaries = ColorAspects::PrimariesOther + 1; // 0x100
        numCoeffs = ColorAspects::MatrixOther + 1; // 0x100;
    }
    if (standard >= start && standard < start + numPrimaries * numCoeffs) {
        int32_t product = standard - start;
        *primaries = (ColorAspects::Primaries)(product % numPrimaries);
        *coeffs = (ColorAspects::MatrixCoeffs)(product / numPrimaries);
        return OK;
    }
    *primaries = ColorAspects::PrimariesOther;
    *coeffs = ColorAspects::MatrixOther;
    return BAD_VALUE;
}

static bool isValid(ColorAspects::Range r) {
    return r <= ColorAspects::RangeOther;
}

static bool isDefined(ColorAspects::Range r) {
    return r <= ColorAspects::RangeLimited;
}

//  static
int32_t CodecBase::wrapColorAspectsIntoColorRange(ColorAspects::Range range) {
    ColorRange res;
    if (sRanges.map(range, &res)) {
        return res;
    } else if (!isValid(range)) {
        return kColorRangeUnspecified;
    } else {
        CHECK(!isDefined(range));
        // all platform values are in sRanges
        return kColorRangeVendorStart + range;
    }
}

//static
status_t CodecBase::unwrapColorAspectsFromColorRange(
        int32_t range, ColorAspects::Range *aspect) {
    if (sRanges.map((ColorRange)range, aspect)) {
        return OK;
    }

    int32_t start = kColorRangeVendorStart;
    int32_t numRanges = ColorAspects::RangeOther + 1; // 0x100
    if (range >= start && range < start + numRanges) {
        *aspect = (ColorAspects::Range)(range - start);
        return OK;
    }
    *aspect = ColorAspects::RangeOther;
    return BAD_VALUE;
}

static bool isValid(ColorAspects::Transfer t) {
    return t <= ColorAspects::TransferOther;
}

static bool isDefined(ColorAspects::Transfer t) {
    return t <= ColorAspects::TransferHLG
            || (t >= ColorAspects::TransferSMPTE240M && t <= ColorAspects::TransferST428);
}

//  static
int32_t CodecBase::wrapColorAspectsIntoColorTransfer(
        ColorAspects::Transfer transfer) {
    ColorTransfer res;
    if (sTransfers.map(transfer, &res)) {
        return res;
    } else if (!isValid(transfer)) {
        return kColorTransferUnspecified;
    } else if (isDefined(transfer)) {
        return kColorTransferExtendedStart + transfer;
    } else {
        // all platform values are in sRanges
        return kColorTransferVendorStart + transfer;
    }
}

//static
status_t CodecBase::unwrapColorAspectsFromColorTransfer(
        int32_t transfer, ColorAspects::Transfer *aspect) {
    if (sTransfers.map((ColorTransfer)transfer, aspect)) {
        return OK;
    }

    int32_t start = kColorTransferExtendedStart;
    int32_t numTransfers = ColorAspects::TransferST428 + 1;
    if (transfer >= (int32_t)kColorTransferVendorStart) {
        start = kColorTransferVendorStart;
        numTransfers = ColorAspects::TransferOther + 1; // 0x100
    }
    if (transfer >= start && transfer < start + numTransfers) {
        *aspect = (ColorAspects::Transfer)(transfer - start);
        return OK;
    }
    *aspect = ColorAspects::TransferOther;
    return BAD_VALUE;
}

// static
status_t CodecBase::convertPlatformColorAspectsToCodecAspects(
    int32_t range, int32_t standard, int32_t transfer, ColorAspects &aspects) {
    status_t res1 = unwrapColorAspectsFromColorRange(range, &aspects.mRange);
    status_t res2 = unwrapColorAspectsFromColorStandard(
            standard, &aspects.mPrimaries, &aspects.mMatrixCoeffs);
    status_t res3 = unwrapColorAspectsFromColorTransfer(transfer, &aspects.mTransfer);
    return res1 != OK ? res1 : (res2 != OK ? res2 : res3);
}

// static
status_t CodecBase::convertCodecColorAspectsToPlatformAspects(
    const ColorAspects &aspects, int32_t *range, int32_t *standard, int32_t *transfer) {
    *range = wrapColorAspectsIntoColorRange(aspects.mRange);
    *standard = wrapColorAspectsIntoColorStandard(aspects.mPrimaries, aspects.mMatrixCoeffs);
    *transfer = wrapColorAspectsIntoColorTransfer(aspects.mTransfer);
    if (isValid(aspects.mRange) && isValid(aspects.mPrimaries)
            && isValid(aspects.mMatrixCoeffs) && isValid(aspects.mTransfer)) {
        return OK;
    } else {
        return BAD_VALUE;
    }
}

// static
void CodecBase::setDefaultPlatformColorAspectsIfNeeded(
        int32_t &range, int32_t &standard, int32_t &transfer,
        int32_t width, int32_t height) {
    if (range == CodecBase::kColorRangeUnspecified) {
        range = CodecBase::kColorRangeLimited;
    }

    if (standard == CodecBase::kColorStandardUnspecified) {
        // Default to BT2020, BT709 or BT601 based on size. Allow 2.35:1 aspect ratio. Limit BT601
        // to PAL or smaller, BT2020 to 4K or larger, leaving BT709 for all resolutions in between.
        if (width >= 3840 || height >= 3840 || width * (int64_t)height >= 3840 * 1634) {
            standard = CodecBase::kColorStandardBT2020;
        } else if ((width <= 720 && height > 480) || (height <= 720 && width > 480)) {
            standard = CodecBase::kColorStandardBT601_625;
        } else if ((width <= 720 && height <= 480) || (height <= 720 && width <= 480)) {
            standard = CodecBase::kColorStandardBT601_525;
        } else {
            standard = CodecBase::kColorStandardBT709;
        }
    }

    if (transfer == CodecBase::kColorTransferUnspecified) {
        transfer = CodecBase::kColorTransferSMPTE_170M;
    }
}

// static
void CodecBase::setDefaultCodecColorAspectsIfNeeded(
        ColorAspects &aspects, int32_t width, int32_t height) {
    // reuse other method to get default aspects
    int32_t range = 0, standard = 0, transfer = 0;
    setDefaultPlatformColorAspectsIfNeeded(range, standard, transfer, width, height);
    ColorAspects defaultAspects;
    memset(&defaultAspects, 0, sizeof(defaultAspects));
    convertPlatformColorAspectsToCodecAspects(range, standard, transfer, defaultAspects);

    if (aspects.mRange == ColorAspects::RangeUnspecified) {
        aspects.mRange = defaultAspects.mRange;
    }
    if (aspects.mPrimaries == ColorAspects::PrimariesUnspecified) {
        aspects.mPrimaries = defaultAspects.mPrimaries;
    }
    if (aspects.mMatrixCoeffs == ColorAspects::MatrixUnspecified) {
        aspects.mMatrixCoeffs = defaultAspects.mMatrixCoeffs;
    }
    if (aspects.mTransfer == ColorAspects::TransferUnspecified) {
        aspects.mTransfer = defaultAspects.mTransfer;
    }
}

/**************************************************************************************************/

}  // namespace android
