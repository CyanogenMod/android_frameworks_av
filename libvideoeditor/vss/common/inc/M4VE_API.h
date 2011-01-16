/*
 * Copyright (C) 2004-2011 NXP Software
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
 ******************************************************************************
 * @file   M4VE_API.h
 * @note   This file declares the generic shell interface retrieving function
 *         of any external encoder.
******************************************************************************
*/

#ifndef __M4VE_API_H__
#define __M4VE_API_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 *    OSAL types definition */
#include "M4OSA_Types.h"
#include "M4OSA_Time.h"
#include "M4OSA_Memory.h"
#include "M4OSA_CoreID.h"
#include "M4OSA_OptionID.h"

/**
 *    Include Video filters interface definition (for the M4VIFI_ImagePlane type) */
#include "M4VIFI_FiltersAPI.h"


/**
 ************************************************************************
 * VE Errors & Warnings definition
 ************************************************************************
*/
#define    M4ERR_VE_FATAL        ((M4OSA_ERR)M4OSA_ERR_CREATE(M4_ERR, M4VE_EXTERNAL, 0x000000))


/**
 *********************************************************************************************
 * enum        M4VE_EncoderMode
 * @brief    This enum defines in which mode the external encoder will be used
 *            ("Standalone encoder" or "Encoder + Grabber").
 *********************************************************************************************
 */
typedef enum
{
    M4VE_kSEMode,        /**< "Standalone Encoder" mode */
    M4VE_kEGMode        /**< "Encoder + Grabber" mode */
} M4VE_EncoderMode;


/**
 *********************************************************************************************
 * enum        M4VE_EncoderType
 * @brief    This enum defines the supported encoder types.
 *********************************************************************************************
 */
typedef enum
{
    M4VE_kMpeg4VideoEnc,     /**< MPEG-4 */
    M4VE_kH263VideoEnc,      /**< H263 */
    M4VE_kH264VideoEnc,      /**< H264 */
    M4VE_kMJPEGEnc,            /**< MJPEG */
    M4VE_kEncoderType_NB
} M4VE_EncoderType;


/**
 *********************************************************************************************
 * struct    M4VE_ImageSize
 * @brief    This structure defines video frame size (for both grabbing and encoding).
 *********************************************************************************************
 */
typedef struct
{
    M4OSA_UInt32     width;     /**< Width of the Image */
    M4OSA_UInt32     height;    /**< Height of the Image */
} M4VE_ImageSize;


/**
 *********************************************************************************************
 * enum        M4VE_FormatConfig
 * @brief    This enum defines the frame format we have in input for the grabbing
 *            part of the external encoder.
 *********************************************************************************************
*/
typedef enum
{
    M4VE_kYUV420=0,    /**< YUV 4:2:0 planar (standard input for mpeg-4 video) */
    M4VE_kYUV422,    /**< YUV422 planar */
    M4VE_kYUYV,        /**< YUV422 interlaced, luma first */
    M4VE_kUYVY,        /**< YUV422 interlaced, chroma first */
    M4VE_kJPEG,        /**< JPEG compressed frames */
    M4VE_kRGB444,    /**< RGB 12 bits 4:4:4 */
    M4VE_kRGB555,    /**< RGB 15 bits 5:5:5 */
    M4VE_kRGB565,    /**< RGB 16 bits 5:6:5 */
    M4VE_kRGB24,    /**< RGB 24 bits 8:8:8 */
    M4VE_kRGB32,    /**< RGB 32 bits  */
    M4VE_kBGR444,    /**< BGR 12 bits 4:4:4 */
    M4VE_kBGR555,    /**< BGR 15 bits 5:5:5 */
    M4VE_kBGR565,    /**< BGR 16 bits 5:6:5 */
    M4VE_kBGR24,    /**< BGR 24 bits 8:8:8 */
    M4VE_kBGR32        /**< BGR 32 bits  */
} M4VE_FormatConfig;


