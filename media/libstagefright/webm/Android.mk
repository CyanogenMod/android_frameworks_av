LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_CPPFLAGS += -D__STDINT_LIMITS

LOCAL_CFLAGS += -Werror -Wall
LOCAL_CLANG := true
LOCAL_SANITIZE := unsigned-integer-overflow signed-integer-overflow

LOCAL_SRC_FILES:= EbmlUtil.cpp        \
                  WebmElement.cpp     \
                  WebmFrame.cpp       \
                  WebmFrameThread.cpp \
                  WebmWriter.cpp


LOCAL_C_INCLUDES += $(TOP)/frameworks/av/include \
                    $(TOP)/frameworks/av/media/libavextensions \
                    $(TOP)/frameworks/av/media/libstagefright/mpeg2ts \
                    $(TOP)/frameworks/native/include/media/openmax

LOCAL_SHARED_LIBRARIES += libstagefright_foundation \
                          libstagefright \
                          libutils \
                          liblog

LOCAL_MODULE:= libstagefright_webm

include $(BUILD_STATIC_LIBRARY)
