/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef GRAPHIC_BUFFER_SOURCE_H_

#define GRAPHIC_BUFFER_SOURCE_H_

#include <gui/IGraphicBufferProducer.h>
#include <gui/BufferQueue.h>
#include <utils/RefBase.h>

#include <OMX_Core.h>
#include <VideoAPI.h>
#include "../include/OMXNodeInstance.h"
#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/AHandlerReflector.h>
#include <media/stagefright/foundation/ALooper.h>

namespace android {

struct FrameDropper;

/*
 * This class is used to feed OMX codecs from a Surface via BufferQueue.
 *
 * Instances of the class don't run on a dedicated thread.  Instead,
 * various events trigger data movement:
 *
 *  - Availability of a new frame of data from the BufferQueue (notified
 *    via the onFrameAvailable callback).
 *  - The return of a codec buffer (via OnEmptyBufferDone).
 *  - Application signaling end-of-stream.
 *  - Transition to or from "executing" state.
 *
 * Frames of data (and, perhaps, the end-of-stream indication) can arrive
 * before the codec is in the "executing" state, so we need to queue
 * things up until we're ready to go.
 */
class GraphicBufferSource : public BufferQueue::ConsumerListener {
public:
    GraphicBufferSource(
            OMXNodeInstance* nodeInstance,
            uint32_t bufferWidth,
            uint32_t bufferHeight,
            uint32_t bufferCount,
            uint32_t consumerUsage,
            const sp<IGraphicBufferConsumer> &consumer = NULL
    );

    virtual ~GraphicBufferSource();

    // We can't throw an exception if the constructor fails, so we just set
    // this and require that the caller test the value.
    status_t initCheck() const {
        return mInitCheck;
    }

    // Returns the handle to the producer side of the BufferQueue.  Buffers
    // queued on this will be received by GraphicBufferSource.
    sp<IGraphicBufferProducer> getIGraphicBufferProducer() const {
        return mProducer;
    }

    // Sets the default buffer data space
    void setDefaultDataSpace(android_dataspace dataSpace);

    // This is called when OMX transitions to OMX_StateExecuting, which means
    // we can start handing it buffers.  If we already have buffers of data
    // sitting in the BufferQueue, this will send them to the codec.
    void omxExecuting();

    // This is called when OMX transitions to OMX_StateIdle, indicating that
    // the codec is meant to return all buffers back to the client for them
    // to be freed. Do NOT submit any more buffers to the component.
    void omxIdle();

    // This is called when OMX transitions to OMX_StateLoaded, indicating that
    // we are shutting down.
    void omxLoaded();

    // A "codec buffer", i.e. a buffer that can be used to pass data into
    // the encoder, has been allocated.  (This call does not call back into
    // OMXNodeInstance.)
    void addCodecBuffer(OMX_BUFFERHEADERTYPE* header);

    // Called from OnEmptyBufferDone.  If we have a BQ buffer available,
    // fill it with a new frame of data; otherwise, just mark it as available.
    void codecBufferEmptied(OMX_BUFFERHEADERTYPE* header, int fenceFd);

    // Called when omx_message::FILL_BUFFER_DONE is received. (Currently the
    // buffer source will fix timestamp in the header if needed.)
    void codecBufferFilled(OMX_BUFFERHEADERTYPE* header);

    // This is called after the last input frame has been submitted.  We
    // need to submit an empty buffer with the EOS flag set.  If we don't
    // have a codec buffer ready, we just set the mEndOfStream flag.
    status_t signalEndOfInputStream();

    // If suspend is true, all incoming buffers (including those currently
    // in the BufferQueue) will be discarded until the suspension is lifted.
    void suspend(bool suspend);

    // Specifies the interval after which we requeue the buffer previously
    // queued to the encoder. This is useful in the case of surface flinger
    // providing the input surface if the resulting encoded stream is to
    // be displayed "live". If we were not to push through the extra frame
    // the decoder on the remote end would be unable to decode the latest frame.
    // This API must be called before transitioning the encoder to "executing"
    // state and once this behaviour is specified it cannot be reset.
    status_t setRepeatPreviousFrameDelayUs(int64_t repeatAfterUs);

