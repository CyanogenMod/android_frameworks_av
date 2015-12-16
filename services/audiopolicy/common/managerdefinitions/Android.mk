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

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_FLAC_OFFLOAD)),true)
LOCAL_CFLAGS     += -DFLAC_OFFLOAD_ENABLED
endif
ifneq ($(strip $(AUDIO_FEATURE_ENABLED_PROXY_DEVICE)),false)
LOCAL_CFLAGS     += -DAUDIO_EXTN_AFE_PROXY_ENABLED
endif
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_WMA_OFFLOAD)),true)
LOCAL_CFLAGS     += -DWMA_OFFLOAD_ENABLED
endif
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_ALAC_OFFLOAD)),true)
LOCAL_CFLAGS     += -DALAC_OFFLOAD_ENABLED
endif
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_APE_OFFLOAD)),true)
LOCAL_CFLAGS     += -DAPE_OFFLOAD_ENABLED
endif
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_AAC_ADTS_OFFLOAD)),true)
LOCAL_CFLAGS     += -DAAC_ADTS_OFFLOAD_ENABLED
endif

LOCAL_MODULE := libaudiopolicycomponents

include $(BUILD_STATIC_LIBRARY)
