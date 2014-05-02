/*
**
** Copyright 2014, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

//#define LOG_NDEBUG 0
#define LOG_TAG "ClockEstimator"
#include <utils/Log.h>

#include <math.h>
#include <media/stagefright/ClockEstimator.h>

#include <media/stagefright/foundation/ADebug.h>

namespace android {

WindowedLinearFitEstimator::WindowedLinearFitEstimator(
        size_t headLength, double headFactor, size_t mainLength, double tailFactor)
    : mHeadFactorInv(1. / headFactor),
      mTailFactor(tailFactor),
      mHistoryLength(mainLength + headLength),
      mHeadLength(headLength) {
    reset();
    mXHistory.resize(mHistoryLength);
    mYHistory.resize(mHistoryLength);
    mFirstWeight = pow(headFactor, mHeadLength);
}

WindowedLinearFitEstimator::LinearFit::LinearFit() {
    reset();
}

void WindowedLinearFitEstimator::LinearFit::reset() {
    mX = mXX = mY = mYY = mXY = mW = 0.;
}

double WindowedLinearFitEstimator::LinearFit::size() const {
    double s = mW * mW + mX * mX + mY * mY + mXX * mXX + mXY * mXY + mYY * mYY;
    if (s > 1e72) {
        // 1e72 corresponds to clock monotonic time of about 8 years
        ALOGW("estimator is overflowing: w=%g x=%g y=%g xx=%g xy=%g yy=%g",
              mW, mX, mY, mXX, mXY, mYY);
    }
    return s;
}

void WindowedLinearFitEstimator::LinearFit::add(double x, double y, double w) {
    mW += w;
    mX += w * x;
    mY += w * y;
    mXX += w * x * x;
    mXY += w * x * y;
    mYY += w * y * y;
}

void WindowedLinearFitEstimator::LinearFit::combine(const LinearFit &lf) {
    mW += lf.mW;
    mX += lf.mX;
    mY += lf.mY;
    mXX += lf.mXX;
    mXY += lf.mXY;
    mYY += lf.mYY;
}

void WindowedLinearFitEstimator::LinearFit::scale(double w) {
    mW *= w;
    mX *= w;
    mY *= w;
    mXX *= w;
    mXY *= w;
    mYY *= w;
}

double WindowedLinearFitEstimator::LinearFit::interpolate(double x) {
    double div = mW * mXX - mX * mX;
    if (fabs(div) < 1e-5 * mW * mW) {
        // this only should happen on the first value
        return x;
        // assuming a = 1, we could also return x + (mY - mX) / mW;
    }
    double a_div = (mW * mXY - mX * mY);
    double b_div = (mXX * mY - mX * mXY);
    ALOGV("a=%.4g b=%.4g in=%g out=%g",
            a_div / div, b_div / div, x, (a_div * x + b_div) / div);
    return (a_div * x + b_div) / div;
}

double WindowedLinearFitEstimator::estimate(double x, double y) {
    /*
     * TODO: We could update the head by adding the new sample to it
     * and amplifying it, but this approach can lead to unbounded
     * error. Instead, we recalculate the head at each step, which
     * is computationally more expensive. We could balance the two
     * methods by recalculating just before the error becomes
     * significant.
     */
    const bool update_head = false;
    if (update_head) {
        // add new sample to the head
        mHead.scale(mHeadFactorInv); // amplify head
        mHead.add(x, y, mFirstWeight);
    }

    /*
     * TRICKY: place elements into the circular buffer at decreasing
     * indices, so that we can access past elements by addition
     * (thereby avoiding potentially negative indices.)
     */
    if (mNumSamples >= mHeadLength) {
        // move last head sample from head to the main window
        size_t lastHeadIx = (mSampleIx + mHeadLength) % mHistoryLength;
        if (update_head) {
            mHead.add(mXHistory[lastHeadIx], mYHistory[lastHeadIx], -1.); // remove
        }
        mMain.add(mXHistory[lastHeadIx], mYHistory[lastHeadIx], 1.);
        if (mNumSamples >= mHistoryLength) {
            // move last main sample from main window to tail
            mMain.add(mXHistory[mSampleIx], mYHistory[mSampleIx], -1.); // remove
            mTail.add(mXHistory[mSampleIx], mYHistory[mSampleIx], 1.);
            mTail.scale(mTailFactor); // attenuate tail
        }
    }

    mXHistory.editItemAt(mSampleIx) = x;
    mYHistory.editItemAt(mSampleIx) = y;
    if (mNumSamples < mHistoryLength) {
        ++mNumSamples;
    }

    // recalculate head unless we were using the update method
    if (!update_head) {
        mHead.reset();
        double w = mFirstWeight;
        for (size_t headIx = 0; headIx < mHeadLength && headIx < mNumSamples; ++headIx) {
            size_t ix = (mSampleIx + headIx) % mHistoryLength;
            mHead.add(mXHistory[ix], mYHistory[ix], w);
            w *= mHeadFactorInv;
        }
    }

    if (mSampleIx > 0) {
        --mSampleIx;
    } else {
        mSampleIx = mHistoryLength - 1;
    }

    // return estimation result
    LinearFit total;
    total.combine(mHead);
    total.combine(mMain);
    total.combine(mTail);
    return total.interpolate(x);
}

void WindowedLinearFitEstimator::reset() {
    mHead.reset();
    mMain.reset();
    mTail.reset();
    mNumSamples = 0;
    mSampleIx = mHistoryLength - 1;
}

}; // namespace android


