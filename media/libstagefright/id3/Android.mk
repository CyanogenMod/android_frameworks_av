LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	ID3.cpp

LOCAL_CFLAGS += -Werror

LOCAL_MODULE := libstagefright_id3

include $(BUILD_STATIC_LIBRARY)

################################################################################

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	testid3.cpp

LOCAL_CFLAGS += -Werror

LOCAL_SHARED_LIBRARIES := \
	libstagefright libutils liblog libbinder libstagefright_foundation

LOCAL_STATIC_LIBRARIES := \
        libstagefright_id3

LOCAL_MODULE_TAGS := optional

LOCAL_MODULE := testid3

LOCAL_32_BIT_ONLY := true

include $(BUILD_EXECUTABLE)
