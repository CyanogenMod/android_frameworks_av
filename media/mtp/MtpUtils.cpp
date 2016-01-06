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

#define LOG_TAG "MtpUtils"

#include <stdio.h>
#include <time.h>

#include "MtpUtils.h"

namespace android {

/*
DateTime strings follow a compatible subset of the definition found in ISO 8601, and
take the form of a Unicode string formatted as: "YYYYMMDDThhmmss.s". In this
representation, YYYY shall be replaced by the year, MM replaced by the month (01-12),
DD replaced by the day (01-31), T is a constant character 'T' delimiting time from date,
hh is replaced by the hour (00-23), mm is replaced by the minute (00-59), and ss by the
second (00-59). The ".s" is optional, and represents tenths of a second.
This is followed by a UTC offset given as "[+-]zzzz" or the literal "Z", meaning UTC.
*/

bool parseDateTime(const char* dateTime, time_t& outSeconds) {
    int year, month, day, hour, minute, second;
    if (sscanf(dateTime, "%04d%02d%02dT%02d%02d%02d",
               &year, &month, &day, &hour, &minute, &second) != 6)
        return false;

    // skip optional tenth of second
    const char* tail = dateTime + 15;
    if (tail[0] == '.' && tail[1]) tail += 2;

    // FIXME: "Z" means UTC, but non-"Z" doesn't mean local time.
    // It might be that you're in Asia/Seoul on vacation and your Android
    // device has noticed this via the network, but your camera was set to
    // America/Los_Angeles once when you bought it and doesn't know where
    // it is right now, so the camera says "20160106T081700-0800" but we
    // just ignore the "-0800" and assume local time which is actually "+0900".
    // I think to support this (without switching to Java or using icu4c)
    // you'd want to always use timegm(3) and then manually add/subtract
    // the UTC offset parsed from the string (taking care of wrapping).
    // mktime(3) ignores the tm_gmtoff field, so you can't let it do the work.
    bool useUTC = (tail[0] == 'Z');

    struct tm tm = {};
    tm.tm_sec = second;
    tm.tm_min = minute;
    tm.tm_hour = hour;
    tm.tm_mday = day;
    tm.tm_mon = month - 1;  // mktime uses months in 0 - 11 range
    tm.tm_year = year - 1900;
    tm.tm_isdst = -1;
    outSeconds = useUTC ? timegm(&tm) : mktime(&tm);

    return true;
}

void formatDateTime(time_t seconds, char* buffer, int bufferLength) {
    struct tm tm;

    localtime_r(&seconds, &tm);
    snprintf(buffer, bufferLength, "%04d%02d%02dT%02d%02d%02d",
        tm.tm_year + 1900,
        tm.tm_mon + 1, // localtime_r uses months in 0 - 11 range
        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

}  // namespace android