/**
 *********************************************************************************************
 * struct    M4VE_Framerate
 * @brief    This structure defines the maximum framerate the encoder will have
 *            at input and will generate at output (in frames per second).
 *********************************************************************************************
*/
typedef struct
{
    M4OSA_UInt32     framerateNum;    /**< Framerate numerator */
    M4OSA_UInt32     framerateDen;    /**< Framrate denominator */
} M4VE_Framerate;
/**<     For example, a framerate of 29.97 fps for H263 encoding will be expressed as:
    FramerateNum = 30000
    FramerateDen = 1001 */


/**
 *********************************************************************************************
 * struct    M4VE_GrabbingParameters
 * @brief    This structure defines the grabbing parameters set at open step.
 *********************************************************************************************
*/
typedef struct
{
    M4VE_ImageSize        size;        /**< Size of grabbed frames */
    M4VE_FormatConfig    format;        /**< Format of the grabbed frames (YUV420, RGB565,etc.) */
} M4VE_GrabbingParameters;


/**
 *********************************************************************************************
 * struct    M4VE_EncodingParameters
 * @brief    This structure defines the encoding parameters set at open step.
 *********************************************************************************************
*/
typedef struct
{
    M4VE_EncoderType  type;             /**< coding type (H263/H264/MPEG-4)*/
    M4VE_ImageSize    size;             /**< Size of frames to encode */
    M4OSA_Bool          bRateControlEnable; /**< Flag to enable/disable rate control */
    M4OSA_Bool          bLowDelay;        /**< force encoder in "low delay" mode */
    M4OSA_UInt32      bitrate;             /**< Average targeted bitrate in bit per sec */
    M4VE_Framerate    framerate;        /**< Maximum input framerate */
    M4OSA_UInt32      timescale;       /**< timescale of the video bitstream */
    M4OSA_Context     pUserSettings;   /**< Additionnal user settings passed by the
                                            application to the service at Codec registration */
} M4VE_EncodingParameters;


/**
 *********************************************************************************************
 * struct    M4VE_VideoBuffer
 * @brief    This structure defines the output buffer where the encoded data
 *            are stored by the encoder.
 *********************************************************************************************
*/
typedef struct
{
    M4OSA_MemAddr32 pBuffer;    /**< pointer to video buffer 32 bits aligned */
    M4OSA_UInt32    bufferSize; /**< the size in bytes of the buffer */
} M4VE_VideoBuffer;


/**
 *********************************************************************************************
 * struct    M4VE_ParameterSet
 * @brief    Parameter set structure used for H264 headers.
 *********************************************************************************************
*/
typedef struct
{
    M4OSA_UInt16    length;                /**< Number of items*/
    M4OSA_UInt8*    pParameterSetUnit;  /**< Array of items*/
} M4VE_ParameterSet;


/**
 *********************************************************************************************
 * struct    M4VE_H264HeaderBuffer
 * @brief    This structure defines the buffer where the stream header is stored
 *            by the encoder, in case of H264
 *********************************************************************************************
*/
typedef struct
{
    M4OSA_UInt8            NALUnitLength;             /**< length in bytes of a NAL access Unit */
    M4OSA_UInt8            nOfSequenceParametersSets; /**< Number of sequence parameter sets*/
    M4OSA_UInt8            nOfPictureParametersSets;  /**< Number of picture parameter sets*/
    M4VE_ParameterSet    *pSequenceParameterSets;    /**< Sequence parameter set array */
    M4VE_ParameterSet    *pPictureParameterSets;        /**< Picture parameter set array */
} M4VE_H264HeaderBuffer;



/**
 *********************************************************************************************
 * struct    M4VE_HeaderBuffer
 * @brief    This structure defines the buffer where the stream header is stored
 *            by the encoder.
 *********************************************************************************************
*/
typedef struct
{
    union
    {
        M4VE_VideoBuffer         header;     /**< MPEG-4, H263, MJPEG */
        M4VE_H264HeaderBuffer     H264Header; /**< H264 */
    }M4VE_SpecificHeader;
} M4VE_HeaderBuffer;


/**
 *********************************************************************************************
 * enum        M4VE_OptionID
 * @brief    This defines the supported options handled by the video encoder interface.
 *********************************************************************************************
*/
typedef enum
{
    dummy=0
} M4VE_OptionID;

