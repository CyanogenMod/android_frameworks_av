LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    src/DeviceDescriptor.cpp \
    src/AudioGain.cpp \
    src/HwModule.cpp \
    src/IOProfile.cpp \
    src/AudioPort.cpp \
    src/AudioPatch.cpp \
    src/AudioInputDescriptor.cpp \
    src/AudioOutputDescriptor.cpp \
    src/ConfigParsingUtils.cpp \

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/include \

LOCAL_EXPORT_C_INCLUDE_DIRS := \
    $(LOCAL_PATH)/include

LOCAL_MODULE := libaudiopolicycomponents

include $(BUILD_STATIC_LIBRARY)
