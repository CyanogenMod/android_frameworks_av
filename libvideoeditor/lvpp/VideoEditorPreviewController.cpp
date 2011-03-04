/*
 * Copyright (C) 2011 NXP Software
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

#define LOG_NDEBUG 1
#define LOG_TAG "VideoEditorPreviewController"
#include "VideoEditorPreviewController.h"

namespace android {

#define PREVIEW_THREAD_STACK_SIZE                           (65536)

VideoEditorPreviewController::VideoEditorPreviewController()
    : mCurrentPlayer(0),
      mThreadContext(NULL),
      mPlayerState(VePlayerIdle),
      mPrepareReqest(M4OSA_FALSE),
      mClipList(NULL),
      mNumberClipsInStoryBoard(0),
      mNumberClipsToPreview(0),
      mStartingClipIndex(0),
      mPreviewLooping(M4OSA_FALSE),
      mCallBackAfterFrameCnt(0),
      mEffectsSettings(NULL),
      mNumberEffects(0),
      mCurrentClipNumber(-1),
      mClipTotalDuration(0),
      mCurrentVideoEffect(VIDEO_EFFECT_NONE),
      mBackgroundAudioSetting(NULL),
      mAudioMixPCMFileHandle(NULL),
      mTarget(NULL),
      mJniCookie(NULL),
      mJniCallback(NULL),
      mCurrentPlayedDuration(0),
      mCurrentClipDuration(0),
      mVideoStoryBoardTimeMsUptoFirstPreviewClip(0),
      mOverlayState(OVERLAY_CLEAR),
      mActivePlayerIndex(0),
      mOutputVideoWidth(0),
      mOutputVideoHeight(0),
      bStopThreadInProgress(false),
      mSemThreadWait(NULL) {
    LOGV("VideoEditorPreviewController");
    mRenderingMode = M4xVSS_kBlackBorders;
    mIsFiftiesEffectStarted = false;

    for (int i=0; i<NBPLAYER_INSTANCES; i++) {
        mVePlayer[i] = NULL;
    }
}

VideoEditorPreviewController::~VideoEditorPreviewController() {
    M4OSA_UInt32 i = 0;
    M4OSA_ERR err = M4NO_ERROR;
    LOGV("~VideoEditorPreviewController");

    // Stop the thread if its still running
    if(mThreadContext != NULL) {
        err = M4OSA_threadSyncStop(mThreadContext);
        if(err != M4NO_ERROR) {
            LOGV("~VideoEditorPreviewController: error 0x%x \
            in trying to stop thread", err);
            // Continue even if error
        }

        err = M4OSA_threadSyncClose(mThreadContext);
        if(err != M4NO_ERROR) {
            LOGE("~VideoEditorPreviewController: error 0x%x \
            in trying to close thread", (unsigned int) err);
            // Continue even if error
        }

        mThreadContext = NULL;
    }

    for (int playerInst=0; playerInst<NBPLAYER_INSTANCES;
         playerInst++) {
        if(mVePlayer[playerInst] != NULL) {
            LOGV("clearing mVePlayer %d", playerInst);
            mVePlayer[playerInst].clear();
        }
    }

    if(mClipList != NULL) {
        // Clean up
        for(i=0;i<mNumberClipsInStoryBoard;i++)
        {
            if(mClipList[i]->pFile != NULL) {
                M4OSA_free((M4OSA_MemAddr32)mClipList[i]->pFile);
                mClipList[i]->pFile = NULL;
            }

            M4OSA_free((M4OSA_MemAddr32)mClipList[i]);
        }
        M4OSA_free((M4OSA_MemAddr32)mClipList);
        mClipList = NULL;
    }

    if(mEffectsSettings) {
        for(i=0;i<mNumberEffects;i++) {
            if(mEffectsSettings[i].xVSS.pFramingBuffer != NULL) {
                M4OSA_free(
                (M4OSA_MemAddr32)mEffectsSettings[i].xVSS.pFramingBuffer->pac_data);

                M4OSA_free(
                (M4OSA_MemAddr32)mEffectsSettings[i].xVSS.pFramingBuffer);

                mEffectsSettings[i].xVSS.pFramingBuffer = NULL;
            }
        }
        M4OSA_free((M4OSA_MemAddr32)mEffectsSettings);
        mEffectsSettings = NULL;
    }

    if (mAudioMixPCMFileHandle) {
        err = M4OSA_fileReadClose (mAudioMixPCMFileHandle);
        mAudioMixPCMFileHandle = M4OSA_NULL;
    }

    if (mBackgroundAudioSetting != NULL) {
        M4OSA_free((M4OSA_MemAddr32)mBackgroundAudioSetting);
        mBackgroundAudioSetting = NULL;
    }

    if(mTarget != NULL) {
        delete mTarget;
        mTarget = NULL;
    }

    mOverlayState = OVERLAY_CLEAR;

    LOGV("~VideoEditorPreviewController returns");
}

M4OSA_ERR VideoEditorPreviewController::loadEditSettings(
    M4VSS3GPP_EditSettings* pSettings,M4xVSS_AudioMixingSettings* bgmSettings) {

    M4OSA_UInt32 i = 0, iClipDuration = 0, rgbSize = 0;
    M4VIFI_UInt8 *tmp = NULL;
    M4OSA_ERR err = M4NO_ERROR;

    LOGV("loadEditSettings");
    LOGV("loadEditSettings Channels = %d, sampling Freq %d",
          bgmSettings->uiNbChannels, bgmSettings->uiSamplingFrequency  );
          bgmSettings->uiSamplingFrequency = 32000;

    LOGV("loadEditSettings Channels = %d, sampling Freq %d",
          bgmSettings->uiNbChannels, bgmSettings->uiSamplingFrequency  );
    Mutex::Autolock autoLock(mLock);

    // Clean up any previous Edit settings before loading new ones
    mCurrentVideoEffect = VIDEO_EFFECT_NONE;

    if(mAudioMixPCMFileHandle) {
        err = M4OSA_fileReadClose (mAudioMixPCMFileHandle);
        mAudioMixPCMFileHandle = M4OSA_NULL;
    }

    if(mBackgroundAudioSetting != NULL) {
        M4OSA_free((M4OSA_MemAddr32)mBackgroundAudioSetting);
        mBackgroundAudioSetting = NULL;
    }

    if(mClipList != NULL) {
        // Clean up
        for(i=0;i<mNumberClipsInStoryBoard;i++)
        {
            if(mClipList[i]->pFile != NULL) {
                M4OSA_free((M4OSA_MemAddr32)mClipList[i]->pFile);
                mClipList[i]->pFile = NULL;
            }

            M4OSA_free((M4OSA_MemAddr32)mClipList[i]);
        }
        M4OSA_free((M4OSA_MemAddr32)mClipList);
        mClipList = NULL;
    }

    if(mEffectsSettings) {
        for(i=0;i<mNumberEffects;i++) {
            if(mEffectsSettings[i].xVSS.pFramingBuffer != NULL) {
                M4OSA_free(
                (M4OSA_MemAddr32)mEffectsSettings[i].xVSS.pFramingBuffer->pac_data);

                M4OSA_free(
                (M4OSA_MemAddr32)mEffectsSettings[i].xVSS.pFramingBuffer);

                mEffectsSettings[i].xVSS.pFramingBuffer = NULL;
            }
        }
        M4OSA_free((M4OSA_MemAddr32)mEffectsSettings);
        mEffectsSettings = NULL;
    }

    if(mClipList == NULL) {
        mNumberClipsInStoryBoard = pSettings->uiClipNumber;
        LOGV("loadEditSettings: # of Clips = %d", mNumberClipsInStoryBoard);

        mClipList = (M4VSS3GPP_ClipSettings**)M4OSA_malloc(
         sizeof(M4VSS3GPP_ClipSettings*)*pSettings->uiClipNumber, M4VS,
         (M4OSA_Char*)"LvPP, copy of pClipList");

        if(NULL == mClipList) {
            LOGE("loadEditSettings: Malloc error");
            return M4ERR_ALLOC;
        }
        M4OSA_memset((M4OSA_MemAddr8)mClipList,
         sizeof(M4VSS3GPP_ClipSettings*)*pSettings->uiClipNumber, 0);

        for(i=0;i<pSettings->uiClipNumber;i++) {

            // Allocate current clip
            mClipList[i] =
             (M4VSS3GPP_ClipSettings*)M4OSA_malloc(
              sizeof(M4VSS3GPP_ClipSettings),M4VS,(M4OSA_Char*)"clip settings");

            if(mClipList[i] == NULL) {

                LOGE("loadEditSettings: Allocation error for mClipList[%d]", (int)i);
                return M4ERR_ALLOC;
            }
            // Copy plain structure
            M4OSA_memcpy((M4OSA_MemAddr8)mClipList[i],
             (M4OSA_MemAddr8)pSettings->pClipList[i],
             sizeof(M4VSS3GPP_ClipSettings));

            if(NULL != pSettings->pClipList[i]->pFile) {
                mClipList[i]->pFile = (M4OSA_Char*)M4OSA_malloc(
                pSettings->pClipList[i]->filePathSize, M4VS,
                (M4OSA_Char*)"pClipSettingsDest->pFile");

                if(NULL == mClipList[i]->pFile)
                {
                    LOGE("loadEditSettings : ERROR allocating filename");
                    return M4ERR_ALLOC;
                }

                M4OSA_memcpy((M4OSA_MemAddr8)mClipList[i]->pFile,
                 (M4OSA_MemAddr8)pSettings->pClipList[i]->pFile,
                 pSettings->pClipList[i]->filePathSize);
            }
            else {
                LOGE("NULL file path");
                return M4ERR_PARAMETER;
            }

            // Calculate total duration of all clips
            iClipDuration = pSettings->pClipList[i]->uiEndCutTime -
             pSettings->pClipList[i]->uiBeginCutTime;

            mClipTotalDuration = mClipTotalDuration+iClipDuration;
        }
    }

    if(mEffectsSettings == NULL) {
        mNumberEffects = pSettings->nbEffects;
        LOGV("loadEditSettings: mNumberEffects = %d", mNumberEffects);

        if(mNumberEffects != 0) {
            mEffectsSettings = (M4VSS3GPP_EffectSettings*)M4OSA_malloc(
             mNumberEffects*sizeof(M4VSS3GPP_EffectSettings),
             M4VS, (M4OSA_Char*)"effects settings");

            if(mEffectsSettings == NULL) {
                LOGE("loadEffectsSettings: Allocation error");
                return M4ERR_ALLOC;
            }

            M4OSA_memset((M4OSA_MemAddr8)mEffectsSettings,
             mNumberEffects*sizeof(M4VSS3GPP_EffectSettings), 0);

            for(i=0;i<mNumberEffects;i++) {

                mEffectsSettings[i].xVSS.pFramingFilePath = NULL;
                mEffectsSettings[i].xVSS.pFramingBuffer = NULL;
                mEffectsSettings[i].xVSS.pTextBuffer = NULL;

                M4OSA_memcpy((M4OSA_MemAddr8)&(mEffectsSettings[i]),
                 (M4OSA_MemAddr8)&(pSettings->Effects[i]),
                 sizeof(M4VSS3GPP_EffectSettings));

                if(pSettings->Effects[i].VideoEffectType ==
                 (M4VSS3GPP_VideoEffectType)M4xVSS_kVideoEffectType_Framing) {
                    // Allocate the pFraming RGB buffer
                    mEffectsSettings[i].xVSS.pFramingBuffer =
                    (M4VIFI_ImagePlane *)M4OSA_malloc(sizeof(M4VIFI_ImagePlane),
                     M4VS, (M4OSA_Char*)"lvpp framing buffer");

                    if(mEffectsSettings[i].xVSS.pFramingBuffer == NULL) {
                        LOGE("loadEffectsSettings:Alloc error for pFramingBuf");
                        M4OSA_free((M4OSA_MemAddr32)mEffectsSettings);
                        mEffectsSettings = NULL;
                        return M4ERR_ALLOC;
                    }

                    // Allocate the pac_data (RGB)
                    if(pSettings->Effects[i].xVSS.rgbType == M4VSS3GPP_kRGB565){
                        rgbSize =
                         pSettings->Effects[i].xVSS.pFramingBuffer->u_width *
                         pSettings->Effects[i].xVSS.pFramingBuffer->u_height*2;
                    }
                    else if(
                     pSettings->Effects[i].xVSS.rgbType == M4VSS3GPP_kRGB888) {
                        rgbSize =
                         pSettings->Effects[i].xVSS.pFramingBuffer->u_width *
                         pSettings->Effects[i].xVSS.pFramingBuffer->u_height*3;
                    }
                    else {
                        LOGE("loadEffectsSettings: wrong RGB type");
                        M4OSA_free((M4OSA_MemAddr32)mEffectsSettings);
                        mEffectsSettings = NULL;
                        return M4ERR_PARAMETER;
                    }

                    tmp = (M4VIFI_UInt8 *)M4OSA_malloc(rgbSize, M4VS,
                     (M4OSA_Char*)"framing buffer pac_data");

                    if(tmp == NULL) {
                        LOGE("loadEffectsSettings:Alloc error pFramingBuf pac");
                        M4OSA_free((M4OSA_MemAddr32)mEffectsSettings);
                        mEffectsSettings = NULL;
                        M4OSA_free(
                        (M4OSA_MemAddr32)mEffectsSettings[i].xVSS.pFramingBuffer);

                        mEffectsSettings[i].xVSS.pFramingBuffer = NULL;
                        return M4ERR_ALLOC;
                    }
                    /* Initialize the pFramingBuffer*/
                    mEffectsSettings[i].xVSS.pFramingBuffer->pac_data = tmp;
                    mEffectsSettings[i].xVSS.pFramingBuffer->u_height =
                     pSettings->Effects[i].xVSS.pFramingBuffer->u_height;

                    mEffectsSettings[i].xVSS.pFramingBuffer->u_width =
                     pSettings->Effects[i].xVSS.pFramingBuffer->u_width;

                    mEffectsSettings[i].xVSS.pFramingBuffer->u_stride =
                     pSettings->Effects[i].xVSS.pFramingBuffer->u_stride;

                    mEffectsSettings[i].xVSS.pFramingBuffer->u_topleft =
                     pSettings->Effects[i].xVSS.pFramingBuffer->u_topleft;

                    mEffectsSettings[i].xVSS.uialphaBlendingStart =
                     pSettings->Effects[i].xVSS.uialphaBlendingStart;

                    mEffectsSettings[i].xVSS.uialphaBlendingMiddle =
                     pSettings->Effects[i].xVSS.uialphaBlendingMiddle;

                    mEffectsSettings[i].xVSS.uialphaBlendingEnd =
                     pSettings->Effects[i].xVSS.uialphaBlendingEnd;

                    mEffectsSettings[i].xVSS.uialphaBlendingFadeInTime =
                     pSettings->Effects[i].xVSS.uialphaBlendingFadeInTime;
                    mEffectsSettings[i].xVSS.uialphaBlendingFadeOutTime =
                     pSettings->Effects[i].xVSS.uialphaBlendingFadeOutTime;

                    // Copy the pFraming data
                    M4OSA_memcpy((M4OSA_MemAddr8)
                    mEffectsSettings[i].xVSS.pFramingBuffer->pac_data,
                    (M4OSA_MemAddr8)pSettings->Effects[i].xVSS.pFramingBuffer->pac_data,
                    rgbSize);

                    mEffectsSettings[i].xVSS.rgbType =
                     pSettings->Effects[i].xVSS.rgbType;
                }
            }
        }
    }

    if (mBackgroundAudioSetting == NULL) {

        mBackgroundAudioSetting = (M4xVSS_AudioMixingSettings*)M4OSA_malloc(
        sizeof(M4xVSS_AudioMixingSettings), M4VS,
        (M4OSA_Char*)"LvPP, copy of bgmSettings");

        if(NULL == mBackgroundAudioSetting) {
            LOGE("loadEditSettings: mBackgroundAudioSetting Malloc failed");
            return M4ERR_ALLOC;
        }

        M4OSA_memset((M4OSA_MemAddr8)mBackgroundAudioSetting, sizeof(M4xVSS_AudioMixingSettings*), 0);
        M4OSA_memcpy((M4OSA_MemAddr8)mBackgroundAudioSetting, (M4OSA_MemAddr8)bgmSettings, sizeof(M4xVSS_AudioMixingSettings));

        if ( mBackgroundAudioSetting->pFile != M4OSA_NULL ) {

            mBackgroundAudioSetting->pFile = (M4OSA_Void*) bgmSettings->pPCMFilePath;
            mBackgroundAudioSetting->uiNbChannels = 2;
            mBackgroundAudioSetting->uiSamplingFrequency = 32000;
        }

        // Open the BG file
        if ( mBackgroundAudioSetting->pFile != M4OSA_NULL ) {
            err = M4OSA_fileReadOpen(&mAudioMixPCMFileHandle,
             mBackgroundAudioSetting->pFile, M4OSA_kFileRead);

            if (err != M4NO_ERROR) {
                LOGE("loadEditSettings: mBackgroundAudio PCM File open failed");
                return M4ERR_PARAMETER;
            }
        }
    }

    mOutputVideoSize = pSettings->xVSS.outputVideoSize;
    mFrameStr.pBuffer = M4OSA_NULL;
    return M4NO_ERROR;
}