/**
 *********************************************************************************************
 * M4OSA_Int32 (*M4VE_SignalOpenEncoderDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);
 * @brief    This function signals to the service that the external encoder is opened.
 * @note    The external encoder returns one of the following codes in the errCode parameter:
 *            M4NO_ERROR    There is no error
 *            M4ERR_VE_FATAL    a fatal error occurred
 * @param    pUserData:                (IN) User data provided by the service at init step.
 * @param    errCode :                (IN) Error code returned to the service internal layers.
 * @return    M4NO_ERROR:                there is no error.
 * @return    M4ERR_VE_FATAL:        a fatal error occurred.
 *********************************************************************************************
*/
typedef M4OSA_Int32 (*M4VE_SignalOpenEncoderDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);


/**
 *********************************************************************************************
 * M4OSA_Int32 (*M4VE_SignalHeaderDone)(M4OSA_Context pUserData, M4OSA_ERR errCode,
 *                                       M4VE_HeaderBuffer *pBuffer);
 * @brief    This function signals to the service that the stream header is ready.
 * @note    The external encoder returns one of the following codes in the errCode parameter:
 *            M4NO_ERROR    There is no error
 *            M4ERR_VE_FATAL    a fatal error occurred
 * @param    pUserData:                (IN) User data provided by the service at init step.
 * @param    errCode :                (IN) Error code returned to the service internal layers.
 * @param    pBuffer :                (IN) Stream header.
 * @return    M4NO_ERROR:                there is no error.
 * @return    M4ERR_PARAMETER            pBuffer field is null or invalid.
 * @return    M4ERR_VE_FATAL:        a fatal error occurred.
 *********************************************************************************************
*/
typedef M4OSA_Int32 (*M4VE_SignalHeaderDone)(M4OSA_Context pUserData, M4OSA_ERR errCode,
                     M4VE_HeaderBuffer *pBuffer);


/**
 *********************************************************************************************
 * M4OSA_Int32 (*M4VE_SignalStartGrabberDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);
 * @brief    This function signals to the service that the grabbing part is started.
 *            This callback is unused in the "standalone encoder" mode.
 * @note    The external encoder returns one of the following codes in the errCode parameter:
 *            M4NO_ERROR    There is no error
 *            M4ERR_VE_FATAL    a fatal error occurred
 * @param    pUserData:                (IN) User data provided by the service at init step.
 * @param    errCode :                (IN) Error code returned to the service internal layers.
 * @return    M4NO_ERROR:                there is no error.
 * @return    M4ERR_VE_FATAL:        a fatal error occurred.
 *********************************************************************************************
*/
typedef M4OSA_Int32 (*M4VE_SignalStartGrabberDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);


/**
 *********************************************************************************************
 * M4OSA_Int32 (*M4VE_SignalStartEncoderDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);
 * @brief    This function signals to the service that the external encoder is started.
 *            This callback is unused in the "standalone encoder" mode.
 * @note    The external encoder returns one of the following codes in the errCode parameter:
 *            M4NO_ERROR    There is no error
 *            M4ERR_VE_FATAL    a fatal error occurred
 * @param    pUserData:                (IN) User data provided by the service at init step.
 * @param    errCode :                (IN) Error code returned to the service internal layers.
 * @return    M4NO_ERROR:                there is no error.
 * @return    M4ERR_VE_FATAL:        a fatal error occurred.
 *********************************************************************************************
*/
typedef M4OSA_Int32 (*M4VE_SignalStartEncoderDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);


/**
 *********************************************************************************************
 * M4OSA_Int32 (*M4VE_SignalEncodeDone)(M4OSA_Context pUserData, M4OSA_ERR    errCode,
                M4OSA_UInt32 cts, M4VE_VideoBuffer* pBuffer);
 * @brief    This function signals to the service that the encoding of a frame is done.
 *            The integrator must call this function when the encoding of the video
 *            frame is completed (for example in an interrupt callback).
 * @note    The external encoder returns one of the following codes in the errCode parameter:
 *            M4NO_ERROR    There is no error
 *            M4ERR_VE_FATAL    a fatal error occurred
 * @param    pUserData:                (IN) User data provided by the service at init step.
 * @param    errCode :                (IN) Error code returned to the service internal layers.
 * @param    cts :                    (IN) Time of the encoded frame (from stepEncode).
 * @param    pBuffer :                (IN) Encoded data Buffer.
 * @return    M4NO_ERROR:                there is no error.
 * @return    M4ERR_PARAMETER            At least one parameter is null or invalid.
 * @return    M4ERR_VE_FATAL:        a fatal error occurred.
 *********************************************************************************************
*/
typedef M4OSA_Int32 (*M4VE_SignalEncodeDone)(M4OSA_Context pUserData, M4OSA_ERR    errCode,
                         M4OSA_Time cts, M4VE_VideoBuffer* pBuffer);


