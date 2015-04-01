LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    service/AudioPolicyService.cpp \
    service/AudioPolicyEffects.cpp

ifeq ($(USE_LEGACY_AUDIO_POLICY), 1)
LOCAL_SRC_FILES += \
    service/AudioPolicyInterfaceImplLegacy.cpp \
    service/AudioPolicyClientImplLegacy.cpp

    LOCAL_CFLAGS += -DUSE_LEGACY_AUDIO_POLICY
else
LOCAL_SRC_FILES += \
    service/AudioPolicyInterfaceImpl.cpp \
    service/AudioPolicyClientImpl.cpp

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
    libhardware_legacy \
    libserviceutility

ifneq ($(USE_LEGACY_AUDIO_POLICY), 1)
LOCAL_SHARED_LIBRARIES += \
    libaudiopolicymanager
endif

LOCAL_STATIC_LIBRARIES := \
    libmedia_helper \
    libaudiopolicycomponents

LOCAL_MODULE:= libaudiopolicyservice

LOCAL_CFLAGS += -fvisibility=hidden

include $(BUILD_SHARED_LIBRARY)


ifneq ($(USE_LEGACY_AUDIO_POLICY), 1)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    managerdefault/AudioPolicyManager.cpp \

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \
    libsoundtrigger

LOCAL_STATIC_LIBRARIES := \
    libmedia_helper \
    libaudiopolicycomponents

LOCAL_MODULE:= libaudiopolicymanagerdefault

include $(BUILD_SHARED_LIBRARY)

ifneq ($(USE_CUSTOM_AUDIO_POLICY), 1)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    manager/AudioPolicyFactory.cpp

LOCAL_SHARED_LIBRARIES := \
    libaudiopolicymanagerdefault

LOCAL_STATIC_LIBRARIES := \
    libaudiopolicycomponents

LOCAL_MODULE:= libaudiopolicymanager

include $(BUILD_SHARED_LIBRARY)

endif
endif

#######################################################################
# Recursive call sub-folder Android.mk
#
include $(call all-makefiles-under,$(LOCAL_PATH))