    // When set, the timestamp fed to the encoder will be modified such that
    // the gap between two adjacent frames is capped at maxGapUs. Timestamp
    // will be restored to the original when the encoded frame is returned to
    // the client.
    // This is to solve a problem in certain real-time streaming case, where
    // encoder's rate control logic produces huge frames after a long period
    // of suspension on input.
    status_t setMaxTimestampGapUs(int64_t maxGapUs);

    // When set, the max frame rate fed to the encoder will be capped at maxFps.
    status_t setMaxFps(float maxFps);

    struct TimeLapseConfig {
        int64_t mTimePerFrameUs;   // the time (us) between two frames for playback
        int64_t mTimePerCaptureUs; // the time (us) between two frames for capture
    };

    // Sets the time lapse (or slow motion) parameters.
    // When set, the sample's timestamp will be modified to playback framerate,
    // and capture timestamp will be modified to capture rate.
    status_t setTimeLapseConfig(const TimeLapseConfig &config);

    // Sets the start time us (in system time), samples before which should
    // be dropped and not submitted to encoder
    void setSkipFramesBeforeUs(int64_t startTimeUs);

    // Sets the desired color aspects, e.g. to be used when producer does not specify a dataspace.
    void setColorAspects(const ColorAspects &aspects);

protected:
    // BufferQueue::ConsumerListener interface, called when a new frame of
    // data is available.  If we're executing and a codec buffer is
    // available, we acquire the buffer, copy the GraphicBuffer reference
    // into the codec buffer, and call Empty[This]Buffer.  If we're not yet
    // executing or there's no codec buffer available, we just increment
    // mNumFramesAvailable and return.
    virtual void onFrameAvailable(const BufferItem& item);

    // BufferQueue::ConsumerListener interface, called when the client has
    // released one or more GraphicBuffers.  We clear out the appropriate
    // set of mBufferSlot entries.
    virtual void onBuffersReleased();

    // BufferQueue::ConsumerListener interface, called when the client has
    // changed the sideband stream. GraphicBufferSource doesn't handle sideband
    // streams so this is a no-op (and should never be called).
    virtual void onSidebandStreamChanged();

private:
    // PersistentProxyListener is similar to BufferQueue::ProxyConsumerListener
    // except that it returns (acquire/detach/re-attache/release) buffers
    // in onFrameAvailable() if the actual consumer object is no longer valid.
    //
    // This class is used in persistent input surface case to prevent buffer
    // loss when onFrameAvailable() is received while we don't have a valid
    // consumer around.
    class PersistentProxyListener : public BnConsumerListener {
        public:
            PersistentProxyListener(
                    const wp<IGraphicBufferConsumer> &consumer,
                    const wp<ConsumerListener>& consumerListener);
            virtual ~PersistentProxyListener();
            virtual void onFrameAvailable(const BufferItem& item) override;
            virtual void onFrameReplaced(const BufferItem& item) override;
            virtual void onBuffersReleased() override;
            virtual void onSidebandStreamChanged() override;
         private:
            // mConsumerListener is a weak reference to the IConsumerListener.
            wp<ConsumerListener> mConsumerListener;
            // mConsumer is a weak reference to the IGraphicBufferConsumer, use
            // a weak ref to avoid circular ref between mConsumer and this class
            wp<IGraphicBufferConsumer> mConsumer;
    };

    // Keep track of codec input buffers.  They may either be available
    // (mGraphicBuffer == NULL) or in use by the codec.
    struct CodecBuffer {
        OMX_BUFFERHEADERTYPE* mHeader;

        // buffer producer's frame-number for buffer
        uint64_t mFrameNumber;

        // buffer producer's buffer slot for buffer
        int mSlot;

        sp<GraphicBuffer> mGraphicBuffer;
    };

    // Returns the index of an available codec buffer.  If none are
    // available, returns -1.  Mutex must be held by caller.
    int findAvailableCodecBuffer_l();

    // Returns true if a codec buffer is available.
    bool isCodecBufferAvailable_l() {
        return findAvailableCodecBuffer_l() >= 0;
    }

    // Finds the mCodecBuffers entry that matches.  Returns -1 if not found.
    int findMatchingCodecBuffer_l(const OMX_BUFFERHEADERTYPE* header);

