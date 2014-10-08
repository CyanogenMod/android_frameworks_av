/*
 * Copyright (C) 2010 The Android Open Source Project
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
#define LOG_TAG "AH263Assembler"
#include <utils/Log.h>

#include "AH263Assembler.h"

#include "ARTPSource.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/Utils.h>

namespace android {

AH263Assembler::AH263Assembler(const sp<AMessage> &notify)
    : mNotifyMsg(notify),
      mAccessUnitRTPTime(0),
      mNextExpectedSeqNoValid(false),
      mNextExpectedSeqNo(0),
      mAccessUnitDamaged(false) {
}

AH263Assembler::~AH263Assembler() {
}

ARTPAssembler::AssemblyStatus AH263Assembler::assembleMore(
        const sp<ARTPSource> &source) {
    AssemblyStatus status = addPacket(source);
    if (status == MALFORMED_PACKET) {
        mAccessUnitDamaged = true;
    }
    return status;
}

ARTPAssembler::AssemblyStatus AH263Assembler::addPacket(
        const sp<ARTPSource> &source) {
    List<sp<ABuffer> > *queue = source->queue();

    if (queue->empty()) {
        return NOT_ENOUGH_DATA;
    }

    if (mNextExpectedSeqNoValid) {
        List<sp<ABuffer> >::iterator it = queue->begin();
        while (it != queue->end()) {
            if ((uint32_t)(*it)->int32Data() >= mNextExpectedSeqNo) {
                break;
            }

            //check whether the rtp time of this later packet is equal
            //to the current one, if yes, it means this packet belongs
            //to the candidate access unit and should be inserted.
            uint32_t rtpTime;
            bool ret = (*it)->meta()->findInt32("rtp-time", (int32_t *)&rtpTime);
            if (ret && mPackets.size() > 0 && rtpTime == mAccessUnitRTPTime) {
                ALOGV("insert the rtp packet into the candidate access unit");
                insertPacket(*it);
            }
            it = queue->erase(it);
        }

        if (queue->empty()) {
            return NOT_ENOUGH_DATA;
        }
    }

    sp<ABuffer> buffer = *queue->begin();

    if (!mNextExpectedSeqNoValid) {
        mNextExpectedSeqNoValid = true;
        mNextExpectedSeqNo = (uint32_t)buffer->int32Data();
    } else if ((uint32_t)buffer->int32Data() != mNextExpectedSeqNo) {
#if VERBOSE
        LOG(VERBOSE) << "Not the sequence number I expected";
#endif

        return WRONG_SEQUENCE_NUMBER;
    }

    uint32_t rtpTime;
    CHECK(buffer->meta()->findInt32("rtp-time", (int32_t *)&rtpTime));

    if (mPackets.size() > 0 && rtpTime != mAccessUnitRTPTime) {
        submitAccessUnit();
    }
    mAccessUnitRTPTime = rtpTime;

    // hexdump(buffer->data(), buffer->size());

    size_t skip;
    if ((skip = getOffsetOfHeader(buffer)) == 1){
        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;
        return MALFORMED_PACKET;
    }

    buffer->setRange(buffer->offset() + skip, buffer->size() - skip);

    if (skip == 0) {
        buffer->data()[0] = 0x00;
        buffer->data()[1] = 0x00;
    }

    mPackets.push_back(buffer);

    queue->erase(queue->begin());
    ++mNextExpectedSeqNo;

    return OK;
}

void AH263Assembler::submitAccessUnit() {
    CHECK(!mPackets.empty());

#if VERBOSE
    LOG(VERBOSE) << "Access unit complete (" << mPackets.size() << " packets)";
#endif

    size_t totalSize = 0;
    List<sp<ABuffer> >::iterator it = mPackets.begin();
    while (it != mPackets.end()) {
        const sp<ABuffer> &unit = *it;

        totalSize += unit->size();
        ++it;
    }

    sp<ABuffer> accessUnit = new ABuffer(totalSize);
    size_t offset = 0;
    it = mPackets.begin();
    while (it != mPackets.end()) {
        const sp<ABuffer> &unit = *it;

        memcpy((uint8_t *)accessUnit->data() + offset,
               unit->data(), unit->size());

        offset += unit->size();

        ++it;
    }

    CopyTimes(accessUnit, *mPackets.begin());

#if 0
    printf(mAccessUnitDamaged ? "X" : ".");
    fflush(stdout);
#endif

    if (mAccessUnitDamaged) {
        accessUnit->meta()->setInt32("damaged", true);
    }

    mPackets.clear();
    mAccessUnitDamaged = false;

    sp<AMessage> msg = mNotifyMsg->dup();
    msg->setBuffer("access-unit", accessUnit);
    msg->post();
}

void AH263Assembler::packetLost() {
    CHECK(mNextExpectedSeqNoValid);
    ++mNextExpectedSeqNo;

    mAccessUnitDamaged = true;
}

void AH263Assembler::onByeReceived() {
    sp<AMessage> msg = mNotifyMsg->dup();
    msg->setInt32("eos", true);
    msg->post();
}

size_t AH263Assembler::getOffsetOfHeader(const sp<ABuffer> buffer) {
    //the final right offset should be 0 or 2, it is
    //initialized to 1 for checking whether errors happen
    size_t offset = 1;

    if (buffer->size() < 2) {
        ALOGW("Packet size is less than 2 bytes");
        return offset;
    }

    unsigned payloadHeader = U16_AT(buffer->data());
    unsigned P = (payloadHeader >> 10) & 1;
    unsigned V = (payloadHeader >> 9) & 1;
    unsigned PLEN = (payloadHeader >> 3) & 0x3f;
    unsigned PEBIT = payloadHeader & 7;

    // V=0
    if (V != 0u) {
        ALOGW("Packet discarded due to VRC (V != 0)");
        return offset;
    }

    // PLEN=0
    if (PLEN != 0u) {
        ALOGW("Packet discarded (PLEN != 0)");
        return offset;
    }

    // PEBIT=0
    if (PEBIT != 0u) {
        ALOGW("Packet discarded (PEBIT != 0)");
        return offset;
    }
    offset = V + PLEN + (P ? 0 : 2);
    return offset;
}

void AH263Assembler::insertPacket(const sp<ABuffer> &buffer){
    size_t skip;
    if ((skip = getOffsetOfHeader(buffer)) == 1){
        ALOGE("Malformed packet in insertPacket");
        return;
    }

    buffer->setRange(buffer->offset() + skip, buffer->size() - skip);

    if (skip == 0) {
        buffer->data()[0] = 0x00;
        buffer->data()[1] = 0x00;
    }
    uint32_t seqNum = (uint32_t)buffer->int32Data();
    List<sp<ABuffer> >::iterator it = mPackets.begin();
    while (it != mPackets.end() && (uint32_t)(*it)->int32Data() < seqNum){
        ++it;
    }

    if (it != mPackets.end() && (uint32_t)(*it)->int32Data() == seqNum) {
        ALOGE("Discarding duplicate buffer in mPackets");
        return;
    }
    ALOGV("insert the buffer into the current packets");
    mPackets.insert(it, buffer);
}
}  // namespace android

