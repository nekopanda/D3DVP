

#define NOMINMAX
#include <Windows.h>
#include "filter.h"

#include <stdint.h>
#include <string.h>

#include <immintrin.h>

void yuv_to_nv12_avx2(
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
		int x = 0;
		for (; x <= (widthUV - 16); x += 16) {
			auto u = _mm_loadu_si128((__m128i*)&srcU[x + y * pitchUV]);
			auto v = _mm_loadu_si128((__m128i*)&srcV[x + y * pitchUV]);
			auto a = _mm256_set_m128i(_mm_unpackhi_epi8(u, v), _mm_unpacklo_epi8(u, v));
			_mm256_storeu_si256((__m256i*)&dstUV[x * 2 + y * dstPitch], a);
		}
		for (; x < widthUV; ++x) {
			dstUV[x * 2 + 0 + y * dstPitch] = srcU[x + y * pitchUV];
			dstUV[x * 2 + 1 + y * dstPitch] = srcV[x + y * pitchUV];
		}
	}
}

void nv12_to_yuv_avx2(
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

	static const __m256i pattern = _mm256_set_epi8(
		15, 13, 11, 9, 7, 5, 3, 1,
		14, 12, 10, 8, 6, 4, 2, 0,
		15, 13, 11, 9, 7, 5, 3, 1,
		14, 12, 10, 8, 6, 4, 2, 0);

	for (int y = 0; y < height; ++y) {
		memcpy(&dstY[y * pitchY], &srcY[y * srcPitch], width);
	}

	for (int y = 0; y < heightUV; ++y) {
		int x = 0;
		for ( ; x <= (widthUV - 16); x += 16) {
			auto a = _mm256_loadu_si256((const __m256i*)&srcUV[x * 2 + y * srcPitch]);
			// 128bit‚²‚Æ‚ÉU‚è•ª‚¯
			auto b = _mm256_shuffle_epi8(a, pattern);
			// 128bit‹«ŠE‚ð‰z‚¦‚éˆÚ“®
			auto c = _mm256_permute4x64_epi64(b, _MM_SHUFFLE(3, 1, 2, 0));
			// 128bit‚²‚Æ‚ÉƒXƒgƒA
			_mm_storeu_si128((__m128i*)&dstU[x + y * pitchUV], _mm256_extracti128_si256(c, 0));
			_mm_storeu_si128((__m128i*)&dstV[x + y * pitchUV], _mm256_extracti128_si256(c, 1));
		}
		for (; x < widthUV; ++x) {
			dstU[x + y * pitchUV] = srcUV[x * 2 + 0 + y * srcPitch];
			dstV[x + y * pitchUV] = srcUV[x * 2 + 1 + y * srcPitch];
		}
	}
}

#define MM_ABS(x) (((x) < 0) ? -(x) : (x))
#define _mm256_alignr256_epi8(a, b, i) \
	((i<=16) ? _mm256_alignr_epi8(_mm256_permute2x128_si256(a, b, (0x00<<4) + 0x03), b, i) \
			 : _mm256_alignr_epi8(a, _mm256_permute2x128_si256(a, b, (0x00<<4) + 0x03), MM_ABS(i-16)))

static const int LSFT_YCC_8    = 4;
static const int UV_OFFSET_x1 = (1<<11);

static const int Y_L_MUL    = 219;
static const int Y_L_ADD_8  = 383;
static const int Y_L_ADD_16 = Y_L_ADD_8>>8;
static const int Y_L_RSH_8  = 12;
static const int Y_L_RSH_16 = Y_L_RSH_8-8;

static const int UV_L_MUL         = 14;
static const int UV_L_ADD_8_444   = 132;
static const int UV_L_ADD_16_444  = UV_L_ADD_8_444>>8;
static const int UV_L_RSH_8_444   =  8;
static const int UV_L_RSH_16_444  =  UV_L_RSH_8_444 + 0 - 8;

alignas(32) static const uint8_t ARRAY_SUFFLE_YCP_Y[32] = {
	0, 1, 6, 7, 12, 13, 2, 3, 8, 9, 14, 15, 4, 5, 10, 11,
	0, 1, 6, 7, 12, 13, 2, 3, 8, 9, 14, 15, 4, 5, 10, 11
};

