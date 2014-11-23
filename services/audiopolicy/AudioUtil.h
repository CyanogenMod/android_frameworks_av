/*Copyright (C) 2014 The Android Open Source Project
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
* This file was modified by DTS, Inc. The portions of the
* code modified by DTS, Inc are copyrighted and
* licensed separately, as follows:
*
*  (C) 2014 DTS, Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#ifndef AUDIO_UTIL_H_
#define AUDIO_UTIL_H_

#ifndef DTS_EAGLE
#define create_route_node() (0)
#define notify_route_node(active_device, devices) (0)
#define remove_route_node() (0)
#else
void create_route_node(void);
void notify_route_node(int active_device, int devices);
void remove_route_node(void);
#endif

#endif  //AUDIO_UTIL_H_
