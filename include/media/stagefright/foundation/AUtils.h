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

#ifndef A_UTILS_H_

#define A_UTILS_H_

/* ============================ math templates ============================ */

template<class T>
void ENSURE_UNSIGNED_TYPE() {
    T TYPE_MUST_BE_UNSIGNED[(T)-1 < 0 ? -1 : 0];
}

// needle is in range [hayStart, hayStart + haySize)
template<class T, class U>
inline static bool isInRange(const T &hayStart, const U &haySize, const T &needle) {
    ENSURE_UNSIGNED_TYPE<U>();
    return (T)(hayStart + haySize) >= hayStart && needle >= hayStart && (U)(needle - hayStart) < haySize;
}

// [needleStart, needleStart + needleSize) is in range [hayStart, hayStart + haySize)
template<class T, class U>
inline static bool isInRange(
        const T &hayStart, const U &haySize, const T &needleStart, const U &needleSize) {
    ENSURE_UNSIGNED_TYPE<U>();
    return isInRange(hayStart, haySize, needleStart)
            && (T)(needleStart + needleSize) >= needleStart
            && (U)(needleStart + needleSize - hayStart) <= haySize;
}

#endif  // A_UTILS_H_
