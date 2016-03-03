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

#ifndef CODEC_BASE_H_

#define CODEC_BASE_H_

#include <stdint.h>

#include <media/IOMX.h>
#include <media/MediaCodecInfo.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/hardware/HardwareAPI.h>

#include <utils/NativeHandle.h>

#include <system/graphics.h>

namespace android {

struct ABuffer;
struct PersistentSurface;

struct CodecBase : public AHandler {
    enum {
        kWhatFillThisBuffer      = 'fill',
        kWhatDrainThisBuffer     = 'drai',
        kWhatEOS                 = 'eos ',
        kWhatShutdownCompleted   = 'scom',
        kWhatFlushCompleted      = 'fcom',
        kWhatOutputFormatChanged = 'outC',
        kWhatError               = 'erro',
        kWhatComponentAllocated  = 'cAll',
        kWhatComponentConfigured = 'cCon',
        kWhatInputSurfaceCreated = 'isfc',
        kWhatInputSurfaceAccepted = 'isfa',
        kWhatSignaledInputEOS    = 'seos',
        kWhatBuffersAllocated    = 'allc',
        kWhatOutputFramesRendered = 'outR',
    };

    virtual void setNotificationMessage(const sp<AMessage> &msg) = 0;

    virtual void initiateAllocateComponent(const sp<AMessage> &msg) = 0;
    virtual void initiateConfigureComponent(const sp<AMessage> &msg) = 0;
    virtual void initiateCreateInputSurface() = 0;
    virtual void initiateSetInputSurface(
            const sp<PersistentSurface> &surface) = 0;
    virtual void initiateStart() = 0;
    virtual void initiateShutdown(bool keepComponentAllocated = false) = 0;

    // require an explicit message handler
    virtual void onMessageReceived(const sp<AMessage> &msg) = 0;

    virtual status_t queryCapabilities(
            const AString &name, const AString &mime, bool isEncoder,
            sp<MediaCodecInfo::Capabilities> *caps /* nonnull */) { return INVALID_OPERATION; }

    virtual status_t setSurface(const sp<Surface> &surface) { return INVALID_OPERATION; }

    virtual void signalFlush() = 0;
    virtual void signalResume() = 0;

    virtual void signalRequestIDRFrame() = 0;
    virtual void signalSetParameters(const sp<AMessage> &msg) = 0;
    virtual void signalEndOfInputStream() = 0;

    struct PortDescription : public RefBase {
        virtual size_t countBuffers() = 0;
        virtual IOMX::buffer_id bufferIDAt(size_t index) const = 0;
        virtual sp<ABuffer> bufferAt(size_t index) const = 0;
        virtual sp<NativeHandle> handleAt(size_t index) const { return NULL; };
        virtual sp<RefBase> memRefAt(size_t index) const { return NULL; }

    protected:
        PortDescription();
        virtual ~PortDescription();

    private:
        DISALLOW_EVIL_CONSTRUCTORS(PortDescription);
    };

    /*
     * Codec-related defines
     */

    /**********************************************************************************************/

    /*
     * Media-platform color constants. MediaCodec uses (an extended version of) platform-defined
     * constants that are derived from HAL_DATASPACE, since these are directly exposed to the user.
     * We extend the values to maintain the richer set of information defined inside media
     * containers and bitstreams that are not supported by the platform. We also expect vendors
     * to extend some of these values with vendor-specific values. These are separated into a
     * vendor-extension section so they won't collide with future platform values.
     */

