/*
 * Copyright (C) 2010 The Android Open Source Project
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

#ifndef A_CODEC_H_

#define A_CODEC_H_

#include <stdint.h>
#include <android/native_window.h>
#include <media/hardware/MetadataBufferType.h>
#include <media/IOMX.h>
#include <media/stagefright/foundation/AHierarchicalStateMachine.h>
#include <media/stagefright/CodecBase.h>
#include <media/stagefright/FrameRenderTracker.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/SkipCutBuffer.h>
#include <utils/NativeHandle.h>
#include <OMX_Audio.h>

#include <system/audio.h>

#define TRACK_BUFFER_TIMING     0

namespace android {

struct ABuffer;
struct MemoryDealer;
struct DescribeColorFormat2Params;
struct DataConverter;

struct ACodec : public AHierarchicalStateMachine, public CodecBase {
    ACodec();

    virtual void setNotificationMessage(const sp<AMessage> &msg);

    void initiateSetup(const sp<AMessage> &msg);

    virtual void initiateAllocateComponent(const sp<AMessage> &msg);
    virtual void initiateConfigureComponent(const sp<AMessage> &msg);
    virtual void initiateCreateInputSurface();
    virtual void initiateSetInputSurface(const sp<PersistentSurface> &surface);
    virtual void initiateStart();
    virtual void initiateShutdown(bool keepComponentAllocated = false);

    virtual status_t queryCapabilities(
            const AString &name, const AString &mime, bool isEncoder,
            sp<MediaCodecInfo::Capabilities> *caps);

    virtual status_t setSurface(const sp<Surface> &surface);

    virtual void signalFlush();
    virtual void signalResume();

    virtual void signalSetParameters(const sp<AMessage> &msg);
    virtual void signalEndOfInputStream();
    virtual void signalRequestIDRFrame();

    // AHierarchicalStateMachine implements the message handling
    virtual void onMessageReceived(const sp<AMessage> &msg) {
        handleMessage(msg);
    }

    struct PortDescription : public CodecBase::PortDescription {
        size_t countBuffers();
        IOMX::buffer_id bufferIDAt(size_t index) const;
        sp<ABuffer> bufferAt(size_t index) const;
        sp<NativeHandle> handleAt(size_t index) const;
        sp<RefBase> memRefAt(size_t index) const;

    private:
        friend struct ACodec;

        Vector<IOMX::buffer_id> mBufferIDs;
        Vector<sp<ABuffer> > mBuffers;
        Vector<sp<NativeHandle> > mHandles;
        Vector<sp<RefBase> > mMemRefs;

        PortDescription();
        void addBuffer(
                IOMX::buffer_id id, const sp<ABuffer> &buffer,
                const sp<NativeHandle> &handle, const sp<RefBase> &memRef);

        DISALLOW_EVIL_CONSTRUCTORS(PortDescription);
    };

    static bool isFlexibleColorFormat(
            const sp<IOMX> &omx, IOMX::node_id node,
            uint32_t colorFormat, bool usingNativeBuffers, OMX_U32 *flexibleEquivalent);

    // Returns 0 if configuration is not supported.  NOTE: this is treated by
    // some OMX components as auto level, and by others as invalid level.
    static int /* OMX_VIDEO_AVCLEVELTYPE */ getAVCLevelFor(
            int width, int height, int rate, int bitrate,
            OMX_VIDEO_AVCPROFILETYPE profile = OMX_VIDEO_AVCProfileBaseline);

    // Quirk still supported, even though deprecated
    enum Quirks {
        kRequiresAllocateBufferOnInputPorts   = 1,
        kRequiresAllocateBufferOnOutputPorts  = 2,
    };

    static status_t getOMXChannelMapping(size_t numChannels, OMX_AUDIO_CHANNELTYPE map[]);

