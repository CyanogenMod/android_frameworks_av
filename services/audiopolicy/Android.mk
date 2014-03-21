LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioPolicyService.cpp

USE_LEGACY_AUDIO_POLICY = 1
ifeq ($(USE_LEGACY_AUDIO_POLICY), 1)
LOCAL_SRC_FILES += \
    AudioPolicyInterfaceImplLegacy.cpp \
    AudioPolicyClientImplLegacy.cpp

    LOCAL_CFLAGS += -DUSE_LEGACY_AUDIO_POLICY
else
LOCAL_SRC_FILES += \
    AudioPolicyInterfaceImpl.cpp \
    AudioPolicyClientImpl.cpp \
    AudioPolicyManager.cpp
endif

LOCAL_C_INCLUDES := \
    $(TOPDIR)frameworks/av/services/audioflinger \
    $(call include-path-for, audio-effects) \
    $(call include-path-for, audio-utils)

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \
    libbinder \
    libmedia \
    libhardware \
    libhardware_legacy

LOCAL_STATIC_LIBRARIES := \
    libmedia_helper \
    libserviceutility

LOCAL_MODULE:= libaudiopolicy

LOCAL_CFLAGS += -fvisibility=hidden

include $(BUILD_SHARED_LIBRARY)
