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

#define LOG_TAG "MtpDataPacket"

#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>

#include <usbhost/usbhost.h>

#include "MtpDataPacket.h"
#include "MtpStringBuffer.h"

namespace android {

MtpDataPacket::MtpDataPacket()
    :   MtpPacket(MTP_BUFFER_SIZE),   // MAX_USBFS_BUFFER_SIZE
        mOffset(MTP_CONTAINER_HEADER_SIZE)
{
}

MtpDataPacket::~MtpDataPacket() {
}

void MtpDataPacket::reset() {
    MtpPacket::reset();
    mOffset = MTP_CONTAINER_HEADER_SIZE;
}

void MtpDataPacket::setOperationCode(MtpOperationCode code) {
    MtpPacket::putUInt16(MTP_CONTAINER_CODE_OFFSET, code);
}

void MtpDataPacket::setTransactionID(MtpTransactionID id) {
    MtpPacket::putUInt32(MTP_CONTAINER_TRANSACTION_ID_OFFSET, id);
}

bool MtpDataPacket::getUInt8(uint8_t& value) {
    if (mPacketSize - mOffset < sizeof(value))
        return false;
    value = mBuffer[mOffset++];
    return true;
}

bool MtpDataPacket::getUInt16(uint16_t& value) {
    if (mPacketSize - mOffset < sizeof(value))
        return false;
    int offset = mOffset;
    value = (uint16_t)mBuffer[offset] | ((uint16_t)mBuffer[offset + 1] << 8);
    mOffset += sizeof(value);
    return true;
}

bool MtpDataPacket::getUInt32(uint32_t& value) {
    if (mPacketSize - mOffset < sizeof(value))
        return false;
    int offset = mOffset;
    value = (uint32_t)mBuffer[offset] | ((uint32_t)mBuffer[offset + 1] << 8) |
           ((uint32_t)mBuffer[offset + 2] << 16)  | ((uint32_t)mBuffer[offset + 3] << 24);
    mOffset += sizeof(value);
    return true;
}

bool MtpDataPacket::getUInt64(uint64_t& value) {
    if (mPacketSize - mOffset < sizeof(value))
        return false;
    int offset = mOffset;
    value = (uint64_t)mBuffer[offset] | ((uint64_t)mBuffer[offset + 1] << 8) |
           ((uint64_t)mBuffer[offset + 2] << 16) | ((uint64_t)mBuffer[offset + 3] << 24) |
           ((uint64_t)mBuffer[offset + 4] << 32) | ((uint64_t)mBuffer[offset + 5] << 40) |
           ((uint64_t)mBuffer[offset + 6] << 48)  | ((uint64_t)mBuffer[offset + 7] << 56);
    mOffset += sizeof(value);
    return true;
}

bool MtpDataPacket::getUInt128(uint128_t& value) {
    return getUInt32(value[0]) && getUInt32(value[1]) && getUInt32(value[2]) && getUInt32(value[3]);
}

bool MtpDataPacket::getString(MtpStringBuffer& string)
{
    return string.readFromPacket(this);
}

Int8List* MtpDataPacket::getAInt8() {
    uint32_t count;
    if (!getUInt32(count))
        return NULL;
    Int8List* result = new Int8List;
    for (uint32_t i = 0; i < count; i++) {
        int8_t value;
        if (!getInt8(value)) {
            delete result;
            return NULL;
        }
        result->push(value);
    }
    return result;
}

UInt8List* MtpDataPacket::getAUInt8() {
    uint32_t count;
    if (!getUInt32(count))
        return NULL;
    UInt8List* result = new UInt8List;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t value;
        if (!getUInt8(value)) {
            delete result;
            return NULL;
        }
        result->push(value);
    }
    return result;
}

Int16List* MtpDataPacket::getAInt16() {
    uint32_t count;
    if (!getUInt32(count))
        return NULL;
    Int16List* result = new Int16List;
    for (uint32_t i = 0; i < count; i++) {
        int16_t value;
        if (!getInt16(value)) {
            delete result;
            return NULL;
        }
        result->push(value);
    }
    return result;
}

