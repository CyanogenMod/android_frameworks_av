/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0
#define LOG_TAG "MediaCodecListOverrides_test"
#include <utils/Log.h>

#include <gtest/gtest.h>

#include "MediaCodecListOverrides.h"

#include <media/MediaCodecInfo.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodecList.h>

namespace android {

static const char kTestOverridesStr[] =
"<MediaCodecs>\n"
"    <Settings>\n"
"        <Setting name=\"max-max-supported-instances\" value=\"8\" update=\"true\" />\n"
"    </Settings>\n"
"    <Encoders>\n"
"        <MediaCodec name=\"OMX.qcom.video.encoder.mpeg4\" type=\"video/mp4v-es\" update=\"true\" >\n"
"            <Quirk name=\"requires-allocate-on-input-ports\" />\n"
"            <Limit name=\"bitrate\" range=\"1-20000000\" />\n"
"            <Feature name=\"can-swap-width-height\" />\n"
"        </MediaCodec>\n"
"    </Encoders>\n"
"    <Decoders>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.avc\" type=\"video/avc\" update=\"true\" >\n"
"            <Quirk name=\"requires-allocate-on-input-ports\" />\n"
"            <Limit name=\"size\" min=\"64x64\" max=\"1920x1088\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.mpeg2\" type=\"different_mime\" update=\"true\" >\n"
"        </MediaCodec>\n"
"    </Decoders>\n"
"</MediaCodecs>\n";

static const char kTestOverridesStrNew1[] =
"<MediaCodecs>\n"
"    <Settings>\n"
"        <Setting name=\"max-max-supported-instances\" value=\"8\" update=\"true\" />\n"
"    </Settings>\n"
"    <Encoders>\n"
"        <MediaCodec name=\"OMX.qcom.video.encoder.avc\" type=\"video/avc\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"4\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.encoder.mpeg4\" type=\"video/mp4v-es\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"4\" />\n"
"            <Quirk name=\"requires-allocate-on-input-ports\" />\n"
"            <Limit name=\"bitrate\" range=\"1-20000000\" />\n"
"            <Feature name=\"can-swap-width-height\" />\n"
"        </MediaCodec>\n"
"    </Encoders>\n"
"    <Decoders>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.mpeg4\" type=\"video/mp4v-es\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"3\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.h263\" type=\"video/3gpp\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"4\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.avc.secure\" type=\"video/avc\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"1\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.avc\" type=\"video/avc\" update=\"true\" >\n"
"            <Quirk name=\"requires-allocate-on-input-ports\" />\n"
"            <Limit name=\"size\" min=\"64x64\" max=\"1920x1088\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.mpeg2\" type=\"different_mime\" update=\"true\" >\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.mpeg2\" type=\"video/mpeg2\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"3\" />\n"
"        </MediaCodec>\n"
"    </Decoders>\n"
"</MediaCodecs>\n";

static const char kTestOverridesStrNew2[] =
"\n"
"<MediaCodecs>\n"
"    <Encoders>\n"
"        <MediaCodec name=\"OMX.qcom.video.encoder.mpeg4\" type=\"video/mp4v-es\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"4\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.encoder.avc\" type=\"video/avc\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"4\" />\n"
"        </MediaCodec>\n"
"    </Encoders>\n"
"    <Decoders>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.mpeg4\" type=\"video/mp4v-es\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"3\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.mpeg2\" type=\"video/mpeg2\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"3\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.h263\" type=\"video/3gpp\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"4\" />\n"
"        </MediaCodec>\n"
"        <MediaCodec name=\"OMX.qcom.video.decoder.avc.secure\" type=\"video/avc\" update=\"true\" >\n"
"            <Limit name=\"max-supported-instances\" value=\"1\" />\n"
"        </MediaCodec>\n"
"    </Decoders>\n"
"</MediaCodecs>\n";

class MediaCodecListOverridesTest : public ::testing::Test {
public:
    MediaCodecListOverridesTest() {}

