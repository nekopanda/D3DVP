#pragma once

#include <stdint.h>
#include "filter.h"

void yuv_to_nv12_c(int height, int width,
	uint8_t* dst, int dstPitch, const uint8_t* srcY, const uint8_t* srcU, const uint8_t* srcV, int pitchY, int pitchUV);
void nv12_to_yuv_c(int height, int width,
	uint8_t* dstY, uint8_t* dstU, uint8_t* dstV, int pitchY, int pitchUV, const uint8_t* src, int srcPitch);
void yuv_to_nv12_avx2(int height, int width,
	uint8_t* dst, int dstPitch, const uint8_t* srcY, const uint8_t* srcU, const uint8_t* srcV, int pitchY, int pitchUV);
void nv12_to_yuv_avx2(int height, int width,
	uint8_t* dstY, uint8_t* dstU, uint8_t* dstV, int pitchY, int pitchUV, const uint8_t* src, int srcPitch);

void yc48_to_yuy2_c(uint8_t* dst, int pitch, const PIXEL_YC* src, int w, int h, int max_w);
void yc48_to_nv12_c(uint8_t* dst, int pitch, const PIXEL_YC* src, int w, int h, int max_w);
void yc48_to_yuy2_avx2(uint8_t* dst, int pitch, const PIXEL_YC* src, int w, int h, int max_w);
void yc48_to_nv12_avx2(uint8_t* dst, int pitch, const PIXEL_YC* src, int w, int h, int max_w);

void yuy2_to_yc48_c(PIXEL_YC* dst, const uint8_t* src, int pitch, int w, int h, int max_w);
void nv12_to_yc48_c(PIXEL_YC* dst, const uint8_t* src, int pitch, int w, int h, int max_w);
void yuy2_to_yc48_avx2(PIXEL_YC* dst, const uint8_t* src, int pitch, int w, int h, int max_w);
void nv12_to_yc48_avx2(PIXEL_YC* dst, const uint8_t* src, int pitch, int w, int h, int max_w);
