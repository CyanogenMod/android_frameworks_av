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
/**
*************************************************************************
* @file   VideoEditorAudioDecoder.cpp
* @brief  StageFright shell Audio Decoder
*************************************************************************
*/

#define LOG_NDEBUG 1
#define LOG_TAG "VIDEOEDITOR_AUDIODECODER"

#include "M4OSA_Debug.h"
#include "VideoEditorAudioDecoder.h"
#include "VideoEditorUtils.h"
#include "M4MCS_InternalTypes.h"

#include "utils/Log.h"
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>

/********************
 *   DEFINITIONS    *
 ********************/
// Version
#define VIDEOEDITOR_AUDIO_DECODER_VERSION_MAJOR 1
#define VIDEOEDITOR_AUDIO_DECODER_VERSION_MINOR 0
#define VIDEOEDITOR_AUDIO_DECODER_VERSION_REV   0

// Force using software decoder as engine does not support prefetch
#define VIDEOEDITOR_FORCECODEC kSoftwareCodecsOnly

namespace android {

struct VideoEditorAudioDecoderSource : public MediaSource {
    public:
        static sp<VideoEditorAudioDecoderSource> Create(
                const sp<MetaData>& format);
        virtual status_t start(MetaData *params = NULL);
        virtual status_t stop();
        virtual sp<MetaData> getFormat();
        virtual status_t read(MediaBuffer **buffer,
        const ReadOptions *options = NULL);
        virtual int32_t storeBuffer(MediaBuffer *buffer);

    protected:
        virtual ~VideoEditorAudioDecoderSource();

