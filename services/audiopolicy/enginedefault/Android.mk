LOCAL_PATH := $(call my-dir)

# Component build
#######################################################################

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    src/Engine.cpp \
    src/EngineInstance.cpp \
    src/Gains.cpp \


audio_policy_engine_includes_common := \
    $(LOCAL_PATH)/include \
    $(TOPDIR)frameworks/av/services/audiopolicy/engine/interface

LOCAL_CFLAGS += \
    -Wall \
    -Werror \
    -Wextra \

LOCAL_EXPORT_C_INCLUDE_DIRS := \
    $(audio_policy_engine_includes_common)

LOCAL_C_INCLUDES := \
    $(audio_policy_engine_includes_common) \
    $(TARGET_OUT_HEADERS)/hw \
    $(call include-path-for, frameworks-av) \
    $(call include-path-for, audio-utils) \
    $(call include-path-for, bionic) \
    $(TOPDIR)frameworks/av/services/audiopolicy/common/include

ifeq ($(call is-vendor-board-platform,QCOM),true)
ifneq ($(strip $(AUDIO_FEATURE_ENABLED_PROXY_DEVICE)),false)
LOCAL_CFLAGS += -DAUDIO_EXTN_AFE_PROXY_ENABLED
endif
endif

LOCAL_MODULE := libaudiopolicyenginedefault
LOCAL_MODULE_TAGS := optional
LOCAL_STATIC_LIBRARIES := \
    libmedia_helper \
    libaudiopolicycomponents

LOCAL_SHARED_LIBRARIES += \
    libcutils \
    libutils \
    libaudioutils \

include $(BUILD_SHARED_LIBRARY)