    enum ColorStandard : uint32_t {
        kColorStandardUnspecified =
                HAL_DATASPACE_STANDARD_UNSPECIFIED >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardBT709 =     HAL_DATASPACE_STANDARD_BT709 >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardBT601_625 = HAL_DATASPACE_STANDARD_BT601_625 >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardBT601_625_Unadjusted =
                HAL_DATASPACE_STANDARD_BT601_625_UNADJUSTED >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardBT601_525 = HAL_DATASPACE_STANDARD_BT601_525 >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardBT601_525_Unadjusted =
                HAL_DATASPACE_STANDARD_BT601_525_UNADJUSTED >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardBT2020 =    HAL_DATASPACE_STANDARD_BT2020 >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardBT2020Constant =
                HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardBT470M =    HAL_DATASPACE_STANDARD_BT470M >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardFilm =      HAL_DATASPACE_STANDARD_FILM >> HAL_DATASPACE_STANDARD_SHIFT,
        kColorStandardMax =       HAL_DATASPACE_STANDARD_MASK >> HAL_DATASPACE_STANDARD_SHIFT,

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
        kColorTransferUnspecified =
                HAL_DATASPACE_TRANSFER_UNSPECIFIED >> HAL_DATASPACE_TRANSFER_SHIFT,
        kColorTransferLinear =      HAL_DATASPACE_TRANSFER_LINEAR >> HAL_DATASPACE_TRANSFER_SHIFT,
        kColorTransferSRGB =        HAL_DATASPACE_TRANSFER_SRGB >> HAL_DATASPACE_TRANSFER_SHIFT,
        kColorTransferSMPTE_170M =
                HAL_DATASPACE_TRANSFER_SMPTE_170M >> HAL_DATASPACE_TRANSFER_SHIFT,
        kColorTransferGamma22 =     HAL_DATASPACE_TRANSFER_GAMMA2_2 >> HAL_DATASPACE_TRANSFER_SHIFT,
        kColorTransferGamma28 =     HAL_DATASPACE_TRANSFER_GAMMA2_8 >> HAL_DATASPACE_TRANSFER_SHIFT,
        kColorTransferST2084 =      HAL_DATASPACE_TRANSFER_ST2084 >> HAL_DATASPACE_TRANSFER_SHIFT,
        kColorTransferHLG =         HAL_DATASPACE_TRANSFER_HLG >> HAL_DATASPACE_TRANSFER_SHIFT,
        kColorTransferMax =         HAL_DATASPACE_TRANSFER_MASK >> HAL_DATASPACE_TRANSFER_SHIFT,

        /* This marks a section of color-transfer values that are not supported by graphics HAL,
           but track media-defined color-transfer. These are stable for a given release. */
        kColorTransferExtendedStart = kColorTransferMax + 1,

        /* This marks a section of color-transfer values that are not supported by graphics HAL
           nor defined by media. These may differ per device. */
        kColorTransferVendorStart = 0x10000,
    };

    enum ColorRange : uint32_t  {
        kColorRangeUnspecified = HAL_DATASPACE_RANGE_UNSPECIFIED >> HAL_DATASPACE_RANGE_SHIFT,
        kColorRangeFull =        HAL_DATASPACE_RANGE_FULL >> HAL_DATASPACE_RANGE_SHIFT,
        kColorRangeLimited =     HAL_DATASPACE_RANGE_LIMITED >> HAL_DATASPACE_RANGE_SHIFT,
        kColorRangeMax =         HAL_DATASPACE_RANGE_MASK >> HAL_DATASPACE_RANGE_SHIFT,

        /* This marks a section of color-transfer values that are not supported by graphics HAL,
           but track media-defined color-transfer. These are stable for a given release. */
        kColorRangeExtendedStart = kColorRangeMax + 1,

        /* This marks a section of color-transfer values that are not supported by graphics HAL
           nor defined by media. These may differ per device. */
        kColorRangeVendorStart = 0x10000,
    };

    /*
     * Static utilities for codec support
     */

    // using int32_t for media range/standard/transfers to denote extended ranges
    static int32_t wrapColorAspectsIntoColorStandard(
            ColorAspects::Primaries primaries, ColorAspects::MatrixCoeffs coeffs);
    static int32_t wrapColorAspectsIntoColorRange(ColorAspects::Range range);
    static int32_t wrapColorAspectsIntoColorTransfer(ColorAspects::Transfer transfer);

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
            const ColorAspects &aspects,
            int32_t *range, int32_t *standard, int32_t *transfer);

    // updates unspecified range, standard and transfer values to their defaults
    static void setDefaultPlatformColorAspectsIfNeeded(
            int32_t &range, int32_t &standard, int32_t &transfer,
            int32_t width, int32_t height);
    static void setDefaultCodecColorAspectsIfNeeded(
            ColorAspects &aspects, int32_t width, int32_t height);

protected:
    CodecBase();
    virtual ~CodecBase();

private:
    DISALLOW_EVIL_CONSTRUCTORS(CodecBase);
};

}  // namespace android

#endif  // CODEC_BASE_H_