static const __m256i yC_Y_L_MA_8       = _mm256_set1_epi32((Y_L_ADD_8 << 16) | Y_L_MUL);
static const __m256i yC_UV_L_MA_8_444  = _mm256_set1_epi32((UV_L_ADD_8_444 << 16) | UV_L_MUL);
static const __m256i yC_Y_L_MA_16      = _mm256_set1_epi32((Y_L_ADD_16 << 16) | Y_L_MUL);
static const __m256i yC_UV_L_MA_16_444 = _mm256_set1_epi32((UV_L_ADD_16_444 << 16) | UV_L_MUL);

static __forceinline void gather_y_uv_from_yc48(__m256i& y0, __m256i& y1, __m256i y2) {
	const int MASK_INT_Y  = 0x80 + 0x10 + 0x02;
	const int MASK_INT_UV = 0x40 + 0x20 + 0x01;
	__m256i y3 = y0;
	__m256i y4 = y1;
	__m256i y5 = y2;

	y0 = _mm256_blend_epi32(y3, y4, 0xf0);                    // 384, 0
	y1 = _mm256_permute2x128_si256(y3, y5, (0x02<<4) + 0x01); // 512, 128
	y2 = _mm256_blend_epi32(y4, y5, 0xf0);                    // 640, 256

	y3 = _mm256_blend_epi16(y0, y1, MASK_INT_Y);
	y3 = _mm256_blend_epi16(y3, y2, MASK_INT_Y>>2);

	y1 = _mm256_blend_epi16(y0, y1, MASK_INT_UV);
	y1 = _mm256_blend_epi16(y1, y2, MASK_INT_UV>>2);
	y1 = _mm256_alignr_epi8(y1, y1, 2);
	y1 = _mm256_shuffle_epi32(y1, _MM_SHUFFLE(1, 2, 3, 0));//UV1s–Ú

	y0 = _mm256_shuffle_epi8(y3, _mm256_load_si256((const __m256i*)ARRAY_SUFFLE_YCP_Y));
}

static __forceinline __m256i convert_y_range_from_yc48(__m256i y0, __m256i yC_Y_MA, int Y_RSH, const __m256i& yC_YCC) {
	__m256i y7;

	y7 = _mm256_unpackhi_epi16(y0, _mm256_set1_epi16(1));
	y0 = _mm256_unpacklo_epi16(y0, _mm256_set1_epi16(1));

	y0 = _mm256_madd_epi16(y0, yC_Y_MA);
	y7 = _mm256_madd_epi16(y7, yC_Y_MA);
	y0 = _mm256_srai_epi32(y0, Y_RSH);
	y7 = _mm256_srai_epi32(y7, Y_RSH);
	y0 = _mm256_add_epi32(y0, yC_YCC);
	y7 = _mm256_add_epi32(y7, yC_YCC);

	y0 = _mm256_packus_epi32(y0, y7);

	return y0;
}
static __forceinline __m256i convert_uv_range_after_adding_offset(__m256i y0, const __m256i& yC_UV_MA, int UV_RSH, const __m256i& yC_YCC) {
	__m256i y7;
	y7 = _mm256_unpackhi_epi16(y0, _mm256_set1_epi16(1));
	y0 = _mm256_unpacklo_epi16(y0, _mm256_set1_epi16(1));

	y0 = _mm256_madd_epi16(y0, yC_UV_MA);
	y7 = _mm256_madd_epi16(y7, yC_UV_MA);
	y0 = _mm256_srai_epi32(y0, UV_RSH);
	y7 = _mm256_srai_epi32(y7, UV_RSH);
	y0 = _mm256_add_epi32(y0, yC_YCC);
	y7 = _mm256_add_epi32(y7, yC_YCC);

	y0 = _mm256_packus_epi32(y0, y7);

	return y0;
}
static __forceinline __m256i convert_uv_range_from_yc48(__m256i y0, const __m256i& yC_UV_OFFSET_x1, const __m256i& yC_UV_MA, int UV_RSH, const __m256i& yC_YCC) {
	y0 = _mm256_add_epi16(y0, yC_UV_OFFSET_x1);

	return convert_uv_range_after_adding_offset(y0, yC_UV_MA, UV_RSH, yC_YCC);
}

