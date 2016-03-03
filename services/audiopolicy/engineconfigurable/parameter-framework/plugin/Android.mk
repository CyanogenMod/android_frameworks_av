LOCAL_PATH := $(call my-dir)

ifneq ($(USE_CUSTOM_PARAMETER_FRAMEWORK), true)

include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := \
    PolicySubsystemBuilder.cpp \
    PolicySubsystem.cpp \
    Strategy.cpp \
    InputSource.cpp \
    Stream.cpp \
    Usage.cpp

LOCAL_CFLAGS += \
    -Wall \
    -Werror \
    -Wextra \
    -fvisibility-inlines-hidden \
    -fvisibility=hidden

LOCAL_C_INCLUDES := \
    $(TOPDIR)frameworks/av/services/audiopolicy/common/include \
    $(TOPDIR)frameworks/av/services/audiopolicy/engineconfigurable/include \
    $(TOPDIR)frameworks/av/services/audiopolicy/engineconfigurable/interface \

LOCAL_SHARED_LIBRARIES := \
    libaudiopolicyengineconfigurable  \
    libparameter \
    liblog \

LOCAL_MULTILIB := $(AUDIOSERVER_MULTILIB)

LOCAL_STATIC_LIBRARIES := libpfw_utility

LOCAL_MODULE_TAGS := optional
LOCAL_MODULE := libpolicy-subsystem

include $(BUILD_SHARED_LIBRARY)

endif # ifneq ($(USE_CUSTOM_PARAMETER_FRAMEWORK), true)
