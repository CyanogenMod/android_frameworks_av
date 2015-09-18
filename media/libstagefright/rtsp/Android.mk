LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=       \
        AAMRAssembler.cpp           \
        AAVCAssembler.cpp           \
        AH263Assembler.cpp          \
        AMPEG2TSAssembler.cpp       \
        AMPEG4AudioAssembler.cpp    \
        AMPEG4ElementaryAssembler.cpp \
        APacketSource.cpp           \
        ARawAudioAssembler.cpp      \
        ARTPAssembler.cpp           \
        ARTPConnection.cpp          \
        ARTPSource.cpp              \
        ARTPWriter.cpp              \
        ARTSPConnection.cpp         \
        ASessionDescription.cpp     \
        SDPLoader.cpp               \

LOCAL_SHARED_LIBRARIES += libcrypto

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/media/libstagefright \
        $(TOP)/frameworks/av/media/libavextensions \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/frameworks/av/media/libmediaplayerservice \

LOCAL_MODULE:= libstagefright_rtsp

ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS += -Wno-psabi
endif

LOCAL_CFLAGS += -Werror -Wall
LOCAL_CLANG := true
LOCAL_SANITIZE := unsigned-integer-overflow signed-integer-overflow

LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

include $(BUILD_STATIC_LIBRARY)

################################################################################

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	rtp_test.cpp \

LOCAL_SHARED_LIBRARIES := \
	libstagefright liblog libutils libbinder libstagefright_foundation libmedia

LOCAL_STATIC_LIBRARIES := \
	libstagefright_rtsp

LOCAL_C_INCLUDES := \
	frameworks/av/media/libstagefright \
	frameworks/av/cmds/stagefright \
	$(TOP)/frameworks/native/include/media/openmax

LOCAL_CFLAGS += -Wno-multichar -Werror -Wall
LOCAL_CLANG := true
LOCAL_SANITIZE := signed-integer-overflow

LOCAL_MODULE_TAGS := optional

LOCAL_MODULE:= rtp_test

LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

# include $(BUILD_EXECUTABLE)
