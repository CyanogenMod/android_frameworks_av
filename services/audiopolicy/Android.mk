LOCAL_PATH:= $(call my-dir)

ifeq ($(call is-vendor-board-platform,QCOM),true)

ifneq ($(strip $(AUDIO_FEATURE_ENABLED_COMPRESS_VOIP)),false)
common_cflags += -DAUDIO_EXTN_COMPRESS_VOIP_ENABLED
endif

ifneq ($(strip $(AUDIO_FEATURE_ENABLED_EXTN_FORMATS)),false)
common_cflags += -DAUDIO_EXTN_FORMATS_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_FM)),true)
common_cflags += -DAUDIO_EXTN_FM_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_HDMI_SPK)),true)
common_cflags += -DAUDIO_EXTN_HDMI_SPK_ENABLED
endif

ifneq ($(strip $(AUDIO_FEATURE_ENABLED_INCALL_MUSIC)),false)
common_cflags += -DAUDIO_EXTN_INCALL_MUSIC_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_MULTIPLE_TUNNEL)), true)
common_cflags += -DMULTIPLE_OFFLOAD_ENABLED
endif

ifneq ($(strip $(AUDIO_FEATURE_ENABLED_PROXY_DEVICE)),false)
common_cflags += -DAUDIO_EXTN_AFE_PROXY_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_SSR)),true)
common_cflags += -DAUDIO_EXTN_SSR_ENABLED
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_VOICE_CONCURRENCY)),true)
common_cflags += -DVOICE_CONCURRENCY
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_RECORD_PLAY_CONCURRENCY)),true)
common_cflags += -DRECORD_PLAY_CONCURRENCY
endif

ifeq ($(strip $(DOLBY_UDC)),true)
common_cflags += -DDOLBY_UDC
endif #DOLBY_UDC
ifeq ($(strip $(DOLBY_DDP)),true)
common_cflags += -DDOLBY_DDP
endif #DOLBY_DDP
ifeq ($(strip $(DOLBY_DAP)),true)
    ifdef DOLBY_DAP_OPENSLES
        common_cflags += -DDOLBY_DAP_OPENSLES
    endif
endif #DOLBY_END

ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
common_cflags += -DENABLE_AV_ENHANCEMENTS
endif

endif

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

ifeq ($(BOARD_HAVE_PRE_KITKAT_AUDIO_POLICY_BLOB),true)
    LOCAL_CFLAGS += -DHAVE_PRE_KITKAT_AUDIO_POLICY_BLOB
endif

LOCAL_STATIC_LIBRARIES := \
    libmedia_helper \
    libserviceutility

LOCAL_CFLAGS += $(common_cflags)

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

LOCAL_CFLAGS += $(common_cflags)

LOCAL_MODULE:= libaudiopolicymanagerdefault

include $(BUILD_SHARED_LIBRARY)

ifneq ($(USE_CUSTOM_AUDIO_POLICY), 1)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioPolicyFactory.cpp

LOCAL_SHARED_LIBRARIES := \
    libaudiopolicymanagerdefault

LOCAL_MODULE:= libaudiopolicymanager

LOCAL_CFLAGS += $(common_cflags)

include $(BUILD_SHARED_LIBRARY)

endif
endif