M4OSA_ERR VideoEditorPreviewController::setSurface(const sp<Surface> &surface) {
    LOGV("setSurface");
    Mutex::Autolock autoLock(mLock);

    mSurface = surface;
    mISurface = surface->getISurface();
    LOGV("setSurface: mISurface = %p", mISurface.get());
    return M4NO_ERROR;
}

M4OSA_ERR VideoEditorPreviewController::startPreview(
    M4OSA_UInt32 fromMS, M4OSA_Int32 toMs, M4OSA_UInt16 callBackAfterFrameCount,
    M4OSA_Bool loop) {

    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_UInt32 i = 0, iIncrementedDuration = 0;
    LOGV("startPreview");

    if(fromMS > (M4OSA_UInt32)toMs) {
        LOGE("startPreview: fromMS > toMs");
        return M4ERR_PARAMETER;
    }

    if(toMs == 0) {
        LOGE("startPreview: toMs is 0");
        return M4ERR_PARAMETER;
    }

    // If already started, then stop preview first
    for(int playerInst=0; playerInst<NBPLAYER_INSTANCES; playerInst++) {
        if(mVePlayer[playerInst] != NULL) {
            LOGV("startPreview: stopping previously started preview playback");
            stopPreview();
            break;
        }
    }

    // If renderPreview was called previously, then delete Renderer object first
    if(mTarget != NULL) {
        LOGV("startPreview: delete previous PreviewRenderer");
        delete mTarget;
        mTarget = NULL;
    }

    // Create Audio player to be used for entire
    // storyboard duration
    mVEAudioSink = new VideoEditorPlayer::VeAudioOutput();
    mVEAudioPlayer = new VideoEditorAudioPlayer(mVEAudioSink);
    mVEAudioPlayer->setAudioMixSettings(mBackgroundAudioSetting);
    mVEAudioPlayer->setAudioMixPCMFileHandle(mAudioMixPCMFileHandle);

    LOGV("startPreview: loop = %d", loop);
    mPreviewLooping = loop;

    LOGV("startPreview: callBackAfterFrameCount = %d", callBackAfterFrameCount);
    mCallBackAfterFrameCnt = callBackAfterFrameCount;

    for (int playerInst=0; playerInst<NBPLAYER_INSTANCES; playerInst++) {
        mVePlayer[playerInst] = new VideoEditorPlayer();
        if(mVePlayer[playerInst] == NULL) {
            LOGE("startPreview:Error creating VideoEditorPlayer %d",playerInst);
            return M4ERR_ALLOC;
        }
        LOGV("startPreview: object created");

        mVePlayer[playerInst]->setNotifyCallback(this,(notify_callback_f)notify);
        LOGV("startPreview: notify callback set");

        mVePlayer[playerInst]->loadEffectsSettings(mEffectsSettings,
         mNumberEffects);
        LOGV("startPreview: effects settings loaded");

        mVePlayer[playerInst]->loadAudioMixSettings(mBackgroundAudioSetting);
        LOGV("startPreview: AudioMixSettings settings loaded");

        mVePlayer[playerInst]->setAudioMixPCMFileHandle(mAudioMixPCMFileHandle);
        LOGV("startPreview: AudioMixPCMFileHandle set");

        mVePlayer[playerInst]->setProgressCallbackInterval(
         mCallBackAfterFrameCnt);
        LOGV("startPreview: setProgressCallBackInterval");
    }

    mPlayerState = VePlayerIdle;
    mPrepareReqest = M4OSA_FALSE;

    if(fromMS == 0) {
        mCurrentClipNumber = -1;
        // Save original value
        mFirstPreviewClipBeginTime = mClipList[0]->uiBeginCutTime;
        mVideoStoryBoardTimeMsUptoFirstPreviewClip = 0;
    }
    else {
        LOGV("startPreview: fromMS=%d", fromMS);
        if(fromMS >= mClipTotalDuration) {
            LOGE("startPreview: fromMS >= mClipTotalDuration");
            return M4ERR_PARAMETER;
        }
        for(i=0;i<mNumberClipsInStoryBoard;i++) {
            if(fromMS < (iIncrementedDuration + (mClipList[i]->uiEndCutTime -
             mClipList[i]->uiBeginCutTime))) {
                // Set to 1 index below,
                // as threadProcess first increments the clip index
                // and then processes clip in thread loop
                mCurrentClipNumber = i-1;
                LOGV("startPreview:mCurrentClipNumber = %d fromMS=%d",i,fromMS);

                // Save original value
                mFirstPreviewClipBeginTime = mClipList[i]->uiBeginCutTime;

                // Set correct begin time to start playback
                if((fromMS+mClipList[i]->uiBeginCutTime) >
                (iIncrementedDuration+mClipList[i]->uiBeginCutTime)) {

                    mClipList[i]->uiBeginCutTime =
                     mClipList[i]->uiBeginCutTime +
                     (fromMS - iIncrementedDuration);
                }
                break;
            }
            else {
                iIncrementedDuration = iIncrementedDuration +
                 (mClipList[i]->uiEndCutTime - mClipList[i]->uiBeginCutTime);
            }
        }
        mVideoStoryBoardTimeMsUptoFirstPreviewClip = iIncrementedDuration;
    }

    for (int playerInst=0; playerInst<NBPLAYER_INSTANCES; playerInst++) {
        mVePlayer[playerInst]->setAudioMixStoryBoardParam(fromMS,
         mFirstPreviewClipBeginTime,
         mClipList[i]->ClipProperties.uiClipAudioVolumePercentage);

        LOGV("startPreview:setAudioMixStoryBoardSkimTimeStamp set %d cuttime \
         %d", fromMS, mFirstPreviewClipBeginTime);
    }

    mStartingClipIndex = mCurrentClipNumber+1;

    // Start playing with player instance 0
    mCurrentPlayer = 0;
    mActivePlayerIndex = 0;

    if(toMs == -1) {
        LOGV("startPreview: Preview till end of storyboard");
        mNumberClipsToPreview = mNumberClipsInStoryBoard;
        // Save original value
        mLastPreviewClipEndTime =
         mClipList[mNumberClipsToPreview-1]->uiEndCutTime;
    }
    else {
        LOGV("startPreview: toMs=%d", toMs);
        if((M4OSA_UInt32)toMs > mClipTotalDuration) {
            LOGE("startPreview: toMs > mClipTotalDuration");
            return M4ERR_PARAMETER;
        }

        iIncrementedDuration = 0;

        for(i=0;i<mNumberClipsInStoryBoard;i++) {
            if((M4OSA_UInt32)toMs <= (iIncrementedDuration +
             (mClipList[i]->uiEndCutTime - mClipList[i]->uiBeginCutTime))) {
                // Save original value
                mLastPreviewClipEndTime = mClipList[i]->uiEndCutTime;
                // Set the end cut time of clip index i to toMs
                mClipList[i]->uiEndCutTime = toMs;

                // Number of clips to be previewed is from index 0 to i
                // increment by 1 as i starts from 0
                mNumberClipsToPreview = i+1;
                break;
            }
            else {
                iIncrementedDuration = iIncrementedDuration +
                 (mClipList[i]->uiEndCutTime - mClipList[i]->uiBeginCutTime);
            }
        }
    }

    // Open the thread semaphore
    M4OSA_semaphoreOpen(&mSemThreadWait, 1);

    // Open the preview process thread
    err = M4OSA_threadSyncOpen(&mThreadContext, (M4OSA_ThreadDoIt)threadProc);
    if (M4NO_ERROR != err) {
        LOGE("VideoEditorPreviewController:M4OSA_threadSyncOpen error %d", (int) err);
        return err;
    }

    // Set the stacksize
    err = M4OSA_threadSyncSetOption(mThreadContext, M4OSA_ThreadStackSize,
     (M4OSA_DataOption)PREVIEW_THREAD_STACK_SIZE);

    if (M4NO_ERROR != err) {
        LOGE("VideoEditorPreviewController: threadSyncSetOption error %d", (int) err);
        M4OSA_threadSyncClose(mThreadContext);
        mThreadContext = NULL;
        return err;
    }

     // Start the thread
     err = M4OSA_threadSyncStart(mThreadContext, (M4OSA_Void*)this);
     if (M4NO_ERROR != err) {
        LOGE("VideoEditorPreviewController: threadSyncStart error %d", (int) err);
        M4OSA_threadSyncClose(mThreadContext);
        mThreadContext = NULL;
        return err;
    }
    bStopThreadInProgress = false;

    LOGV("startPreview: process thread started");
    return M4NO_ERROR;
}