protected:
    virtual ~ACodec();
    virtual status_t setupCustomCodec(
            status_t err, const char *mime, const sp<AMessage> &msg);
    virtual status_t GetVideoCodingTypeFromMime(
            const char *mime, OMX_VIDEO_CODINGTYPE *codingType);

    struct BaseState;
    struct UninitializedState;
    struct LoadedState;
    struct LoadedToIdleState;
    struct IdleToExecutingState;
    struct ExecutingState;
    struct OutputPortSettingsChangedState;
    struct ExecutingToIdleState;
    struct IdleToLoadedState;
    struct FlushingState;
    struct DeathNotifier;

    enum {
        kWhatSetup                   = 'setu',
        kWhatOMXMessage              = 'omx ',
        // same as kWhatOMXMessage - but only used with
        // handleMessage during OMX message-list handling
        kWhatOMXMessageItem          = 'omxI',
        kWhatOMXMessageList          = 'omxL',
        kWhatInputBufferFilled       = 'inpF',
        kWhatOutputBufferDrained     = 'outD',
        kWhatShutdown                = 'shut',
        kWhatFlush                   = 'flus',
        kWhatResume                  = 'resm',
        kWhatDrainDeferredMessages   = 'drai',
        kWhatAllocateComponent       = 'allo',
        kWhatConfigureComponent      = 'conf',
        kWhatSetSurface              = 'setS',
        kWhatCreateInputSurface      = 'cisf',
        kWhatSetInputSurface         = 'sisf',
        kWhatSignalEndOfInputStream  = 'eois',
        kWhatStart                   = 'star',
        kWhatRequestIDRFrame         = 'ridr',
        kWhatSetParameters           = 'setP',
        kWhatSubmitOutputMetadataBufferIfEOS = 'subm',
        kWhatOMXDied                 = 'OMXd',
        kWhatReleaseCodecInstance    = 'relC',
    };

    enum {
        kPortIndexInput  = 0,
        kPortIndexOutput = 1
    };

    enum {
        kFlagIsSecure                                 = 1,
        kFlagPushBlankBuffersToNativeWindowOnShutdown = 2,
        kFlagIsGrallocUsageProtected                  = 4,
    };

    enum {
        kVideoGrallocUsage = (GRALLOC_USAGE_HW_TEXTURE
                            | GRALLOC_USAGE_HW_COMPOSER
                            | GRALLOC_USAGE_EXTERNAL_DISP),
    };

    struct BufferInfo {
        enum Status {
            OWNED_BY_US,
            OWNED_BY_COMPONENT,
            OWNED_BY_UPSTREAM,
            OWNED_BY_DOWNSTREAM,
            OWNED_BY_NATIVE_WINDOW,
            UNRECOGNIZED,            // not a tracked buffer
        };

        static inline Status getSafeStatus(BufferInfo *info) {
            return info == NULL ? UNRECOGNIZED : info->mStatus;
        }

        IOMX::buffer_id mBufferID;
        Status mStatus;
        unsigned mDequeuedAt;

        sp<ABuffer> mData;      // the client's buffer; if not using data conversion, this is the
                                // codec buffer; otherwise, it is allocated separately
        sp<RefBase> mMemRef;    // and a reference to the IMemory, so it does not go away
        sp<ABuffer> mCodecData; // the codec's buffer
        sp<RefBase> mCodecRef;  // and a reference to the IMemory
        sp<GraphicBuffer> mGraphicBuffer;
        sp<NativeHandle> mNativeHandle;
        int mFenceFd;
        FrameRenderTracker::Info *mRenderInfo;

        // The following field and 4 methods are used for debugging only
        bool mIsReadFence;
        // Store |fenceFd| and set read/write flag. Log error, if there is already a fence stored.
        void setReadFence(int fenceFd, const char *dbg);
        void setWriteFence(int fenceFd, const char *dbg);
        // Log error, if the current fence is not a read/write fence.
        void checkReadFence(const char *dbg);
        void checkWriteFence(const char *dbg);
    };

    static const char *_asString(BufferInfo::Status s);
    void dumpBuffers(OMX_U32 portIndex);

    // If |fd| is non-negative, waits for fence with |fd| and logs an error if it fails. Returns
    // the error code or OK on success. If |fd| is negative, it returns OK
    status_t waitForFence(int fd, const char *dbg);

