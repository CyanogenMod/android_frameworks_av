/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_RADIO_REGIONS_H
#define ANDROID_HARDWARE_RADIO_REGIONS_H

namespace android {

#define RADIO_BAND_LOWER_FM_ITU1    87500
#define RADIO_BAND_UPPER_FM_ITU1    108000
#define RADIO_BAND_SPACING_FM_ITU1  100

#define RADIO_BAND_LOWER_FM_ITU2    87900
#define RADIO_BAND_UPPER_FM_ITU2    107900
#define RADIO_BAND_SPACING_FM_ITU2  200

#define RADIO_BAND_LOWER_FM_JAPAN    76000
#define RADIO_BAND_UPPER_FM_JAPAN    90000
#define RADIO_BAND_SPACING_FM_JAPAN  100

#define RADIO_BAND_LOWER_FM_OIRT    65800
#define RADIO_BAND_UPPER_FM_OIRT    74000
#define RADIO_BAND_SPACING_FM_OIRT  10

#define RADIO_BAND_LOWER_LW         153
#define RADIO_BAND_UPPER_LW         279
#define RADIO_BAND_SPACING_LW       9

#define RADIO_BAND_LOWER_MW_IUT1    531
#define RADIO_BAND_UPPER_MW_ITU1    1611
#define RADIO_BAND_SPACING_MW_ITU1  9

#define RADIO_BAND_LOWER_MW_IUT2    540
#define RADIO_BAND_UPPER_MW_ITU2    1610
#define RADIO_BAND_SPACING_MW_ITU2  10

#define RADIO_BAND_LOWER_SW         2300
#define RADIO_BAND_UPPER_SW         26100
#define RADIO_BAND_SPACING_SW       5


#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

const radio_band_config_t sKnownRegionConfigs[] = {
    {   // FM ITU 1
        RADIO_REGION_ITU_1,
        {
        RADIO_BAND_FM,
            false,
            RADIO_BAND_LOWER_FM_ITU1,
            RADIO_BAND_UPPER_FM_ITU1,
            1,
            {RADIO_BAND_SPACING_FM_ITU1},
            {
            RADIO_DEEMPHASIS_50,
            true,
            RADIO_RDS_WORLD,
            true,
            true,
            }
        }
    },
    {   // FM Americas
        RADIO_REGION_ITU_2,
        {
        RADIO_BAND_FM,
            false,
            RADIO_BAND_LOWER_FM_ITU2,
            RADIO_BAND_UPPER_FM_ITU2,
            1,
            {RADIO_BAND_SPACING_FM_ITU2},
            {
            RADIO_DEEMPHASIS_75,
            true,
            RADIO_RDS_US,
            true,
            true,
            }
        }
    },
    {   // FM Japan
        RADIO_REGION_JAPAN,
        {
        RADIO_BAND_FM,
            false,
            RADIO_BAND_LOWER_FM_JAPAN,
            RADIO_BAND_UPPER_FM_JAPAN,
            1,
            {RADIO_BAND_SPACING_FM_JAPAN},
            {
            RADIO_DEEMPHASIS_50,
            true,
            RADIO_RDS_WORLD,
            true,
            true,
            }
        }
    },
    {   // FM Korea
        RADIO_REGION_KOREA,
        {
        RADIO_BAND_FM,
            false,
            RADIO_BAND_LOWER_FM_ITU1,
            RADIO_BAND_UPPER_FM_ITU1,
            1,
            {RADIO_BAND_SPACING_FM_ITU1},
            {
            RADIO_DEEMPHASIS_75,
            true,
            RADIO_RDS_WORLD,
            true,
            true,
            }
        }
    },
    {   // FM OIRT
        RADIO_REGION_OIRT,
        {
        RADIO_BAND_FM,
            false,
            RADIO_BAND_LOWER_FM_OIRT,
            RADIO_BAND_UPPER_FM_OIRT,
            1,
            {RADIO_BAND_SPACING_FM_OIRT},
            {
            RADIO_DEEMPHASIS_50,
            true,
            RADIO_RDS_WORLD,
            true,
            true,
            }
        }
    },
    {   // FM US HD radio
        RADIO_REGION_ITU_2,
        {
            RADIO_BAND_FM_HD,
            false,
            RADIO_BAND_LOWER_FM_ITU2,
            RADIO_BAND_UPPER_FM_ITU2,
            1,
            {RADIO_BAND_SPACING_FM_ITU2},
            {
            RADIO_DEEMPHASIS_75,
            true,
            RADIO_RDS_US,
            true,
            true,
            }
        }
    },
    {   // AM LW
        RADIO_REGION_ITU_1,
        {
            RADIO_BAND_AM,
            false,
            RADIO_BAND_LOWER_LW,
            RADIO_BAND_UPPER_LW,
            1,
            {RADIO_BAND_SPACING_LW},
            {
            }
        }
    },
    {   // AM SW
        RADIO_REGION_ITU_1,
        {
            RADIO_BAND_AM,
            false,
            RADIO_BAND_LOWER_SW,
            RADIO_BAND_UPPER_SW,
            1,
            {RADIO_BAND_SPACING_SW},
            {
            }
        }
    },
    {   // AM MW ITU1
        RADIO_REGION_ITU_1,
        {
            RADIO_BAND_AM,
            false,
            RADIO_BAND_LOWER_MW_IUT1,
            RADIO_BAND_UPPER_MW_ITU1,
            1,
            {RADIO_BAND_SPACING_MW_ITU1},
            {
            }
        }
    },
    {   // AM MW ITU2
        RADIO_REGION_ITU_2,
        {
            RADIO_BAND_AM,
            false,
            RADIO_BAND_LOWER_MW_IUT2,
            RADIO_BAND_UPPER_MW_ITU2,
            1,
            {RADIO_BAND_SPACING_MW_ITU2},
            {
            }
        }
    }
};


} // namespace android

#endif // ANDROID_HARDWARE_RADIO_REGIONS_H
