LOCAL_PATH:= $(call my-dir)

#
# libmediadrm
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	Crypto.cpp \
	Drm.cpp \
	DrmSessionManager.cpp \
	SharedLibrary.cpp

LOCAL_SHARED_LIBRARIES := \
	libbinder \
	libcrypto \
	libcutils \
	libdl \
	liblog \
	libmedia \
	libstagefright \
	libstagefright_foundation \
	libutils

LOCAL_C_INCLUDES := \
    libcore/include

LOCAL_CFLAGS += -Werror -Wno-error=deprecated-declarations -Wall
LOCAL_CLANG := true

LOCAL_MODULE:= libmediadrm

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
