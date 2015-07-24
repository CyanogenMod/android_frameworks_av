LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    src/DeviceDescriptor.cpp \
    src/AudioGain.cpp \
    src/StreamDescriptor.cpp \
    src/HwModule.cpp \
    src/IOProfile.cpp \
    src/AudioPort.cpp \
    src/AudioPolicyMix.cpp \
    src/AudioPatch.cpp \
    src/AudioInputDescriptor.cpp \
    src/AudioOutputDescriptor.cpp \
    src/EffectDescriptor.cpp \
    src/ConfigParsingUtils.cpp \
    src/SoundTriggerSession.cpp \
    src/SessionRoute.cpp \

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/include \
    $(TOPDIR)frameworks/av/services/audiopolicy/common/include \
    $(TOPDIR)frameworks/av/services/audiopolicy

LOCAL_EXPORT_C_INCLUDE_DIRS := \
    $(LOCAL_PATH)/include

ifeq ($(call is-vendor-board-platform,QCOM),true)
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_FLAC_OFFLOAD)),true)
LOCAL_CFLAGS     += -DFLAC_OFFLOAD_ENABLED
endif
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_PROXY_DEVICE)),true)
LOCAL_CFLAGS     += -DAUDIO_EXTN_AFE_PROXY_ENABLED
endif
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_WMA_OFFLOAD)),true)
LOCAL_CFLAGS     += -DWMA_OFFLOAD_ENABLED
endif
endif

LOCAL_MODULE := libaudiopolicycomponents

include $(BUILD_STATIC_LIBRARY)
