
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
