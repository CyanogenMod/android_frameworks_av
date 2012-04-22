LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    AudioBufferProviderSource.cpp   \
    AudioStreamOutSink.cpp          \
    AudioStreamInSource.cpp         \
    NBAIO.cpp                       \
    MonoPipe.cpp                    \
    MonoPipeReader.cpp              \
    Pipe.cpp                        \
    PipeReader.cpp                  \
    roundup.c                       \
    SourceAudioBufferProvider.cpp

# libsndfile license is incompatible; uncomment to use for local debug only
#LOCAL_SRC_FILES += LibsndfileSink.cpp LibsndfileSource.cpp
#LOCAL_C_INCLUDES += path/to/libsndfile/src
#LOCAL_STATIC_LIBRARIES += libsndfile

LOCAL_MODULE := libnbaio

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    AudioFlinger.cpp            \
    AudioMixer.cpp.arm          \
    AudioResampler.cpp.arm      \
    AudioPolicyService.cpp      \
    ServiceUtilities.cpp
#   AudioResamplerSinc.cpp.arm
#   AudioResamplerCubic.cpp.arm

LOCAL_SRC_FILES += StateQueue.cpp

LOCAL_C_INCLUDES := \
    $(call include-path-for, audio-effects) \
    $(call include-path-for, audio-utils)

# FIXME keep libmedia_native but remove libmedia after split
LOCAL_SHARED_LIBRARIES := \
    libaudioutils \
    libcommon_time_client \
    libcutils \
    libutils \
    libbinder \
    libmedia \
    libmedia_native \
    libhardware \
    libhardware_legacy \
    libeffects \
    libdl \
    libpowermanager

LOCAL_STATIC_LIBRARIES := \
    libnbaio \
    libcpustats \
    libmedia_helper

LOCAL_MODULE:= libaudioflinger

LOCAL_SRC_FILES += FastMixer.cpp FastMixerState.cpp

LOCAL_CFLAGS += -DFAST_MIXER_STATISTICS

LOCAL_CFLAGS += -DSTATE_QUEUE_INSTANTIATIONS='"StateQueueInstantiations.cpp"'

LOCAL_CFLAGS += -UHAVE_REQUEST_PRIORITY

include $(BUILD_SHARED_LIBRARY)
