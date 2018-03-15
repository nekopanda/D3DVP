
#define NOMINMAX
#include <Windows.h>
#include <string.h>
#include "convert.h"

void yuv_to_nv12_c(
	int height, int width,
	uint8_t* dst, int dstPitch,
	const uint8_t* srcY, const uint8_t* srcU, const uint8_t* srcV,
	int pitchY, int pitchUV)
{
	int widthUV = width >> 1;
	int heightUV = height >> 1;
	int offsetUV = width * height;

	uint8_t* dstY = dst;
	uint8_t* dstUV = dstY + height * dstPitch;

	for (int y = 0; y < height; ++y) {
		memcpy(&dstY[y * dstPitch], &srcY[y * pitchY], width);
	}

	for (int y = 0; y < heightUV; ++y) {
		for (int x = 0; x < widthUV; ++x) {
			dstUV[x * 2 + 0 + y * dstPitch] = srcU[x + y * pitchUV];
			dstUV[x * 2 + 1 + y * dstPitch] = srcV[x + y * pitchUV];
		}
	}
}

void nv12_to_yuv_c(
	int height, int width,
	uint8_t* dstY, uint8_t* dstU, uint8_t* dstV,
	int pitchY, int pitchUV,
	const uint8_t* src, int srcPitch)
{
	int widthUV = width >> 1;
	int heightUV = height >> 1;
	int offsetUV = width * height;

	const uint8_t* srcY = src;
	const uint8_t* srcUV = srcY + height * srcPitch;

	for (int y = 0; y < height; ++y) {
		memcpy(&dstY[y * pitchY], &srcY[y * srcPitch], width);
	}

	for (int y = 0; y < heightUV; ++y) {
		for (int x = 0; x < widthUV; ++x) {
			dstU[x + y * pitchUV] = srcUV[x * 2 + 0 + y * srcPitch];
			dstV[x + y * pitchUV] = srcUV[x * 2 + 1 + y * srcPitch];
		}
	}
}


template<typename T>
T clamp(T n, T min, T max)
{
	n = n > max ? max : n;
	return n < min ? min : n;
}

void yc48_to_yuy2_c(uint8_t* dst, int pitch, const PIXEL_YC* src, int w, int h, int max_w) {
	uint8_t* dstptr = dst;

	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; x += 2) {
			short y0 = src[x + y * max_w].y;
			short y1 = src[x + 1 + y * max_w].y;
			short cb = src[x + y * max_w].cb;
			short cr = src[x + y * max_w].cr;

			uint8_t Y0 = clamp(((y0 * 219 + 383) >> 12) + 16, 0, 255);
			uint8_t Y1 = clamp(((y1 * 219 + 383) >> 12) + 16, 0, 255);
			uint8_t U = clamp((((cb + 2048) * 7 + 66) >> 7) + 16, 0, 255);
			uint8_t V = clamp((((cr + 2048) * 7 + 66) >> 7) + 16, 0, 255);

			dstptr[x * 2 + 0 + y * pitch] = Y0;
			dstptr[x * 2 + 1 + y * pitch] = U;
			dstptr[x * 2 + 2 + y * pitch] = Y1;
			dstptr[x * 2 + 3 + y * pitch] = V;
		}
	}
}

void yc48_to_nv12_c(uint8_t* dst, int pitch, const PIXEL_YC* src, int w, int h, int max_w) {
	uint8_t* dstY = dst;
	uint8_t* dstUV = dstY + h * pitch;

	for (int y = 0; y < h; ++y) {
		int y2 = (y >> 1);
		for (int x = 0; x < w; x += 2) {
			short y0 = src[x + y * max_w].y;
			short y1 = src[x + 1 + y * max_w].y;
			short cb = src[x + y * max_w].cb;
			short cr = src[x + y * max_w].cr;

			uint8_t Y0 = clamp(((y0 * 219 + 383) >> 12) + 16, 0, 255);
			uint8_t Y1 = clamp(((y1 * 219 + 383) >> 12) + 16, 0, 255);
			uint8_t U = clamp((((cb + 2048) * 7 + 66) >> 7) + 16, 0, 255);
			uint8_t V = clamp((((cr + 2048) * 7 + 66) >> 7) + 16, 0, 255);

			dstY[x + 0 + y * pitch] = Y0;
			dstY[x + 1 + y * pitch] = Y1;

			if ((y & 1) == (y2 & 1)) {
				dstUV[x + 0 + y2 * pitch] = U;
				dstUV[x + 1 + y2 * pitch] = V;
			}
		}
	}
}