    void verifyOverrides(const KeyedVector<AString, CodecSettings> &overrides) {
        EXPECT_EQ(3u, overrides.size());

        EXPECT_TRUE(overrides.keyAt(0) == "OMX.qcom.video.decoder.avc video/avc decoder");
        const CodecSettings &settings0 = overrides.valueAt(0);
        EXPECT_EQ(1u, settings0.size());
        EXPECT_TRUE(settings0.keyAt(0) == "max-supported-instances");
        EXPECT_TRUE(settings0.valueAt(0) == "4");

        EXPECT_TRUE(overrides.keyAt(1) == "OMX.qcom.video.encoder.avc video/avc encoder");
        const CodecSettings &settings1 = overrides.valueAt(1);
        EXPECT_EQ(1u, settings1.size());
        EXPECT_TRUE(settings1.keyAt(0) == "max-supported-instances");
        EXPECT_TRUE(settings1.valueAt(0) == "3");

        EXPECT_TRUE(overrides.keyAt(2) == "global");
        const CodecSettings &settings2 = overrides.valueAt(2);
        EXPECT_EQ(3u, settings2.size());
        EXPECT_TRUE(settings2.keyAt(0) == "max-max-supported-instances");
        EXPECT_TRUE(settings2.valueAt(0) == "8");
        EXPECT_TRUE(settings2.keyAt(1) == "supports-multiple-secure-codecs");
        EXPECT_TRUE(settings2.valueAt(1) == "false");
        EXPECT_TRUE(settings2.keyAt(2) == "supports-secure-with-non-secure-codec");
        EXPECT_TRUE(settings2.valueAt(2) == "true");
    }

    void verifySetting(const sp<AMessage> &details, const char *name, const char *value) {
        AString value1;
        EXPECT_TRUE(details->findString(name, &value1));
        EXPECT_TRUE(value1 == value);
    }

    void createTestInfos(Vector<sp<MediaCodecInfo>> *infos) {
        const char *name = "OMX.qcom.video.decoder.avc";
        const bool encoder = false;
        const char *mime = "video/avc";
        sp<MediaCodecInfo> info = new MediaCodecInfo(name, encoder, mime);
        infos->push_back(info);
        const sp<MediaCodecInfo::Capabilities> caps = info->getCapabilitiesFor(mime);
        const sp<AMessage> details = caps->getDetails();
        details->setString("cap1", "value1");
        details->setString("max-max-supported-instances", "16");

        info = new MediaCodecInfo("anothercodec", true, "anothermime");
        infos->push_back(info);
    }

    void addMaxInstancesSetting(
            const AString &key,
            const AString &value,
            KeyedVector<AString, CodecSettings> *results) {
        CodecSettings settings;
        settings.add("max-supported-instances", value);
        results->add(key, settings);
    }

