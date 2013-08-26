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

#include <utils/String8.h>
#include <drm/DrmInfoEvent.h>
#include <stdlib.h>

using namespace android;

DrmInfoEvent::DrmInfoEvent(int uniqueId, int infoType, const String8 message)
    : mUniqueId(uniqueId),
      mInfoType(infoType),
      mMessage(message),
      mDrmBuffer() {

}

DrmInfoEvent::DrmInfoEvent(int uniqueId, int infoType, const String8 message,
        const DrmBuffer& drmBuffer)
        : mUniqueId(uniqueId), mInfoType(infoType), mMessage(message), mDrmBuffer() {
    setData(drmBuffer);
}

DrmInfoEvent::~DrmInfoEvent() {
    delete [] mDrmBuffer.data;
}


int DrmInfoEvent::getUniqueId() const {
    return mUniqueId;
}

int DrmInfoEvent::getType() const {
    return mInfoType;
}

const String8 DrmInfoEvent::getMessage() const {
    return mMessage;
}

int DrmInfoEvent::getCount() const {
    return mAttributes.size();
}

status_t DrmInfoEvent::put(const String8& key, String8& value) {
        mAttributes.add(key, value);
    return DRM_NO_ERROR;
}

const String8 DrmInfoEvent::get(const String8& key) const {
    if (mAttributes.indexOfKey(key) != NAME_NOT_FOUND) {
        return mAttributes.valueFor(key);
    }
    return String8("");
}

const DrmBuffer& DrmInfoEvent::getData() const {
    return mDrmBuffer;
}

void DrmInfoEvent::setData(const DrmBuffer& drmBuffer) {
    delete [] mDrmBuffer.data;
    mDrmBuffer.data = new char[drmBuffer.length];;
    mDrmBuffer.length = drmBuffer.length;
    memcpy(mDrmBuffer.data, drmBuffer.data, drmBuffer.length);
}

DrmInfoEvent::KeyIterator DrmInfoEvent::keyIterator() const {
    return KeyIterator(this);
}

DrmInfoEvent::Iterator DrmInfoEvent::iterator() const {
    return Iterator(this);
}

// KeyIterator implementation
DrmInfoEvent::KeyIterator::KeyIterator(const DrmInfoEvent::KeyIterator& keyIterator)
        : mDrmInfoEvent(keyIterator.mDrmInfoEvent), mIndex(keyIterator.mIndex) {
}

bool DrmInfoEvent::KeyIterator::hasNext() {
    return (mIndex < mDrmInfoEvent->mAttributes.size());
}

const String8& DrmInfoEvent::KeyIterator::next() {
    const String8& key = mDrmInfoEvent->mAttributes.keyAt(mIndex);
    mIndex++;
    return key;
}

DrmInfoEvent::KeyIterator& DrmInfoEvent::KeyIterator::operator=(
        const DrmInfoEvent::KeyIterator& keyIterator) {
    mDrmInfoEvent = keyIterator.mDrmInfoEvent;
    mIndex = keyIterator.mIndex;
    return *this;
}

// Iterator implementation
DrmInfoEvent::Iterator::Iterator(const DrmInfoEvent::Iterator& iterator)
        : mDrmInfoEvent(iterator.mDrmInfoEvent), mIndex(iterator.mIndex) {
}

DrmInfoEvent::Iterator& DrmInfoEvent::Iterator::operator=(const DrmInfoEvent::Iterator& iterator) {
    mDrmInfoEvent = iterator.mDrmInfoEvent;
    mIndex = iterator.mIndex;
    return *this;
}

bool DrmInfoEvent::Iterator::hasNext() {
    return mIndex < mDrmInfoEvent->mAttributes.size();
}

const String8& DrmInfoEvent::Iterator::next() {
    const String8& value = mDrmInfoEvent->mAttributes.editValueAt(mIndex);
    mIndex++;
    return value;
}