static __forceinline __m256i convert_y_range_to_yc48(__m256i y0) {
	//coef = 4788
	//((( y - 32768 ) * coef) >> 16 ) + (coef/2 - 299)
	const __m256i yC_0x8000 = _mm256_slli_epi16(_mm256_cmpeq_epi32(y0, y0), 15);
	y0 = _mm256_add_epi16(y0, yC_0x8000); // -32768
	y0 = _mm256_mulhi_epi16(y0, _mm256_set1_epi16(4788));
	y0 = _mm256_adds_epi16(y0, _mm256_set1_epi16(4788/2 - 299));
	return y0;
}

static __forceinline __m256i convert_uv_range_to_yc48(__m256i y0) {
	//coeff = 4682
	//UV = (( uv - 32768 ) * coef + (1<<15) ) >> 16
	const __m256i yC_coeff = _mm256_unpacklo_epi16(_mm256_set1_epi16(4682), _mm256_set1_epi16(-1));
	const __m256i yC_0x8000 = _mm256_slli_epi16(_mm256_cmpeq_epi32(y0, y0), 15);
	__m256i y1;
	y0 = _mm256_add_epi16(y0, yC_0x8000); // -32768
	y1 = _mm256_unpackhi_epi16(y0, yC_0x8000);
	y0 = _mm256_unpacklo_epi16(y0, yC_0x8000);
	y0 = _mm256_madd_epi16(y0, yC_coeff);
	y1 = _mm256_madd_epi16(y1, yC_coeff);
	y0 = _mm256_srai_epi32(y0, 16);
	y1 = _mm256_srai_epi32(y1, 16);
	y0 = _mm256_packs_epi32(y0, y1);
	return y0;
}

static __forceinline void gather_y_u_v_from_yc48(__m256i& y0, __m256i& y1, __m256i& y2) {
	__m256i y3, y4, y5;
	const int MASK_INT = 0x40 + 0x08 + 0x01;
	y3 = _mm256_blend_epi32(y0, y1, 0xf0);                    // 384, 0
	y4 = _mm256_permute2x128_si256(y0, y2, (0x02<<4) + 0x01); // 512, 128
	y5 = _mm256_blend_epi32(y1, y2, 0xf0);                    // 640, 256

	y0 = _mm256_blend_epi16(y5, y3, MASK_INT);
	y1 = _mm256_blend_epi16(y4, y5, MASK_INT);
	y2 = _mm256_blend_epi16(y3, y4, MASK_INT);

	y0 = _mm256_blend_epi16(y0, y4, MASK_INT<<1);
	y1 = _mm256_blend_epi16(y1, y3, MASK_INT<<1);
	y2 = _mm256_blend_epi16(y2, y5, MASK_INT<<1);

	__m256i yShuffle = _mm256_load_si256((const __m256i*)ARRAY_SUFFLE_YCP_Y);
	y0 = _mm256_shuffle_epi8(y0, yShuffle);
	y1 = _mm256_shuffle_epi8(y1, _mm256_alignr_epi8(yShuffle, yShuffle, 6));
	y2 = _mm256_shuffle_epi8(y2, _mm256_alignr_epi8(yShuffle, yShuffle, 12));
}


static __forceinline void yc48_to_yuy2_avx2_block(uint8_t *dst, const PIXEL_YC *ycp) {
	const __m256i yC_YCC = _mm256_set1_epi32(1<<LSFT_YCC_8);
	__m256i y1 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycp +  0));
	__m256i y2 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycp + 32));
	__m256i y3 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycp + 64));
	gather_y_uv_from_yc48(y1, y2, y3);
	y1 = convert_y_range_from_yc48(y1, yC_Y_L_MA_8, Y_L_RSH_8, yC_YCC);
	y2 = convert_uv_range_from_yc48(y2, _mm256_set1_epi16(UV_OFFSET_x1), yC_UV_L_MA_8_444, UV_L_RSH_8_444, yC_YCC);
	y2 = _mm256_slli_epi16(y2, 8);
	y1 = _mm256_or_si256(y1, y2);
	_mm256_storeu_si256((__m256i *)dst, y1);
}

