/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "SoftAAC2"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include "SoftAAC2.h"

#include <cutils/properties.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaErrors.h>

#include <math.h>

#define FILEREAD_MAX_LAYERS 2

#define DRC_DEFAULT_MOBILE_REF_LEVEL 64  /* 64*-0.25dB = -16 dB below full scale for mobile conf */
#define DRC_DEFAULT_MOBILE_DRC_CUT   127 /* maximum compression of dynamic range for mobile conf */
#define DRC_DEFAULT_MOBILE_DRC_BOOST 127 /* maximum compression of dynamic range for mobile conf */
#define DRC_DEFAULT_MOBILE_DRC_HEAVY 1   /* switch for heavy compression for mobile conf */
#define DRC_DEFAULT_MOBILE_ENC_LEVEL -1 /* encoder target level; -1 => the value is unknown, otherwise dB step value (e.g. 64 for -16 dB) */
#define MAX_CHANNEL_COUNT            8  /* maximum number of audio channels that can be decoded */
// names of properties that can be used to override the default DRC settings
#define PROP_DRC_OVERRIDE_REF_LEVEL  "aac_drc_reference_level"
#define PROP_DRC_OVERRIDE_CUT        "aac_drc_cut"
#define PROP_DRC_OVERRIDE_BOOST      "aac_drc_boost"
#define PROP_DRC_OVERRIDE_HEAVY      "aac_drc_heavy"
#define PROP_DRC_OVERRIDE_ENC_LEVEL "aac_drc_enc_target_level"