/**
 *********************************************************************************************
 * M4OSA_Int32 (*M4VE_SignalStopGrabberDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);
 * @brief    This function signals to the service that the grabbing part is stopped.
 *            This callback is unused in the "standalone encoder" mode.
 * @note    The external encoder returns one of the following codes in the errCode parameter:
 *            M4NO_ERROR    There is no error
 *            M4ERR_VE_FATAL    a fatal error occurred
 * @param    pUserData:                (IN) User data provided by the service at init step.
 * @param    errCode :                (IN) Error code returned to the service internal layers.
 * @return    M4NO_ERROR:                there is no error.
 * @return    M4ERR_VE_FATAL:        a fatal error occurred.
 *********************************************************************************************
*/
typedef M4OSA_Int32 (*M4VE_SignalStopGrabberDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);


/**
 *********************************************************************************************
 * M4OSA_Int32 (*M4VE_SignalStopEncoderDone)(M4OSA_Context    pUserData, M4OSA_ERR errCode);
 * @brief    This function signals to the service that the external encoder is stopped.
 *            This callback is unused in the "standalone encoder" mode.
 * @note    The external encoder returns one of the following codes in the errCode parameter:
 *            M4NO_ERROR    There is no error
 *            M4ERR_VE_FATAL    a fatal error occurred
 * @param    pUserData:                (IN) User data provided by the service at init step.
 * @param    errCode :                (IN) Error code returned to the service internal layers.
 * @return    M4NO_ERROR:                there is no error.
 * @return    M4ERR_VE_FATAL:        a fatal error occurred.
 *********************************************************************************************
*/
typedef M4OSA_Int32 (*M4VE_SignalStopEncoderDone)(M4OSA_Context    pUserData, M4OSA_ERR errCode);


/**
 *********************************************************************************************
 * M4OSA_Int32 (*M4VE_SignalCloseEncoderDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);
 * @brief    This function signals to the service that the external encoder is closed.
 * @note    The external encoder returns one of the following codes in the errCode parameter:
 *            M4NO_ERROR    There is no error
 *            M4ERR_VE_FATAL    a fatal error occurred
 * @param    pUserData:                (IN) User data provided by the service at init step.
 * @param    errCode :                (IN) Error code returned to the service internal layers.
 * @return    M4NO_ERROR:                there is no error.
 * @return    M4ERR_VE_FATAL:        a fatal error occurred.
 *********************************************************************************************
*/
typedef M4OSA_Int32 (*M4VE_SignalCloseEncoderDone)(M4OSA_Context pUserData, M4OSA_ERR errCode);




/**
 *********************************************************************************************
 * struct    M4VE_GenericCallback
 * @brief    This structure is used to pass the generic callbacks, i.e. the ones that are used
 *            in both "Standalone Encoder" and "Encoder + Grabber" modes.
 *********************************************************************************************
*/
typedef struct
{
    M4VE_SignalOpenEncoderDone        pOpenEncoderDone; /**< Callback to use at open completion */
    M4VE_SignalHeaderDone             pHeaderDone;         /**< Callback to use when the stream
                                                                 header is ready */
    M4VE_SignalEncodeDone             pEncodeDone;         /**< Callback to use for any frame
                                                                    encoding completion */
    M4VE_SignalCloseEncoderDone       pCloseEncoderDone;/**< Callback to use at close completion */
} M4VE_GenericCallback;    /**< Callbacks used in all encoder modes */