void yc48_to_yuy2_avx2(uint8_t *dst, int pitch, const PIXEL_YC *src, int w, int h, int max_w) {
	for (int y = 0; y < h; y++, dst += pitch, src += max_w) {
		const PIXEL_YC *srcptr = src;
		uint8_t *dstptr = dst;
		int x = 0;
		for (; x < w - 16; x += 16, dstptr += 32, srcptr += 16) {
			yc48_to_yuy2_avx2_block(dstptr, srcptr);
		}
		int offset = x - (w - 16);
		dstptr -= offset * 2;
		srcptr -= offset;
		yc48_to_yuy2_avx2_block(dstptr, srcptr);
	}
}

//c•ûŒü•âŠÔ‚È‚µ
static __forceinline void yc48_to_nv12_avx2_block(uint8_t *dstY, uint8_t *dstUV, const PIXEL_YC *ycp, const PIXEL_YC *ycpw, int pitch, int ifield) {
	const __m256i yC_YCC = _mm256_set1_epi32(1<<LSFT_YCC_8);
	__m256i y0, y1, y2, y3, y4, y5, y6, y7;
	y1 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycp +   0));
	y2 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycp +  32));
	y3 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycp +  64));
	y5 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycp +  96));
	y6 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycp + 128));
	y7 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycp + 160));
	gather_y_uv_from_yc48(y1, y2, y3);
	gather_y_uv_from_yc48(y5, y6, y7);
	y0 = y2;
	y4 = y6;
	y1 = convert_y_range_from_yc48(y1, yC_Y_L_MA_8, Y_L_RSH_8, yC_YCC);
	y5 = convert_y_range_from_yc48(y5, yC_Y_L_MA_8, Y_L_RSH_8, yC_YCC);
	y1 = _mm256_packus_epi16(y1, y5);
	y1 = _mm256_permute4x64_epi64(y1, _MM_SHUFFLE(3, 1, 2, 0));
	_mm256_storeu_si256((__m256i *)dstY, y1);

	y1 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycpw +   0));
	y2 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycpw +  32));
	y3 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycpw +  64));
	y5 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycpw +  96));
	y6 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycpw + 128));
	y7 = _mm256_loadu_si256((const __m256i *)((const uint8_t *)ycpw + 160));
	gather_y_uv_from_yc48(y1, y2, y3);
	gather_y_uv_from_yc48(y5, y6, y7);
	y1 = convert_y_range_from_yc48(y1, yC_Y_L_MA_8, Y_L_RSH_8, yC_YCC);
	y5 = convert_y_range_from_yc48(y5, yC_Y_L_MA_8, Y_L_RSH_8, yC_YCC);
	y1 = _mm256_packus_epi16(y1, y5);
	y1 = _mm256_permute4x64_epi64(y1, _MM_SHUFFLE(3, 1, 2, 0));
	_mm256_storeu_si256((__m256i *)(dstY + pitch*2), y1);

	y0 = convert_uv_range_from_yc48(y0, _mm256_set1_epi16(UV_OFFSET_x1), yC_UV_L_MA_8_444, UV_L_RSH_8_444, yC_YCC);
	y6 = convert_uv_range_from_yc48(y6, _mm256_set1_epi16(UV_OFFSET_x1), yC_UV_L_MA_8_444, UV_L_RSH_8_444, yC_YCC);
	y0 = _mm256_packus_epi16(y0, y6);
	y0 = _mm256_permute4x64_epi64(y0, _MM_SHUFFLE(3, 1, 2, 0));
	_mm256_storeu_si256((__m256i *)dstUV, y0);
}

void yc48_to_nv12_avx2(uint8_t *dst, int pitch, const PIXEL_YC *src, int w, int h, int max_w) {
	for (int y = 0; y < h; y += 4) {
		for (int ifield = 0; ifield < 2; ifield++) {
			const PIXEL_YC *ycp = &src[max_w * (y + ifield)];
			const PIXEL_YC *ycpw = &ycp[2 * max_w];
			uint8_t *dstY = &dst[pitch * (y + ifield)];
			uint8_t *dstUV = &dst[pitch * (h + ifield + (y >> 1))];
			int x = 0;
			for (; x < w - 32; x += 32, ycp += 32, ycpw += 32, dstY += 32, dstUV += 32) {
				yc48_to_nv12_avx2_block(dstY, dstUV, ycp, ycpw, pitch, ifield);
			}
			int offset = x - (w - 32);
			dstY -= offset;
			dstUV -= offset;
			ycp -= offset;
			ycpw -= offset;
			yc48_to_nv12_avx2_block(dstY, dstUV, ycp, ycpw, pitch, ifield);
		}
	}
}

