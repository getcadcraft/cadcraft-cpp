// SPDX-License-Identifier: Apache-2.0
// The wuffs modules CadCraft builds (PNG and JPEG for pictures, with what they need), the same for the library's
// implementation (wuffs_impl.cpp) and every file that uses its API (libs/image/src/decode.cpp).
#pragma once

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__AUX__BASE
#define WUFFS_CONFIG__MODULE__AUX__IMAGE
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__JPEG
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB
