LOCAL_PATH:= $(call my-dir)

#
# libmediaplayerservice
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    ActivityManager.cpp         \
    Crypto.cpp                  \
    Drm.cpp                     \
    HDCP.cpp                    \
    MediaPlayerFactory.cpp      \
    MediaPlayerService.cpp      \
    MediaRecorderClient.cpp     \
    MetadataRetrieverClient.cpp \
    MidiFile.cpp                \
    MidiMetadataRetriever.cpp   \
    RemoteDisplay.cpp           \
    SharedLibrary.cpp           \
    StagefrightPlayer.cpp       \
    StagefrightRecorder.cpp     \
    TestPlayerStub.cpp          \

LOCAL_SHARED_LIBRARIES :=       \
    libbinder                   \
    libcamera_client            \
    libcutils                   \
    liblog                      \
    libdl                       \
    libgui                      \
    libmedia                    \
    libsonivox                  \
    libstagefright              \
    libstagefright_foundation   \
    libstagefright_httplive     \
    libstagefright_omx          \
    libstagefright_wfd          \
    libutils                    \
    libdl                       \
    libvorbisidec               \

LOCAL_STATIC_LIBRARIES :=       \
    libstagefright_nuplayer     \
    libstagefright_rtsp         \

LOCAL_C_INCLUDES :=                                                 \
    $(call include-path-for, graphics corecg)                       \
    $(TOP)/frameworks/av/media/libstagefright/include               \
    $(TOP)/frameworks/av/media/libstagefright/rtsp                  \
    $(TOP)/frameworks/av/media/libstagefright/wifi-display          \
    $(TOP)/frameworks/native/include/media/openmax                  \
    $(TOP)/external/tremolo/Tremolo                                 \

ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
    LOCAL_C_INCLUDES += \
        $(call project-path-for,qcom-media)/mm-core/inc
endif

ifeq ($(BOARD_HAS_MTK_HARDWARE),true)
    LOCAL_C_INCLUDES += \
        $(TOP)/hardware/mediatek/media/include
    LOCAL_STATIC_LIBRARIES += \
        libstagefright_bufferallocator
endif

LOCAL_MODULE:= libmediaplayerservice

ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
    LOCAL_CFLAGS += -DENABLE_AV_ENHANCEMENTS
    LOCAL_C_INCLUDES += $(TOP)/frameworks/av/include/media
    LOCAL_C_INCLUDES += \
        $(call project-path-for,qcom-media)/mm-core/inc
endif #TARGET_ENABLE_QC_AV_ENHANCEMENTS

ifeq ($(TARGET_BOARD_PLATFORM),msm7x27a)
    LOCAL_CFLAGS += -DUSE_SUBMIT_ONE_INPUT_BUFFER
endif

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
