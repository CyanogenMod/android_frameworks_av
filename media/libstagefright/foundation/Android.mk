LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                 \
    AAtomizer.cpp                 \
    ABitReader.cpp                \
    ABuffer.cpp                   \
    ADebug.cpp                    \
    AHandler.cpp                  \
    AHierarchicalStateMachine.cpp \
    ALooper.cpp                   \
    ALooperRoster.cpp             \
    AMessage.cpp                  \
    ANetworkSession.cpp           \
    AString.cpp                   \
    AStringUtils.cpp              \
    AWakeLock.cpp                 \
    ParsedMessage.cpp             \
    base64.cpp                    \
    hexdump.cpp

LOCAL_C_INCLUDES:= \
    frameworks/av/include/media/stagefright/foundation

LOCAL_SHARED_LIBRARIES := \
        libbinder         \
        libutils          \
        libcutils         \
        liblog            \
        libpowermanager

LOCAL_CFLAGS += -Wno-multichar -Werror

LOCAL_MODULE:= libstagefright_foundation



include $(BUILD_SHARED_LIBRARY)
