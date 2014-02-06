LOCAL_PATH:= $(call my-dir)

ifneq ($(TARGET_BUILD_PDK), true)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=       \
        HTTPHelper.cpp          \

LOCAL_C_INCLUDES:= \
	$(TOP)/frameworks/av/media/libstagefright \
	$(TOP)/frameworks/native/include/media/openmax \
	$(TOP)/frameworks/base/core/jni \

LOCAL_SHARED_LIBRARIES := \
	libstagefright liblog libutils libbinder libstagefright_foundation \
        libandroid_runtime \
        libmedia

LOCAL_MODULE:= libstagefright_http_support

LOCAL_CFLAGS += -Wno-multichar

include $(BUILD_SHARED_LIBRARY)

endif