#if TRACK_BUFFER_TIMING
    struct BufferStats {
        int64_t mEmptyBufferTimeUs;
        int64_t mFillBufferDoneTimeUs;
    };

    KeyedVector<int64_t, BufferStats> mBufferStats;
#endif

    sp<AMessage> mNotify;

    sp<UninitializedState> mUninitializedState;
    sp<LoadedState> mLoadedState;
    sp<LoadedToIdleState> mLoadedToIdleState;
    sp<IdleToExecutingState> mIdleToExecutingState;
    sp<ExecutingState> mExecutingState;
    sp<OutputPortSettingsChangedState> mOutputPortSettingsChangedState;
    sp<ExecutingToIdleState> mExecutingToIdleState;
    sp<IdleToLoadedState> mIdleToLoadedState;
    sp<FlushingState> mFlushingState;
    sp<SkipCutBuffer> mSkipCutBuffer;

    AString mComponentName;
    uint32_t mFlags;
    uint32_t mQuirks;
    sp<IOMX> mOMX;
    sp<IBinder> mNodeBinder;
    IOMX::node_id mNode;
    sp<MemoryDealer> mDealer[2];

    bool mUsingNativeWindow;
    sp<ANativeWindow> mNativeWindow;
    int mNativeWindowUsageBits;
    android_native_rect_t mLastNativeWindowCrop;
    int32_t mLastNativeWindowDataSpace;
    sp<AMessage> mConfigFormat;
    sp<AMessage> mInputFormat;
    sp<AMessage> mOutputFormat;

    // Initial output format + configuration params that is reused as the base for all subsequent
    // format updates. This will equal to mOutputFormat until the first actual frame is received.
    sp<AMessage> mBaseOutputFormat;

    FrameRenderTracker mRenderTracker; // render information for buffers rendered by ACodec
    Vector<BufferInfo> mBuffers[2];
    bool mPortEOS[2];
    status_t mInputEOSResult;

    List<sp<AMessage> > mDeferredQueue;

    sp<AMessage> mLastOutputFormat;
    bool mIsVideo;
    bool mIsEncoder;
    bool mFatalError;
    bool mShutdownInProgress;
    bool mExplicitShutdown;
    bool mIsLegacyVP9Decoder;

    // If "mKeepComponentAllocated" we only transition back to Loaded state
    // and do not release the component instance.
    bool mKeepComponentAllocated;

    int32_t mEncoderDelay;
    int32_t mEncoderPadding;
    int32_t mRotationDegrees;

    bool mChannelMaskPresent;
    int32_t mChannelMask;
    unsigned mDequeueCounter;
    MetadataBufferType mInputMetadataType;
    MetadataBufferType mOutputMetadataType;
    bool mLegacyAdaptiveExperiment;
    int32_t mMetadataBuffersToSubmit;
    size_t mNumUndequeuedBuffers;
    sp<DataConverter> mConverter[2];

    int64_t mRepeatFrameDelayUs;
    int64_t mMaxPtsGapUs;
    float mMaxFps;

    int64_t mTimePerFrameUs;
    int64_t mTimePerCaptureUs;

    bool mCreateInputBuffersSuspended;

    bool mTunneled;

    OMX_INDEXTYPE mDescribeColorAspectsIndex;
    OMX_INDEXTYPE mDescribeHDRStaticInfoIndex;

    status_t setCyclicIntraMacroblockRefresh(const sp<AMessage> &msg, int32_t mode);
    status_t allocateBuffersOnPort(OMX_U32 portIndex);
    status_t freeBuffersOnPort(OMX_U32 portIndex);
    status_t freeBuffer(OMX_U32 portIndex, size_t i);

    status_t handleSetSurface(const sp<Surface> &surface);
    status_t setupNativeWindowSizeFormatAndUsage(
            ANativeWindow *nativeWindow /* nonnull */, int *finalUsage /* nonnull */,
            bool reconnect);

    status_t configureOutputBuffersFromNativeWindow(
            OMX_U32 *nBufferCount, OMX_U32 *nBufferSize,
            OMX_U32 *nMinUndequeuedBuffers, bool preregister);
    status_t allocateOutputMetadataBuffers();
    status_t submitOutputMetadataBuffer();
    void signalSubmitOutputMetadataBufferIfEOS_workaround();
    status_t allocateOutputBuffersFromNativeWindow();
