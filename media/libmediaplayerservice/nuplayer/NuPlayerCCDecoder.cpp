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
#define LOG_TAG "NuPlayerCCDecoder"
#include <utils/Log.h>
#include <inttypes.h>

#include "avc_utils.h"
#include "NuPlayerCCDecoder.h"

#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaDefs.h>

namespace android {

struct CCData {
    CCData(uint8_t type, uint8_t data1, uint8_t data2)
        : mType(type), mData1(data1), mData2(data2) {
    }
    bool getChannel(size_t *channel) const {
        if (mData1 >= 0x10 && mData1 <= 0x1f) {
            *channel = (mData1 >= 0x18 ? 1 : 0) + (mType ? 2 : 0);
            return true;
        }
        return false;
    }

    uint8_t mType;
    uint8_t mData1;
    uint8_t mData2;
};

static bool isNullPad(CCData *cc) {
    return cc->mData1 < 0x10 && cc->mData2 < 0x10;
}

static void dumpBytePair(const sp<ABuffer> &ccBuf) __attribute__ ((unused));
static void dumpBytePair(const sp<ABuffer> &ccBuf) {
    size_t offset = 0;
    AString out;

    while (offset < ccBuf->size()) {
        char tmp[128];

        CCData *cc = (CCData *) (ccBuf->data() + offset);

        if (isNullPad(cc)) {
            // 1 null pad or XDS metadata, ignore
            offset += sizeof(CCData);
            continue;
        }

        if (cc->mData1 >= 0x20 && cc->mData1 <= 0x7f) {
            // 2 basic chars
            sprintf(tmp, "[%d]Basic: %c %c", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 == 0x11 || cc->mData1 == 0x19)
                 && cc->mData2 >= 0x30 && cc->mData2 <= 0x3f) {
            // 1 special char
            sprintf(tmp, "[%d]Special: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 == 0x12 || cc->mData1 == 0x1A)
                 && cc->mData2 >= 0x20 && cc->mData2 <= 0x3f){
            // 1 Spanish/French char
            sprintf(tmp, "[%d]Spanish: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 == 0x13 || cc->mData1 == 0x1B)
                 && cc->mData2 >= 0x20 && cc->mData2 <= 0x3f){
            // 1 Portuguese/German/Danish char
            sprintf(tmp, "[%d]German: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 == 0x11 || cc->mData1 == 0x19)
                 && cc->mData2 >= 0x20 && cc->mData2 <= 0x2f){
            // Mid-Row Codes (Table 69)
            sprintf(tmp, "[%d]Mid-row: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if (((cc->mData1 == 0x14 || cc->mData1 == 0x1c)
                  && cc->mData2 >= 0x20 && cc->mData2 <= 0x2f)
                  ||
                   ((cc->mData1 == 0x17 || cc->mData1 == 0x1f)
                  && cc->mData2 >= 0x21 && cc->mData2 <= 0x23)){
            // Misc Control Codes (Table 70)
            sprintf(tmp, "[%d]Ctrl: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 & 0x70) == 0x10
                && (cc->mData2 & 0x40) == 0x40
                && ((cc->mData1 & 0x07) || !(cc->mData2 & 0x20)) ) {
            // Preamble Address Codes (Table 71)
            sprintf(tmp, "[%d]PAC: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else {
            sprintf(tmp, "[%d]Invalid: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        }

        if (out.size() > 0) {
            out.append(", ");
        }

        out.append(tmp);

        offset += sizeof(CCData);
    }

    ALOGI("%s", out.c_str());
}

NuPlayer::CCDecoder::CCDecoder(const sp<AMessage> &notify)
    : mNotify(notify),
      mCurrentChannel(0),
      mSelectedTrack(-1) {
      for (size_t i = 0; i < sizeof(mTrackIndices)/sizeof(mTrackIndices[0]); ++i) {
          mTrackIndices[i] = -1;
      }
}

size_t NuPlayer::CCDecoder::getTrackCount() const {
    return mFoundChannels.size();
}

sp<AMessage> NuPlayer::CCDecoder::getTrackInfo(size_t index) const {
    if (!isTrackValid(index)) {
        return NULL;
    }

    sp<AMessage> format = new AMessage();

    format->setInt32("type", MEDIA_TRACK_TYPE_SUBTITLE);
    format->setString("language", "und");
    format->setString("mime", MEDIA_MIMETYPE_TEXT_CEA_608);
    //CC1, field 0 channel 0
    bool isDefaultAuto = (mFoundChannels[index] == 0);
    format->setInt32("auto", isDefaultAuto);
    format->setInt32("default", isDefaultAuto);
    format->setInt32("forced", 0);

    return format;
}

status_t NuPlayer::CCDecoder::selectTrack(size_t index, bool select) {
    if (!isTrackValid(index)) {
        return BAD_VALUE;
    }

    if (select) {
        if (mSelectedTrack == (ssize_t)index) {
            ALOGE("track %zu already selected", index);
            return BAD_VALUE;
        }
        ALOGV("selected track %zu", index);
        mSelectedTrack = index;
    } else {
        if (mSelectedTrack != (ssize_t)index) {
            ALOGE("track %zu is not selected", index);
            return BAD_VALUE;
        }
        ALOGV("unselected track %zu", index);
        mSelectedTrack = -1;
    }

    return OK;
}

bool NuPlayer::CCDecoder::isSelected() const {
    return mSelectedTrack >= 0 && mSelectedTrack < (int32_t) getTrackCount();
}

bool NuPlayer::CCDecoder::isTrackValid(size_t index) const {
    return index < getTrackCount();
}

int32_t NuPlayer::CCDecoder::getTrackIndex(size_t channel) const {
    if (channel < sizeof(mTrackIndices)/sizeof(mTrackIndices[0])) {
        return mTrackIndices[channel];
    }
    return -1;
}

// returns true if a new CC track is found
bool NuPlayer::CCDecoder::extractFromSEI(const sp<ABuffer> &accessUnit) {
    sp<ABuffer> sei;
    if (!accessUnit->meta()->findBuffer("sei", &sei) || sei == NULL) {
        return false;
    }

    int64_t timeUs;
    CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

    bool trackAdded = false;

    const NALPosition *nal = (NALPosition *) sei->data();

    for (size_t i = 0; i < sei->size() / sizeof(NALPosition); ++i, ++nal) {
        trackAdded |= parseSEINalUnit(
                timeUs, accessUnit->data() + nal->nalOffset, nal->nalSize);
    }

    return trackAdded;
}

// returns true if a new CC track is found
bool NuPlayer::CCDecoder::parseSEINalUnit(
        int64_t timeUs, const uint8_t *nalStart, size_t nalSize) {
    unsigned nalType = nalStart[0] & 0x1f;

    // the buffer should only have SEI in it
    if (nalType != 6) {
        return false;
    }

    bool trackAdded = false;
    NALBitReader br(nalStart + 1, nalSize - 1);
    // sei_message()
    while (br.atLeastNumBitsLeft(16)) { // at least 16-bit for sei_message()
        uint32_t payload_type = 0;
        size_t payload_size = 0;
        uint8_t last_byte;

        do {
            last_byte = br.getBits(8);
            payload_type += last_byte;
        } while (last_byte == 0xFF);

        do {
            last_byte = br.getBits(8);
            payload_size += last_byte;
        } while (last_byte == 0xFF);

        if (payload_size > SIZE_MAX / 8
                || !br.atLeastNumBitsLeft(payload_size * 8)) {
            ALOGV("Malformed SEI payload");
            break;
        }

        // sei_payload()
        if (payload_type == 4) {
            bool isCC = false;
            if (payload_size > 1 + 2 + 4 + 1) {
                // user_data_registered_itu_t_t35()

                // ATSC A/72: 6.4.2
                uint8_t itu_t_t35_country_code = br.getBits(8);
                uint16_t itu_t_t35_provider_code = br.getBits(16);
                uint32_t user_identifier = br.getBits(32);
                uint8_t user_data_type_code = br.getBits(8);

                payload_size -= 1 + 2 + 4 + 1;

                isCC = itu_t_t35_country_code == 0xB5
                        && itu_t_t35_provider_code == 0x0031
                        && user_identifier == 'GA94'
                        && user_data_type_code == 0x3;
            }

            if (isCC && payload_size > 2) {
                // MPEG_cc_data()
                // ATSC A/53 Part 4: 6.2.3.1
                br.skipBits(1); //process_em_data_flag
                bool process_cc_data_flag = br.getBits(1);
                br.skipBits(1); //additional_data_flag
                size_t cc_count = br.getBits(5);
                br.skipBits(8); // em_data;
                payload_size -= 2;

                if (process_cc_data_flag) {
                    AString out;

                    sp<ABuffer> ccBuf = new ABuffer(cc_count * sizeof(CCData));
                    ccBuf->setRange(0, 0);

                    for (size_t i = 0; i < cc_count && payload_size >= 3; i++) {
                        uint8_t marker = br.getBits(5);
                        CHECK_EQ(marker, 0x1f);

                        bool cc_valid = br.getBits(1);
                        uint8_t cc_type = br.getBits(2);
                        // remove odd parity bit
                        uint8_t cc_data_1 = br.getBits(8) & 0x7f;
                        uint8_t cc_data_2 = br.getBits(8) & 0x7f;

                        payload_size -= 3;

                        if (cc_valid
                                && (cc_type == 0 || cc_type == 1)) {
                            CCData cc(cc_type, cc_data_1, cc_data_2);
                            if (!isNullPad(&cc)) {
                                size_t channel;
                                if (cc.getChannel(&channel) && getTrackIndex(channel) < 0) {
                                    mTrackIndices[channel] = mFoundChannels.size();
                                    mFoundChannels.push_back(channel);
                                    trackAdded = true;
                                }
                                memcpy(ccBuf->data() + ccBuf->size(),
                                        (void *)&cc, sizeof(cc));
                                ccBuf->setRange(0, ccBuf->size() + sizeof(CCData));
                            }
                        }
                    }

                    mCCMap.add(timeUs, ccBuf);
                    break;
                }
            } else {
                ALOGV("Malformed SEI payload type 4");
            }
        } else {
            ALOGV("Unsupported SEI payload type %d", payload_type);
        }

        // skipping remaining bits of this payload
        br.skipBits(payload_size * 8);
    }

    return trackAdded;
}

sp<ABuffer> NuPlayer::CCDecoder::filterCCBuf(
        const sp<ABuffer> &ccBuf, size_t index) {
    sp<ABuffer> filteredCCBuf = new ABuffer(ccBuf->size());
    filteredCCBuf->setRange(0, 0);

    size_t cc_count = ccBuf->size() / sizeof(CCData);
    const CCData* cc_data = (const CCData*)ccBuf->data();
    for (size_t i = 0; i < cc_count; ++i) {
        size_t channel;
        if (cc_data[i].getChannel(&channel)) {
            mCurrentChannel = channel;
        }
        if (mCurrentChannel == mFoundChannels[index]) {
            memcpy(filteredCCBuf->data() + filteredCCBuf->size(),
                    (void *)&cc_data[i], sizeof(CCData));
            filteredCCBuf->setRange(0, filteredCCBuf->size() + sizeof(CCData));
        }
    }

    return filteredCCBuf;
}

void NuPlayer::CCDecoder::decode(const sp<ABuffer> &accessUnit) {
    if (extractFromSEI(accessUnit)) {
        ALOGI("Found CEA-608 track");
        sp<AMessage> msg = mNotify->dup();
        msg->setInt32("what", kWhatTrackAdded);
        msg->post();
    }
    // TODO: extract CC from other sources
}

void NuPlayer::CCDecoder::display(int64_t timeUs) {
    if (!isTrackValid(mSelectedTrack)) {
        ALOGE("Could not find current track(index=%d)", mSelectedTrack);
        return;
    }

    ssize_t index = mCCMap.indexOfKey(timeUs);
    if (index < 0) {
        ALOGV("cc for timestamp %" PRId64 " not found", timeUs);
        return;
    }

    sp<ABuffer> ccBuf = filterCCBuf(mCCMap.valueAt(index), mSelectedTrack);

    if (ccBuf->size() > 0) {
#if 0
        dumpBytePair(ccBuf);
#endif

        ccBuf->meta()->setInt32("trackIndex", mSelectedTrack);
        ccBuf->meta()->setInt64("timeUs", timeUs);
        ccBuf->meta()->setInt64("durationUs", 0ll);

        sp<AMessage> msg = mNotify->dup();
        msg->setInt32("what", kWhatClosedCaptionData);
        msg->setBuffer("buffer", ccBuf);
        msg->post();
    }

    // remove all entries before timeUs
    mCCMap.removeItemsAt(0, index + 1);
}

void NuPlayer::CCDecoder::flush() {
    mCCMap.clear();
}

}  // namespace android