    private:
        struct MediaBufferChain {
            MediaBuffer* buffer;
            MediaBufferChain* nextLink;
        };
        enum State {
            CREATED,
            STARTED,
            ERROR
        };
        VideoEditorAudioDecoderSource(const sp<MetaData>& format);
        sp<MetaData> mFormat;
        MediaBufferChain* mFirstBufferLink;
        MediaBufferChain* mLastBufferLink;
        int32_t mNbBuffer;
        bool mIsEOS;
        State mState;
};

sp<VideoEditorAudioDecoderSource> VideoEditorAudioDecoderSource::Create(
        const sp<MetaData>& format) {

    sp<VideoEditorAudioDecoderSource> aSource =
        new VideoEditorAudioDecoderSource(format);

    return aSource;
}

VideoEditorAudioDecoderSource::VideoEditorAudioDecoderSource(
        const sp<MetaData>& format):
        mFormat(format),
        mFirstBufferLink(NULL),
        mLastBufferLink(NULL),
        mNbBuffer(0),
        mIsEOS(false),
        mState(CREATED) {
}

VideoEditorAudioDecoderSource::~VideoEditorAudioDecoderSource() {

    if( STARTED == mState ) {
        stop();
    }
}

status_t VideoEditorAudioDecoderSource::start(MetaData *meta) {
    status_t err = OK;

    if( CREATED != mState ) {
        LOGV("VideoEditorAudioDecoderSource::start: invalid state %d", mState);
        return UNKNOWN_ERROR;
    }

    mState = STARTED;

cleanUp:
    LOGV("VideoEditorAudioDecoderSource::start END (0x%x)", err);
    return err;
}

status_t VideoEditorAudioDecoderSource::stop() {
    status_t err = OK;
    int32_t i = 0;

    LOGV("VideoEditorAudioDecoderSource::stop begin");

    if( STARTED != mState ) {
        LOGV("VideoEditorAudioDecoderSource::stop: invalid state %d", mState);
        return UNKNOWN_ERROR;
    }

    // Release the buffer chain
    MediaBufferChain* tmpLink = NULL;
    while( mFirstBufferLink ) {
        i++;
        tmpLink = mFirstBufferLink;
        mFirstBufferLink = mFirstBufferLink->nextLink;
        delete tmpLink;
    }
    LOGV("VideoEditorAudioDecoderSource::stop : %d buffer remained", i);
    mFirstBufferLink = NULL;
    mLastBufferLink = NULL;

    mState = CREATED;

    LOGV("VideoEditorAudioDecoderSource::stop END (0x%x)", err);
    return err;
}

sp<MetaData> VideoEditorAudioDecoderSource::getFormat() {

    LOGV("VideoEditorAudioDecoderSource::getFormat");
    return mFormat;
}

status_t VideoEditorAudioDecoderSource::read(MediaBuffer **buffer,
        const ReadOptions *options) {
    MediaSource::ReadOptions readOptions;
    status_t err = OK;
    MediaBufferChain* tmpLink = NULL;

    LOGV("VideoEditorAudioDecoderSource::read begin");

    if ( STARTED != mState ) {
        LOGV("VideoEditorAudioDecoderSource::read invalid state %d", mState);
        return UNKNOWN_ERROR;
    }

    // Get a buffer from the chain
    if( NULL == mFirstBufferLink ) {
        *buffer = NULL;
        if( mIsEOS ) {
            LOGV("VideoEditorAudioDecoderSource::read : EOS");
            return ERROR_END_OF_STREAM;
        } else {
            LOGV("VideoEditorAudioDecoderSource::read : no buffer available");
            return UNKNOWN_ERROR;
        }
    }
    *buffer = mFirstBufferLink->buffer;

    tmpLink = mFirstBufferLink;
    mFirstBufferLink = mFirstBufferLink->nextLink;
    if( NULL == mFirstBufferLink ) {
        mLastBufferLink = NULL;
    }
    delete tmpLink;
    mNbBuffer--;

    LOGV("VideoEditorAudioDecoderSource::read END (0x%x)", err);
    return err;
}

int32_t VideoEditorAudioDecoderSource::storeBuffer(MediaBuffer *buffer) {
    status_t err = OK;

    LOGV("VideoEditorAudioDecoderSource::storeBuffer begin");

    // A NULL input buffer means that the end of stream was reached
    if( NULL == buffer ) {
        mIsEOS = true;
    } else {
        MediaBufferChain* newLink = new MediaBufferChain;
        newLink->buffer = buffer;
        newLink->nextLink = NULL;
        if( NULL != mLastBufferLink ) {
            mLastBufferLink->nextLink = newLink;
        } else {
            mFirstBufferLink = newLink;
        }
        mLastBufferLink = newLink;
        mNbBuffer++;
    }
    LOGV("VideoEditorAudioDecoderSource::storeBuffer END");
    return mNbBuffer;
}

/********************
 *      TOOLS       *
 ********************/

M4OSA_ERR VideoEditorAudioDecoder_getBits(M4OSA_Int8* pData,
        M4OSA_UInt32 dataSize, M4OSA_UInt8 nbBits, M4OSA_Int32* pResult,
        M4OSA_UInt32* pOffset) {

    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_UInt32 startByte = 0;
    M4OSA_UInt32 startBit = 0;
    M4OSA_UInt32 endByte = 0;
    M4OSA_UInt32 endBit = 0;
    M4OSA_UInt32 currentByte = 0;
    M4OSA_UInt32 result = 0;
    M4OSA_UInt32 ui32Tmp = 0;
    M4OSA_UInt32 ui32Mask = 0;

    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pData, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != pOffset, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(32 >= nbBits, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK((*pOffset + nbBits) <= 8*dataSize, M4ERR_PARAMETER);

    LOGV("VideoEditorAudioDecoder_getBits begin");

    startByte   = (*pOffset) >> 3;
    endByte     = (*pOffset + nbBits) >> 3;
    startBit    = (*pOffset) % 8;
    endBit      = (*pOffset + nbBits) % 8;
    currentByte = startByte;

    // Extract the requested nunber of bits from memory
    while( currentByte <= endByte) {
        ui32Mask = 0x000000FF;
        if( currentByte == startByte ) {
            ui32Mask >>= startBit;
        }
        ui32Tmp = ui32Mask & ((M4OSA_UInt32)pData[currentByte]);
        if( currentByte == endByte ) {
            ui32Tmp >>= (8-endBit);
            result <<= endBit;
        } else {
            result <<= 8;
        }
        result |= ui32Tmp;
        currentByte++;
    }

    *pResult = result;
    *pOffset += nbBits;

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_getBits no error");
    } else {
        LOGV("VideoEditorAudioDecoder_getBits ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_getBits end");
    return err;
}


#define FREQ_TABLE_SIZE 16
const M4OSA_UInt32 AD_AAC_FREQ_TABLE[FREQ_TABLE_SIZE] =
    {96000, 88200, 64000, 48000, 44100,
    32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0};


M4OSA_ERR VideoEditorAudioDecoder_parse_AAC_DSI(M4OSA_Int8* pDSI,
        M4OSA_UInt32 dsiSize, AAC_DEC_STREAM_PROPS* pProperties) {

    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_UInt32 offset = 0;
    M4OSA_Int32 result = 0;

    LOGV("VideoEditorAudioDecoder_parse_AAC_DSI begin");

    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pDSI, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != pProperties, M4ERR_PARAMETER);

    // Get the object type
    err = VideoEditorAudioDecoder_getBits(pDSI, dsiSize, 5, &result, &offset);
    VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);
    switch( result ) {
        case 2:
            pProperties->aPSPresent  = 0;
            pProperties->aSBRPresent = 0;
            break;
        default:
            LOGV("parse_AAC_DSI ERROR : object type %d is not supported",
                result);
            VIDEOEDITOR_CHECK(!"invalid AAC object type", M4ERR_BAD_OPTION_ID);
            break;
    }
    pProperties->aAudioObjectType = (M4OSA_Int32)result;