M4OSA_UInt32 VideoEditorPreviewController::stopPreview() {
    M4OSA_ERR err = M4NO_ERROR;
    uint32_t lastRenderedFrameTimeMs = 0;
    LOGV("stopPreview");

    // Stop the thread
    if(mThreadContext != NULL) {
        bStopThreadInProgress = true;
        {
            Mutex::Autolock autoLock(mLockSem);
            if (mSemThreadWait != NULL) {
                err = M4OSA_semaphorePost(mSemThreadWait);
            }
        }

        err = M4OSA_threadSyncStop(mThreadContext);
        if(err != M4NO_ERROR) {
            LOGV("stopPreview: error 0x%x in trying to stop thread", err);
            // Continue even if error
        }

        err = M4OSA_threadSyncClose(mThreadContext);
        if(err != M4NO_ERROR) {
            LOGE("stopPreview: error 0x%x in trying to close thread", (unsigned int)err);
            // Continue even if error
        }

        mThreadContext = NULL;
    }

    // Close the semaphore first
    {
        Mutex::Autolock autoLock(mLockSem);
        if(mSemThreadWait != NULL) {
            err = M4OSA_semaphoreClose(mSemThreadWait);
            LOGV("stopPreview: close semaphore returns 0x%x", err);
            mSemThreadWait = NULL;
        }
    }

    for (int playerInst=0; playerInst<NBPLAYER_INSTANCES; playerInst++) {
        if(mVePlayer[playerInst] != NULL) {
            if(mVePlayer[playerInst]->isPlaying()) {
                LOGV("stop the player first");
                mVePlayer[playerInst]->stop();
            }
            if (playerInst == mActivePlayerIndex) {
                // Return the last rendered frame time stamp
                mVePlayer[mActivePlayerIndex]->getLastRenderedTimeMs(&lastRenderedFrameTimeMs);
            }

            LOGV("stopPreview: clearing mVePlayer");
            mVePlayer[playerInst].clear();
            mVePlayer[playerInst] = NULL;
        }
    }
    LOGV("stopPreview: clear audioSink and audioPlayer");
    mVEAudioSink.clear();
    if (mVEAudioPlayer) {
        delete mVEAudioPlayer;
        mVEAudioPlayer = NULL;
    }

    // If image file playing, then free the buffer pointer
    if(mFrameStr.pBuffer != M4OSA_NULL) {
        M4OSA_free((M4OSA_MemAddr32)mFrameStr.pBuffer);
        mFrameStr.pBuffer = M4OSA_NULL;
    }

    // Reset original begin cuttime of first previewed clip*/
    mClipList[mStartingClipIndex]->uiBeginCutTime = mFirstPreviewClipBeginTime;
    // Reset original end cuttime of last previewed clip*/
    mClipList[mNumberClipsToPreview-1]->uiEndCutTime = mLastPreviewClipEndTime;

    mPlayerState = VePlayerIdle;
    mPrepareReqest = M4OSA_FALSE;

    mCurrentPlayedDuration = 0;
    mCurrentClipDuration = 0;
    mRenderingMode = M4xVSS_kBlackBorders;
    mOutputVideoWidth = 0;
    mOutputVideoHeight = 0;

    LOGV("stopPreview() lastRenderedFrameTimeMs %ld", lastRenderedFrameTimeMs);
    return lastRenderedFrameTimeMs;
}

