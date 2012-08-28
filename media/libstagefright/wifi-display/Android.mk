LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        udptest.cpp                 \
        ANetworkSession.cpp         \
        ParsedMessage.cpp           \

LOCAL_SHARED_LIBRARIES:= \
        libbinder                       \
        libgui                          \
        libmedia                        \
        libstagefright                  \
        libstagefright_foundation       \
        libutils                        \

LOCAL_MODULE:= udptest

LOCAL_MODULE_TAGS := debug

include $(BUILD_EXECUTABLE)
