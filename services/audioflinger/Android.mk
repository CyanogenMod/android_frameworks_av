LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    ServiceUtilities.cpp

# FIXME Move this library to frameworks/native
LOCAL_MODULE := libserviceutility

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \
    libbinder

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    AudioFlinger.cpp            \
    Threads.cpp                 \
    Tracks.cpp                  \
    AudioHwDevice.cpp           \
    AudioStreamOut.cpp          \
    SpdifStreamOut.cpp          \
    Effects.cpp                 \
    AudioMixer.cpp.arm          \
    BufferProviders.cpp         \
    PatchPanel.cpp              \
    StateQueue.cpp

LOCAL_C_INCLUDES := \
    $(TOPDIR)frameworks/av/services/audiopolicy \
    $(TOPDIR)external/sonic \
    $(call include-path-for, audio-effects) \
    $(call include-path-for, audio-utils)

LOCAL_SHARED_LIBRARIES := \
    libaudioresampler \
    libaudiospdif \
    libaudioutils \
    libcutils \
    libutils \
    liblog \
    libbinder \
    libmedia \
    libmediautils \
    libnbaio \
    libhardware \
    libhardware_legacy \
    libeffects \
    libpowermanager \
    libserviceutility \
    libsonic \
    libmediautils

LOCAL_STATIC_LIBRARIES := \
    libcpustats \
    libmedia_helper

LOCAL_MODULE:= libaudioflinger
LOCAL_32_BIT_ONLY := true

LOCAL_SRC_FILES += \
    AudioWatchdog.cpp        \
    FastCapture.cpp          \
    FastCaptureDumpState.cpp \
    FastCaptureState.cpp     \
    FastMixer.cpp            \
    FastMixerDumpState.cpp   \
    FastMixerState.cpp       \
    FastThread.cpp           \
    FastThreadDumpState.cpp  \
    FastThreadState.cpp

LOCAL_CFLAGS += -DSTATE_QUEUE_INSTANTIATIONS='"StateQueueInstantiations.cpp"'

LOCAL_CFLAGS += -fvisibility=hidden

include $(BUILD_SHARED_LIBRARY)

#
# build audio resampler test tool
#
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    test-resample.cpp           \

LOCAL_C_INCLUDES := \
    $(call include-path-for, audio-utils)

LOCAL_STATIC_LIBRARIES := \
    libsndfile

LOCAL_SHARED_LIBRARIES := \
    libaudioresampler \
    libaudioutils \
    libdl \
    libcutils \
    libutils \
    liblog

LOCAL_MODULE:= test-resample

LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioResampler.cpp.arm \
    AudioResamplerCubic.cpp.arm \
    AudioResamplerSinc.cpp.arm \
    AudioResamplerDyn.cpp.arm

LOCAL_C_INCLUDES := \
    $(call include-path-for, audio-utils)

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libdl \
    liblog

LOCAL_MODULE := libaudioresampler

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