M4OSA_ERR VideoEditorPreviewController::clearSurface(
    const sp<Surface> &surface, VideoEditor_renderPreviewFrameStr* pFrameInfo) {

    M4OSA_ERR err = M4NO_ERROR;
    VideoEditor_renderPreviewFrameStr* pFrameStr = pFrameInfo;
    M4OSA_UInt32 outputBufferWidth =0, outputBufferHeight=0;
    M4VIFI_ImagePlane planeOut[3];
    LOGV("Inside preview clear frame");

    Mutex::Autolock autoLock(mLock);

    // Get the Isurface to be passed to renderer
    mISurface = surface->getISurface();

    // Delete previous renderer instance
    if(mTarget != NULL) {
        delete mTarget;
        mTarget = NULL;
    }

    outputBufferWidth = pFrameStr->uiFrameWidth;
    outputBufferHeight = pFrameStr->uiFrameHeight;

    // Initialize the renderer
    if(mTarget == NULL) {

        mTarget = PreviewRenderer::CreatePreviewRenderer(
            OMX_COLOR_FormatYUV420Planar, surface, outputBufferWidth, outputBufferHeight,
            outputBufferWidth, outputBufferHeight, 0);

        if(mTarget == NULL) {
            LOGE("renderPreviewFrame: cannot create PreviewRenderer");
            return M4ERR_ALLOC;
        }
    }

    // Out plane
    uint8_t* outBuffer;
    size_t outBufferStride = 0;

    LOGV("doMediaRendering CALL getBuffer()");
    mTarget->getBufferYV12(&outBuffer, &outBufferStride);

    // Set the output YUV420 plane to be compatible with YV12 format
    //In YV12 format, sizes must be even
    M4OSA_UInt32 yv12PlaneWidth = ((outputBufferWidth +1)>>1)<<1;
    M4OSA_UInt32 yv12PlaneHeight = ((outputBufferHeight+1)>>1)<<1;

    prepareYV12ImagePlane(planeOut, yv12PlaneWidth, yv12PlaneHeight,
     (M4OSA_UInt32)outBufferStride, (M4VIFI_UInt8 *)outBuffer);

    /* Fill the surface with black frame */
    M4OSA_memset((M4OSA_MemAddr8)planeOut[0].pac_data,planeOut[0].u_width *
                            planeOut[0].u_height * 1.5,0x00);
    M4OSA_memset((M4OSA_MemAddr8)planeOut[1].pac_data,planeOut[1].u_width *
                            planeOut[1].u_height,128);
    M4OSA_memset((M4OSA_MemAddr8)planeOut[2].pac_data,planeOut[2].u_width *
                             planeOut[2].u_height,128);

    mTarget->renderYV12();
    return err;
}