/**
 *********************************************************************************************
 * struct    M4VE_EGModeCallback
 * @brief    This structure is used to pass the callbacks used in the "Encoder + Grabber" mode
 *********************************************************************************************
*/
typedef struct
{
    M4VE_SignalStartGrabberDone     pStartGrabberDone;/**< Callback to use at start
                                                            completion of the grabber part*/
    M4VE_SignalStartEncoderDone     pStartEncoderDone;/**< Callback to use at start
                                                            completion of the encoder part*/
    M4VE_SignalStopGrabberDone      pStopGrabberDone; /**< Callback to use at stop
                                                            completion of the grabber part*/
    M4VE_SignalStopEncoderDone      pStopEncoderDone; /**< Callback to use at stop
                                                            completion of the encoder part*/
} M4VE_EGModeCallback; /**< Callbacks used in "Encoder + Grabber" mode */

/**
 *********************************************************************************************
 * struct    M4VE_SEModeCallback
 * @brief    This structure is used to pass the callbacks used in the "Standalone Encoder" mode
 * @note    There's no specific callback for the standalone encoder mode,
 *               but we have to declare one
 * @note        for some compilers
 *********************************************************************************************
*/
typedef M4OSA_Int32 (*M4VE_SEDummyCB)  (M4OSA_Context pUserData, M4OSA_ERR errCode);

typedef struct
{
    M4VE_SEDummyCB                  pDummySECB; /**< No specific callback for
                                                        Standalone encoder mode */
} M4VE_SEModeCallback; /**< Callbacks used in "Standalone Encoder" mode */


/**
 *********************************************************************************************
 * struct    M4VE_CallbackInterface
 * @brief    This structure is the container for the whole set of callback used by external encoder
  *********************************************************************************************
*/

typedef struct
{
    M4VE_GenericCallback    genericCallback;/**< Callbacks used in all modes */
    union
    {
        M4VE_EGModeCallback    EGModeCallback; /**< Callbacks used in "Encoder + Grabber" mode */
        M4VE_SEModeCallback    SEModeCallback; /**< Callbacks used in "Standalone Encoder" mode */
    } M4VE_SpecificModeCallBack;
    M4OSA_Context            pUserData;      /**< Internal user data to be retrieved in each
                                                    callbach above */
} M4VE_CallbackInterface;


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_initEncoder_fct)(M4OSA_Context* pContext,
 *                                       M4VE_CallbackInterface* pCallbackInterface);
 * @brief    This function initializes the external video encoder API.
 * @note    This function typically allocates the user context that will be provided
 *            to the other functions as their first argument. The second argument is
 *            the callback interface given by the service. Encoder implementation is supposed
 *            to use these callbacks in response to each asynchronous API function.
 *            All these callbacks must be called with the pUserData field specified
 *            by the service inside the M4VE_CallbackInterface structure.
 * @param    pContext:            (OUT) Execution context of the encoder.
 * @param    pCallbackInterface:    (IN) Callback interface.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    At least one parameter is not correct (NULL or invalid).
 * @return    M4ERR_ALLOC:        there is no more available memory.
 *********************************************************************************************
