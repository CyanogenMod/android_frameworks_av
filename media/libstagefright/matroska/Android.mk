LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                 \
        MatroskaExtractor.cpp

LOCAL_C_INCLUDES:= \
        $(TOP)/external/libvpx/libwebm \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/frameworks/av/media/libstagefright/include \

ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
        LOCAL_CFLAGS     += -DENABLE_AV_ENHANCEMENTS
        LOCAL_C_INCLUDES+= $(TOP)/$(call project-path-for,qcom-media)/mm-core/inc
endif

LOCAL_CFLAGS += -Wno-multichar -Werror

LOCAL_MODULE:= libstagefright_matroska

include $(BUILD_STATIC_LIBRARY)