    // Get the frequency index
    err = VideoEditorAudioDecoder_getBits(pDSI, dsiSize, 4, &result, &offset);
    VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);
    VIDEOEDITOR_CHECK((0 <= result) && (FREQ_TABLE_SIZE > result),
        M4ERR_PARAMETER);
    pProperties->aSampFreq = AD_AAC_FREQ_TABLE[result];
    pProperties->aExtensionSampFreq = 0;

    // Get the number of channels
    err = VideoEditorAudioDecoder_getBits(pDSI, dsiSize, 4, &result, &offset);
    VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);
    pProperties->aNumChan = (M4OSA_UInt32)result;

    // Set the max PCM samples per channel
    pProperties->aMaxPCMSamplesPerCh = (pProperties->aSBRPresent) ? 2048 : 1024;

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_parse_AAC_DSI no error");
    } else {
        LOGV("VideoEditorAudioDecoder_parse_AAC_DSI ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_parse_AAC_DSI end");
    return err;
}

/********************
 * ENGINE INTERFACE *
 ********************/

/**
 ******************************************************************************
 * structure VideoEditorAudioDecoder_Context
 * @brief    This structure defines the context of the StageFright audio decoder
 *           shell
 ******************************************************************************
*/
typedef struct {
    M4AD_Type                          mDecoderType;
    M4_AudioStreamHandler*             mAudioStreamHandler;
    sp<VideoEditorAudioDecoderSource>  mDecoderSource;
    OMXClient                          mClient;
    sp<MediaSource>                    mDecoder;
    int32_t                            mNbOutputChannels;
    uint32_t                           mNbInputFrames;
    uint32_t                           mNbOutputFrames;
} VideoEditorAudioDecoder_Context;

M4OSA_ERR VideoEditorAudioDecoder_destroy(M4AD_Context pContext) {
    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorAudioDecoder_Context* pDecoderContext = M4OSA_NULL;

    LOGV("VideoEditorAudioDecoder_destroy begin");
    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext, M4ERR_PARAMETER);

    pDecoderContext = (VideoEditorAudioDecoder_Context*)pContext;

    // Stop the graph
    if( M4OSA_NULL != pDecoderContext->mDecoder.get() ) {
        pDecoderContext->mDecoder->stop();
    }

    // Destroy the graph
    pDecoderContext->mDecoderSource.clear();
    pDecoderContext->mDecoder.clear();
    pDecoderContext->mClient.disconnect();

    SAFE_FREE(pDecoderContext);
    pContext = M4OSA_NULL;
    LOGV("VideoEditorAudioDecoder_destroy : DONE");

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_destroy no error");
    } else {
        LOGV("VideoEditorAudioDecoder_destroy ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_destroy : end");
    return err;
}

