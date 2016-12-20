LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    src/DeviceDescriptor.cpp \
    src/AudioGain.cpp \
    src/HwModule.cpp \
    src/IOProfile.cpp \
    src/AudioPort.cpp \
    src/AudioProfile.cpp \
    src/AudioRoute.cpp \
    src/AudioPolicyMix.cpp \
    src/AudioPatch.cpp \
    src/AudioInputDescriptor.cpp \
    src/AudioOutputDescriptor.cpp \
    src/AudioCollections.cpp \
    src/EffectDescriptor.cpp \
    src/SoundTriggerSession.cpp \
    src/SessionRoute.cpp \
    src/AudioSourceDescriptor.cpp \
    src/VolumeCurve.cpp \
    src/TypeConverter.cpp \
    src/AudioSession.cpp

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/include \
    $(TOPDIR)frameworks/av/services/audiopolicy/common/include \
    $(TOPDIR)frameworks/av/services/audiopolicy \
    $(TOPDIR)frameworks/av/services/audiopolicy/utilities \

ifeq ($(call is-vendor-board-platform,QCOM),true)
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_FLAC_OFFLOAD)),true)
LOCAL_CFLAGS     += -DFLAC_OFFLOAD_ENABLED
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
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_EXTN_FORMATS)),true)
LOCAL_CFLAGS     += -DAUDIO_EXTN_FORMATS_ENABLED
endif
endif
ifeq ($(USE_XML_AUDIO_POLICY_CONF), 1)

LOCAL_SRC_FILES += src/Serializer.cpp

LOCAL_STATIC_LIBRARIES += libxml2

LOCAL_SHARED_LIBRARIES += libicuuc

LOCAL_C_INCLUDES += \
    $(TOPDIR)external/libxml2/include \
    $(TOPDIR)external/icu/icu4c/source/common

else

LOCAL_SRC_FILES += \
    src/ConfigParsingUtils.cpp \
    src/StreamDescriptor.cpp \
    src/Gains.cpp

endif #ifeq ($(USE_XML_AUDIO_POLICY_CONF), 1)

ifeq ($(call is-vendor-board-platform,QCOM),true)
ifneq ($(strip $(AUDIO_FEATURE_ENABLED_PROXY_DEVICE)),false)
LOCAL_CFLAGS += -DAUDIO_EXTN_AFE_PROXY_ENABLED
endif
endif

LOCAL_EXPORT_C_INCLUDE_DIRS := \
    $(LOCAL_PATH)/include

LOCAL_MULTILIB := $(AUDIOSERVER_MULTILIB)

LOCAL_CFLAGS += -Wall -Werror

LOCAL_MODULE := libaudiopolicycomponents

include $(BUILD_STATIC_LIBRARY)