    void exportTestResultsToXML(const char *fileName) {
        KeyedVector<AString, CodecSettings> r;
        addMaxInstancesSetting("OMX.qcom.video.decoder.avc.secure video/avc decoder", "1", &r);
        addMaxInstancesSetting("OMX.qcom.video.decoder.h263 video/3gpp decoder", "4", &r);
        addMaxInstancesSetting("OMX.qcom.video.decoder.mpeg2 video/mpeg2 decoder", "3", &r);
        addMaxInstancesSetting("OMX.qcom.video.decoder.mpeg4 video/mp4v-es decoder", "3", &r);
        addMaxInstancesSetting("OMX.qcom.video.encoder.avc video/avc encoder", "4", &r);
        addMaxInstancesSetting("OMX.qcom.video.encoder.mpeg4 video/mp4v-es encoder", "4", &r);

        exportResultsToXML(fileName, r);
    }
};

TEST_F(MediaCodecListOverridesTest, splitString) {
    AString s = "abc123";
    AString delimiter = " ";
    AString s1;
    AString s2;
    EXPECT_FALSE(splitString(s, delimiter, &s1, &s2));
    s = "abc 123";
    EXPECT_TRUE(splitString(s, delimiter, &s1, &s2));
    EXPECT_TRUE(s1 == "abc");
    EXPECT_TRUE(s2 == "123");

    s = "abc123xyz";
    delimiter = ",";
    AString s3;
    EXPECT_FALSE(splitString(s, delimiter, &s1, &s2, &s3));
    s = "abc,123xyz";
    EXPECT_FALSE(splitString(s, delimiter, &s1, &s2, &s3));
    s = "abc,123,xyz";
    EXPECT_TRUE(splitString(s, delimiter, &s1, &s2, &s3));
    EXPECT_TRUE(s1 == "abc");
    EXPECT_TRUE(s2 == "123" );
    EXPECT_TRUE(s3 == "xyz");
}

// TODO: the codec component never returns OMX_EventCmdComplete in unit test.
TEST_F(MediaCodecListOverridesTest, DISABLED_profileCodecs) {
    sp<IMediaCodecList> list = MediaCodecList::getInstance();
    Vector<sp<MediaCodecInfo>> infos;
    for (size_t i = 0; i < list->countCodecs(); ++i) {
        infos.push_back(list->getCodecInfo(i));
    }
    KeyedVector<AString, CodecSettings> results;
    profileCodecs(infos, &results, true /* forceToMeasure */);
    EXPECT_LT(0u, results.size());
    for (size_t i = 0; i < results.size(); ++i) {
        AString key = results.keyAt(i);
        CodecSettings settings = results.valueAt(i);
        EXPECT_EQ(1u, settings.size());
        EXPECT_TRUE(settings.keyAt(0) == "max-supported-instances");
        AString valueS = settings.valueAt(0);
        int32_t value = strtol(valueS.c_str(), NULL, 10);
        EXPECT_LT(0, value);
        ALOGV("profileCodecs results %s %s", key.c_str(), valueS.c_str());
    }
}

TEST_F(MediaCodecListOverridesTest, applyCodecSettings) {
    AString codecInfo = "OMX.qcom.video.decoder.avc video/avc decoder";
    Vector<sp<MediaCodecInfo>> infos;
    createTestInfos(&infos);
    CodecSettings settings;
    settings.add("max-supported-instances", "3");
    settings.add("max-max-supported-instances", "8");
    applyCodecSettings(codecInfo, settings, &infos);

    EXPECT_EQ(2u, infos.size());
    EXPECT_TRUE(AString(infos[0]->getCodecName()) == "OMX.qcom.video.decoder.avc");
    const sp<AMessage> details = infos[0]->getCapabilitiesFor("video/avc")->getDetails();
    verifySetting(details, "max-supported-instances", "3");
    verifySetting(details, "max-max-supported-instances", "8");

    EXPECT_TRUE(AString(infos[1]->getCodecName()) == "anothercodec");
    EXPECT_EQ(0u, infos[1]->getCapabilitiesFor("anothermime")->getDetails()->countEntries());
}

TEST_F(MediaCodecListOverridesTest, exportResultsToExistingFile) {
    const char *fileName = "/sdcard/mediacodec_list_overrides_test.xml";
    remove(fileName);

    FILE *f = fopen(fileName, "wb");
    if (f == NULL) {
        ALOGW("Failed to open %s for writing.", fileName);
        return;
    }
    EXPECT_EQ(
            strlen(kTestOverridesStr),
            fwrite(kTestOverridesStr, 1, strlen(kTestOverridesStr), f));
    fclose(f);

    exportTestResultsToXML(fileName);

    // verify
    AString overrides;
    f = fopen(fileName, "rb");
    ASSERT_TRUE(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buf = (char *)malloc(size);
    EXPECT_EQ((size_t)1, fread(buf, size, 1, f));
    overrides.setTo(buf, size);
    fclose(f);
    free(buf);

    EXPECT_TRUE(overrides == kTestOverridesStrNew1);

    remove(fileName);
}

TEST_F(MediaCodecListOverridesTest, exportResultsToEmptyFile) {
    const char *fileName = "/sdcard/mediacodec_list_overrides_test.xml";
    remove(fileName);

    exportTestResultsToXML(fileName);

    // verify
    AString overrides;
    FILE *f = fopen(fileName, "rb");
    ASSERT_TRUE(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buf = (char *)malloc(size);
    EXPECT_EQ((size_t)1, fread(buf, size, 1, f));
    overrides.setTo(buf, size);
    fclose(f);
    free(buf);

    EXPECT_TRUE(overrides == kTestOverridesStrNew2);

    remove(fileName);
}

} // namespace android