M4OSA_ERR VideoEditorAudioDecoder_create(M4AD_Type decoderType,
        M4AD_Context* pContext, M4_AudioStreamHandler* pStreamHandler,
        void* pUserData) {
    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorAudioDecoder_Context* pDecoderContext = M4OSA_NULL;
    AAC_DEC_STREAM_PROPS aacProperties;
    status_t result = OK;
    sp<MetaData> decoderMetaData = NULL;
    const char* mime = NULL;
    uint32_t codecFlags = 0;

    LOGV("VideoEditorAudioDecoder_create begin: decoderType %d", decoderType);

    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext,       M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != pStreamHandler, M4ERR_PARAMETER);

    // Context allocation & initialization
    SAFE_MALLOC(pDecoderContext, VideoEditorAudioDecoder_Context, 1,
        "AudioDecoder");
    pDecoderContext->mDecoderType = decoderType;
    pDecoderContext->mAudioStreamHandler = pStreamHandler;

    pDecoderContext->mNbInputFrames  = 0;
    pDecoderContext->mNbOutputFrames = 0;

    LOGV("VideoEditorAudioDecoder_create : maxAUSize %d",
        pDecoderContext->mAudioStreamHandler->m_basicProperties.m_maxAUSize);

    // Create the meta data for the decoder
    decoderMetaData = new MetaData;
    switch( pDecoderContext->mDecoderType ) {
        case M4AD_kTypeAMRNB:
            // StageFright parameters
            mime = MEDIA_MIMETYPE_AUDIO_AMR_NB;
            // Engine parameters
            pDecoderContext->mAudioStreamHandler->m_byteFrameLength = 160;
            // Number of bytes per sample
            pDecoderContext->mAudioStreamHandler->m_byteSampleSize = 2;
            pDecoderContext->mAudioStreamHandler->m_samplingFrequency = 8000;
            pDecoderContext->mAudioStreamHandler->m_nbChannels = 1;
            break;

        case M4AD_kTypeAMRWB:
            // StageFright parameters
            mime = MEDIA_MIMETYPE_AUDIO_AMR_WB;

            pDecoderContext->mAudioStreamHandler->m_byteFrameLength = 160;
            // Number of bytes per sample
            pDecoderContext->mAudioStreamHandler->m_byteSampleSize = 2;
            pDecoderContext->mAudioStreamHandler->m_samplingFrequency = 16000;
            pDecoderContext->mAudioStreamHandler->m_nbChannels = 1;
            break;

        case M4AD_kTypeAAC:
            // Reject ADTS & ADIF (or any incorrect type)
            VIDEOEDITOR_CHECK(M4DA_StreamTypeAudioAac ==
                pDecoderContext->mAudioStreamHandler->\
                m_basicProperties.m_streamType,M4ERR_PARAMETER);

            // StageFright parameters
            mime = MEDIA_MIMETYPE_AUDIO_AAC;

            decoderMetaData->setData(kKeyESDS, kTypeESDS,
                pStreamHandler->m_basicProperties.m_pESDSInfo,
                pStreamHandler->m_basicProperties.m_ESDSInfoSize);

            // Engine parameters
            // Retrieve sampling frequency and number of channels from the DSI
            err = VideoEditorAudioDecoder_parse_AAC_DSI(
                (M4OSA_Int8*)pStreamHandler->m_basicProperties.\
                    m_pDecoderSpecificInfo,
                pStreamHandler->m_basicProperties.m_decoderSpecificInfoSize,
                &aacProperties);

            VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);
            pDecoderContext->mAudioStreamHandler->m_byteFrameLength = 1024;
            // Number of bytes per sample
            pDecoderContext->mAudioStreamHandler->m_byteSampleSize = 2;
            pDecoderContext->mAudioStreamHandler->m_samplingFrequency =
                aacProperties.aSampFreq;
            pDecoderContext->mAudioStreamHandler->m_nbChannels =
                aacProperties.aNumChan;

            // Copy the stream properties into userdata
            if( M4OSA_NULL != pUserData ) {
                memcpy((void *)pUserData,
                    (void *)&aacProperties,
                    sizeof(AAC_DEC_STREAM_PROPS));
            }
            break;

        case M4AD_kTypeMP3:
            // StageFright parameters
            mime = MEDIA_MIMETYPE_AUDIO_MPEG;
            break;

        default:
            VIDEOEDITOR_CHECK(!"AudioDecoder_open : incorrect input format",
                M4ERR_STATE);
            break;
    }
    decoderMetaData->setCString(kKeyMIMEType, mime);
    decoderMetaData->setInt32(kKeySampleRate,
        (int32_t)pDecoderContext->mAudioStreamHandler->m_samplingFrequency);
    decoderMetaData->setInt32(kKeyChannelCount,
        pDecoderContext->mAudioStreamHandler->m_nbChannels);
    decoderMetaData->setInt64(kKeyDuration,
        (int64_t)pDecoderContext->mAudioStreamHandler->\
        m_basicProperties.m_duration);

    // Create the decoder source
    pDecoderContext->mDecoderSource = VideoEditorAudioDecoderSource::Create(
        decoderMetaData);
    VIDEOEDITOR_CHECK(NULL != pDecoderContext->mDecoderSource.get(),
        M4ERR_STATE);

    // Connect to the OMX client
    result = pDecoderContext->mClient.connect();
    VIDEOEDITOR_CHECK(OK == result, M4ERR_STATE);

    // Create the OMX codec
