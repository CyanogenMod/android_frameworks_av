/*
 * Copyright (C) 2009 The Android Open Source Project
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

#ifndef IMEDIA_SOURCE_BASE_H_

#define IMEDIA_SOURCE_BASE_H_

#include <binder/IInterface.h>
#include <media/stagefright/MediaErrors.h>

namespace android {

struct MediaSource;
class MetaData;
class MediaBuffer;
class MediaBufferGroup;

class IMediaSource : public IInterface {
public:
    DECLARE_META_INTERFACE(MediaSource);

    enum {
        // Maximum number of buffers would be read in readMultiple.
        kMaxNumReadMultiple = 128,
    };

    // To be called before any other methods on this object, except
    // getFormat().
    virtual status_t start(MetaData *params = NULL) = 0;

    // Any blocking read call returns immediately with a result of NO_INIT.
    // It is an error to call any methods other than start after this call
    // returns. Any buffers the object may be holding onto at the time of
    // the stop() call are released.
    // Also, it is imperative that any buffers output by this object and
    // held onto by callers be released before a call to stop() !!!
    virtual status_t stop() = 0;

    // Returns the format of the data output by this media source.
    virtual sp<MetaData> getFormat() = 0;

    // Options that modify read() behaviour. The default is to
    // a) not request a seek
    // b) not be late, i.e. lateness_us = 0
    struct ReadOptions {
        enum SeekMode {
            SEEK_PREVIOUS_SYNC,
            SEEK_NEXT_SYNC,
            SEEK_CLOSEST_SYNC,
            SEEK_CLOSEST,
        };

        ReadOptions();

        // Reset everything back to defaults.
        void reset();

        void setSeekTo(int64_t time_us, SeekMode mode = SEEK_CLOSEST_SYNC);
        void clearSeekTo();
        bool getSeekTo(int64_t *time_us, SeekMode *mode) const;

        void setLateBy(int64_t lateness_us);
        int64_t getLateBy() const;

        void setNonBlocking();
        void clearNonBlocking();
        bool getNonBlocking() const;

    private:
        enum Options {
            kSeekTo_Option      = 1,
        };

        uint32_t mOptions;
        int64_t mSeekTimeUs;
        SeekMode mSeekMode;
        int64_t mLatenessUs;
        bool mNonBlocking;
    } __attribute__((packed)); // sent through Binder

    // Returns a new buffer of data. Call blocks until a
    // buffer is available, an error is encountered or the end of the stream
    // is reached.
    // End of stream is signalled by a result of ERROR_END_OF_STREAM.
    // A result of INFO_FORMAT_CHANGED indicates that the format of this
    // MediaSource has changed mid-stream, the client can continue reading
    // but should be prepared for buffers of the new configuration.
    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL) = 0;

    // Returns a vector of new buffers of data. The vector size could be
    // <= |maxNumBuffers|. Used for buffers with small size
    // since all buffer data are passed back by binder, not shared memory.
    // Call blocks until an error is encountered, or the end of the stream is
    // reached, or format change is hit, or |kMaxNumReadMultiple| buffers have
    // been read.
    // End of stream is signalled by a result of ERROR_END_OF_STREAM.
    // A result of INFO_FORMAT_CHANGED indicates that the format of this
    // MediaSource has changed mid-stream, the client can continue reading
    // but should be prepared for buffers of the new configuration.
    virtual status_t readMultiple(
            Vector<MediaBuffer *> *buffers, uint32_t maxNumBuffers = 1) = 0;

    // Causes this source to suspend pulling data from its upstream source
    // until a subsequent read-with-seek. Currently only supported by
    // OMXCodec.
    virtual status_t pause()  = 0;

    // The consumer of this media source requests that the given buffers
    // are to be returned exclusively in response to read calls.
    // This will be called after a successful start() and before the
    // first read() call.
    // Callee assumes ownership of the buffers if no error is returned.
    virtual status_t setBuffers(const Vector<MediaBuffer *> & /* buffers */) = 0;

};

class BnMediaSource: public BnInterface<IMediaSource>
{
public:
    BnMediaSource();

    virtual status_t    onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                uint32_t flags = 0);

    virtual status_t pause() {
        return ERROR_UNSUPPORTED;
    }

    virtual status_t setBuffers(const Vector<MediaBuffer *> & /* buffers */) {
        return ERROR_UNSUPPORTED;
    }

    virtual status_t readMultiple(
            Vector<MediaBuffer *> * /* buffers */, uint32_t /* maxNumBuffers = 1 */) {
        return ERROR_UNSUPPORTED;
    }
protected:
    virtual ~BnMediaSource();

private:
    MediaBufferGroup *mGroup;
};


}  // namespace android

#endif  // IMEDIA_SOURCE_BASE_H_
