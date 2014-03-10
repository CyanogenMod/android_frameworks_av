LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_CPPFLAGS += -D__STDINT_LIMITS \
                  -Werror

LOCAL_SRC_FILES:= EbmlUtil.cpp        \
                  WebmElement.cpp     \
                  WebmFrame.cpp       \
                  WebmFrameThread.cpp \
                  WebmWriter.cpp


LOCAL_C_INCLUDES += $(TOP)/frameworks/av/include

LOCAL_SHARED_LIBRARIES += libstagefright_foundation \
                          libstagefright \
                          libutils \
                          liblog

LOCAL_MODULE:= libstagefright_webm

include $(BUILD_STATIC_LIBRARY)