*/
typedef    M4OSA_ERR (*M4VE_initEncoder_fct)(M4OSA_Context* pContext,
                     M4VE_CallbackInterface*    pCallbackInterface);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_setOption_fct)(M4OSA_Context, M4VE_OptionID, M4OSA_DataOption);
 * @brief    This function is used to set an option in the video encoder interface.
 * @note    none
 * @param    pContext:        (IN) Execution context of the encoder.
 * @param    optionId:        (IN) Id of the option to set.
 * @param    pValue:            (IN) Pointer of the option data to set.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    At least one parameter is not correct (NULL or invalid).
 * @return    M4ERR_BAD_OPTION_ID:The requested option Id is invalid.
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_setOption_fct)(M4OSA_Context pContext,    M4VE_OptionID optionId,
                                        M4OSA_DataOption pValue);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_getOption_fct)(M4OSA_Context, M4VE_OptionID, M4OSA_DataOption*);
 * @brief    This function is used to retrieve an option in the video interface.
 * @note    none
 * @param    pContext:        (IN) Execution context of the encoder.
 * @param    optionId:        (IN) Id of the option to set.
 * @param    pValue:            (OUT) Pointer to the location where the requested option will
 *                                      be stored.
 * @return    M4NO_ERROR:        there is no error.
 * @return    M4ERR_PARAMETER:    At least one parameter is not correct (NULL or invalid).
 * @return    M4ERR_BAD_OPTION_ID:The requested option Id is invalid.
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_getOption_fct)(M4OSA_Context pContext, M4VE_OptionID optionId,
                             M4OSA_DataOption* pValue);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_openEncoder_fct)(M4OSA_Context pContext,
 *                                     M4VE_GrabbingParameters *pGrabbingParams,
 *                                     M4VE_EncodingParameters *pEncodingParams);
 * @brief    This function opens an instance of the video encoder.
 *            Both encoding and grabbing parameters are specified here.
 * @note    This function is asynchronous, thus the external encoder must call the corresponding
 *            M4VE_SignalOpenEncoderDone callback function when the opening step is internally
 *            completed.
 *            Please note that both grabber and encoder components are opened at this step in
 *            the "encoder + grabber" mode. In response to this open, the encoder must also return
 *            the stream header (including VOS, VO & VOL) using the M4VE_SignalHeaderDone callback
 *            function. Usually the service waits for this callback between the
 *            M4VE_SignalOpenEncoderDone
 *            callback and the M4VE_SignalCloseEncoderDone callback in order to handle it.
 * @param    pContext:            (IN) Execution context of the encoder.
 * @param    pGrabbingParams:    (IN) Grabbing parameters (can be optional, in this case is
 *                                    must be NULL).
 * @param    pEncodingParams:    (IN) Encoding parameters.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    At least one parameter is not correct (NULL or invalid).
 * @return    M4ERR_ALLOC:        there is no more available memory.
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_FATAL:    The encoder could not be opened
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_openEncoder_fct)(M4OSA_Context pContext,
                         M4VE_GrabbingParameters *pGrabbingParams,
                          M4VE_EncodingParameters *pEncodingParams);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_forceIFrame_fct)(M4OSA_Context pContext);
 * @brief    This function is used by the service to signal the external encoder that an Intra
 *           refresh frame must be encoded. This function is used in both "Standalone Encoder" and
 *            "Encoder + grabber" modes and can be called at any time during the encoding session.
 * @note    For the "Encoder + Grabber" mode, this function can be called between the reception
 *            of the M4VE_SignalStartEncoderDone callback and the call to M4VE_stopEncoder_fct.
 *            For the "Standalone Encoder" mode, this function can be called between the reception
 *            of the M4VE_SignalOpenEncoderDone callback and the call to M4VE_closeEncoder_fct.
 *            The expected behavior is that the external encoder encodes an intra refresh frame
 *            for one of the frames coming next to the call of M4VE_forceIFrame_fct.
 * @param    pContext:            (IN) Execution context of the encoder.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    pContext field is not valid
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_FATAL:    The encoder could not handle this call
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_forceIFrame_fct)(M4OSA_Context pContext);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_releaseOutputBuffer_fct)(M4OSA_Context pContext, M4VE_VideoBuffer *pBuffer);
 * @brief    This function is called by the service to signal that a particular output buffer,
 *           provided in the M4VE_SignalEncodeDone callback by the external encoder, is no more
 *           needed by the service and can be considered as free for any remaining data processing.
 * @note    none.
 * @param    pContext:            (IN) Execution context of the encoder.
 * @param    pBuffer:            (IN) Encoded data Buffer.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    At least one parameter is not correct (NULL or invalid).
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_FATAL:    The encoder could not acknowledge the buffer release for any
 *                                other reason.
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_releaseOutputBuffer_fct)(M4OSA_Context pContext,
                                                    M4VE_VideoBuffer *pBuffer);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_closeEncoder_fct)(M4OSA_Context pContext);
 * @brief    This function closes the encoding session.
 * @note    This function is asynchronous, thus the external encoder must call the corresponding
 *            M4VE_SignalCloseEncoderDone callback function when the closing step is internally
 *            completed.
 * @param    pContext:            (IN) Execution context of the encoder.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    pContext pointer is null or invalid.
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_FATAL:    The encoder could not be closed for any other reason.
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_closeEncoder_fct)(M4OSA_Context pContext);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_cleanUpEncoder_fct)(M4OSA_Context pContext);
 * @brief    The function cleans up the encoder context.
 * @note    none
 * @param    pContext:            (IN) Execution context of the encoder.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    pContext pointer is null or invalid.
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_FATAL:    The encoder could not be closed for any other reason.
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_cleanUpEncoder_fct)(M4OSA_Context pContext);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_stepEncode_fct)(M4OSA_Context pContext,M4VIFI_ImagePlane *pInputPlane,
 *                                  M4OSA_Time cts);
 * @brief    The function gives a video frame to the external encoder in the "Standalone encoder"
 *            mode. The input buffer consists of a raw YUV420 planar frame,
 *            allocated by the service.
 *            The time (cts) is the composition time stamp of the frame to encode and is unique
 *            for each frame. This time is expressed in milliseconds.
 * @note    This function is asynchronous and its completion is signaled by the
 *            M4VE_SignalEncodeDone callback. It applies that the input buffer is maintained valid
 *            by the service till the call of this callback. The encoded data are retrieved in
 *            this callback function in a dedicated structure, allocated by the external encoder.
 *            The input buffer (YUV raw frame) is considered by the service as free for any
 *             remaining data processing after receiving the M4VE_SignalEncodeDone callback.
 * @param    pContext:            (IN) Execution context of the encoder.
 * @param    pInputPlane:        (IN) Input buffer where video frame is stored.
 * @param    cts:                (IN) Composition time stamp in milliseconds.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    pContext field is not valid
 * @return    M4ERR_ALLOC:        there is no more available memory.
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_FATAL:    The encoder could not encode the frame for any other reason.
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_stepEncode_fct)(M4OSA_Context pContext,M4VIFI_ImagePlane *pInputPlane,
                                            M4OSA_Time cts);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_startGrabber_fct)(M4OSA_Context pContext);
 * @brief    This function starts the grabber sub-component of the external encoder, in the
 *            "encoder + grabber" mode. This function is asynchronous, thus the external
 *            encoder must call the corresponding M4VE_SignalStartGrabberDone callback function
 *            when this start is internally effective.
 * @note    During this step, the service waits for the grabber to launch any video preview if
 *            needed.
 * @param    pContext:            (IN) Execution context of the encoder.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    pContext field is not valid
 * @return    M4ERR_ALLOC:        there is no more available memory.
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_FATAL:    the encoder could not be started for any other reason.
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_startGrabber_fct)(M4OSA_Context pContext);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_startEncoder_fct)(M4OSA_Context pContext);
 * @brief    This function starts the video encoder in the "encoder + grabber" mode.
 * @note    This function is asynchronous, thus the external encoder must call the corresponding
 *            M4VE_SignalStartEncoderDone callback function when this start is internally
 *            effective.
 *            After the completion of this asynchronous function, the service waits for the
 *            external encoder to periodically call the M4VE_SignalEncodeDone callback each time
 *            a new frame has been encoded. The external encoder must expect to have several
 *            M4VE_startEncoder_fct calls before being closed. See the description of
 *            M4VE_stopEncoder_fct function for the expected behaviour.
 * @param    pContext:            (IN) Execution context of the encoder.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    pContext field is not valid
 * @return    M4ERR_ALLOC:        there is no more available memory.
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_FATAL:    the encoder could not be started for any other reason.
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_startEncoder_fct)(M4OSA_Context pContext);