static __forceinline void gather_y_u_v_to_yc48(__m256i& y0, __m256i& y1, __m256i& y2) {
	__m256i y3, y4, y5;

	alignas(16) static const uint8_t shuffle_yc48[32] = {
		0x00, 0x01, 0x06, 0x07, 0x0C, 0x0D, 0x02, 0x03, 0x08, 0x09, 0x0E, 0x0F, 0x04, 0x05, 0x0A, 0x0B,
		0x00, 0x01, 0x06, 0x07, 0x0C, 0x0D, 0x02, 0x03, 0x08, 0x09, 0x0E, 0x0F, 0x04, 0x05, 0x0A, 0x0B
	};
	y5 = _mm256_load_si256((__m256i *)shuffle_yc48);
	y0 = _mm256_shuffle_epi8(y0, y5);                             //5,2,7,4,1,6,3,0
	y1 = _mm256_shuffle_epi8(y1, _mm256_alignr_epi8(y5, y5, 14)); //2,7,4,1,6,3,0,5
	y2 = _mm256_shuffle_epi8(y2, _mm256_alignr_epi8(y5, y5, 12)); //7,4,1,6,3,0,5,2

	y3 = _mm256_blend_epi16(y0, y1, 0x80 + 0x10 + 0x02);
	y3 = _mm256_blend_epi16(y3, y2, 0x20 + 0x04);        //384, 0

	y4 = _mm256_blend_epi16(y2, y1, 0x20 + 0x04);
	y4 = _mm256_blend_epi16(y4, y0, 0x80 + 0x10 + 0x02); //512, 128

	y2 = _mm256_blend_epi16(y2, y0, 0x20 + 0x04);
	y2 = _mm256_blend_epi16(y2, y1, 0x40 + 0x08 + 0x01); //640, 256

	y0 = _mm256_permute2x128_si256(y3, y4, (0x02<<4) + 0x00); // 128, 0
	y1 = _mm256_blend_epi32(y2, y3, 0xf0);                    // 384, 256
	y2 = _mm256_permute2x128_si256(y4, y2, (0x03<<4) + 0x01); // 640, 512
}

template<bool lastBlock>
static __forceinline void yuy2_to_yc48_avx2_block(__m256i& yPrev, PIXEL_YC *dstptr, const uint8_t *srcptr) {
	__m256i yY = _mm256_and_si256(yPrev, _mm256_set1_epi16(0x00ff));
	__m256i yUV0 = _mm256_srli_epi16(_mm256_andnot_si256(_mm256_set1_epi16(0x00ff), yPrev), 8);
	__m256i yUV1, yNext;
	if (lastBlock) {
		yUV1 = _mm256_shuffle_epi32(yUV0, _MM_SHUFFLE(3,3,3,3));
		yUV1 = _mm256_permute4x64_epi64(yUV1, _MM_SHUFFLE(3,3,3,3));
	} else {
		yNext = _mm256_loadu_si256((const __m256i *)(srcptr + 32));
		yUV1 = _mm256_srli_epi16(_mm256_andnot_si256(_mm256_set1_epi16(0x00ff), yNext), 8);
	}
	__m256i yUV01 = _mm256_alignr256_epi8(yUV1, yUV0, 4);
	yUV01 = _mm256_avg_epu16(yUV01, yUV0);

	yY = _mm256_slli_epi16(yY, 8);
	yUV0 = _mm256_slli_epi16(yUV0, 8);
	yUV01 = _mm256_slli_epi16(yUV01, 8);

	__m256i yU = _mm256_blend_epi16(yUV0, _mm256_slli_epi32(yUV01, 16), 0x80+0x20+0x08+0x02);
	__m256i yV = _mm256_blend_epi16(_mm256_srli_epi32(yUV0, 16), yUV01, 0x80+0x20+0x08+0x02);

	__m256i y1 = convert_y_range_to_yc48(yY);
	__m256i y2 = convert_uv_range_to_yc48(yU);
	__m256i y3 = convert_uv_range_to_yc48(yV);
	gather_y_u_v_to_yc48(y1, y2, y3);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr +  0), y1);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 32), y2);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 64), y3);
	if (!lastBlock) {
		yPrev = yNext;
	}
}