UInt16List* MtpDataPacket::getAUInt16() {
    uint32_t count;
    if (!getUInt32(count))
        return NULL;
    UInt16List* result = new UInt16List;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t value;
        if (!getUInt16(value)) {
            delete result;
            return NULL;
        }
        result->push(value);
    }
    return result;
}

Int32List* MtpDataPacket::getAInt32() {
    uint32_t count;
    if (!getUInt32(count))
        return NULL;
    Int32List* result = new Int32List;
    for (uint32_t i = 0; i < count; i++) {
        int32_t value;
        if (!getInt32(value)) {
            delete result;
            return NULL;
        }
        result->push(value);
    }
    return result;
}

UInt32List* MtpDataPacket::getAUInt32() {
    uint32_t count;
    if (!getUInt32(count))
        return NULL;
    UInt32List* result = new UInt32List;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t value;
        if (!getUInt32(value)) {
            delete result;
            return NULL;
        }
        result->push(value);
    }
    return result;
}

Int64List* MtpDataPacket::getAInt64() {
    uint32_t count;
    if (!getUInt32(count))
        return NULL;
    Int64List* result = new Int64List;
    for (uint32_t i = 0; i < count; i++) {
        int64_t value;
        if (!getInt64(value)) {
            delete result;
            return NULL;
        }
        result->push(value);
    }
    return result;
}

UInt64List* MtpDataPacket::getAUInt64() {
    uint32_t count;
    if (!getUInt32(count))
        return NULL;
    UInt64List* result = new UInt64List;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t value;
        if (!getUInt64(value)) {
            delete result;
            return NULL;
        }
        result->push(value);
    }
    return result;
}

void MtpDataPacket::putInt8(int8_t value) {
    allocate(mOffset + 1);
    mBuffer[mOffset++] = (uint8_t)value;
    if (mPacketSize < mOffset)
        mPacketSize = mOffset;
}

void MtpDataPacket::putUInt8(uint8_t value) {
    allocate(mOffset + 1);
    mBuffer[mOffset++] = (uint8_t)value;
    if (mPacketSize < mOffset)
        mPacketSize = mOffset;
}

void MtpDataPacket::putInt16(int16_t value) {
    allocate(mOffset + 2);
    mBuffer[mOffset++] = (uint8_t)(value & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 8) & 0xFF);
    if (mPacketSize < mOffset)
        mPacketSize = mOffset;
}

void MtpDataPacket::putUInt16(uint16_t value) {
    allocate(mOffset + 2);
    mBuffer[mOffset++] = (uint8_t)(value & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 8) & 0xFF);
    if (mPacketSize < mOffset)
        mPacketSize = mOffset;
}

void MtpDataPacket::putInt32(int32_t value) {
    allocate(mOffset + 4);
    mBuffer[mOffset++] = (uint8_t)(value & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 8) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 16) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 24) & 0xFF);
    if (mPacketSize < mOffset)
        mPacketSize = mOffset;
}

void MtpDataPacket::putUInt32(uint32_t value) {
    allocate(mOffset + 4);
    mBuffer[mOffset++] = (uint8_t)(value & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 8) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 16) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 24) & 0xFF);
    if (mPacketSize < mOffset)
        mPacketSize = mOffset;
}

void MtpDataPacket::putInt64(int64_t value) {
    allocate(mOffset + 8);
    mBuffer[mOffset++] = (uint8_t)(value & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 8) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 16) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 24) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 32) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 40) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 48) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 56) & 0xFF);
    if (mPacketSize < mOffset)
        mPacketSize = mOffset;
}

void MtpDataPacket::putUInt64(uint64_t value) {
    allocate(mOffset + 8);
    mBuffer[mOffset++] = (uint8_t)(value & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 8) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 16) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 24) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 32) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 40) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 48) & 0xFF);
    mBuffer[mOffset++] = (uint8_t)((value >> 56) & 0xFF);
    if (mPacketSize < mOffset)
        mPacketSize = mOffset;
}

void MtpDataPacket::putInt128(const int128_t& value) {
    putInt32(value[0]);
    putInt32(value[1]);
    putInt32(value[2]);
    putInt32(value[3]);
}