namespace android {

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

SoftAAC2::SoftAAC2(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mAACDecoder(NULL),
      mStreamInfo(NULL),
      mIsADTS(false),
      mInputBufferCount(0),
      mOutputBufferCount(0),
      mSignalledError(false),
      mOutputPortSettingsChange(NONE) {
    for (unsigned int i = 0; i < kNumDelayBlocksMax; i++) {
        mAnchorTimeUs[i] = 0;
    }
    initPorts();
    CHECK_EQ(initDecoder(), (status_t)OK);
}

SoftAAC2::~SoftAAC2() {
    aacDecoder_Close(mAACDecoder);
    delete mOutputDelayRingBuffer;
}

void SoftAAC2::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = 0;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumInputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 8192;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    def.format.audio.cMIMEType = const_cast<char *>("audio/aac");
    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
    def.format.audio.eEncoding = OMX_AUDIO_CodingAAC;

    addPort(def);

    def.nPortIndex = 1;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = kNumOutputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 4096 * MAX_CHANNEL_COUNT;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.audio.cMIMEType = const_cast<char *>("audio/raw");
    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;

    addPort(def);
}

status_t SoftAAC2::initDecoder() {
    status_t status = UNKNOWN_ERROR;
    mAACDecoder = aacDecoder_Open(TT_MP4_ADIF, /* num layers */ 1);
    if (mAACDecoder != NULL) {
        mStreamInfo = aacDecoder_GetStreamInfo(mAACDecoder);
        if (mStreamInfo != NULL) {
            status = OK;
        }
    }

    mEndOfInput = false;
    mEndOfOutput = false;
    mOutputDelayCompensated = 0;
    mOutputDelayRingBufferSize = 2048 * MAX_CHANNEL_COUNT * kNumDelayBlocksMax;
    mOutputDelayRingBuffer = new short[mOutputDelayRingBufferSize];
    mOutputDelayRingBufferWritePos = 0;
    mOutputDelayRingBufferReadPos = 0;

    if (mAACDecoder == NULL) {
        ALOGE("AAC decoder is null. TODO: Can not call aacDecoder_SetParam in the following code");
    }

    //aacDecoder_SetParam(mAACDecoder, AAC_PCM_LIMITER_ENABLE, 0);

    //init DRC wrapper
    mDrcWrap.setDecoderHandle(mAACDecoder);
    mDrcWrap.submitStreamData(mStreamInfo);

    // for streams that contain metadata, use the mobile profile DRC settings unless overridden by platform properties
    // TODO: change the DRC settings depending on audio output device type (HDMI, loadspeaker, headphone)
    char value[PROPERTY_VALUE_MAX];
    //  DRC_PRES_MODE_WRAP_DESIRED_TARGET
    if (property_get(PROP_DRC_OVERRIDE_REF_LEVEL, value, NULL)) {
        unsigned refLevel = atoi(value);
        ALOGV("AAC decoder using desired DRC target reference level of %d instead of %d", refLevel,
                DRC_DEFAULT_MOBILE_REF_LEVEL);
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_DESIRED_TARGET, refLevel);
    } else {
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_DESIRED_TARGET, DRC_DEFAULT_MOBILE_REF_LEVEL);
    }
    //  DRC_PRES_MODE_WRAP_DESIRED_ATT_FACTOR
    if (property_get(PROP_DRC_OVERRIDE_CUT, value, NULL)) {
        unsigned cut = atoi(value);
        ALOGV("AAC decoder using desired DRC attenuation factor of %d instead of %d", cut,
                DRC_DEFAULT_MOBILE_DRC_CUT);
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_DESIRED_ATT_FACTOR, cut);
    } else {
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_DESIRED_ATT_FACTOR, DRC_DEFAULT_MOBILE_DRC_CUT);
    }
    //  DRC_PRES_MODE_WRAP_DESIRED_BOOST_FACTOR
    if (property_get(PROP_DRC_OVERRIDE_BOOST, value, NULL)) {
        unsigned boost = atoi(value);
        ALOGV("AAC decoder using desired DRC boost factor of %d instead of %d", boost,
                DRC_DEFAULT_MOBILE_DRC_BOOST);
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_DESIRED_BOOST_FACTOR, boost);
    } else {
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_DESIRED_BOOST_FACTOR, DRC_DEFAULT_MOBILE_DRC_BOOST);
    }
    //  DRC_PRES_MODE_WRAP_DESIRED_HEAVY
    if (property_get(PROP_DRC_OVERRIDE_HEAVY, value, NULL)) {
        unsigned heavy = atoi(value);
        ALOGV("AAC decoder using desried DRC heavy compression switch of %d instead of %d", heavy,
                DRC_DEFAULT_MOBILE_DRC_HEAVY);
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_DESIRED_HEAVY, heavy);
    } else {
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_DESIRED_HEAVY, DRC_DEFAULT_MOBILE_DRC_HEAVY);
    }
    // DRC_PRES_MODE_WRAP_ENCODER_TARGET
    if (property_get(PROP_DRC_OVERRIDE_ENC_LEVEL, value, NULL)) {
        unsigned encoderRefLevel = atoi(value);
        ALOGV("AAC decoder using encoder-side DRC reference level of %d instead of %d",
                encoderRefLevel, DRC_DEFAULT_MOBILE_ENC_LEVEL);
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_ENCODER_TARGET, encoderRefLevel);
    } else {
        mDrcWrap.setParam(DRC_PRES_MODE_WRAP_ENCODER_TARGET, DRC_DEFAULT_MOBILE_ENC_LEVEL);
    }

    return status;
}

