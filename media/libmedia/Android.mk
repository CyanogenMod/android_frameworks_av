LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioParameter.cpp
LOCAL_MODULE:= libmedia_helper
LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioTrack.cpp \
    AudioTrackShared.cpp \
    IAudioFlinger.cpp \
    IAudioFlingerClient.cpp \
    IAudioTrack.cpp \
    IAudioRecord.cpp \
    ICrypto.cpp \
    IDrm.cpp \
    IDrmClient.cpp \
    IHDCP.cpp \
    AudioRecord.cpp \
    AudioSystem.cpp \
    mediaplayer.cpp \
    IMediaCodecList.cpp \
    IMediaHTTPConnection.cpp \
    IMediaHTTPService.cpp \
    IMediaLogService.cpp \
    IMediaPlayerService.cpp \
    IMediaPlayerClient.cpp \
    IMediaRecorderClient.cpp \
    IMediaPlayer.cpp \
    IMediaRecorder.cpp \
    IRemoteDisplay.cpp \
    IRemoteDisplayClient.cpp \
    IStreamSource.cpp \
    MediaCodecInfo.cpp \
    Metadata.cpp \
    mediarecorder.cpp \
    IMediaMetadataRetriever.cpp \
    mediametadataretriever.cpp \
    ToneGenerator.cpp \
    JetPlayer.cpp \
    IOMX.cpp \
    IAudioPolicyService.cpp \
    IAudioPolicyServiceClient.cpp \
    MediaScanner.cpp \
    MediaScannerClient.cpp \
    CharacterEncodingDetector.cpp \
    IMediaDeathNotifier.cpp \
    MediaProfiles.cpp \
    IEffect.cpp \
    IEffectClient.cpp \
    AudioEffect.cpp \
    Visualizer.cpp \
    MemoryLeakTrackUtil.cpp \
    SoundPool.cpp \
    SoundPoolThread.cpp \
    StringArray.cpp

LOCAL_SRC_FILES += ../libnbaio/roundup.c

LOCAL_SHARED_LIBRARIES := \
	libui liblog libcutils libutils libbinder libsonivox libicuuc libicui18n libexpat \
        libcamera_client libstagefright_foundation \
        libgui libdl libaudioutils libnbaio

LOCAL_STATIC_LIBRARIES += libinstantssq

LOCAL_WHOLE_STATIC_LIBRARY := libmedia_helper

LOCAL_MODULE:= libmedia

LOCAL_C_INCLUDES := \
    $(TOP)/frameworks/native/include/media/openmax \
    $(TOP)/frameworks/av/include/media/ \
    $(TOP)/frameworks/av/media/libstagefright \
    $(TOP)/external/icu/icu4c/source/common \
    $(TOP)/external/icu/icu4c/source/i18n \
    $(call include-path-for, audio-effects) \
    $(call include-path-for, audio-utils)

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

# for <cutils/atomic-inline.h>
LOCAL_CFLAGS += -DANDROID_SMP=$(if $(findstring true,$(TARGET_CPU_SMP)),1,0)
LOCAL_SRC_FILES += SingleStateQueue.cpp
LOCAL_CFLAGS += -DSINGLE_STATE_QUEUE_INSTANTIATIONS='"SingleStateQueueInstantiations.cpp"'

LOCAL_MODULE := libinstantssq
LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)