M4OSA_ERR VideoEditorPreviewController::renderPreviewFrame(
            const sp<Surface> &surface,
            VideoEditor_renderPreviewFrameStr* pFrameInfo,
            VideoEditorCurretEditInfo *pCurrEditInfo) {

    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_UInt32 i = 0, iIncrementedDuration = 0, tnTimeMs=0, framesize =0;
    VideoEditor_renderPreviewFrameStr* pFrameStr = pFrameInfo;
    M4VIFI_UInt8 *pixelArray = NULL;
    Mutex::Autolock autoLock(mLock);

    // Get the Isurface to be passed to renderer
    mISurface = surface->getISurface();
    if (pCurrEditInfo != NULL) {
        pCurrEditInfo->overlaySettingsIndex = -1;
    }
    // Delete previous renderer instance
    if(mTarget != NULL) {
        delete mTarget;
        mTarget = NULL;
    }

    if(mOutputVideoWidth == 0) {
        mOutputVideoWidth = pFrameStr->uiFrameWidth;
    }

    if(mOutputVideoHeight == 0) {
        mOutputVideoHeight = pFrameStr->uiFrameHeight;
    }

    // Initialize the renderer
    if(mTarget == NULL) {
        /*mTarget = new PreviewRenderer(
            OMX_COLOR_FormatYUV420Planar, surface, mOutputVideoWidth, mOutputVideoHeight,
            mOutputVideoWidth, mOutputVideoHeight, 0);*/

         mTarget = PreviewRenderer::CreatePreviewRenderer(
            OMX_COLOR_FormatYUV420Planar, surface, mOutputVideoWidth, mOutputVideoHeight,
            mOutputVideoWidth, mOutputVideoHeight, 0);

        if(mTarget == NULL) {
            LOGE("renderPreviewFrame: cannot create PreviewRenderer");
            return M4ERR_ALLOC;
        }
    }

    pixelArray = NULL;

    // Postprocessing (apply video effect)
    if(pFrameStr->bApplyEffect == M4OSA_TRUE) {

        for(i=0;i<mNumberEffects;i++) {
            // First check if effect starttime matches the clip being previewed
            if((mEffectsSettings[i].uiStartTime < pFrameStr->clipBeginCutTime)
             ||(mEffectsSettings[i].uiStartTime >= pFrameStr->clipEndCutTime)) {
                // This effect doesn't belong to this clip, check next one
                continue;
            }
            if((mEffectsSettings[i].uiStartTime <= pFrameStr->timeMs) &&
            ((mEffectsSettings[i].uiStartTime+mEffectsSettings[i].uiDuration) >=
             pFrameStr->timeMs) && (mEffectsSettings[i].uiDuration != 0)) {
                setVideoEffectType(mEffectsSettings[i].VideoEffectType, TRUE);
            }
            else {
                setVideoEffectType(mEffectsSettings[i].VideoEffectType, FALSE);
            }
        }

        //Provide the overlay Update indication when there is an overlay effect
        if (mCurrentVideoEffect & VIDEO_EFFECT_FRAMING) {
            M4OSA_UInt32 index;
            mCurrentVideoEffect &= ~VIDEO_EFFECT_FRAMING; //never apply framing here.

            // Find the effect in effectSettings array
            for (index = 0; index < mNumberEffects; index++) {
                if(mEffectsSettings[index].VideoEffectType ==
                    (M4VSS3GPP_VideoEffectType)M4xVSS_kVideoEffectType_Framing) {

                    if((mEffectsSettings[index].uiStartTime <= pFrameInfo->timeMs) &&
                        ((mEffectsSettings[index].uiStartTime+
                        mEffectsSettings[index].uiDuration) >= pFrameInfo->timeMs))
                    {
                        break;
                    }
                }
            }
            if ((index < mNumberEffects) && (pCurrEditInfo != NULL)) {
                pCurrEditInfo->overlaySettingsIndex = index;
                LOGV("Framing index = %d", index);
            } else {
                LOGV("No framing effects found");
            }
        }

        if(mCurrentVideoEffect != VIDEO_EFFECT_NONE) {
            err = applyVideoEffect((M4OSA_Void *)pFrameStr->pBuffer,
             OMX_COLOR_FormatYUV420Planar, pFrameStr->uiFrameWidth,
             pFrameStr->uiFrameHeight, pFrameStr->timeMs,
             (M4OSA_Void *)pixelArray);

            if(err != M4NO_ERROR) {
                LOGE("renderPreviewFrame: applyVideoEffect error 0x%x", (unsigned int)err);
                delete mTarget;
                mTarget = NULL;
                M4OSA_free((M4OSA_MemAddr32)pixelArray);
                pixelArray = NULL;
                return err;
           }
           mCurrentVideoEffect = VIDEO_EFFECT_NONE;
        }
        else {
            // Apply the rendering mode
            err = doImageRenderingMode((M4OSA_Void *)pFrameStr->pBuffer,
             OMX_COLOR_FormatYUV420Planar, pFrameStr->uiFrameWidth,
             pFrameStr->uiFrameHeight, (M4OSA_Void *)pixelArray);

            if(err != M4NO_ERROR) {
                LOGE("renderPreviewFrame:doImageRenderingMode error 0x%x", (unsigned int)err);
                delete mTarget;
                mTarget = NULL;
                M4OSA_free((M4OSA_MemAddr32)pixelArray);
                pixelArray = NULL;
                return err;
            }
        }
    }
    else {
        // Apply the rendering mode
        err = doImageRenderingMode((M4OSA_Void *)pFrameStr->pBuffer,
         OMX_COLOR_FormatYUV420Planar, pFrameStr->uiFrameWidth,
         pFrameStr->uiFrameHeight, (M4OSA_Void *)pixelArray);

        if(err != M4NO_ERROR) {
            LOGE("renderPreviewFrame: doImageRenderingMode error 0x%x", (unsigned int)err);
            delete mTarget;
            mTarget = NULL;
            M4OSA_free((M4OSA_MemAddr32)pixelArray);
            pixelArray = NULL;
            return err;
        }
    }

    mTarget->renderYV12();
    return err;
}

M4OSA_Void VideoEditorPreviewController::setJniCallback(void* cookie,
    jni_progress_callback_fct callbackFct) {
    //LOGV("setJniCallback");
    mJniCookie = cookie;
    mJniCallback = callbackFct;
}

