/*
 * Copyright 2014 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "Utils_test"

#include <gtest/gtest.h>
#include <utils/String8.h>
#include <utils/Errors.h>
#include <fcntl.h>
#include <unistd.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AStringUtils.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/Utils.h>

namespace android {

class UtilsTest : public ::testing::Test {
};

TEST_F(UtilsTest, TestStringUtils) {
    ASSERT_EQ(AStringUtils::Compare("Audio", "AudioExt", 5, false), 0);
    ASSERT_EQ(AStringUtils::Compare("Audio", "audiOExt", 5, true), 0);
    ASSERT_NE(AStringUtils::Compare("Audio", "audioExt", 5, false), 0);
    ASSERT_NE(AStringUtils::Compare("Audio", "AudiOExt", 5, false), 0);

    ASSERT_LT(AStringUtils::Compare("Audio", "AudioExt", 7, false), 0);
    ASSERT_LT(AStringUtils::Compare("Audio", "audiOExt", 7, true), 0);

    ASSERT_GT(AStringUtils::Compare("AudioExt", "Audio", 7, false), 0);
    ASSERT_GT(AStringUtils::Compare("audiOext", "Audio", 7, true), 0);

    ASSERT_LT(AStringUtils::Compare("Audio", "Video", 5, false), 0);
    ASSERT_LT(AStringUtils::Compare("Audio1", "Audio2", 6, false), 0);
    ASSERT_LT(AStringUtils::Compare("audio", "VIDEO", 5, true), 0);
    ASSERT_LT(AStringUtils::Compare("audio1", "AUDIO2", 6, true), 0);

    ASSERT_GT(AStringUtils::Compare("Video", "Audio", 5, false), 0);
    ASSERT_GT(AStringUtils::Compare("Audio2", "Audio1", 6, false), 0);
    ASSERT_GT(AStringUtils::Compare("VIDEO", "audio", 5, true), 0);
    ASSERT_GT(AStringUtils::Compare("AUDIO2", "audio1", 6, true), 0);

    ASSERT_TRUE(AStringUtils::MatchesGlob("AudioA", 5, "AudioB", 5, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("AudioA", 6, "AudioA", 5, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("AudioA", 5, "AudioA", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("AudioA", 5, "audiOB", 5, false));
    ASSERT_TRUE(AStringUtils::MatchesGlob("AudioA", 5, "audiOB", 5, true));
    ASSERT_FALSE(AStringUtils::MatchesGlob("AudioA", 6, "AudioA", 5, true));
    ASSERT_FALSE(AStringUtils::MatchesGlob("AudioA", 5, "AudioA", 6, true));

    ASSERT_TRUE(AStringUtils::MatchesGlob("*1", 1, "String8", 6, true));
    ASSERT_TRUE(AStringUtils::MatchesGlob("*1", 1, "String8", 6, false));
    ASSERT_TRUE(AStringUtils::MatchesGlob("*1", 1, "String8", 0, true));
    ASSERT_TRUE(AStringUtils::MatchesGlob("*1", 1, "String8", 0, false));

    ASSERT_TRUE(AStringUtils::MatchesGlob("*ring1", 5, "String8", 6, false));
    ASSERT_TRUE(AStringUtils::MatchesGlob("*ring2", 5, "STRING8", 6, true));
    ASSERT_FALSE(AStringUtils::MatchesGlob("*ring4", 5, "StRing8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("*ring5", 5, "StrinG8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("*ring8", 5, "String8", 7, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("*ring8", 5, "String8", 7, true));

    ASSERT_TRUE(AStringUtils::MatchesGlob("Str*1", 4, "String8", 6, false));
    ASSERT_TRUE(AStringUtils::MatchesGlob("Str*2", 4, "STRING8", 6, true));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*3", 4, "string8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*4", 4, "StRing8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*5", 4, "AString8", 7, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*6", 4, "AString8", 7, true));

    ASSERT_TRUE(AStringUtils::MatchesGlob("Str*ng1", 6, "String8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ng2", 6, "string8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ng3", 6, "StRing8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ng4", 6, "StriNg8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ng5", 6, "StrinG8", 6, false));
    ASSERT_TRUE(AStringUtils::MatchesGlob("Str*ng6", 6, "STRING8", 6, true));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ng8", 6, "AString8", 7, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ng1", 6, "String16", 7, false));
    ASSERT_TRUE(AStringUtils::MatchesGlob("Str*ing9", 7, "String8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ringA", 8, "String8", 6, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ng8", 6, "AString8", 7, true));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ng1", 6, "String16", 7, true));
    ASSERT_TRUE(AStringUtils::MatchesGlob("Str*ing9", 7, "STRING8", 6, true));
    ASSERT_FALSE(AStringUtils::MatchesGlob("Str*ringA", 8, "String8", 6, true));

    ASSERT_TRUE(AStringUtils::MatchesGlob("*str*str1", 8, "bestrestroom", 9, false));
    ASSERT_TRUE(AStringUtils::MatchesGlob("*str*str1", 8, "bestrestrestroom", 13, false));
    ASSERT_FALSE(AStringUtils::MatchesGlob("*str*stro", 8, "bestrestrestroom", 14, false));
    ASSERT_TRUE(AStringUtils::MatchesGlob("*str*str*1", 9, "bestrestrestroom", 14, false));
    ASSERT_TRUE(AStringUtils::MatchesGlob("*str*str1", 8, "beSTReSTRoom", 9, true));
    ASSERT_TRUE(AStringUtils::MatchesGlob("*str*str1", 8, "beSTRestreSTRoom", 13, true));
    ASSERT_FALSE(AStringUtils::MatchesGlob("*str*stro", 8, "bestreSTReSTRoom", 14, true));
    ASSERT_TRUE(AStringUtils::MatchesGlob("*str*str*1", 9, "bestreSTReSTRoom", 14, true));
}

TEST_F(UtilsTest, TestDebug) {
#define LVL(x) (ADebug::Level)(x)
    ASSERT_EQ(ADebug::GetDebugLevelFromString("video", "", LVL(5)), LVL(5));
    ASSERT_EQ(ADebug::GetDebugLevelFromString("video", "   \t  \n ", LVL(2)), LVL(2));
    ASSERT_EQ(ADebug::GetDebugLevelFromString("video", "3", LVL(5)), LVL(3));
    ASSERT_EQ(ADebug::GetDebugLevelFromString("video", "3:*deo", LVL(5)), LVL(3));
    ASSERT_EQ(ADebug::GetDebugLevelFromString(
            "video", "\t\n 3 \t\n:\t\n video \t\n", LVL(5)), LVL(3));
    ASSERT_EQ(ADebug::GetDebugLevelFromString("video", "3:*deo,2:vid*", LVL(5)), LVL(2));
    ASSERT_EQ(ADebug::GetDebugLevelFromString(
            "avideo", "\t\n 3 \t\n:\t\n avideo \t\n,\t\n 2 \t\n:\t\n video \t\n", LVL(5)), LVL(3));
    ASSERT_EQ(ADebug::GetDebugLevelFromString(
            "audio.omx", "4:*omx,3:*d*o*,2:audio*", LVL(5)), LVL(2));
    ASSERT_EQ(ADebug::GetDebugLevelFromString(
            "video.omx", "4:*omx,3:*d*o*,2:audio*", LVL(5)), LVL(3));
    ASSERT_EQ(ADebug::GetDebugLevelFromString("video", "4:*omx,3:*d*o*,2:audio*", LVL(5)), LVL(3));
    ASSERT_EQ(ADebug::GetDebugLevelFromString("omx", "4:*omx,3:*d*o*,2:audio*", LVL(5)), LVL(4));
#undef LVL
}

TEST_F(UtilsTest, TestFourCC) {
    ASSERT_EQ(FOURCC('s', 't', 'm' , 'u'), 'stmu');
}

TEST_F(UtilsTest, TestMathTemplates) {
    ASSERT_EQ(divRound(-10, -4), 3);
    ASSERT_EQ(divRound(-11, -4), 3);
    ASSERT_EQ(divRound(-12, -4), 3);
    ASSERT_EQ(divRound(-13, -4), 3);
    ASSERT_EQ(divRound(-14, -4), 4);

    ASSERT_EQ(divRound(10, -4), -3);
    ASSERT_EQ(divRound(11, -4), -3);
    ASSERT_EQ(divRound(12, -4), -3);
    ASSERT_EQ(divRound(13, -4), -3);
    ASSERT_EQ(divRound(14, -4), -4);

    ASSERT_EQ(divRound(-10, 4), -3);
    ASSERT_EQ(divRound(-11, 4), -3);
    ASSERT_EQ(divRound(-12, 4), -3);
    ASSERT_EQ(divRound(-13, 4), -3);
    ASSERT_EQ(divRound(-14, 4), -4);

    ASSERT_EQ(divRound(10, 4), 3);
    ASSERT_EQ(divRound(11, 4), 3);
    ASSERT_EQ(divRound(12, 4), 3);
    ASSERT_EQ(divRound(13, 4), 3);
    ASSERT_EQ(divRound(14, 4), 4);

    ASSERT_EQ(divUp(-11, -4), 3);
    ASSERT_EQ(divUp(-12, -4), 3);
    ASSERT_EQ(divUp(-13, -4), 4);

    ASSERT_EQ(divUp(11, -4), -2);
    ASSERT_EQ(divUp(12, -4), -3);
    ASSERT_EQ(divUp(13, -4), -3);

    ASSERT_EQ(divUp(-11, 4), -2);
    ASSERT_EQ(divUp(-12, 4), -3);
    ASSERT_EQ(divUp(-13, 4), -3);

    ASSERT_EQ(divUp(11, 4), 3);
    ASSERT_EQ(divUp(12, 4), 3);
    ASSERT_EQ(divUp(13, 4), 4);

    ASSERT_EQ(align(11, 4), 12);
    ASSERT_EQ(align(12, 4), 12);
    ASSERT_EQ(align(13, 4), 16);
    ASSERT_EQ(align(11, 8), 16);
    ASSERT_EQ(align(11, 2), 12);
    ASSERT_EQ(align(11, 1), 11);

    ASSERT_EQ(abs(5L), 5L);
    ASSERT_EQ(abs(-25), 25);

    ASSERT_EQ(min(5.6f, 6.0f), 5.6f);
    ASSERT_EQ(min(6.0f, 5.6f), 5.6f);
    ASSERT_EQ(min(-4.3, 8.6), -4.3);
    ASSERT_EQ(min(8.6, -4.3), -4.3);

    ASSERT_EQ(max(5.6f, 6.0f), 6.0f);
    ASSERT_EQ(max(6.0f, 5.6f), 6.0f);
    ASSERT_EQ(max(-4.3, 8.6), 8.6);
    ASSERT_EQ(max(8.6, -4.3), 8.6);

    ASSERT_EQ(periodicError(124, 100), 24);
    ASSERT_EQ(periodicError(288, 100), 12);
    ASSERT_EQ(periodicError(-345, 100), 45);
    ASSERT_EQ(periodicError(-493, 100), 7);
    ASSERT_EQ(periodicError(-550, 100), 50);
    ASSERT_EQ(periodicError(-600, 100), 0);
}

} // namespace android