#ifdef VIDEOEDITOR_FORCECODEC
    codecFlags |= OMXCodec::VIDEOEDITOR_FORCECODEC;
#endif /* VIDEOEDITOR_FORCECODEC */

    pDecoderContext->mDecoder = OMXCodec::Create(pDecoderContext->\
        mClient.interface(),
        decoderMetaData, false, pDecoderContext->mDecoderSource, NULL,
            codecFlags);
    VIDEOEDITOR_CHECK(NULL != pDecoderContext->mDecoder.get(), M4ERR_STATE);

    // Get the output channels, the decoder might overwrite the input metadata
    pDecoderContext->mDecoder->getFormat()->findInt32(kKeyChannelCount,
        &pDecoderContext->mNbOutputChannels);
    LOGV("VideoEditorAudioDecoder_create : output chan %d",
        pDecoderContext->mNbOutputChannels);

    // Start the decoder
    result = pDecoderContext->mDecoder->start();
    VIDEOEDITOR_CHECK(OK == result, M4ERR_STATE);

    *pContext = pDecoderContext;
    LOGV("VideoEditorAudioDecoder_create : DONE");

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_create no error");
    } else {
        VideoEditorAudioDecoder_destroy(pDecoderContext);
        *pContext = M4OSA_NULL;
        LOGV("VideoEditorAudioDecoder_create ERROR 0x%X", err);
    }
    return err;
}

M4OSA_ERR VideoEditorAudioDecoder_create_AAC(M4AD_Context* pContext,
        M4_AudioStreamHandler* pStreamHandler, void* pUserData) {

    return VideoEditorAudioDecoder_create(
        M4AD_kTypeAAC, pContext, pStreamHandler,pUserData);
}


M4OSA_ERR VideoEditorAudioDecoder_create_AMRNB(M4AD_Context* pContext,
        M4_AudioStreamHandler* pStreamHandler, void* pUserData) {

    return VideoEditorAudioDecoder_create(
        M4AD_kTypeAMRNB, pContext, pStreamHandler, pUserData);
}


M4OSA_ERR VideoEditorAudioDecoder_create_AMRWB(M4AD_Context* pContext,
        M4_AudioStreamHandler* pStreamHandler, void* pUserData) {

    return VideoEditorAudioDecoder_create(
        M4AD_kTypeAMRWB, pContext, pStreamHandler, pUserData);
}


M4OSA_ERR VideoEditorAudioDecoder_create_MP3(M4AD_Context* pContext,
        M4_AudioStreamHandler* pStreamHandler, void* pUserData) {

    return VideoEditorAudioDecoder_create(
        M4AD_kTypeMP3, pContext, pStreamHandler, pUserData);
}

