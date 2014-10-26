LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioPolicyService.cpp \
    AudioPolicyEffects.cpp

ifeq ($(USE_LEGACY_AUDIO_POLICY), 1)
LOCAL_SRC_FILES += \
    AudioPolicyInterfaceImplLegacy.cpp \
    AudioPolicyClientImplLegacy.cpp

    LOCAL_CFLAGS += -DUSE_LEGACY_AUDIO_POLICY
else
LOCAL_SRC_FILES += \
    AudioPolicyInterfaceImpl.cpp \
    AudioPolicyClientImpl.cpp
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

ifneq ($(USE_LEGACY_AUDIO_POLICY), 1)
LOCAL_SHARED_LIBRARIES += \
    libaudiopolicymanager
endif

LOCAL_STATIC_LIBRARIES := \
    libmedia_helper \
    libserviceutility

LOCAL_MODULE:= libaudiopolicyservice

LOCAL_CFLAGS += -fvisibility=hidden

include $(BUILD_SHARED_LIBRARY)


ifneq ($(USE_LEGACY_AUDIO_POLICY), 1)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioPolicyManager.cpp

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \
    libsoundtrigger

LOCAL_STATIC_LIBRARIES := \
    libmedia_helper

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_COMPRESS_VOIP)),true)
LOCAL_CFLAGS += -DAUDIO_EXTN_COMPRESS_VOIP_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_EXTN_FORMATS)),true)
LOCAL_CFLAGS += -DAUDIO_EXTN_FORMATS_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_FM)),true)
LOCAL_CFLAGS += -DAUDIO_EXTN_FM_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_HDMI_SPK)),true)
LOCAL_CFLAGS += -DAUDIO_EXTN_HDMI_SPK_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_INCALL_MUSIC)),true)
LOCAL_CFLAGS += -DAUDIO_EXTN_INCALL_MUSIC_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_MULTIPLE_TUNNEL)), true)
LOCAL_CFLAGS += -DMULTIPLE_OFFLOAD_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_PCM_OFFLOAD)),true)
    LOCAL_CFLAGS += -DPCM_OFFLOAD_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_PROXY_DEVICE)),true)
LOCAL_CFLAGS += -DAUDIO_EXTN_AFE_PROXY_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_SSR)),true)
LOCAL_CFLAGS += -DAUDIO_EXTN_SSR_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_VOICE_CONCURRENCY)),true)
LOCAL_CFLAGS += -DVOICE_CONCURRENCY
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_WFD_CONCURRENCY)),true)
LOCAL_CFLAGS += -DWFD_CONCURRENCY
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_RECORD_PLAY_CONCURRENCY)),true)
LOCAL_CFLAGS += -DRECORD_PLAY_CONCURRENCY
endif

ifeq ($(strip $(DOLBY_UDC)),true)
  LOCAL_CFLAGS += -DDOLBY_UDC
endif #DOLBY_UDC
ifeq ($(strip $(DOLBY_DDP)),true)
  LOCAL_CFLAGS += -DDOLBY_DDP
endif #DOLBY_DDP
ifeq ($(strip $(DOLBY_DAP)),true)
    ifdef DOLBY_DAP_OPENSLES
        LOCAL_CFLAGS += -DDOLBY_DAP_OPENSLES
    endif
endif #DOLBY_END

LOCAL_MODULE:= libaudiopolicymanagerdefault

include $(BUILD_SHARED_LIBRARY)

ifneq ($(USE_CUSTOM_AUDIO_POLICY), 1)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioPolicyFactory.cpp

LOCAL_SHARED_LIBRARIES := \
    libaudiopolicymanagerdefault

LOCAL_MODULE:= libaudiopolicymanager

include $(BUILD_SHARED_LIBRARY)

endif
endif