void yuy2_to_yc48_avx2(PIXEL_YC *dst, const uint8_t *src, int pitch, int w, int h, int max_w) {
	for (int y = 0; y < h; y++, dst += max_w, src += pitch) {
		const uint8_t *srcptr = src;
		PIXEL_YC *dstptr = dst;
		__m256i y0 = _mm256_loadu_si256((const __m256i *)srcptr);
		int x = 0;
		for (; x < w - 16; x += 16, dstptr += 16, srcptr += 32) {
			yuy2_to_yc48_avx2_block<false>(y0, dstptr, srcptr);
		}
		int offset = x - (w - 16);
		if (offset > 0) {
			dstptr -= offset;
			srcptr -= offset * 2;
			y0 = _mm256_loadu_si256((const __m256i *)srcptr);
		}
		yuy2_to_yc48_avx2_block<true>(y0, dstptr, srcptr);
	}
}

static __forceinline void nv12_to_yc48_avx2_load_y(
	__m256i& yY0lo, __m256i& yY0hi, __m256i& yY1lo, __m256i& yY1hi,
	const uint8_t *srcYptr, int pitch) {
	__m256i yY0 = _mm256_loadu_si256((const __m256i *)srcYptr);
	__m256i yY1 = _mm256_loadu_si256((const __m256i *)&srcYptr[pitch]);

	__m256i y0 = _mm256_unpacklo_epi8(_mm256_setzero_si256(), yY0);
	__m256i y1 = _mm256_unpackhi_epi8(_mm256_setzero_si256(), yY0);
	__m256i y2 = _mm256_unpacklo_epi8(_mm256_setzero_si256(), yY1);
	__m256i y3 = _mm256_unpackhi_epi8(_mm256_setzero_si256(), yY1);

	yY0lo = _mm256_permute2x128_si256(y0, y1, (2<<4)+0);
	yY0hi = _mm256_permute2x128_si256(y0, y1, (3<<4)+1);
	yY1lo = _mm256_permute2x128_si256(y2, y3, (2<<4)+0);
	yY1hi = _mm256_permute2x128_si256(y2, y3, (3<<4)+1);
}

static __forceinline void nv12_to_yc48_avx2_interpUV_vert(
	__m256i& yUV0lo, __m256i& yUV0hi, const uint8_t *srcUVptr) {
	__m256i y0, y1 = _mm256_loadu_si256((const __m256i *) srcUVptr);

	y0 = _mm256_unpacklo_epi8(_mm256_setzero_si256(), y1);
	y1 = _mm256_unpackhi_epi8(_mm256_setzero_si256(), y1);

	yUV0lo = _mm256_permute2x128_si256(y0, y1, (2<<4)+0);
	yUV0hi = _mm256_permute2x128_si256(y0, y1, (3<<4)+1);
}

static __forceinline void nv12_to_yc48_avx2_interpUV_horizontal(__m256i& yU, __m256i& yV, const __m256i& yUV0, const __m256i& yUV1) {
	__m256i yUV01 = _mm256_alignr256_epi8(yUV1, yUV0, 4);
	yUV01 = _mm256_avg_epu16(yUV01, yUV0);

	yU = _mm256_blend_epi16(yUV0, _mm256_slli_epi32(yUV01, 16), 0x80+0x20+0x08+0x02);
	yV = _mm256_blend_epi16(_mm256_srli_epi32(yUV0, 16), yUV01, 0x80+0x20+0x08+0x02);
}