void yuy2_to_yc48_c(PIXEL_YC* dst, const uint8_t* src, int pitch, int w, int h, int max_w)
{
	const uint8_t* srcptr = src;

	for (int y = 0; y < h; ++y) {

		// ‚Ü‚¸‚ÍUV‚Í¶‚É‚»‚Ì‚Ü‚Ü“ü‚ê‚é
		for (int x = 0, x2 = 0; x < w; x += 2, ++x2) {
			uint8_t Y0 = srcptr[x * 2 + 0 + y * pitch];
			uint8_t U = srcptr[x * 2 + 1 + y * pitch];
			uint8_t Y1 = srcptr[x * 2 + 2 + y * pitch];
			uint8_t V = srcptr[x * 2 + 3 + y * pitch];

			short y0 = ((Y0 * 1197) >> 6) - 299;
			short y1 = ((Y1 * 1197) >> 6) - 299;
			short cb = (U * 4682 - (4681 << 7)) >> 8;
			short cr = (V * 4682 - (4681 << 7)) >> 8;

			dst[x + y * max_w].y = y0;
			dst[x + 1 + y * max_w].y = y1;
			dst[x + y * max_w].cb = cb;
			dst[x + y * max_w].cr = cr;
		}

		// UV‚Ì“ü‚Á‚Ä‚¢‚È‚¢‚Æ‚±‚ë‚ð•âŠÔ
		short cb0 = dst[y * max_w].cb;
		short cr0 = dst[y * max_w].cr;
		for (int x = 0; x < w - 2; x += 2) {
			short cb2 = dst[x + 2 + y * max_w].cb;
			short cr2 = dst[x + 2 + y * max_w].cr;
			dst[x + 1 + y * max_w].cb = (cb0 + cb2) >> 1;
			dst[x + 1 + y * max_w].cr = (cr0 + cr2) >> 1;
			cb0 = cb2;
			cr0 = cr2;
		}
		dst[w - 1 + y * max_w].cb = cb0;
		dst[w - 1 + y * max_w].cr = cr0;
	}
}

void nv12_to_yc48_c(PIXEL_YC* dst, const uint8_t* src, int pitch, int w, int h, int max_w)
{
	const uint8_t* srcY = src;
	const uint8_t* srcUV = srcY + h * pitch;

	for (int y = 0; y < h; ++y) {

		// ‚Ü‚¸‚ÍUV‚Í¶‚É‚»‚Ì‚Ü‚Ü“ü‚ê‚é
		for (int x = 0, x2 = 0; x < w; x += 2, ++x2) {
			uint8_t Y0 = srcY[x + 0 + y * pitch];
			uint8_t Y1 = srcY[x + 1 + y * pitch];
			uint8_t U = srcUV[x + 0 + (y >> 1) * pitch];
			uint8_t V = srcUV[x + 1 + (y >> 1) * pitch];

			short y0 = ((Y0 * 1197) >> 6) - 299;
			short y1 = ((Y1 * 1197) >> 6) - 299;
			short cb = ((U - 128) * 4681 + 164) >> 8;
			short cr = ((V - 128) * 4681 + 164) >> 8;

			dst[x + y * max_w].y = y0;
			dst[x + 1 + y * max_w].y = y1;
			dst[x + y * max_w].cb = cb;
			dst[x + y * max_w].cr = cr;
		}

		// UV‚Ì“ü‚Á‚Ä‚¢‚È‚¢‚Æ‚±‚ë‚ð•âŠÔ
		short cb0 = dst[y * max_w].cb;
		short cr0 = dst[y * max_w].cr;
		for (int x = 0; x < w - 2; x += 2) {
			short cb2 = dst[x + 2 + y * max_w].cb;
			short cr2 = dst[x + 2 + y * max_w].cr;
			dst[x + 1 + y * max_w].cb = (cb0 + cb2) >> 1;
			dst[x + 1 + y * max_w].cr = (cr0 + cr2) >> 1;
			cb0 = cb2;
			cr0 = cr2;
		}
		dst[w - 1 + y * max_w].cb = cb0;
		dst[w - 1 + y * max_w].cr = cr0;
	}

	// y•ûŒü•âŠÔ
	for (int y = 1; y + 1 < h; y += 2) {
		for (int x = 0; x < w; ++x) {
			short cb0 = dst[x + (y + 0) * max_w].cb;
			short cr0 = dst[x + (y + 0) * max_w].cr;
			short cb1 = dst[x + (y + 1) * max_w].cb;
			short cr1 = dst[x + (y + 1) * max_w].cr;
			dst[x + (y + 0) * max_w].cb = (cb0 * 3 + cb1 + 2) >> 2;
			dst[x + (y + 0) * max_w].cr = (cr0 * 3 + cr1 + 2) >> 2;
			dst[x + (y + 1) * max_w].cb = (cb0 + cb1 * 3 + 2) >> 2;
			dst[x + (y + 1) * max_w].cr = (cr0 + cr1 * 3 + 2) >> 2;
		}
	}
}

