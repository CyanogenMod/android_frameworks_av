LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioParameter.cpp
LOCAL_MODULE:= libmedia_helper
LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
LOCAL_SRC_FILES:= AudioParameter.cpp
LOCAL_MODULE:= libaudioparameter
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libutils

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
endif

LOCAL_SRC_FILES:= \
    AudioTrack.cpp \
    IAudioFlinger.cpp \
    IAudioFlingerClient.cpp \
    IAudioTrack.cpp \
    IAudioRecord.cpp \
    ICrypto.cpp \
    IHDCP.cpp \
    AudioRecord.cpp \
    AudioSystem.cpp \
    mediaplayer.cpp \
    IMediaPlayerService.cpp \
    IMediaPlayerClient.cpp \
    IMediaRecorderClient.cpp \
    IMediaPlayer.cpp \
    IMediaRecorder.cpp \
    IRemoteDisplay.cpp \
    IRemoteDisplayClient.cpp \
    IStreamSource.cpp \
    Metadata.cpp \
    mediarecorder.cpp \
    IMediaMetadataRetriever.cpp \
    mediametadataretriever.cpp \
    ToneGenerator.cpp \
    JetPlayer.cpp \
    IOMX.cpp \
    IAudioPolicyService.cpp \
    MediaScanner.cpp \
    MediaScannerClient.cpp \
    autodetect.cpp \
    IMediaDeathNotifier.cpp \
    MediaProfiles.cpp \
    IEffect.cpp \
    IEffectClient.cpp \
    AudioEffect.cpp \
    Visualizer.cpp \
    MemoryLeakTrackUtil.cpp \
    SoundPool.cpp \
    SoundPoolThread.cpp

ifeq ($(BOARD_USES_LIBMEDIA_WITH_AUDIOPARAMETER),true)
LOCAL_SRC_FILES+= \
    AudioParameter.cpp
endif

ifeq ($(BOARD_USE_SAMSUNG_SEPARATEDSTREAM),true)
LOCAL_CFLAGS += -DUSE_SAMSUNG_SEPARATEDSTREAM
endif

ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
LOCAL_SRC_FILES += \
    IDirectTrack.cpp \
    IDirectTrackClient.cpp

ifeq ($(TARGET_QCOM_AUDIO_VARIANT),caf)
LOCAL_CFLAGS += -DQCOM_ENHANCED_AUDIO
endif
endif

LOCAL_SHARED_LIBRARIES := \
	libui libcutils libutils libbinder libsonivox libicuuc libexpat \
        libcamera_client libstagefright_foundation \
        libgui libdl libaudioutils libmedia_native

LOCAL_WHOLE_STATIC_LIBRARY := libmedia_helper

LOCAL_MODULE:= libmedia

LOCAL_C_INCLUDES := \
    $(call include-path-for, graphics corecg) \
    $(TOP)/frameworks/native/include/media/openmax \
    external/icu4c/common \
    $(call include-path-for, audio-effects) \
    $(call include-path-for, audio-utils)

include $(BUILD_SHARED_LIBRARY)