#ifdef USE_SAMSUNG_COLORFORMAT
    void setNativeWindowColorFormat(OMX_COLOR_FORMATTYPE &eNativeColorFormat);
#endif
    status_t cancelBufferToNativeWindow(BufferInfo *info);
    status_t freeOutputBuffersNotOwnedByComponent();
    BufferInfo *dequeueBufferFromNativeWindow();

    inline bool storingMetadataInDecodedBuffers() {
        return mOutputMetadataType >= 0 && !mIsEncoder;
    }

    inline bool usingMetadataOnEncoderOutput() {
        return mOutputMetadataType >= 0 && mIsEncoder;
    }

    BufferInfo *findBufferByID(
            uint32_t portIndex, IOMX::buffer_id bufferID,
            ssize_t *index = NULL);

    virtual status_t setComponentRole(bool isEncoder, const char *mime);
    virtual const char *getComponentRole(bool isEncoder, const char *mime);
    static status_t setComponentRole(
            const sp<IOMX> &omx, IOMX::node_id node, const char *role);
    virtual status_t configureCodec(const char *mime, const sp<AMessage> &msg);

    status_t configureTunneledVideoPlayback(int32_t audioHwSync,
            const sp<ANativeWindow> &nativeWindow);

    status_t setVideoPortFormatType(
            OMX_U32 portIndex,
            OMX_VIDEO_CODINGTYPE compressionFormat,
            OMX_COLOR_FORMATTYPE colorFormat,
            bool usingNativeBuffers = false);

    status_t setSupportedOutputFormat(bool getLegacyFlexibleFormat);

    virtual status_t setupVideoDecoder(
            const char *mime, const sp<AMessage> &msg, bool usingNativeBuffers, bool haveSwRenderer,
            sp<AMessage> &outputformat);

    virtual status_t setupVideoEncoder(
            const char *mime, const sp<AMessage> &msg,
            sp<AMessage> &outputformat, sp<AMessage> &inputformat);

    status_t setVideoFormatOnPort(
            OMX_U32 portIndex,
            int32_t width, int32_t height,
            OMX_VIDEO_CODINGTYPE compressionFormat, float frameRate = -1.0);

    // gets index or sets it to 0 on error. Returns error from codec.
    status_t initDescribeColorAspectsIndex();

    // sets |params|. If |readBack| is true, it re-gets them afterwards if set succeeded.
    // returns the codec error.
    status_t setCodecColorAspects(DescribeColorAspectsParams &params, bool readBack = false);

    // gets |params|; returns the codec error. |param| should not change on error.
    status_t getCodecColorAspects(DescribeColorAspectsParams &params);

    // gets dataspace guidance from codec and platform. |params| should be set up with the color
    // aspects to use. If |tryCodec| is true, the codec is queried first. If it succeeds, we
    // return OK. Otherwise, we fall back to the platform guidance and return the codec error;
    // though, we return OK if the codec failed with UNSUPPORTED, as codec guidance is optional.
    status_t getDataSpace(
            DescribeColorAspectsParams &params, android_dataspace *dataSpace /* nonnull */,
            bool tryCodec);

    // sets color aspects for the encoder for certain |width/height| based on |configFormat|, and
    // set resulting color config into |outputFormat|. If |usingNativeWindow| is true, we use
    // video defaults if config is unspecified. Returns error from the codec.
    status_t setColorAspectsForVideoDecoder(
            int32_t width, int32_t height, bool usingNativeWindow,
            const sp<AMessage> &configFormat, sp<AMessage> &outputFormat);

    // gets color aspects for the encoder for certain |width/height| based on |configFormat|, and
    // set resulting color config into |outputFormat|. If |dataSpace| is non-null, it requests
    // dataspace guidance from the codec and platform and sets it into |dataSpace|. Returns the
    // error from the codec.
    status_t getColorAspectsAndDataSpaceForVideoDecoder(
            int32_t width, int32_t height, const sp<AMessage> &configFormat,
            sp<AMessage> &outputFormat, android_dataspace *dataSpace);

    // sets color aspects for the video encoder assuming bytebuffer mode for certain |configFormat|
    // and sets resulting color config into |outputFormat|. For mediarecorder, also set dataspace
    // into |inputFormat|. Returns the error from the codec.
    status_t setColorAspectsForVideoEncoder(
            const sp<AMessage> &configFormat,
            sp<AMessage> &outputFormat, sp<AMessage> &inputFormat);

    // sets color aspects for the video encoder in surface mode. This basically sets the default
    // video values for unspecified aspects and sets the dataspace to use in the input format.
    // Also sets the dataspace into |dataSpace|.
    // Returns any codec errors during this configuration, except for optional steps.
    status_t setInitialColorAspectsForVideoEncoderSurfaceAndGetDataSpace(
            android_dataspace *dataSpace /* nonnull */);

    // gets color aspects for the video encoder input port and sets them into the |format|.
    // Returns any codec errors.
    status_t getInputColorAspectsForVideoEncoder(sp<AMessage> &format);

    // updates the encoder output format with |aspects| defaulting to |dataSpace| for
    // unspecified values.
    void onDataSpaceChanged(android_dataspace dataSpace, const ColorAspects &aspects);

    // gets index or sets it to 0 on error. Returns error from codec.
    status_t initDescribeHDRStaticInfoIndex();

    // sets HDR static metadata for the video encoder/decoder based on |configFormat|, and
    // sets resulting HDRStaticInfo config into |outputFormat|. Returns error from the codec.
    status_t setHDRStaticInfoForVideoCodec(
            OMX_U32 portIndex, const sp<AMessage> &configFormat, sp<AMessage> &outputFormat);

    // sets |params|. Returns the codec error.
    status_t setHDRStaticInfo(const DescribeHDRStaticInfoParams &params);

    // gets |params|. Returns the codec error.
    status_t getHDRStaticInfo(DescribeHDRStaticInfoParams &params);

    // gets HDR static information for the video encoder/decoder port and sets them into |format|.
    status_t getHDRStaticInfoForVideoCodec(OMX_U32 portIndex, sp<AMessage> &format);

    typedef struct drcParams {
        int32_t drcCut;
        int32_t drcBoost;
        int32_t heavyCompression;
        int32_t targetRefLevel;
        int32_t encodedTargetLevel;
    } drcParams_t;

    status_t setupAACCodec(
            bool encoder,
            int32_t numChannels, int32_t sampleRate, int32_t bitRate,
            int32_t aacProfile, bool isADTS, int32_t sbrMode,
            int32_t maxOutputChannelCount, const drcParams_t& drc,
            int32_t pcmLimiterEnable,
            AudioEncoding encoding = kAudioEncodingPcm16bit);

    status_t setupAC3Codec(bool encoder, int32_t numChannels, int32_t sampleRate,
            AudioEncoding encoding = kAudioEncodingPcm16bit);

    status_t setupEAC3Codec(bool encoder, int32_t numChannels, int32_t sampleRate,
            AudioEncoding encoding = kAudioEncodingPcm16bit);

    status_t selectAudioPortFormat(
            OMX_U32 portIndex, OMX_AUDIO_CODINGTYPE desiredFormat);

    status_t setupAMRCodec(bool encoder, bool isWAMR, int32_t bitRate);
    status_t setupG711Codec(bool encoder, int32_t sampleRate, int32_t numChannels);

    status_t setupFlacCodec(
            bool encoder, int32_t numChannels, int32_t sampleRate, int32_t compressionLevel);

    status_t setupRawAudioFormat(
            OMX_U32 portIndex, int32_t sampleRate, int32_t numChannels,
            AudioEncoding encoding = kAudioEncodingPcm16bit);

    status_t setPriority(int32_t priority);
    status_t setOperatingRate(float rateFloat, bool isVideo);
    status_t getIntraRefreshPeriod(uint32_t *intraRefreshPeriod);
    status_t setIntraRefreshPeriod(uint32_t intraRefreshPeriod, bool inConfigure);

    status_t setMinBufferSize(OMX_U32 portIndex, size_t size);

    status_t setupMPEG4EncoderParameters(const sp<AMessage> &msg);
    status_t setupH263EncoderParameters(const sp<AMessage> &msg);
    status_t setupAVCEncoderParameters(const sp<AMessage> &msg);
    status_t setupHEVCEncoderParameters(const sp<AMessage> &msg);
    status_t setupVPXEncoderParameters(const sp<AMessage> &msg);

    status_t verifySupportForProfileAndLevel(int32_t profile, int32_t level);

    status_t configureBitrate(
            int32_t bitrate, OMX_VIDEO_CONTROLRATETYPE bitrateMode);

    virtual status_t setupErrorCorrectionParameters();

    status_t initNativeWindow();

    // Returns true iff all buffers on the given port have status
    // OWNED_BY_US or OWNED_BY_NATIVE_WINDOW.
    bool allYourBuffersAreBelongToUs(OMX_U32 portIndex);

    bool allYourBuffersAreBelongToUs();

    void waitUntilAllPossibleNativeWindowBuffersAreReturnedToUs();

    size_t countBuffersOwnedByComponent(OMX_U32 portIndex) const;
    size_t countBuffersOwnedByNativeWindow() const;

    void deferMessage(const sp<AMessage> &msg);
    void processDeferredMessages();

    void onFrameRendered(int64_t mediaTimeUs, nsecs_t systemNano);
    // called when we have dequeued a buffer |buf| from the native window to track render info.
    // |fenceFd| is the dequeue fence, and |info| points to the buffer info where this buffer is
    // stored.
    void updateRenderInfoForDequeuedBuffer(
            ANativeWindowBuffer *buf, int fenceFd, BufferInfo *info);

    // Checks to see if any frames have rendered up until |until|, and to notify client
    // (MediaCodec) of rendered frames up-until the frame pointed to by |until| or the first
    // unrendered frame. These frames are removed from the render queue.
    // If |dropIncomplete| is true, unrendered frames up-until |until| will be dropped from the
    // queue, allowing all rendered framed up till then to be notified of.
    // (This will effectively clear the render queue up-until (and including) |until|.)
    // If |until| is NULL, or is not in the rendered queue, this method will check all frames.
    void notifyOfRenderedFrames(
            bool dropIncomplete = false, FrameRenderTracker::Info *until = NULL);

    // Pass |expectedFormat| to print a warning if the format differs from it.
    // Using sp<> instead of const sp<>& because expectedFormat is likely the current mOutputFormat
    // which will get updated inside.
    void onOutputFormatChanged(sp<const AMessage> expectedFormat = NULL);
    void addKeyFormatChangesToRenderBufferNotification(sp<AMessage> &notify);
    void sendFormatChange();

    virtual status_t getPortFormat(OMX_U32 portIndex, sp<AMessage> &notify);

    void signalError(
            OMX_ERRORTYPE error = OMX_ErrorUndefined,
            status_t internalError = UNKNOWN_ERROR);

    static bool describeDefaultColorFormat(DescribeColorFormat2Params &describeParams);
    static bool describeColorFormat(
        const sp<IOMX> &omx, IOMX::node_id node,
        DescribeColorFormat2Params &describeParams);

    status_t requestIDRFrame();
    virtual status_t setParameters(const sp<AMessage> &params);

    // Send EOS on input stream.
    void onSignalEndOfInputStream();

    static const int32_t kNumBFramesPerPFrame = 1;

    virtual void setBFrames(OMX_VIDEO_PARAM_MPEG4TYPE *mpeg4type);
    virtual void setBFrames(OMX_VIDEO_PARAM_AVCTYPE *h264type,
        const int32_t iFramesInterval, const int32_t frameRate);

    virtual status_t getVQZIPInfo(const sp<AMessage> &msg) {
        return OK;
    }

    sp<IOMXObserver> createObserver();

    DISALLOW_EVIL_CONSTRUCTORS(ACodec);
};

}  // namespace android

#endif  // A_CODEC_H_
