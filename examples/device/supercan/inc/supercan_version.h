/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021-2022 Jean Gressmann <jean@0x42.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#pragma once

#define SUPERCAN_STR2(x) #x
#define SUPERCAN_STR(x) SUPERCAN_STR2(x)

#define SUPERCAN_VERSION_MAJOR 0
#define SUPERCAN_VERSION_MINOR 5
#define SUPERCAN_VERSION_PATCH 5

#define SUPERCAN_VERSION_STR SUPERCAN_STR(SUPERCAN_VERSION_MAJOR) "." SUPERCAN_STR(SUPERCAN_VERSION_MINOR) "." SUPERCAN_STR(SUPERCAN_VERSION_PATCH)
