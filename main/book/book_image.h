/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：有界 JPEG/PNG 解码与灰度缩放，调用方持有输出。
 * English: Bounded JPEG/PNG decoding and grayscale resizing; caller owns output.
 * 冻结：不访问文件和显示；失败交还空输出，不影响正文阅读。
 * Frozen: No file or display access; failure leaves empty output and must not block text reading.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// 解码到不超过648×1000灰度图；输入至多2MiB；基线JPEG至多16M像素，其余至多1M像素、解码堆至多4MiB。
/// Decode at most 648x1000 grayscale pixels; input <=2MiB; baseline JPEG <=16M pixels, others <=1M pixels with a <=4MiB decoder heap.
bool book_image_decode(const uint8_t* data, size_t len, size_t budget,
                       uint8_t** pixels, uint16_t* width, uint16_t* height);

/// 按目标尺寸等比铺满并居中裁切解码灰度图（无留边）；输入与解码上限同 book_image_decode。
/// Decode grayscale with aspect-fill center crop to exact target dimensions (no borders); same input and decoder limits as book_image_decode.
bool book_image_decode_cover(const uint8_t* data, size_t len,
                             uint16_t target_w, uint16_t target_h, size_t budget,
                             uint8_t** pixels, uint16_t* width, uint16_t* height);

/// 在给定上限内等比适配解码灰度图（用于全屏 CBZ 漫画），可选 4x4 Bayer 有序抖动量化至 16 灰阶。
/// Decode grayscale proportionally fitted within max_w x max_h (for fullscreen CBZ), with optional 4x4 Bayer 16-gray dithering.
bool book_image_decode_fit(const uint8_t* data, size_t len,
                           uint16_t max_w, uint16_t max_h, size_t budget, bool dither,
                           uint8_t** pixels, uint16_t* width, uint16_t* height);

/// 对 8 位灰度缓冲原地执行 4x4 Bayer 有序抖动并量化为 16 级墨水屏灰阶（0, 17, …, 255）。
/// Apply in-place 4x4 Bayer ordered dithering to 8-bit grayscale, quantizing to 16 e-ink levels (0, 17, ..., 255).
void book_image_dither_bayer4(uint8_t* pixels, uint16_t width, uint16_t height);