M4OSA_ERR VideoEditorPreviewController::preparePlayer(
    void* param, int playerInstance, int index) {

    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorPreviewController *pController =
     (VideoEditorPreviewController *)param;

    LOGV("preparePlayer: instance %d file %d", playerInstance, index);

    pController->mVePlayer[playerInstance]->setDataSource(
    (const char *)pController->mClipList[index]->pFile, NULL);
    LOGV("preparePlayer: setDataSource instance %s",
     (const char *)pController->mClipList[index]->pFile);

    pController->mVePlayer[playerInstance]->setVideoISurface(
     pController->mISurface);
    LOGV("preparePlayer: setVideoISurface");

    pController->mVePlayer[playerInstance]->setVideoSurface(
     pController->mSurface);
    LOGV("preparePlayer: setVideoSurface");

    pController->mVePlayer[playerInstance]->setMediaRenderingMode(
     pController->mClipList[index]->xVSS.MediaRendering,
     pController->mOutputVideoSize);
    LOGV("preparePlayer: setMediaRenderingMode");

    if((M4OSA_UInt32)index == pController->mStartingClipIndex) {
        pController->mVePlayer[playerInstance]->setPlaybackBeginTime(
        pController->mFirstPreviewClipBeginTime);
    }
    else {
        pController->mVePlayer[playerInstance]->setPlaybackBeginTime(
        pController->mClipList[index]->uiBeginCutTime);
    }
    LOGV("preparePlayer: setPlaybackBeginTime(%d)",
     pController->mClipList[index]->uiBeginCutTime);

    pController->mVePlayer[playerInstance]->setPlaybackEndTime(
     pController->mClipList[index]->uiEndCutTime);
    LOGV("preparePlayer: setPlaybackEndTime(%d)",
     pController->mClipList[index]->uiEndCutTime);

    if(pController->mClipList[index]->FileType == M4VIDEOEDITING_kFileType_ARGB8888) {
        pController->mVePlayer[playerInstance]->setImageClipProperties(
                 pController->mClipList[index]->ClipProperties.uiVideoWidth,
                 pController->mClipList[index]->ClipProperties.uiVideoHeight);
        LOGV("preparePlayer: setImageClipProperties");
    }

    pController->mVePlayer[playerInstance]->prepare();
    LOGV("preparePlayer: prepared");

    if(pController->mClipList[index]->uiBeginCutTime > 0) {
        pController->mVePlayer[playerInstance]->seekTo(
         pController->mClipList[index]->uiBeginCutTime);

        LOGV("preparePlayer: seekTo(%d)",
         pController->mClipList[index]->uiBeginCutTime);
    }
    pController->mVePlayer[pController->mCurrentPlayer]->setAudioPlayer(pController->mVEAudioPlayer);

    pController->mVePlayer[playerInstance]->readFirstVideoFrame();
    LOGV("preparePlayer: readFirstVideoFrame of clip");

    return err;
}

M4OSA_ERR VideoEditorPreviewController::threadProc(M4OSA_Void* param) {
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_Int32 index = 0;
    VideoEditorPreviewController *pController =
     (VideoEditorPreviewController *)param;

    LOGV("inside threadProc");
    if(pController->mPlayerState == VePlayerIdle) {
        (pController->mCurrentClipNumber)++;

        LOGV("threadProc: playing file index %d total clips %d",
         pController->mCurrentClipNumber, pController->mNumberClipsToPreview);

        if((M4OSA_UInt32)pController->mCurrentClipNumber >=
         pController->mNumberClipsToPreview) {

            LOGV("All clips previewed");

            pController->mCurrentPlayedDuration = 0;
            pController->mCurrentClipDuration = 0;
            pController->mCurrentPlayer = 0;

            if(pController->mPreviewLooping == M4OSA_TRUE) {
                pController->mCurrentClipNumber =
                 pController->mStartingClipIndex;

                LOGV("Preview looping TRUE, restarting from clip index %d",
                 pController->mCurrentClipNumber);

                // Reset the story board timestamp inside the player
                for (int playerInst=0; playerInst<NBPLAYER_INSTANCES;
                 playerInst++) {
                    pController->mVePlayer[playerInst]->resetJniCallbackTimeStamp();
                }
            }
            else {
                M4OSA_UInt32 endArgs = 0;
                if(pController->mJniCallback != NULL) {
                    pController->mJniCallback(
                     pController->mJniCookie, MSG_TYPE_PREVIEW_END, &endArgs);
                }
                pController->mPlayerState = VePlayerAutoStop;

                // Reset original begin cuttime of first previewed clip
                pController->mClipList[pController->mStartingClipIndex]->uiBeginCutTime =
                 pController->mFirstPreviewClipBeginTime;
                // Reset original end cuttime of last previewed clip
                pController->mClipList[pController->mNumberClipsToPreview-1]->uiEndCutTime =
                 pController->mLastPreviewClipEndTime;

                // Return a warning to M4OSA thread handler
                // so that thread is moved from executing state to open state
                return M4WAR_NO_MORE_STREAM;
            }
        }

        index=pController->mCurrentClipNumber;
        if((M4OSA_UInt32)pController->mCurrentClipNumber == pController->mStartingClipIndex) {
            pController->mCurrentPlayedDuration +=
             pController->mVideoStoryBoardTimeMsUptoFirstPreviewClip;

            pController->mCurrentClipDuration =
             pController->mClipList[pController->mCurrentClipNumber]->uiEndCutTime
              - pController->mFirstPreviewClipBeginTime;

            preparePlayer((void*)pController, pController->mCurrentPlayer, index);
        }
        else {
            pController->mCurrentPlayedDuration +=
             pController->mCurrentClipDuration;

            pController->mCurrentClipDuration =
             pController->mClipList[pController->mCurrentClipNumber]->uiEndCutTime -
             pController->mClipList[pController->mCurrentClipNumber]->uiBeginCutTime;
        }

        pController->mVePlayer[pController->mCurrentPlayer]->setStoryboardStartTime(
         pController->mCurrentPlayedDuration);
        LOGV("threadProc: setStoryboardStartTime");

        // Set the next clip duration for Audio mix here
        if((M4OSA_UInt32)pController->mCurrentClipNumber != pController->mStartingClipIndex) {

            pController->mVePlayer[pController->mCurrentPlayer]->setAudioMixStoryBoardParam(
             pController->mCurrentPlayedDuration,
             pController->mClipList[index]->uiBeginCutTime,
             pController->mClipList[index]->ClipProperties.uiClipAudioVolumePercentage);

            LOGV("threadProc: setAudioMixStoryBoardParam fromMS %d \
             ClipBeginTime %d", pController->mCurrentPlayedDuration +
             pController->mClipList[index]->uiBeginCutTime,
             pController->mClipList[index]->uiBeginCutTime,
             pController->mClipList[index]->ClipProperties.uiClipAudioVolumePercentage);
        }
        // Capture the active player being used
        pController->mActivePlayerIndex = pController->mCurrentPlayer;

        pController->mVePlayer[pController->mCurrentPlayer]->start();
        LOGV("threadProc: started");

        pController->mPlayerState = VePlayerBusy;

    } else if(pController->mPlayerState == VePlayerAutoStop) {
        LOGV("Preview completed..auto stop the player");
    } else if ((pController->mPlayerState == VePlayerBusy) && (pController->mPrepareReqest)) {
        // Prepare the player here
        pController->mPrepareReqest = M4OSA_FALSE;
        preparePlayer((void*)pController, pController->mCurrentPlayer,
            pController->mCurrentClipNumber+1);
        if (pController->mSemThreadWait != NULL) {
            err = M4OSA_semaphoreWait(pController->mSemThreadWait,
                M4OSA_WAIT_FOREVER);
        }
    } else {
        if (!pController->bStopThreadInProgress) {
            LOGV("threadProc: state busy...wait for sem");
            if (pController->mSemThreadWait != NULL) {
                err = M4OSA_semaphoreWait(pController->mSemThreadWait,
                 M4OSA_WAIT_FOREVER);
             }
        }
        LOGV("threadProc: sem wait returned err = 0x%x", err);
    }

    //Always return M4NO_ERROR to ensure the thread keeps running
    return M4NO_ERROR;
}

