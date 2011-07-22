/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef YV12_COLOR_CONVERTER_H
#define YV12_COLOR_CONVERTER_H

#include <IYV12ColorConverter.h>

// This is a wrapper around the YV12 color converter functions in
// IYV12ColorConverter, which is loaded from a shared library.
class YV12ColorConverter: public IYV12ColorConverter {
public:
    YV12ColorConverter();
    ~YV12ColorConverter();

    // Returns true if the converter functions are successfully loaded.
    bool isLoaded();
private:
    void* mHandle;
};

#endif /* YV12_COLOR_CONVERTER_H */