M4OSA_ERR VideoEditorAudioDecoder_processInputBuffer(
        M4AD_Context pContext, M4AD_Buffer* pInputBuffer) {
    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorAudioDecoder_Context* pDecoderContext = M4OSA_NULL;
    MediaBuffer* buffer = NULL;
    int32_t nbBuffer = 0;

    LOGV("VideoEditorAudioDecoder_processInputBuffer begin");
    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext, M4ERR_PARAMETER);


    pDecoderContext = (VideoEditorAudioDecoder_Context*)pContext;

    if( M4OSA_NULL != pInputBuffer ) {
        buffer = new MediaBuffer((size_t)pInputBuffer->m_bufferSize);
        memcpy((void *)((M4OSA_Int8*)buffer->data() + buffer->range_offset()),
            (void *)pInputBuffer->m_dataAddress, pInputBuffer->m_bufferSize);
    }
    nbBuffer = pDecoderContext->mDecoderSource->storeBuffer(buffer);

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_processInputBuffer no error");
    } else {
        LOGV("VideoEditorAudioDecoder_processInputBuffer ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_processInputBuffer end");
    return err;
}

M4OSA_ERR VideoEditorAudioDecoder_processOutputBuffer(M4AD_Context pContext,
        MediaBuffer* buffer, M4AD_Buffer* pOuputBuffer) {
    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorAudioDecoder_Context* pDecoderContext = M4OSA_NULL;
    int32_t i32Tmp = 0;
    int64_t i64Tmp = 0;
    status_t result = OK;

    LOGV("VideoEditorAudioDecoder_processOutputBuffer begin");
    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != buffer, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != pOuputBuffer, M4ERR_PARAMETER);

    pDecoderContext = (VideoEditorAudioDecoder_Context*)pContext;

    // Process the returned data
    if( 0 == buffer->range_length() ) {
        // Decoder has no data yet, nothing unusual
        goto cleanUp;
    }

    pDecoderContext->mNbOutputFrames++;

    if( pDecoderContext->mAudioStreamHandler->m_nbChannels ==
        (M4OSA_UInt32)pDecoderContext->mNbOutputChannels ) {
        // Just copy the PCMs
        pOuputBuffer->m_bufferSize = (M4OSA_UInt32)buffer->range_length();
        memcpy((void *)pOuputBuffer->m_dataAddress,
            (void *)(((M4OSA_MemAddr8)buffer->data())+buffer->range_offset()),
            buffer->range_length());
    } else if( pDecoderContext->mAudioStreamHandler->m_nbChannels <
        (M4OSA_UInt32)pDecoderContext->mNbOutputChannels ) {
        // The decoder forces stereo output, downsample
        pOuputBuffer->m_bufferSize = (M4OSA_UInt32)(buffer->range_length()/2);
        M4OSA_Int16* pDataIn  = ((M4OSA_Int16*)buffer->data()) +
            buffer->range_offset();
        M4OSA_Int16* pDataOut = (M4OSA_Int16*)pOuputBuffer->m_dataAddress;
        M4OSA_Int16* pDataEnd = pDataIn + \
            (buffer->range_length()/sizeof(M4OSA_Int16));
        while( pDataIn < pDataEnd ) {
            *pDataOut = *pDataIn;
            pDataIn+=2;
            pDataOut++;
        }
    } else {
        // The decoder forces mono output, not supported
        VIDEOEDITOR_CHECK(M4OSA_FALSE, M4ERR_PARAMETER);
    }

cleanUp:
    // Release the buffer
    buffer->release();
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_processOutputBuffer no error");
    } else {
        pOuputBuffer->m_bufferSize = 0;
        LOGV("VideoEditorAudioDecoder_processOutputBuffer ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_processOutputBuffer end");
    return err;
}