void MtpDataPacket::putUInt128(const uint128_t& value) {
    putUInt32(value[0]);
    putUInt32(value[1]);
    putUInt32(value[2]);
    putUInt32(value[3]);
}

void MtpDataPacket::putInt128(int64_t value) {
    putInt64(value);
    putInt64(value < 0 ? -1 : 0);
}

void MtpDataPacket::putUInt128(uint64_t value) {
    putUInt64(value);
    putUInt64(0);
}

void MtpDataPacket::putAInt8(const int8_t* values, int count) {
    putUInt32(count);
    for (int i = 0; i < count; i++)
        putInt8(*values++);
}

void MtpDataPacket::putAUInt8(const uint8_t* values, int count) {
    putUInt32(count);
    for (int i = 0; i < count; i++)
        putUInt8(*values++);
}

void MtpDataPacket::putAInt16(const int16_t* values, int count) {
    putUInt32(count);
    for (int i = 0; i < count; i++)
        putInt16(*values++);
}

void MtpDataPacket::putAUInt16(const uint16_t* values, int count) {
    putUInt32(count);
    for (int i = 0; i < count; i++)
        putUInt16(*values++);
}

void MtpDataPacket::putAUInt16(const UInt16List* values) {
    size_t count = (values ? values->size() : 0);
    putUInt32(count);
    for (size_t i = 0; i < count; i++)
        putUInt16((*values)[i]);
}

void MtpDataPacket::putAInt32(const int32_t* values, int count) {
    putUInt32(count);
    for (int i = 0; i < count; i++)
        putInt32(*values++);
}

void MtpDataPacket::putAUInt32(const uint32_t* values, int count) {
    putUInt32(count);
    for (int i = 0; i < count; i++)
        putUInt32(*values++);
}

void MtpDataPacket::putAUInt32(const UInt32List* list) {
    if (!list) {
        putEmptyArray();
    } else {
        size_t size = list->size();
        putUInt32(size);
        for (size_t i = 0; i < size; i++)
            putUInt32((*list)[i]);
    }
}

void MtpDataPacket::putAInt64(const int64_t* values, int count) {
    putUInt32(count);
    for (int i = 0; i < count; i++)
        putInt64(*values++);
}

void MtpDataPacket::putAUInt64(const uint64_t* values, int count) {
    putUInt32(count);
    for (int i = 0; i < count; i++)
        putUInt64(*values++);
}

void MtpDataPacket::putString(const MtpStringBuffer& string) {
    string.writeToPacket(this);
}

void MtpDataPacket::putString(const char* s) {
    MtpStringBuffer string(s);
    string.writeToPacket(this);
}

void MtpDataPacket::putString(const uint16_t* string) {
    int count = 0;
    for (int i = 0; i <= MTP_STRING_MAX_CHARACTER_NUMBER; i++) {
        if (string[i])
            count++;
        else
            break;
    }
    putUInt8(count > 0 ? count + 1 : 0);
    for (int i = 0; i < count; i++)
        putUInt16(string[i]);
    // only terminate with zero if string is not empty
    if (count > 0)
        putUInt16(0);
}

#ifdef MTP_DEVICE 
int MtpDataPacket::read(int fd) {
    int ret = ::read(fd, mBuffer, MTP_BUFFER_SIZE);
    if (ret < MTP_CONTAINER_HEADER_SIZE)
        return -1;
    mPacketSize = ret;
    mOffset = MTP_CONTAINER_HEADER_SIZE;
    return ret;
}

int MtpDataPacket::write(int fd) {
    MtpPacket::putUInt32(MTP_CONTAINER_LENGTH_OFFSET, mPacketSize);
    MtpPacket::putUInt16(MTP_CONTAINER_TYPE_OFFSET, MTP_CONTAINER_TYPE_DATA);
    int ret = ::write(fd, mBuffer, mPacketSize);
    return (ret < 0 ? ret : 0);
}