void VideoEditorPreviewController::notify(
    void* cookie, int msg, int ext1, int ext2)
{
    VideoEditorPreviewController *pController =
     (VideoEditorPreviewController *)cookie;

    M4OSA_ERR err = M4NO_ERROR;
    uint32_t clipDuration = 0;
    switch (msg) {
        case MEDIA_NOP: // interface test message
            LOGV("MEDIA_NOP");
            break;
        case MEDIA_PREPARED:
            LOGV("MEDIA_PREPARED");
            break;
        case MEDIA_PLAYBACK_COMPLETE:
        {
            LOGV("notify:MEDIA_PLAYBACK_COMPLETE");
            pController->mPlayerState = VePlayerIdle;

            //send progress callback with last frame timestamp
            if((M4OSA_UInt32)pController->mCurrentClipNumber ==
             pController->mStartingClipIndex) {
                clipDuration =
                 pController->mClipList[pController->mCurrentClipNumber]->uiEndCutTime
                  - pController->mFirstPreviewClipBeginTime;
            }
            else {
                clipDuration =
                 pController->mClipList[pController->mCurrentClipNumber]->uiEndCutTime
                  - pController->mClipList[pController->mCurrentClipNumber]->uiBeginCutTime;
            }

            M4OSA_UInt32 playedDuration = clipDuration+pController->mCurrentPlayedDuration;
            pController->mJniCallback(
                 pController->mJniCookie, MSG_TYPE_PROGRESS_INDICATION,
                 &playedDuration);

            if ((pController->mOverlayState == OVERLAY_UPDATE) &&
                ((M4OSA_UInt32)pController->mCurrentClipNumber !=
                (pController->mNumberClipsToPreview-1))) {
                VideoEditorCurretEditInfo *pEditInfo =
                    (VideoEditorCurretEditInfo*)M4OSA_malloc(sizeof(VideoEditorCurretEditInfo),
                    M4VS, (M4OSA_Char*)"Current Edit info");
                pEditInfo->overlaySettingsIndex = ext2;
                pEditInfo->clipIndex = pController->mCurrentClipNumber;
                pController->mOverlayState == OVERLAY_CLEAR;
                if (pController->mJniCallback != NULL) {
                        pController->mJniCallback(pController->mJniCookie,
                            MSG_TYPE_OVERLAY_CLEAR, pEditInfo);
                }
                M4OSA_free((M4OSA_MemAddr32)pEditInfo);
            }
            {
                Mutex::Autolock autoLock(pController->mLockSem);
                if (pController->mSemThreadWait != NULL) {
                    M4OSA_semaphorePost(pController->mSemThreadWait);
                }
            }

            break;
        }
        case MEDIA_ERROR:
        {
            int err_val = ext1;
          // Always log errors.
          // ext1: Media framework error code.
          // ext2: Implementation dependant error code.
            LOGE("MEDIA_ERROR; error (%d, %d)", ext1, ext2);
            if(pController->mJniCallback != NULL) {
                pController->mJniCallback(pController->mJniCookie,
                 MSG_TYPE_PLAYER_ERROR, &err_val);
            }
            break;
        }
        case MEDIA_INFO:
        {
            int info_val = ext2;
            // ext1: Media framework error code.
            // ext2: Implementation dependant error code.
            //LOGW("MEDIA_INFO; info/warning (%d, %d)", ext1, ext2);
            if(pController->mJniCallback != NULL) {
                pController->mJniCallback(pController->mJniCookie,
                 MSG_TYPE_PROGRESS_INDICATION, &info_val);
            }
            break;
        }
        case MEDIA_SEEK_COMPLETE:
            LOGV("MEDIA_SEEK_COMPLETE; Received seek complete");
            break;
        case MEDIA_BUFFERING_UPDATE:
            LOGV("MEDIA_BUFFERING_UPDATE; buffering %d", ext1);
            break;
        case MEDIA_SET_VIDEO_SIZE:
            LOGV("MEDIA_SET_VIDEO_SIZE; New video size %d x %d", ext1, ext2);
            break;
        case 0xAAAAAAAA:
            LOGV("VIDEO PLAYBACK ALMOST over, prepare next player");
            // Select next player and prepare it
            // If there is a clip after this one
            if ((M4OSA_UInt32)(pController->mCurrentClipNumber+1) <
             pController->mNumberClipsToPreview) {
                pController->mPrepareReqest = M4OSA_TRUE;
                pController->mCurrentPlayer++;
                if (pController->mCurrentPlayer >= NBPLAYER_INSTANCES) {
                    pController->mCurrentPlayer = 0;
                }
                // Prepare the first clip to be played
                {
                    Mutex::Autolock autoLock(pController->mLockSem);
                    if (pController->mSemThreadWait != NULL) {
                        M4OSA_semaphorePost(pController->mSemThreadWait);
                    }
                }
            }
            break;
        case 0xBBBBBBBB:
        {
            LOGV("VIDEO PLAYBACK, Update Overlay");
            int overlayIndex = ext2;
            VideoEditorCurretEditInfo *pEditInfo =
                    (VideoEditorCurretEditInfo*)M4OSA_malloc(sizeof(VideoEditorCurretEditInfo),
                    M4VS, (M4OSA_Char*)"Current Edit info");
            //ext1 = 1; start the overlay display
            //     = 2; Clear the overlay.
            pEditInfo->overlaySettingsIndex = ext2;
            pEditInfo->clipIndex = pController->mCurrentClipNumber;
            LOGV("pController->mCurrentClipNumber = %d",pController->mCurrentClipNumber);
            if (pController->mJniCallback != NULL) {
                if (ext1 == 1) {
                    pController->mOverlayState = OVERLAY_UPDATE;
                    pController->mJniCallback(pController->mJniCookie,
                        MSG_TYPE_OVERLAY_UPDATE, pEditInfo);
                } else {
                    pController->mOverlayState = OVERLAY_CLEAR;
                    pController->mJniCallback(pController->mJniCookie,
                        MSG_TYPE_OVERLAY_CLEAR, pEditInfo);
                }
            }
            M4OSA_free((M4OSA_MemAddr32)pEditInfo);
            break;
        }
        default:
            LOGV("unrecognized message: (%d, %d, %d)", msg, ext1, ext2);
            break;
    }
}

