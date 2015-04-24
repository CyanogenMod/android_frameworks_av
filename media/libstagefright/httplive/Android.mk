LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
        HTTPDownloader.cpp      \
        LiveDataSource.cpp      \
        LiveSession.cpp         \
        M3UParser.cpp           \
        PlaylistFetcher.cpp     \

LOCAL_C_INCLUDES:= \
	$(TOP)/frameworks/av/media/libstagefright \
	$(TOP)/frameworks/native/include/media/openmax

LOCAL_CFLAGS += -Werror -Wall
LOCAL_CLANG := true

LOCAL_SHARED_LIBRARIES := \
        libbinder \
        libcrypto \
        libcutils \
        libmedia \
        libstagefright \
        libstagefright_foundation \
        libutils \

LOCAL_MODULE:= libstagefright_httplive

ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS += -Wno-psabi
endif

include $(BUILD_SHARED_LIBRARY)
