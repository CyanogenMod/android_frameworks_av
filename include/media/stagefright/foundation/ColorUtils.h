/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef COLOR_UTILS_H_

#define COLOR_UTILS_H_

#include <stdint.h>

#define STRINGIFY_ENUMS

#include <media/stagefright/foundation/AMessage.h>

#include <media/hardware/VideoAPI.h>
#include <system/graphics.h>

namespace android {

struct ColorUtils {
    /*
     * Media-platform color constants. MediaCodec uses (an extended version of) platform-defined
     * constants that are derived from HAL_DATASPACE, since these are directly exposed to the user.
     * We extend the values to maintain the richer set of information defined inside media
     * containers and bitstreams that are not supported by the platform. We also expect vendors
     * to extend some of these values with vendor-specific values. These are separated into a
     * vendor-extension section so they won't collide with future platform values.
     */

#define GET_HAL_ENUM(class, name) HAL_DATASPACE_##class##name
#define GET_HAL_BITFIELD(class, name) (GET_HAL_ENUM(class, _##name) >> GET_HAL_ENUM(class, _SHIFT))

    enum ColorStandard : uint32_t {
        kColorStandardUnspecified =          GET_HAL_BITFIELD(STANDARD, UNSPECIFIED),
        kColorStandardBT709 =                GET_HAL_BITFIELD(STANDARD, BT709),
        kColorStandardBT601_625 =            GET_HAL_BITFIELD(STANDARD, BT601_625),
        kColorStandardBT601_625_Unadjusted = GET_HAL_BITFIELD(STANDARD, BT601_625_UNADJUSTED),
        kColorStandardBT601_525 =            GET_HAL_BITFIELD(STANDARD, BT601_525),
        kColorStandardBT601_525_Unadjusted = GET_HAL_BITFIELD(STANDARD, BT601_525_UNADJUSTED),
        kColorStandardBT2020 =               GET_HAL_BITFIELD(STANDARD, BT2020),
        kColorStandardBT2020Constant =       GET_HAL_BITFIELD(STANDARD, BT2020_CONSTANT_LUMINANCE),
        kColorStandardBT470M =               GET_HAL_BITFIELD(STANDARD, BT470M),
        kColorStandardFilm =                 GET_HAL_BITFIELD(STANDARD, FILM),
        kColorStandardMax =                  GET_HAL_BITFIELD(STANDARD, MASK),

        /* This marks a section of color-standard values that are not supported by graphics HAL,
           but track defined color primaries-matrix coefficient combinations in media.
           These are stable for a given release. */
        kColorStandardExtendedStart = kColorStandardMax + 1,

        /* This marks a section of color-standard values that are not supported by graphics HAL
           nor using media defined color primaries or matrix coefficients. These may differ per
           device. */
        kColorStandardVendorStart = 0x10000,
    };

    enum ColorTransfer : uint32_t  {
        kColorTransferUnspecified = GET_HAL_BITFIELD(TRANSFER, UNSPECIFIED),
        kColorTransferLinear =      GET_HAL_BITFIELD(TRANSFER, LINEAR),
        kColorTransferSRGB =        GET_HAL_BITFIELD(TRANSFER, SRGB),
        kColorTransferSMPTE_170M =  GET_HAL_BITFIELD(TRANSFER, SMPTE_170M),
        kColorTransferGamma22 =     GET_HAL_BITFIELD(TRANSFER, GAMMA2_2),
        kColorTransferGamma28 =     GET_HAL_BITFIELD(TRANSFER, GAMMA2_8),
        kColorTransferST2084 =      GET_HAL_BITFIELD(TRANSFER, ST2084),
        kColorTransferHLG =         GET_HAL_BITFIELD(TRANSFER, HLG),
        kColorTransferMax =         GET_HAL_BITFIELD(TRANSFER, MASK),

        /* This marks a section of color-transfer values that are not supported by graphics HAL,
           but track media-defined color-transfer. These are stable for a given release. */
        kColorTransferExtendedStart = kColorTransferMax + 1,

        /* This marks a section of color-transfer values that are not supported by graphics HAL
           nor defined by media. These may differ per device. */
        kColorTransferVendorStart = 0x10000,
    };

    enum ColorRange : uint32_t  {
        kColorRangeUnspecified = GET_HAL_BITFIELD(RANGE, UNSPECIFIED),
        kColorRangeFull =        GET_HAL_BITFIELD(RANGE, FULL),
        kColorRangeLimited =     GET_HAL_BITFIELD(RANGE, LIMITED),
        kColorRangeMax =         GET_HAL_BITFIELD(RANGE, MASK),

        /* This marks a section of color-transfer values that are not supported by graphics HAL,
           but track media-defined color-transfer. These are stable for a given release. */
        kColorRangeExtendedStart = kColorRangeMax + 1,

        /* This marks a section of color-transfer values that are not supported by graphics HAL
           nor defined by media. These may differ per device. */
        kColorRangeVendorStart = 0x10000,
    };

#undef GET_HAL_BITFIELD
#undef GET_HAL_ENUM

    /*
     * Static utilities for codec support
     */

    // using int32_t for media range/standard/transfers to denote extended ranges
    // wrap methods change invalid aspects to the Unspecified value
    static int32_t wrapColorAspectsIntoColorStandard(
            ColorAspects::Primaries primaries, ColorAspects::MatrixCoeffs coeffs);
    static int32_t wrapColorAspectsIntoColorRange(ColorAspects::Range range);
    static int32_t wrapColorAspectsIntoColorTransfer(ColorAspects::Transfer transfer);