M4OSA_ERR VideoEditorAudioDecoder_step(M4AD_Context pContext,
        M4AD_Buffer* pInputBuffer, M4AD_Buffer* pOutputBuffer,
        M4OSA_Bool bJump) {
    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorAudioDecoder_Context* pDecoderContext = M4OSA_NULL;
    status_t result = OK;
    MediaBuffer* outputBuffer = NULL;

    LOGV("VideoEditorAudioDecoder_step begin");
    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext, M4ERR_PARAMETER);

    pDecoderContext = (VideoEditorAudioDecoder_Context*)pContext;
    pDecoderContext->mNbInputFrames++;

    // Push the input buffer to the decoder source
    err = VideoEditorAudioDecoder_processInputBuffer(pDecoderContext,
        pInputBuffer);
    VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);

    // Read
    result = pDecoderContext->mDecoder->read(&outputBuffer, NULL);
    if(OK != result) {
        LOGE("VideoEditorAudioDecoder_step  result = %d",result);

    }
    VIDEOEDITOR_CHECK(OK == result, M4ERR_STATE);

    // Convert the PCM buffer
    err = VideoEditorAudioDecoder_processOutputBuffer(pDecoderContext,
        outputBuffer, pOutputBuffer);
    VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_step no error");
    } else {
        LOGV("VideoEditorAudioDecoder_step ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_step end");
    return err;
}

M4OSA_ERR VideoEditorAudioDecoder_getVersion(M4_VersionInfo* pVersionInfo) {
    M4OSA_ERR err = M4NO_ERROR;

    LOGV("VideoEditorAudioDecoder_getVersion begin");
    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pVersionInfo, M4ERR_PARAMETER);

    pVersionInfo->m_major      = VIDEOEDITOR_AUDIO_DECODER_VERSION_MAJOR;
    pVersionInfo->m_minor      = VIDEOEDITOR_AUDIO_DECODER_VERSION_MINOR;
    pVersionInfo->m_revision   = VIDEOEDITOR_AUDIO_DECODER_VERSION_REV;
    pVersionInfo->m_structSize = sizeof(M4_VersionInfo);

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_getVersion no error");
    } else {
        LOGV("VideoEditorAudioDecoder_getVersion ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_getVersion end");
    return err;
}

M4OSA_ERR VideoEditorAudioDecoder_setOption(M4AD_Context pContext,
        M4OSA_UInt32 optionID, M4OSA_DataOption optionValue) {

    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorAudioDecoder_Context* pDecoderContext = M4OSA_NULL;

    LOGV("VideoEditorAudioDecoder_setOption begin 0x%X", optionID);
    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext, M4ERR_PARAMETER);

    pDecoderContext = (VideoEditorAudioDecoder_Context*)pContext;

    switch( optionID ) {
        case M4AD_kOptionID_UserParam:
            LOGV("VideoEditorAudioDecodersetOption UserParam is not supported");
            err = M4ERR_NOT_IMPLEMENTED;
            break;
        default:
            LOGV("VideoEditorAudioDecoder_setOption  unsupported optionId 0x%X",
                optionID);
            VIDEOEDITOR_CHECK(M4OSA_FALSE, M4ERR_BAD_OPTION_ID);
            break;
    }

cleanUp:
    if( ((M4OSA_UInt32)M4NO_ERROR == err) || ((M4OSA_UInt32)M4ERR_NOT_IMPLEMENTED == err) ) {
        LOGV("VideoEditorAudioDecoder_setOption error 0x%X", err);
    } else {
        LOGV("VideoEditorAudioDecoder_setOption ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_setOption end");
    return err;
}

M4OSA_ERR VideoEditorAudioDecoder_getOption(M4AD_Context pContext,
        M4OSA_UInt32 optionID, M4OSA_DataOption optionValue) {

    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorAudioDecoder_Context* pDecoderContext = M4OSA_NULL;

    LOGV("VideoEditorAudioDecoder_getOption begin: optionID 0x%X", optionID);
    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext, M4ERR_PARAMETER);

    pDecoderContext = (VideoEditorAudioDecoder_Context*)pContext;

    switch( optionID ) {
        default:
            LOGV("VideoEditorAudioDecoder_getOption unsupported optionId 0x%X",
                optionID);
            VIDEOEDITOR_CHECK(M4OSA_FALSE, M4ERR_BAD_OPTION_ID);
            break;
    }

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_getOption no error");
    } else {
        LOGV("VideoEditorAudioDecoder_getOption ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_getOption end");
    return err;
}