OMX_ERRORTYPE SoftAAC2::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamAudioAac:
        {
            OMX_AUDIO_PARAM_AACPROFILETYPE *aacParams =
                (OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (aacParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            aacParams->nBitRate = 0;
            aacParams->nAudioBandWidth = 0;
            aacParams->nAACtools = 0;
            aacParams->nAACERtools = 0;
            aacParams->eAACProfile = OMX_AUDIO_AACObjectMain;

            aacParams->eAACStreamFormat =
                mIsADTS
                    ? OMX_AUDIO_AACStreamFormatMP4ADTS
                    : OMX_AUDIO_AACStreamFormatMP4FF;

            aacParams->eChannelMode = OMX_AUDIO_ChannelModeStereo;

            if (!isConfigured()) {
                aacParams->nChannels = 1;
                aacParams->nSampleRate = 44100;
                aacParams->nFrameLength = 0;
            } else {
                aacParams->nChannels = mStreamInfo->numChannels;
                aacParams->nSampleRate = mStreamInfo->sampleRate;
                aacParams->nFrameLength = mStreamInfo->frameSize;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioPcm:
        {
            OMX_AUDIO_PARAM_PCMMODETYPE *pcmParams =
                (OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (pcmParams->nPortIndex != 1) {
                return OMX_ErrorUndefined;
            }

            pcmParams->eNumData = OMX_NumericalDataSigned;
            pcmParams->eEndian = OMX_EndianBig;
            pcmParams->bInterleaved = OMX_TRUE;
            pcmParams->nBitPerSample = 16;
            pcmParams->ePCMMode = OMX_AUDIO_PCMModeLinear;
            pcmParams->eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcmParams->eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            pcmParams->eChannelMapping[2] = OMX_AUDIO_ChannelCF;
            pcmParams->eChannelMapping[3] = OMX_AUDIO_ChannelLFE;
            pcmParams->eChannelMapping[4] = OMX_AUDIO_ChannelLS;
            pcmParams->eChannelMapping[5] = OMX_AUDIO_ChannelRS;

            if (!isConfigured()) {
                pcmParams->nChannels = 1;
                pcmParams->nSamplingRate = 44100;
            } else {
                pcmParams->nChannels = mStreamInfo->numChannels;
                pcmParams->nSamplingRate = mStreamInfo->sampleRate;
            }

            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftAAC2::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;

            if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.aac",
                        OMX_MAX_STRINGNAME_SIZE - 1)) {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAac:
        {
            const OMX_AUDIO_PARAM_AACPROFILETYPE *aacParams =
                (const OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (aacParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            if (aacParams->eAACStreamFormat == OMX_AUDIO_AACStreamFormatMP4FF) {
                mIsADTS = false;
            } else if (aacParams->eAACStreamFormat
                        == OMX_AUDIO_AACStreamFormatMP4ADTS) {
                mIsADTS = true;
            } else {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioPcm:
        {
            const OMX_AUDIO_PARAM_PCMMODETYPE *pcmParams =
                (OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (pcmParams->nPortIndex != 1) {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalSetParameter(index, params);
    }
}

bool SoftAAC2::isConfigured() const {
    return mInputBufferCount > 0;
}

void SoftAAC2::configureDownmix() const {
    char value[PROPERTY_VALUE_MAX];
    if (!(property_get("media.aac_51_output_enabled", value, NULL)
            && (!strcmp(value, "1") || !strcasecmp(value, "true")))) {
        ALOGI("limiting to stereo output");
        aacDecoder_SetParam(mAACDecoder, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);
        // By default, the decoder creates a 5.1 channel downmix signal
        // for seven and eight channel input streams. To enable 6.1 and 7.1 channel output
        // use aacDecoder_SetParam(mAACDecoder, AAC_PCM_MAX_OUTPUT_CHANNELS, -1)
    }
}

bool SoftAAC2::outputDelayRingBufferPutSamples(INT_PCM *samples, int32_t numSamples) {
    if (mOutputDelayRingBufferWritePos + numSamples <= mOutputDelayRingBufferSize
            && (mOutputDelayRingBufferReadPos <= mOutputDelayRingBufferWritePos
                    || mOutputDelayRingBufferReadPos > mOutputDelayRingBufferWritePos + numSamples)) {
        // faster memcopy loop without checks, if the preconditions allow this
        for (int32_t i = 0; i < numSamples; i++) {
            mOutputDelayRingBuffer[mOutputDelayRingBufferWritePos++] = samples[i];
        }

        if (mOutputDelayRingBufferWritePos >= mOutputDelayRingBufferSize) {
            mOutputDelayRingBufferWritePos -= mOutputDelayRingBufferSize;
        }
        if (mOutputDelayRingBufferWritePos == mOutputDelayRingBufferReadPos) {
            ALOGE("RING BUFFER OVERFLOW");
            return false;
        }
    } else {
        ALOGV("slow SoftAAC2::outputDelayRingBufferPutSamples()");

        for (int32_t i = 0; i < numSamples; i++) {
            mOutputDelayRingBuffer[mOutputDelayRingBufferWritePos] = samples[i];
            mOutputDelayRingBufferWritePos++;
            if (mOutputDelayRingBufferWritePos >= mOutputDelayRingBufferSize) {
                mOutputDelayRingBufferWritePos -= mOutputDelayRingBufferSize;
            }
            if (mOutputDelayRingBufferWritePos == mOutputDelayRingBufferReadPos) {
                ALOGE("RING BUFFER OVERFLOW");
                return false;
            }
        }
    }
    return true;
}

int32_t SoftAAC2::outputDelayRingBufferGetSamples(INT_PCM *samples, int32_t numSamples) {
    if (mOutputDelayRingBufferReadPos + numSamples <= mOutputDelayRingBufferSize
            && (mOutputDelayRingBufferWritePos < mOutputDelayRingBufferReadPos
                    || mOutputDelayRingBufferWritePos >= mOutputDelayRingBufferReadPos + numSamples)) {
        // faster memcopy loop without checks, if the preconditions allow this
        if (samples != 0) {
            for (int32_t i = 0; i < numSamples; i++) {
                samples[i] = mOutputDelayRingBuffer[mOutputDelayRingBufferReadPos++];
            }
        } else {
            mOutputDelayRingBufferReadPos += numSamples;
        }
        if (mOutputDelayRingBufferReadPos >= mOutputDelayRingBufferSize) {
            mOutputDelayRingBufferReadPos -= mOutputDelayRingBufferSize;
        }
    } else {
        ALOGV("slow SoftAAC2::outputDelayRingBufferGetSamples()");

        for (int32_t i = 0; i < numSamples; i++) {
            if (mOutputDelayRingBufferWritePos == mOutputDelayRingBufferReadPos) {
                ALOGE("RING BUFFER UNDERRUN");
                return -1;
            }
            if (samples != 0) {
                samples[i] = mOutputDelayRingBuffer[mOutputDelayRingBufferReadPos];
            }
            mOutputDelayRingBufferReadPos++;
            if (mOutputDelayRingBufferReadPos >= mOutputDelayRingBufferSize) {
                mOutputDelayRingBufferReadPos -= mOutputDelayRingBufferSize;
            }
        }
    }
    return numSamples;
}

int32_t SoftAAC2::outputDelayRingBufferSamplesAvailable() {
    int32_t available = mOutputDelayRingBufferWritePos - mOutputDelayRingBufferReadPos;
    if (available < 0) {
        available += mOutputDelayRingBufferSize;
    }
    if (available < 0) {
        ALOGE("FATAL RING BUFFER ERROR");
        return 0;
    }
    return available;
}

int32_t SoftAAC2::outputDelayRingBufferSamplesLeft() {
    return mOutputDelayRingBufferSize - outputDelayRingBufferSamplesAvailable();
}

void SoftAAC2::onQueueFilled(OMX_U32 portIndex) {
    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    UCHAR* inBuffer[FILEREAD_MAX_LAYERS];
    UINT inBufferLength[FILEREAD_MAX_LAYERS] = {0};
    UINT bytesValid[FILEREAD_MAX_LAYERS] = {0};

    List<BufferInfo *> &inQueue = getPortQueue(0);
    List<BufferInfo *> &outQueue = getPortQueue(1);

    if (portIndex == 0 && mInputBufferCount == 0) {
        BufferInfo *inInfo = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

        inBuffer[0] = inHeader->pBuffer + inHeader->nOffset;
        inBufferLength[0] = inHeader->nFilledLen;

        AAC_DECODER_ERROR decoderErr =
            aacDecoder_ConfigRaw(mAACDecoder,
                                 inBuffer,
                                 inBufferLength);

        if (decoderErr != AAC_DEC_OK) {
            ALOGW("aacDecoder_ConfigRaw decoderErr = 0x%4.4x", decoderErr);
            mSignalledError = true;
            notify(OMX_EventError, OMX_ErrorUndefined, decoderErr, NULL);
            return;
        }

        mInputBufferCount++;
        mOutputBufferCount++; // fake increase of outputBufferCount to keep the counters aligned

        inInfo->mOwnedByUs = false;
        inQueue.erase(inQueue.begin());
        inInfo = NULL;
        notifyEmptyBufferDone(inHeader);
        inHeader = NULL;

        configureDownmix();
        // Only send out port settings changed event if both sample rate
        // and numChannels are valid.
        if (mStreamInfo->sampleRate && mStreamInfo->numChannels) {
            ALOGI("Initially configuring decoder: %d Hz, %d channels",
                mStreamInfo->sampleRate,
                mStreamInfo->numChannels);

            notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
            mOutputPortSettingsChange = AWAITING_DISABLED;
        }

        return;
    }

    while ((!inQueue.empty() || mEndOfInput) && !outQueue.empty()) {
        if (!inQueue.empty()) {
            INT_PCM tmpOutBuffer[2048 * MAX_CHANNEL_COUNT];
            BufferInfo *inInfo = *inQueue.begin();
            OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

            if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
                mEndOfInput = true;
            } else {
                mEndOfInput = false;
            }
            if (inHeader->nOffset == 0) { // TODO: does nOffset != 0 happen?
                mAnchorTimeUs[mInputBufferCount % kNumDelayBlocksMax] =
                        inHeader->nTimeStamp;
            }

            if (inHeader->nFilledLen == 0) {
                inInfo->mOwnedByUs = false;
                inQueue.erase(inQueue.begin());
                inInfo = NULL;
                notifyEmptyBufferDone(inHeader);
                inHeader = NULL;
            } else {
                if (mIsADTS) {
                    size_t adtsHeaderSize = 0;
                    // skip 30 bits, aac_frame_length follows.
                    // ssssssss ssssiiip ppffffPc ccohCCll llllllll lll?????

                    const uint8_t *adtsHeader = inHeader->pBuffer + inHeader->nOffset;

                    bool signalError = false;
                    if (inHeader->nFilledLen < 7) {
                        ALOGE("Audio data too short to contain even the ADTS header. "
                                "Got %d bytes.", inHeader->nFilledLen);
                        hexdump(adtsHeader, inHeader->nFilledLen);
                        signalError = true;
                    } else {
                        bool protectionAbsent = (adtsHeader[1] & 1);

                        unsigned aac_frame_length =
                            ((adtsHeader[3] & 3) << 11)
                            | (adtsHeader[4] << 3)
                            | (adtsHeader[5] >> 5);

                        if (inHeader->nFilledLen < aac_frame_length) {
                            ALOGE("Not enough audio data for the complete frame. "
                                    "Got %d bytes, frame size according to the ADTS "
                                    "header is %u bytes.",
                                    inHeader->nFilledLen, aac_frame_length);
                            hexdump(adtsHeader, inHeader->nFilledLen);
                            signalError = true;
                        } else {
                            adtsHeaderSize = (protectionAbsent ? 7 : 9);

                            inBuffer[0] = (UCHAR *)adtsHeader + adtsHeaderSize;
                            inBufferLength[0] = aac_frame_length - adtsHeaderSize;

                            inHeader->nOffset += adtsHeaderSize;
                            inHeader->nFilledLen -= adtsHeaderSize;
                        }
                    }

                    if (signalError) {
                        mSignalledError = true;

                        notify(OMX_EventError,
                               OMX_ErrorStreamCorrupt,
                               ERROR_MALFORMED,
                               NULL);

                        return;
                    }
                } else {
                    inBuffer[0] = inHeader->pBuffer + inHeader->nOffset;
                    inBufferLength[0] = inHeader->nFilledLen;
                }

                // Fill and decode
                bytesValid[0] = inBufferLength[0];

                INT prevSampleRate = mStreamInfo->sampleRate;
                INT prevNumChannels = mStreamInfo->numChannels;

                aacDecoder_Fill(mAACDecoder,
                                inBuffer,
                                inBufferLength,
                                bytesValid);

                 // run DRC check
                 mDrcWrap.submitStreamData(mStreamInfo);
                 mDrcWrap.update();

                AAC_DECODER_ERROR decoderErr =
                    aacDecoder_DecodeFrame(mAACDecoder,
                                           tmpOutBuffer,
                                           2048 * MAX_CHANNEL_COUNT,
                                           0 /* flags */);

                if (decoderErr != AAC_DEC_OK) {
                    ALOGW("aacDecoder_DecodeFrame decoderErr = 0x%4.4x", decoderErr);
                }

                if (decoderErr == AAC_DEC_NOT_ENOUGH_BITS) {
                    ALOGE("AAC_DEC_NOT_ENOUGH_BITS should never happen");
                    mSignalledError = true;
                    notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                    return;
                }

                if (bytesValid[0] != 0) {
                    ALOGE("bytesValid[0] != 0 should never happen");
                    mSignalledError = true;
                    notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                    return;
                }

                size_t numOutBytes =
                    mStreamInfo->frameSize * sizeof(int16_t) * mStreamInfo->numChannels;

                if (decoderErr == AAC_DEC_OK) {
                    if (!outputDelayRingBufferPutSamples(tmpOutBuffer,
                            mStreamInfo->frameSize * mStreamInfo->numChannels)) {
                        mSignalledError = true;
                        notify(OMX_EventError, OMX_ErrorUndefined, decoderErr, NULL);
                        return;
                    }
                    UINT inBufferUsedLength = inBufferLength[0] - bytesValid[0];
                    inHeader->nFilledLen -= inBufferUsedLength;
                    inHeader->nOffset += inBufferUsedLength;
                } else {
                    ALOGW("AAC decoder returned error 0x%4.4x, substituting silence", decoderErr);

                    memset(tmpOutBuffer, 0, numOutBytes); // TODO: check for overflow

                    if (!outputDelayRingBufferPutSamples(tmpOutBuffer,
                            mStreamInfo->frameSize * mStreamInfo->numChannels)) {
                        mSignalledError = true;
                        notify(OMX_EventError, OMX_ErrorUndefined, decoderErr, NULL);
                        return;
                    }

                    // Discard input buffer.
                    inHeader->nFilledLen = 0;

                    aacDecoder_SetParam(mAACDecoder, AAC_TPDEC_CLEAR_BUFFER, 1);

                    // fall through
                }

                /*
                 * AAC+/eAAC+ streams can be signalled in two ways: either explicitly
                 * or implicitly, according to MPEG4 spec. AAC+/eAAC+ is a dual
                 * rate system and the sampling rate in the final output is actually
                 * doubled compared with the core AAC decoder sampling rate.
                 *
                 * Explicit signalling is done by explicitly defining SBR audio object
                 * type in the bitstream. Implicit signalling is done by embedding
                 * SBR content in AAC extension payload specific to SBR, and hence
                 * requires an AAC decoder to perform pre-checks on actual audio frames.
                 *
                 * Thus, we could not say for sure whether a stream is
                 * AAC+/eAAC+ until the first data frame is decoded.
                 */
                if (mOutputBufferCount > 1) {
                    if (mStreamInfo->sampleRate != prevSampleRate ||
                        mStreamInfo->numChannels != prevNumChannels) {
                        ALOGE("can not reconfigure AAC output");
                        mSignalledError = true;
                        notify(OMX_EventError, OMX_ErrorUndefined, decoderErr, NULL);
                        return;
                    }
                }
                if (mInputBufferCount <= 2) { // TODO: <= 1
                    if (mStreamInfo->sampleRate != prevSampleRate ||
                        mStreamInfo->numChannels != prevNumChannels) {
                        ALOGI("Reconfiguring decoder: %d->%d Hz, %d->%d channels",
                              prevSampleRate, mStreamInfo->sampleRate,
                              prevNumChannels, mStreamInfo->numChannels);

                        notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
                        mOutputPortSettingsChange = AWAITING_DISABLED;

                        if (inHeader->nFilledLen == 0) {
                            inInfo->mOwnedByUs = false;
                            mInputBufferCount++;
                            inQueue.erase(inQueue.begin());
                            inInfo = NULL;
                            notifyEmptyBufferDone(inHeader);
                            inHeader = NULL;
                        }
                        return;
                    }
                } else if (!mStreamInfo->sampleRate || !mStreamInfo->numChannels) {
                    ALOGW("Invalid AAC stream");
                    mSignalledError = true;
                    notify(OMX_EventError, OMX_ErrorUndefined, decoderErr, NULL);
                    return;
                }
                if (inHeader->nFilledLen == 0) {
                    inInfo->mOwnedByUs = false;
                    mInputBufferCount++;
                    inQueue.erase(inQueue.begin());
                    inInfo = NULL;
                    notifyEmptyBufferDone(inHeader);
                    inHeader = NULL;
                } else {
                    ALOGW("inHeader->nFilledLen = %d", inHeader->nFilledLen);
                }
            }
        }

        int32_t outputDelay = mStreamInfo->outputDelay * mStreamInfo->numChannels;

        if (!mEndOfInput && mOutputDelayCompensated < outputDelay) {
            // discard outputDelay at the beginning
            int32_t toCompensate = outputDelay - mOutputDelayCompensated;
            int32_t discard = outputDelayRingBufferSamplesAvailable();
            if (discard > toCompensate) {
                discard = toCompensate;
            }
            int32_t discarded = outputDelayRingBufferGetSamples(0, discard);
            mOutputDelayCompensated += discarded;
            continue;
        }

        if (mEndOfInput) {
            while (mOutputDelayCompensated > 0) {
                // a buffer big enough for MAX_CHANNEL_COUNT channels of decoded HE-AAC
                INT_PCM tmpOutBuffer[2048 * MAX_CHANNEL_COUNT];
 
                 // run DRC check
                 mDrcWrap.submitStreamData(mStreamInfo);
                 mDrcWrap.update();

                AAC_DECODER_ERROR decoderErr =
                    aacDecoder_DecodeFrame(mAACDecoder,
                                           tmpOutBuffer,
                                           2048 * MAX_CHANNEL_COUNT,
                                           AACDEC_FLUSH);
                if (decoderErr != AAC_DEC_OK) {
                    ALOGW("aacDecoder_DecodeFrame decoderErr = 0x%4.4x", decoderErr);
                }

                int32_t tmpOutBufferSamples = mStreamInfo->frameSize * mStreamInfo->numChannels;
                if (tmpOutBufferSamples > mOutputDelayCompensated) {
                    tmpOutBufferSamples = mOutputDelayCompensated;
                }
                outputDelayRingBufferPutSamples(tmpOutBuffer, tmpOutBufferSamples);
                mOutputDelayCompensated -= tmpOutBufferSamples;
            }
        }

        while (!outQueue.empty()
                && outputDelayRingBufferSamplesAvailable()
                        >= mStreamInfo->frameSize * mStreamInfo->numChannels) {
            BufferInfo *outInfo = *outQueue.begin();
            OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

            if (outHeader->nOffset != 0) {
                ALOGE("outHeader->nOffset != 0 is not handled");
                mSignalledError = true;
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                return;
            }

            INT_PCM *outBuffer =
                    reinterpret_cast<INT_PCM *>(outHeader->pBuffer + outHeader->nOffset);
            if (outHeader->nOffset
                    + mStreamInfo->frameSize * mStreamInfo->numChannels * sizeof(int16_t)
                    > outHeader->nAllocLen) {
                ALOGE("buffer overflow");
                mSignalledError = true;
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                return;

            }
            int32_t ns = outputDelayRingBufferGetSamples(outBuffer,
                    mStreamInfo->frameSize * mStreamInfo->numChannels); // TODO: check for overflow
            if (ns != mStreamInfo->frameSize * mStreamInfo->numChannels) {
                ALOGE("not a complete frame of samples available");
                mSignalledError = true;
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                return;
            }

            outHeader->nFilledLen = mStreamInfo->frameSize * mStreamInfo->numChannels
                    * sizeof(int16_t);
            if (mEndOfInput && !outQueue.empty() && outputDelayRingBufferSamplesAvailable() == 0) {
                outHeader->nFlags = OMX_BUFFERFLAG_EOS;
                mEndOfOutput = true;
            } else {
                outHeader->nFlags = 0;
            }

            outHeader->nTimeStamp = mAnchorTimeUs[mOutputBufferCount
                    % kNumDelayBlocksMax];

            mOutputBufferCount++;
            outInfo->mOwnedByUs = false;
            outQueue.erase(outQueue.begin());
            outInfo = NULL;
            notifyFillBufferDone(outHeader);
            outHeader = NULL;
        }

        if (mEndOfInput) {
            if (outputDelayRingBufferSamplesAvailable() > 0
                    && outputDelayRingBufferSamplesAvailable()
                            < mStreamInfo->frameSize * mStreamInfo->numChannels) {
                ALOGE("not a complete frame of samples available");
                mSignalledError = true;
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                return;
            }

            if (mEndOfInput && !outQueue.empty() && outputDelayRingBufferSamplesAvailable() == 0) {
                if (!mEndOfOutput) {
                    // send empty block signaling EOS
                    mEndOfOutput = true;
                    BufferInfo *outInfo = *outQueue.begin();
                    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

                    if (outHeader->nOffset != 0) {
                        ALOGE("outHeader->nOffset != 0 is not handled");
                        mSignalledError = true;
                        notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                        return;
                    }

                    INT_PCM *outBuffer = reinterpret_cast<INT_PCM *>(outHeader->pBuffer
                            + outHeader->nOffset);
                    int32_t ns = 0;
                    outHeader->nFilledLen = 0;
                    outHeader->nFlags = OMX_BUFFERFLAG_EOS;

                    outHeader->nTimeStamp = mAnchorTimeUs[mOutputBufferCount
                            % kNumDelayBlocksMax];

                    mOutputBufferCount++;
                    outInfo->mOwnedByUs = false;
                    outQueue.erase(outQueue.begin());
                    outInfo = NULL;
                    notifyFillBufferDone(outHeader);
                    outHeader = NULL;
                }
                break; // if outQueue not empty but no more output
            }
        }
    }
}

void SoftAAC2::onPortFlushCompleted(OMX_U32 portIndex) {
    if (portIndex == 0) {
        // Make sure that the next buffer output does not still
        // depend on fragments from the last one decoded.
        // drain all existing data
        drainDecoder();
        // force decoder loop to drop the first decoded buffer by resetting these state variables,
        // but only if initialization has already happened.
        if (mInputBufferCount != 0) {
            mInputBufferCount = 1;
        }
    } else {
        while (outputDelayRingBufferSamplesAvailable() > 0) {
            int32_t ns = outputDelayRingBufferGetSamples(0,
                    mStreamInfo->frameSize * mStreamInfo->numChannels);
            if (ns != mStreamInfo->frameSize * mStreamInfo->numChannels) {
                ALOGE("not a complete frame of samples available");
            }
            mOutputBufferCount++;
        }
        mOutputDelayRingBufferReadPos = mOutputDelayRingBufferWritePos;
    }
}

void SoftAAC2::drainDecoder() {
    int32_t outputDelay = mStreamInfo->outputDelay * mStreamInfo->numChannels;

    // flush decoder until outputDelay is compensated
    while (mOutputDelayCompensated > 0) {
        // a buffer big enough for MAX_CHANNEL_COUNT channels of decoded HE-AAC
        INT_PCM tmpOutBuffer[2048 * MAX_CHANNEL_COUNT];

        // run DRC check
        mDrcWrap.submitStreamData(mStreamInfo);
        mDrcWrap.update();

        AAC_DECODER_ERROR decoderErr =
            aacDecoder_DecodeFrame(mAACDecoder,
                                   tmpOutBuffer,
                                   2048 * MAX_CHANNEL_COUNT,
                                   AACDEC_FLUSH);
        if (decoderErr != AAC_DEC_OK) {
            ALOGW("aacDecoder_DecodeFrame decoderErr = 0x%4.4x", decoderErr);
        }

        int32_t tmpOutBufferSamples = mStreamInfo->frameSize * mStreamInfo->numChannels;
        if (tmpOutBufferSamples > mOutputDelayCompensated) {
            tmpOutBufferSamples = mOutputDelayCompensated;
        }
        outputDelayRingBufferPutSamples(tmpOutBuffer, tmpOutBufferSamples);

        mOutputDelayCompensated -= tmpOutBufferSamples;
    }
}

void SoftAAC2::onReset() {
    drainDecoder();
    // reset the "configured" state
    mInputBufferCount = 0;
    mOutputBufferCount = 0;
    mOutputDelayCompensated = 0;
    mOutputDelayRingBufferWritePos = 0;
    mOutputDelayRingBufferReadPos = 0;
    mEndOfInput = false;
    mEndOfOutput = false;

    // To make the codec behave the same before and after a reset, we need to invalidate the
    // streaminfo struct. This does that:
    mStreamInfo->sampleRate = 0; // TODO: mStreamInfo is read only

    mSignalledError = false;
    mOutputPortSettingsChange = NONE;
}

void SoftAAC2::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    if (portIndex != 1) {
        return;
    }

    switch (mOutputPortSettingsChange) {
        case NONE:
            break;

        case AWAITING_DISABLED:
        {
            CHECK(!enabled);
            mOutputPortSettingsChange = AWAITING_ENABLED;
            break;
        }

        default:
        {
            CHECK_EQ((int)mOutputPortSettingsChange, (int)AWAITING_ENABLED);
            CHECK(enabled);
            mOutputPortSettingsChange = NONE;
            break;
        }
    }
}

}  // namespace android

android::SoftOMXComponent *createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    return new android::SoftAAC2(name, callbacks, appData, component);
}