void VideoEditorPreviewController::setVideoEffectType(
    M4VSS3GPP_VideoEffectType type, M4OSA_Bool enable) {

    M4OSA_UInt32 effect = VIDEO_EFFECT_NONE;

    // map M4VSS3GPP_VideoEffectType to local enum
    switch(type) {
        case M4VSS3GPP_kVideoEffectType_FadeFromBlack:
            effect = VIDEO_EFFECT_FADEFROMBLACK;
            break;

        case M4VSS3GPP_kVideoEffectType_FadeToBlack:
            effect = VIDEO_EFFECT_FADETOBLACK;
            break;

        case M4VSS3GPP_kVideoEffectType_CurtainOpening:
            effect = VIDEO_EFFECT_CURTAINOPEN;
            break;

        case M4VSS3GPP_kVideoEffectType_CurtainClosing:
            effect = VIDEO_EFFECT_CURTAINCLOSE;
            break;

        case M4xVSS_kVideoEffectType_BlackAndWhite:
            effect = VIDEO_EFFECT_BLACKANDWHITE;
            break;

        case M4xVSS_kVideoEffectType_Pink:
            effect = VIDEO_EFFECT_PINK;
            break;

        case M4xVSS_kVideoEffectType_Green:
            effect = VIDEO_EFFECT_GREEN;
            break;

        case M4xVSS_kVideoEffectType_Sepia:
            effect = VIDEO_EFFECT_SEPIA;
            break;

        case M4xVSS_kVideoEffectType_Negative:
            effect = VIDEO_EFFECT_NEGATIVE;
            break;

        case M4xVSS_kVideoEffectType_Framing:
            effect = VIDEO_EFFECT_FRAMING;
            break;

        case M4xVSS_kVideoEffectType_Fifties:
            effect = VIDEO_EFFECT_FIFTIES;
            break;

        case M4xVSS_kVideoEffectType_ColorRGB16:
            effect = VIDEO_EFFECT_COLOR_RGB16;
            break;

        case M4xVSS_kVideoEffectType_Gradient:
            effect = VIDEO_EFFECT_GRADIENT;
            break;

        default:
            effect = VIDEO_EFFECT_NONE;
            break;
    }

    if(enable == M4OSA_TRUE) {
        // If already set, then no need to set again
        if(!(mCurrentVideoEffect & effect))
            mCurrentVideoEffect |= effect;
            if(effect == VIDEO_EFFECT_FIFTIES) {
                mIsFiftiesEffectStarted = true;
            }
    }
    else  {
        // Reset only if already set
        if(mCurrentVideoEffect & effect)
            mCurrentVideoEffect &= ~effect;
    }

    return;
}


M4OSA_ERR VideoEditorPreviewController::applyVideoEffect(
    M4OSA_Void * dataPtr, M4OSA_UInt32 colorFormat, M4OSA_UInt32 videoWidth,
    M4OSA_UInt32 videoHeight, M4OSA_UInt32 timeMs, M4OSA_Void* outPtr) {

    M4OSA_ERR err = M4NO_ERROR;
    vePostProcessParams postProcessParams;

    postProcessParams.vidBuffer = (M4VIFI_UInt8*)dataPtr;
    postProcessParams.videoWidth = videoWidth;
    postProcessParams.videoHeight = videoHeight;
    postProcessParams.timeMs = timeMs;
    postProcessParams.timeOffset = 0; //Since timeMS already takes care of offset in this case
    postProcessParams.effectsSettings = mEffectsSettings;
    postProcessParams.numberEffects = mNumberEffects;
    postProcessParams.outVideoWidth = mOutputVideoWidth;
    postProcessParams.outVideoHeight = mOutputVideoHeight;
    postProcessParams.currentVideoEffect = mCurrentVideoEffect;
    postProcessParams.renderingMode = mRenderingMode;
    if(mIsFiftiesEffectStarted == M4OSA_TRUE) {
        postProcessParams.isFiftiesEffectStarted = M4OSA_TRUE;
        mIsFiftiesEffectStarted = M4OSA_FALSE;
    }
    else {
       postProcessParams.isFiftiesEffectStarted = M4OSA_FALSE;
    }
    //postProcessParams.renderer = mTarget;
    postProcessParams.overlayFrameRGBBuffer = NULL;
    postProcessParams.overlayFrameYUVBuffer = NULL;

    mTarget->getBufferYV12(&(postProcessParams.pOutBuffer), &(postProcessParams.outBufferStride));

    err = applyEffectsAndRenderingMode(&postProcessParams, videoWidth, videoHeight);
    return err;
}

M4OSA_ERR VideoEditorPreviewController::setPreviewFrameRenderingMode(
    M4xVSS_MediaRendering mode, M4VIDEOEDITING_VideoFrameSize outputVideoSize) {

    LOGV("setMediaRenderingMode: outputVideoSize = %d", outputVideoSize);
    mRenderingMode = mode;

    switch(outputVideoSize) {
        case M4VIDEOEDITING_kSQCIF:
            mOutputVideoWidth = 128;
            mOutputVideoHeight = 96;
            break;

        case M4VIDEOEDITING_kQQVGA:
            mOutputVideoWidth = 160;
            mOutputVideoHeight = 120;
            break;

        case M4VIDEOEDITING_kQCIF:
            mOutputVideoWidth = 176;
            mOutputVideoHeight = 144;
            break;

        case M4VIDEOEDITING_kQVGA:
            mOutputVideoWidth = 320;
            mOutputVideoHeight = 240;
            break;

        case M4VIDEOEDITING_kCIF:
            mOutputVideoWidth = 352;
            mOutputVideoHeight = 288;
            break;

        case M4VIDEOEDITING_kVGA:
            mOutputVideoWidth = 640;
            mOutputVideoHeight = 480;
            break;

        case M4VIDEOEDITING_kWVGA:
            mOutputVideoWidth = 800;
            mOutputVideoHeight = 480;
            break;

        case M4VIDEOEDITING_kNTSC:
            mOutputVideoWidth = 720;
            mOutputVideoHeight = 480;
            break;

        case M4VIDEOEDITING_k640_360:
            mOutputVideoWidth = 640;
            mOutputVideoHeight = 360;
            break;

        case M4VIDEOEDITING_k854_480:
            mOutputVideoWidth = 854;
            mOutputVideoHeight = 480;
            break;

        case M4VIDEOEDITING_kHD1280:
            mOutputVideoWidth = 1280;
            mOutputVideoHeight = 720;
            break;

        case M4VIDEOEDITING_kHD1080:
            mOutputVideoWidth = 1080;
            mOutputVideoHeight = 720;
            break;

        case M4VIDEOEDITING_kHD960:
            mOutputVideoWidth = 960;
            mOutputVideoHeight = 720;
            break;

        default:
            mOutputVideoWidth = 0;
            mOutputVideoHeight = 0;
            break;
    }

    return OK;
}

M4OSA_ERR VideoEditorPreviewController::doImageRenderingMode(
    M4OSA_Void * dataPtr, M4OSA_UInt32 colorFormat, M4OSA_UInt32 videoWidth,
    M4OSA_UInt32 videoHeight, M4OSA_Void* outPtr) {

    M4OSA_ERR err = M4NO_ERROR;
    M4VIFI_ImagePlane planeIn[3], planeOut[3];
    M4VIFI_UInt8 *inBuffer = M4OSA_NULL;
    M4OSA_UInt32 outputBufferWidth =0, outputBufferHeight=0;

    //frameSize = (videoWidth*videoHeight*3) >> 1;
    inBuffer = (M4OSA_UInt8 *)dataPtr;

    // In plane
    prepareYUV420ImagePlane(planeIn, videoWidth,
      videoHeight, (M4VIFI_UInt8 *)inBuffer, videoWidth, videoHeight);

    outputBufferWidth = mOutputVideoWidth;
    outputBufferHeight = mOutputVideoHeight;

    // Out plane
    uint8_t* outBuffer;
    size_t outBufferStride = 0;

    LOGV("doMediaRendering CALL getBuffer()");
    mTarget->getBufferYV12(&outBuffer, &outBufferStride);

    // Set the output YUV420 plane to be compatible with YV12 format
    //In YV12 format, sizes must be even
    M4OSA_UInt32 yv12PlaneWidth = ((mOutputVideoWidth +1)>>1)<<1;
    M4OSA_UInt32 yv12PlaneHeight = ((mOutputVideoHeight+1)>>1)<<1;

    prepareYV12ImagePlane(planeOut, yv12PlaneWidth, yv12PlaneHeight,
     (M4OSA_UInt32)outBufferStride, (M4VIFI_UInt8 *)outBuffer);

    err = applyRenderingMode(planeIn, planeOut, mRenderingMode);
    if(err != M4NO_ERROR) {
        LOGE("doImageRenderingMode: applyRenderingMode returned err=0x%x", (unsigned int)err);
    }
    return err;
}

} //namespace android