//‰¡•ûŒü•âŠÔ‚ ‚èAc•ûŒü•âŠÔ‚È‚µ
template<bool lastBlock>
static __forceinline void nv12_to_yc48_avx2_block(
	__m256i& yUV00lo, __m256i& yUV00hi,
	PIXEL_YC *dstptr, const uint8_t *srcYptr, const uint8_t *srcUVptr, int y, int h, int pitch, int max_w) {
	__m256i yY0lo, yY0hi, yY1lo, yY1hi;
	nv12_to_yc48_avx2_load_y(yY0lo, yY0hi, yY1lo, yY1hi, srcYptr, pitch);

	__m256i yUV01lo, yUV01hi;
	if (lastBlock) {
		yUV01lo = _mm256_shuffle_epi32(yUV00hi, _MM_SHUFFLE(3,3,3,3));
		yUV01lo = _mm256_permute4x64_epi64(yUV01lo, _MM_SHUFFLE(3,3,3,3));
	} else {
		nv12_to_yc48_avx2_interpUV_vert(yUV01lo, yUV01hi, srcUVptr+32);
	}

	__m256i yU0lo, yU0hi, yV0lo, yV0hi;
	nv12_to_yc48_avx2_interpUV_horizontal(yU0lo, yV0lo, yUV00lo, yUV00hi);
	nv12_to_yc48_avx2_interpUV_horizontal(yU0hi, yV0hi, yUV00hi, yUV01lo);
	__m256i y1 = convert_y_range_to_yc48(yY0lo);
	__m256i y2 = convert_uv_range_to_yc48(yU0lo);
	__m256i y3 = convert_uv_range_to_yc48(yV0lo);
	__m256i y2lo = y2;
	__m256i y3lo = y3;
	gather_y_u_v_to_yc48(y1, y2, y3);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr +  0), y1);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 32), y2);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 64), y3);

	y1 = convert_y_range_to_yc48(yY0hi);
	y2 = convert_uv_range_to_yc48(yU0hi);
	y3 = convert_uv_range_to_yc48(yV0hi);
	__m256i y2hi = y2;
	__m256i y3hi = y3;
	gather_y_u_v_to_yc48(y1, y2, y3);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr +  96), y1);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 128), y2);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 160), y3);

	dstptr += max_w;

	y1 = convert_y_range_to_yc48(yY1lo);
	y2 = y2lo;
	y3 = y3lo;
	gather_y_u_v_to_yc48(y1, y2, y3);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr +  0), y1);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 32), y2);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 64), y3);

	y1 = convert_y_range_to_yc48(yY1hi);
	y2 = y2hi;
	y3 = y3hi;
	gather_y_u_v_to_yc48(y1, y2, y3);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr +  96), y1);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 128), y2);
	_mm256_storeu_si256((__m256i *)((uint8_t *)dstptr + 160), y3);

	if (!lastBlock) {
		yUV00lo = yUV01lo;
		yUV00hi = yUV01hi;
	}
}

void nv12_to_yc48_avx2(PIXEL_YC *dst, const uint8_t *src, int pitch, int w, int h, int max_w) {
	const uint8_t *srcY = src;
	const uint8_t *srcUV = srcY + h * pitch;

	for (int y = 0; y < h; y += 2, dst += max_w*2, srcY += pitch*2, srcUV += pitch) {
		const uint8_t *srcYptr = srcY;
		const uint8_t *srcUVptr = srcUV;
		PIXEL_YC *dstptr = dst;
		__m256i yUV00lo, yUV00hi;
		nv12_to_yc48_avx2_interpUV_vert(yUV00lo, yUV00hi, srcUVptr);
		int x = 0;
		for (; x < w - 32; x += 32, dstptr += 32, srcYptr += 32, srcUVptr += 32) {
			nv12_to_yc48_avx2_block<false>(yUV00lo, yUV00hi,
				dstptr, srcYptr, srcUVptr, y, h, pitch, max_w);
		}
		int offset = x - (w - 32);
		if (offset > 0) {
			dstptr -= offset;
			srcYptr -= offset;
			srcUVptr -= offset;
			nv12_to_yc48_avx2_interpUV_vert(yUV00lo, yUV00hi, srcUVptr);
		}
		nv12_to_yc48_avx2_block<true>(yUV00lo, yUV00hi,
			dstptr, srcYptr, srcUVptr, y, h, pitch, max_w);
	}
}