M4OSA_ERR VideoEditorAudioDecoder_getInterface(M4AD_Type decoderType,
        M4AD_Type* pDecoderType, M4AD_Interface** pDecoderInterface) {

    M4OSA_ERR err = M4NO_ERROR;

    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pDecoderType, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != pDecoderInterface, M4ERR_PARAMETER);

    LOGV("VideoEditorAudioDecoder_getInterface begin %d 0x%x 0x%x",
        decoderType, pDecoderType, pDecoderInterface);

    SAFE_MALLOC(*pDecoderInterface, M4AD_Interface, 1,
        "VideoEditorAudioDecoder");

    *pDecoderType = decoderType;

    switch( decoderType ) {
        case M4AD_kTypeAMRNB:
            (*pDecoderInterface)->m_pFctCreateAudioDec =
                VideoEditorAudioDecoder_create_AMRNB;
            break;
        case M4AD_kTypeAMRWB:
            (*pDecoderInterface)->m_pFctCreateAudioDec =
                VideoEditorAudioDecoder_create_AMRWB;
            break;
        case M4AD_kTypeAAC:
            (*pDecoderInterface)->m_pFctCreateAudioDec =
                VideoEditorAudioDecoder_create_AAC;
            break;
        case M4AD_kTypeMP3:
            (*pDecoderInterface)->m_pFctCreateAudioDec =
                VideoEditorAudioDecoder_create_MP3;
            break;
        default:
            LOGV("VEAD_getInterface ERROR: unsupported type %d", decoderType);
            VIDEOEDITOR_CHECK(M4OSA_FALSE, M4ERR_PARAMETER);
        break;
    }
    (*pDecoderInterface)->m_pFctDestroyAudioDec   =
        VideoEditorAudioDecoder_destroy;
    (*pDecoderInterface)->m_pFctResetAudioDec     = M4OSA_NULL;
    (*pDecoderInterface)->m_pFctStartAudioDec     = M4OSA_NULL;
    (*pDecoderInterface)->m_pFctStepAudioDec      =
        VideoEditorAudioDecoder_step;
    (*pDecoderInterface)->m_pFctGetVersionAudioDec =
        VideoEditorAudioDecoder_getVersion;
    (*pDecoderInterface)->m_pFctSetOptionAudioDec =
        VideoEditorAudioDecoder_setOption;
    (*pDecoderInterface)->m_pFctGetOptionAudioDec =
        VideoEditorAudioDecoder_getOption;

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorAudioDecoder_getInterface no error");
    } else {
        *pDecoderInterface = M4OSA_NULL;
        LOGV("VideoEditorAudioDecoder_getInterface ERROR 0x%X", err);
    }
    LOGV("VideoEditorAudioDecoder_getInterface end");
    return err;
}


extern "C" {

M4OSA_ERR VideoEditorAudioDecoder_getInterface_AAC(M4AD_Type* pDecoderType,
        M4AD_Interface** pDecoderInterface) {
    LOGV("TEST: AAC VideoEditorAudioDecoder_getInterface no error");
    return VideoEditorAudioDecoder_getInterface(
        M4AD_kTypeAAC, pDecoderType, pDecoderInterface);
}

M4OSA_ERR VideoEditorAudioDecoder_getInterface_AMRNB(M4AD_Type* pDecoderType,
        M4AD_Interface** pDecoderInterface) {
    LOGV("TEST: AMR VideoEditorAudioDecoder_getInterface no error");
    return VideoEditorAudioDecoder_getInterface(
        M4AD_kTypeAMRNB, pDecoderType, pDecoderInterface);
}

M4OSA_ERR VideoEditorAudioDecoder_getInterface_AMRWB(M4AD_Type* pDecoderType,
        M4AD_Interface** pDecoderInterface) {

    return VideoEditorAudioDecoder_getInterface(
        M4AD_kTypeAMRWB, pDecoderType, pDecoderInterface);
}

M4OSA_ERR VideoEditorAudioDecoder_getInterface_MP3(M4AD_Type* pDecoderType,
        M4AD_Interface** pDecoderInterface) {

    return VideoEditorAudioDecoder_getInterface(
        M4AD_kTypeMP3, pDecoderType, pDecoderInterface);
}

}  // extern "C"

}  // namespace android
