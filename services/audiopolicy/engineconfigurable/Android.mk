ifeq ($(USE_CONFIGURABLE_AUDIO_POLICY), 1)

LOCAL_PATH := $(call my-dir)

# Component build
#######################################################################

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    src/Engine.cpp \
    src/EngineInstance.cpp \
    src/Stream.cpp \
    src/Strategy.cpp \
    src/Usage.cpp \
    src/InputSource.cpp \

audio_policy_engine_includes_common := \
    $(TOPDIR)frameworks/av/services/audiopolicy/engineconfigurable/include \
    $(TOPDIR)frameworks/av/services/audiopolicy/engineconfigurable/interface \
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
    $(TOPDIR)frameworks/av/services/audiopolicy/common/include

LOCAL_MULTILIB := $(AUDIOSERVER_MULTILIB)

LOCAL_MODULE := libaudiopolicyengineconfigurable
LOCAL_MODULE_TAGS := optional
LOCAL_STATIC_LIBRARIES := \
    libmedia_helper \
    libaudiopolicypfwwrapper \
    libaudiopolicycomponents \
    libxml2

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    libaudioutils \
    libparameter

include $(BUILD_SHARED_LIBRARY)

#######################################################################
# Recursive call sub-folder Android.mk
#
include $(call all-makefiles-under,$(LOCAL_PATH))

endif
