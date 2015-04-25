LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := \
    PolicySubsystemBuilder.cpp \
    PolicySubsystem.cpp \
    Strategy.cpp \
    InputSource.cpp \
    VolumeProfile.cpp \
    Stream.cpp \
    Usage.cpp

LOCAL_CFLAGS += \
    -Wall \
    -Werror \
    -Wextra \

LOCAL_C_INCLUDES := \
    $(TOPDIR)frameworks/av/services/audiopolicy/common/include \
    $(TOPDIR)frameworks/av/services/audiopolicy/engineconfigurable/include \
    $(TOPDIR)frameworks/av/services/audiopolicy/engineconfigurable/interface \

LOCAL_SHARED_LIBRARIES := \
    libaudiopolicyengineconfigurable  \
    libparameter \
    liblog \

LOCAL_STATIC_LIBRARIES := \
    libparameter_includes \
    libxmlserializer_includes \
    libpfw_utility \

LOCAL_MODULE_TAGS := optional
LOCAL_MODULE := libpolicy-subsystem

include external/stlport/libstlport.mk
include $(BUILD_SHARED_LIBRARY)