/**
 *********************************************************************************************
 * M4OSA_ERR M4OSA_ERR (*M4VE_stopGrabber_fct)(M4OSA_Context pContext);
 * @brief    This function stops the video grabber in the "encoder + grabber" mode.
 * @note    This function is asynchronous, thus the external encoder must call the corresponding
 *          M4VE_SignalStopGrabberDone callback function when this stop is internally effective.
 *          During this step, the service waits for the grabber to stop the video preview
 *          if needed.
 * @param    pContext:            (IN) Execution context of the encoder.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    pContext field is not valid
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_FATAL:    the encoder could not be stopped for any other reason.
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_stopGrabber_fct)(M4OSA_Context pContext);


/**
 *********************************************************************************************
 * M4OSA_ERR (*M4VE_stopEncoder_fct)(M4OSA_Context pContext);
 * @brief    This function stops the video encoder in the "encoder + grabber" mode.
 * @note    This function is asynchronous, thus the external encoder must call the corresponding
 *            M4VE_SignalStopEncoderDone callback function when this stop is internally effective.
 *            After the reception of this callback, the service considers that no new frame will be
 *            retrieved via the M4VE_SignalEncodeDone callback.
 *            The external encoder must expect to have a possible call to M4VE_startEncoder_fct
 *            after M4VE_stopEncoder_fct. In this case, the external encoder must consider that it
 *             has been paused/resumed. The expected behaviour is the following one:
 *            - The result from this two encoding sessions is a Standalone stream, no header is
 *            generated for this new session. The external encoder is free to encode a refresh
 *            frame (like I VOP) for this new session.
 *            - The time stamps of this new session must directly follow the time stamps of the
 *            previous one (ie: no time hole coming from the delay between the stop of the first
 *            session and the start of the new one).
 * @param    pContext:            (IN) Execution context of the encoder.
 * @return    M4NO_ERROR:            there is no error.
 * @return    M4ERR_PARAMETER:    pContext field is not valid
 * @return    M4ERR_STATE:        This call is not allowed in the current encoder state.
 * @return    M4ERR_VE_ERR_FATAL:    the encoder could not be stopped for any other reason
 *********************************************************************************************
*/
typedef M4OSA_ERR (*M4VE_stopEncoder_fct)(M4OSA_Context pContext);





