LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                          \
        stagefright/ExtendedMediaDefs.cpp  \
        stagefright/AVUtils.cpp            \
        stagefright/AVFactory.cpp          \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/ \
        $(TOP)/frameworks/av/media/libavextensions \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/external/flac/include \
        $(TOP)/hardware/qcom/media/mm-core/inc \
        $(TOP)/frameworks/av/media/libstagefright \
        $(TOP)/frameworks/av/media/libstagefright/mpeg2ts \

LOCAL_CFLAGS += -Wno-multichar -Werror

ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
       LOCAL_CFLAGS += -DENABLE_AV_ENHANCEMENTS
endif

LOCAL_MODULE:= libavextensions

LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)

########################################################

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                          \
        media/AVMediaUtils.cpp             \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/ \
        $(TOP)/frameworks/av/media/libavextensions \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/external/flac/include \
        $(TOP)/hardware/qcom/media/mm-core/inc

LOCAL_CFLAGS += -Wno-multichar -Werror

ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
       LOCAL_CFLAGS += -DENABLE_AV_ENHANCEMENTS
endif

LOCAL_MODULE:= libavmediaextentions

LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)

########################################################

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                                      \
        mediaplayerservice/AVMediaServiceFactory.cpp   \
        mediaplayerservice/AVMediaServiceUtils.cpp     \
        mediaplayerservice/AVNuFactory.cpp             \
        mediaplayerservice/AVNuUtils.cpp               \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/ \
        $(TOP)/frameworks/av/media/libmediaplayerservice \
        $(TOP)/frameworks/av/media/libavextensions \
        $(TOP)/frameworks/av/media/libstagefright/include \
        $(TOP)/frameworks/av/media/libstagefright/rtsp \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/external/flac/include \
        $(TOP)/hardware/qcom/media/mm-core/inc

LOCAL_CFLAGS += -Wno-multichar -Werror

ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
       LOCAL_CFLAGS += -DENABLE_AV_ENHANCEMENTS
endif

LOCAL_MODULE:= libavmediaserviceextensions

LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)