    // Fills a codec buffer with a frame from the BufferQueue.  This must
    // only be called when we know that a frame of data is ready (i.e. we're
    // in the onFrameAvailable callback, or if we're in codecBufferEmptied
    // and mNumFramesAvailable is nonzero).  Returns without doing anything if
    // we don't have a codec buffer available.
    //
    // Returns true if we successfully filled a codec buffer with a BQ buffer.
    bool fillCodecBuffer_l();

    // Marks the mCodecBuffers entry as in-use, copies the GraphicBuffer
    // reference into the codec buffer, and submits the data to the codec.
    status_t submitBuffer_l(const BufferItem &item, int cbi);

    // Submits an empty buffer, with the EOS flag set.   Returns without
    // doing anything if we don't have a codec buffer available.
    void submitEndOfInputStream_l();

    // Release buffer to the consumer
    void releaseBuffer(
            int &id, uint64_t frameNum,
            const sp<GraphicBuffer> buffer, const sp<Fence> &fence);

    void setLatestBuffer_l(const BufferItem &item, bool dropped);
    bool repeatLatestBuffer_l();
    int64_t getTimestamp(const BufferItem &item);

    // called when the data space of the input buffer changes
    void onDataSpaceChanged_l(android_dataspace dataSpace, android_pixel_format pixelFormat);

    // Lock, covers all member variables.
    mutable Mutex mMutex;

    // Used to report constructor failure.
    status_t mInitCheck;

    // Pointer back to the object that contains us.  We send buffers here.
    OMXNodeInstance* mNodeInstance;

    // Set by omxExecuting() / omxIdling().
    bool mExecuting;

    bool mSuspended;

    // Last dataspace seen
    android_dataspace mLastDataSpace;

    // Our BufferQueue interfaces. mProducer is passed to the producer through
    // getIGraphicBufferProducer, and mConsumer is used internally to retrieve
    // the buffers queued by the producer.
    bool mIsPersistent;
    sp<IGraphicBufferProducer> mProducer;
    sp<IGraphicBufferConsumer> mConsumer;

    // Number of frames pending in BufferQueue that haven't yet been
    // forwarded to the codec.
    size_t mNumFramesAvailable;

    // Number of frames acquired from consumer (debug only)
    int32_t mNumBufferAcquired;

    // Set to true if we want to send end-of-stream after we run out of
    // frames in BufferQueue.
    bool mEndOfStream;
    bool mEndOfStreamSent;

    // Cache of GraphicBuffers from the buffer queue.  When the codec
    // is done processing a GraphicBuffer, we can use this to map back
    // to a slot number.
    sp<GraphicBuffer> mBufferSlot[BufferQueue::NUM_BUFFER_SLOTS];

    // Tracks codec buffers.
    Vector<CodecBuffer> mCodecBuffers;

    ////
    friend struct AHandlerReflector<GraphicBufferSource>;

    enum {
        kWhatRepeatLastFrame,
    };
    enum {
        kRepeatLastFrameCount = 10,
    };

    KeyedVector<int64_t, int64_t> mOriginalTimeUs;
    int64_t mMaxTimestampGapUs;
    int64_t mPrevOriginalTimeUs;
    int64_t mPrevModifiedTimeUs;
    int64_t mSkipFramesBeforeNs;

    sp<FrameDropper> mFrameDropper;

    sp<ALooper> mLooper;
    sp<AHandlerReflector<GraphicBufferSource> > mReflector;

    int64_t mRepeatAfterUs;
    int32_t mRepeatLastFrameGeneration;
    int64_t mRepeatLastFrameTimestamp;
    int32_t mRepeatLastFrameCount;

    int mLatestBufferId;
    uint64_t mLatestBufferFrameNum;
    int32_t mLatestBufferUseCount;
    sp<Fence> mLatestBufferFence;

    // The previous buffer should've been repeated but
    // no codec buffer was available at the time.
    bool mRepeatBufferDeferred;

    // Time lapse / slow motion configuration
    int64_t mTimePerCaptureUs;
    int64_t mTimePerFrameUs;
    int64_t mPrevCaptureUs;
    int64_t mPrevFrameUs;

    MetadataBufferType mMetadataBufferType;
    ColorAspects mColorAspects;

    void onMessageReceived(const sp<AMessage> &msg);

    DISALLOW_EVIL_CONSTRUCTORS(GraphicBufferSource);
};

}  // namespace android

#endif  // GRAPHIC_BUFFER_SOURCE_H_