int MtpDataPacket::writeData(int fd, void* data, uint32_t length) {
    allocate(length + MTP_CONTAINER_HEADER_SIZE);
    memcpy(mBuffer + MTP_CONTAINER_HEADER_SIZE, data, length);
    length += MTP_CONTAINER_HEADER_SIZE;
    MtpPacket::putUInt32(MTP_CONTAINER_LENGTH_OFFSET, length);
    MtpPacket::putUInt16(MTP_CONTAINER_TYPE_OFFSET, MTP_CONTAINER_TYPE_DATA);
    int ret = ::write(fd, mBuffer, length);
    return (ret < 0 ? ret : 0);
}

#endif // MTP_DEVICE

#ifdef MTP_HOST
int MtpDataPacket::read(struct usb_request *request) {
    // first read the header
    request->buffer = mBuffer;
    request->buffer_length = mBufferSize;
    int length = transfer(request);
    if (length >= MTP_CONTAINER_HEADER_SIZE) {
        // look at the length field to see if the data spans multiple packets
        uint32_t totalLength = MtpPacket::getUInt32(MTP_CONTAINER_LENGTH_OFFSET);
        allocate(totalLength);
        while (totalLength > static_cast<uint32_t>(length)) {
            request->buffer = mBuffer + length;
            request->buffer_length = totalLength - length;
            int ret = transfer(request);
            if (ret >= 0)
                length += ret;
            else {
                length = ret;
                break;
            }
        }
    }
    if (length >= 0)
        mPacketSize = length;
    return length;
}

int MtpDataPacket::readData(struct usb_request *request, void* buffer, int length) {
    int read = 0;
    while (read < length) {
        request->buffer = (char *)buffer + read;
        request->buffer_length = length - read;
        int ret = transfer(request);
        if (ret < 0) {
            return ret;
        }
        read += ret;
    }
    return read;
}

// Queue a read request.  Call readDataWait to wait for result
int MtpDataPacket::readDataAsync(struct usb_request *req) {
    if (usb_request_queue(req)) {
        ALOGE("usb_endpoint_queue failed, errno: %d", errno);
        return -1;
    }
    return 0;
}

// Wait for result of readDataAsync
int MtpDataPacket::readDataWait(struct usb_device *device) {
    struct usb_request *req = usb_request_wait(device);
    return (req ? req->actual_length : -1);
}

int MtpDataPacket::readDataHeader(struct usb_request *request) {
    request->buffer = mBuffer;
    request->buffer_length = request->max_packet_size;
    int length = transfer(request);
    if (length >= 0)
        mPacketSize = length;
    return length;
}

int MtpDataPacket::writeDataHeader(struct usb_request *request, uint32_t length) {
    MtpPacket::putUInt32(MTP_CONTAINER_LENGTH_OFFSET, length);
    MtpPacket::putUInt16(MTP_CONTAINER_TYPE_OFFSET, MTP_CONTAINER_TYPE_DATA);
    request->buffer = mBuffer;
    request->buffer_length = MTP_CONTAINER_HEADER_SIZE;
    int ret = transfer(request);
    return (ret < 0 ? ret : 0);
}

int MtpDataPacket::write(struct usb_request *request) {
    MtpPacket::putUInt32(MTP_CONTAINER_LENGTH_OFFSET, mPacketSize);
    MtpPacket::putUInt16(MTP_CONTAINER_TYPE_OFFSET, MTP_CONTAINER_TYPE_DATA);
    request->buffer = mBuffer;
    request->buffer_length = mPacketSize;
    int ret = transfer(request);
    return (ret < 0 ? ret : 0);
}

int MtpDataPacket::write(struct usb_request *request, void* buffer, uint32_t length) {
    request->buffer = buffer;
    request->buffer_length = length;
    int ret = transfer(request);
    return (ret < 0 ? ret : 0);
}

#endif // MTP_HOST

void* MtpDataPacket::getData(int* outLength) const {
    int length = mPacketSize - MTP_CONTAINER_HEADER_SIZE;
    if (length > 0) {
        void* result = malloc(length);
        if (result) {
            memcpy(result, mBuffer + MTP_CONTAINER_HEADER_SIZE, length);
            *outLength = length;
            return result;
        }
    }
    *outLength = 0;
    return NULL;
}

}  // namespace android