    // unwrap methods change invalid aspects to the Other value
    static status_t unwrapColorAspectsFromColorRange(
            int32_t range, ColorAspects::Range *aspect);
    static status_t unwrapColorAspectsFromColorTransfer(
            int32_t transfer, ColorAspects::Transfer *aspect);
    static status_t unwrapColorAspectsFromColorStandard(
            int32_t standard,
            ColorAspects::Primaries *primaries, ColorAspects::MatrixCoeffs *coeffs);

    static status_t convertPlatformColorAspectsToCodecAspects(
            int32_t range, int32_t standard, int32_t transfer, ColorAspects &aspects);
    static status_t convertCodecColorAspectsToPlatformAspects(
            const ColorAspects &aspects, int32_t *range, int32_t *standard, int32_t *transfer);

    // converts Other values to Unspecified
    static void convertCodecColorAspectsToIsoAspects(
            const ColorAspects &aspects,
            int32_t *primaries, int32_t *transfer, int32_t *coeffs, bool *fullRange);
    // converts unsupported values to Other
    static void convertIsoColorAspectsToCodecAspects(
            int32_t primaries, int32_t transfer, int32_t coeffs, bool fullRange,
            ColorAspects &aspects);

    // updates Unspecified color aspects to their defaults based on the video size
    static void setDefaultCodecColorAspectsIfNeeded(
            ColorAspects &aspects, int32_t width, int32_t height);

    // it returns the closest dataSpace for given color |aspects|. if |mayExpand| is true, it allows
    // returning a larger dataSpace that contains the color space given by |aspects|, and is better
    // suited to blending. This requires implicit color space conversion on part of the device.
    static android_dataspace getDataSpaceForColorAspects(ColorAspects &aspects, bool mayExpand);

    // converts |dataSpace| to a V0 enum, and returns true if dataSpace is an aspect-only value
    static bool convertDataSpaceToV0(android_dataspace &dataSpace);

    // compares |aspect| to |orig|. Returns |true| if any aspects have changed, except if they
    // changed to Unspecified value. It also sets the changed values to Unspecified in |aspect|.
    static bool checkIfAspectsChangedAndUnspecifyThem(
            ColorAspects &aspects, const ColorAspects &orig, bool usePlatformAspects = false);

    // finds color config in format, defaulting them to 0.
    static void getColorConfigFromFormat(
            const sp<AMessage> &format, int *range, int *standard, int *transfer);

    // copies existing color config from |source| to |target|.
    static void copyColorConfig(const sp<AMessage> &source, sp<AMessage> &target);

    // finds color config in format as ColorAspects, defaulting them to 0.
    static void getColorAspectsFromFormat(const sp<AMessage> &format, ColorAspects &aspects);

    // writes |aspects| into format. iff |force| is false, Unspecified values are not
    // written.
    static void setColorAspectsIntoFormat(
            const ColorAspects &aspects, sp<AMessage> &format, bool force = false);

    // finds HDR metadata in format as HDRStaticInfo, defaulting them to 0.
    // Return |true| if could find HDR metadata in format. Otherwise, return |false|.
    static bool getHDRStaticInfoFromFormat(const sp<AMessage> &format, HDRStaticInfo *info);

    // writes |info| into format.
    static void setHDRStaticInfoIntoFormat(const HDRStaticInfo &info, sp<AMessage> &format);
};

inline static const char *asString(android::ColorUtils::ColorStandard i, const char *def = "??") {
    using namespace android;
    switch (i) {
        case ColorUtils::kColorStandardUnspecified:          return "Unspecified";
        case ColorUtils::kColorStandardBT709:                return "BT709";
        case ColorUtils::kColorStandardBT601_625:            return "BT601_625";
        case ColorUtils::kColorStandardBT601_625_Unadjusted: return "BT601_625_Unadjusted";
        case ColorUtils::kColorStandardBT601_525:            return "BT601_525";
        case ColorUtils::kColorStandardBT601_525_Unadjusted: return "BT601_525_Unadjusted";
        case ColorUtils::kColorStandardBT2020:               return "BT2020";
        case ColorUtils::kColorStandardBT2020Constant:       return "BT2020Constant";
        case ColorUtils::kColorStandardBT470M:               return "BT470M";
        case ColorUtils::kColorStandardFilm:                 return "Film";
        default:                                            return def;
    }
}

inline static const char *asString(android::ColorUtils::ColorTransfer i, const char *def = "??") {
    using namespace android;
    switch (i) {
        case ColorUtils::kColorTransferUnspecified: return "Unspecified";
        case ColorUtils::kColorTransferLinear:      return "Linear";
        case ColorUtils::kColorTransferSRGB:        return "SRGB";
        case ColorUtils::kColorTransferSMPTE_170M:  return "SMPTE_170M";
        case ColorUtils::kColorTransferGamma22:     return "Gamma22";
        case ColorUtils::kColorTransferGamma28:     return "Gamma28";
        case ColorUtils::kColorTransferST2084:      return "ST2084";
        case ColorUtils::kColorTransferHLG:         return "HLG";
        default:                                   return def;
    }
}

inline static const char *asString(android::ColorUtils::ColorRange i, const char *def = "??") {
    using namespace android;
    switch (i) {
        case ColorUtils::kColorRangeUnspecified: return "Unspecified";
        case ColorUtils::kColorRangeFull:        return "Full";
        case ColorUtils::kColorRangeLimited:     return "Limited";
        default:                                return def;
    }
}

}  // namespace android

#endif  // COLOR_UTILS_H_

