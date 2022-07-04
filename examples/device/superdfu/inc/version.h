/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2021-2022 Jean Gressmann <jean@0x42.de>
 *
 */


#pragma once

#define SUPERDFU_STR2(x) #x
#define SUPERDFU_STR(x) SUPERDFU_STR2(x)

#define SUPERDFU_VERSION_MAJOR 0
#define SUPERDFU_VERSION_MINOR 6
#define SUPERDFU_VERSION_PATCH 1

#define SUPERDFU_VERSION_STR SUPERDFU_STR(SUPERDFU_VERSION_MAJOR) "." SUPERDFU_STR(SUPERDFU_VERSION_MINOR) "." SUPERDFU_STR(SUPERDFU_VERSION_PATCH)