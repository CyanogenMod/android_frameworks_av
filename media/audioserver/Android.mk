LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	main_audioserver.cpp

LOCAL_SHARED_LIBRARIES := \
	libaudioflinger \
	libaudiopolicyservice \
	libbinder \
	liblog \
	libmedia \
	libradioservice \
	libsoundtriggerservice \
	libutils \

LOCAL_C_INCLUDES := \
	frameworks/av/services/audioflinger \
	frameworks/av/services/audiopolicy \
	frameworks/av/services/audiopolicy/common/managerdefinitions/include \
	frameworks/av/services/audiopolicy/common/include \
	frameworks/av/services/audiopolicy/engine/interface \
	frameworks/av/services/audiopolicy/service \
	frameworks/av/services/radio \
	frameworks/av/services/soundtrigger \
	$(call include-path-for, audio-utils) \
	external/sonic \

LOCAL_MODULE := audioserver
LOCAL_32_BIT_ONLY := true

LOCAL_INIT_RC := audioserver.rc

include $(BUILD_EXECUTABLE)