/**
 *********************************************************************************************
 * struct    M4VE_GenericInterface
 * @brief    The M4VE_GenericInterface structure defines the set of functions used in
 *               both encoder modes.
 *********************************************************************************************
*/
typedef struct
{
    M4VE_initEncoder_fct            m_pFctInitEncoder;
    M4VE_setOption_fct                m_pFctSetOption;
    M4VE_getOption_fct                m_pFctGetOption;
    M4VE_openEncoder_fct            m_pFctOpenEncoder;
    M4VE_forceIFrame_fct            m_pFctForceIFrame;
    M4VE_releaseOutputBuffer_fct    m_pFctReleaseOutputBuffer;
    M4VE_closeEncoder_fct            m_pFctCloseEncoder;
    M4VE_cleanUpEncoder_fct            m_pFctCleanUpEncoder;
} M4VE_GenericInterface;            /**< Functions used in both "Standalone Encoder" and
                                        "Encoder + Grabber" modes */


/**
 *********************************************************************************************
 * struct    M4VE_SEModeInterface
 * @brief    The M4VE_SEModeInterface structure defines the set of functions used in
 *              "Standalone Encoder" mode.
 *********************************************************************************************
*/
typedef struct
{
    M4VE_stepEncode_fct            m_pFctStepEncode;
} M4VE_SEModeInterface;            /**< Functions used only in "Standalone Encoder" mode */


/**
 *********************************************************************************************
 * struct    M4VE_EGModeInterface
 * @brief    The M4VE_EGModeInterface structure defines the set of functions used in
 *              "Encoder + Grabber" mode.
 *********************************************************************************************
*/
typedef struct
{
    M4VE_startGrabber_fct        m_pFctStartGrabber;
    M4VE_startEncoder_fct        m_pFctStartEncoder;
    M4VE_stopGrabber_fct        m_pFctStopGrabber;
    M4VE_stopEncoder_fct        m_pFctStopEncoder;
} M4VE_EGModeInterface;            /**< Functions used only in "Encoder + Grabber" mode */



/**
 *********************************************************************************************
 * struct    M4VE_Interface
 * @brief    The M4VE_Interface structure stores pointers to the video encoder functions.
 *********************************************************************************************
*/
typedef struct
{
    M4VE_GenericInterface        genericInterface;    /**< Functions used everytime */
    M4VE_EncoderMode            encoderMode;        /**< "Standalone Encoder"
                                                    or "Encoder + Grabber" */
    union
    {
        M4VE_SEModeInterface    SEModeInterface;    /**< Functions used only in
                                                    "Standalone Encoder" mode */
        M4VE_EGModeInterface    EGModeInterface;    /**< Functions used only in
                                                    "Encoder + Grabber" mode */
    }M4VE_SpecificInterface;
} M4VE_Interface;

#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /*__M4VE_API_H__*/
