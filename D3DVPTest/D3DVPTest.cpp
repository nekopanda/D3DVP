
#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#undef max
#undef min

#define AVS_LINKAGE_DLLIMPORT
#include "avisynth.h"
#pragma comment(lib, "avisynth.lib")

#include "gtest/gtest.h"

#include <fstream>
#include <string>
#include <memory>
#include <algorithm>

#include "convert.h"

std::string GetDirectoryName(const std::string& filename)
{
	std::string directory;
	const size_t last_slash_idx = filename.rfind('\\');
	if (std::string::npos != last_slash_idx)
	{
		directory = filename.substr(0, last_slash_idx);
	}
	return directory;
}

struct ScriptEnvironmentDeleter {
	void operator()(IScriptEnvironment* env) {
		env->DeleteScriptEnvironment();
	}
};

typedef std::unique_ptr<IScriptEnvironment2, ScriptEnvironmentDeleter> PEnv;

// テスト対象となるクラス Foo のためのフィクスチャ
class TestBase : public ::testing::Test {
protected:
	TestBase() { }

	virtual ~TestBase() {
		// テスト毎に実行される，例外を投げない clean-up をここに書きます．
	}

	// コンストラクタとデストラクタでは不十分な場合．
	// 以下のメソッドを定義することができます：
	
	virtual void SetUp() {
		// このコードは，コンストラクタの直後（各テストの直前）
		// に呼び出されます．
		char buf[MAX_PATH];
		GetModuleFileName(nullptr, buf, MAX_PATH);
		modulePath = GetDirectoryName(buf);
		workDirPath = GetDirectoryName(GetDirectoryName(modulePath)) + "\\TestScripts";
	}

	virtual void TearDown() {
		// このコードは，各テストの直後（デストラクタの直前）
		// に呼び出されます．
	}

	std::string modulePath;
	std::string workDirPath;

	enum TEST_FRAMES {
		TF_MID, TF_BEGIN, TF_END
	};

	void GetFrames(PClip& clip, TEST_FRAMES tf, IScriptEnvironment2* env);

	void DeintTest(TEST_FRAMES tf);
};

void TestBase::GetFrames(PClip& clip, TEST_FRAMES tf, IScriptEnvironment2* env)
{
	int nframes = clip->GetVideoInfo().num_frames;
	switch (tf) {
	case TF_MID:
		for (int i = 0; i < std::min(1000, nframes); ++i) {
			clip->GetFrame(i, env);
		}
		break;
	case TF_BEGIN:
		clip->GetFrame(0, env);
		clip->GetFrame(1, env);
		clip->GetFrame(2, env);
		clip->GetFrame(3, env);
		clip->GetFrame(4, env);
		clip->GetFrame(5, env);
		break;
	case TF_END:
		clip->GetFrame(nframes - 6, env);
		clip->GetFrame(nframes - 5, env);
		clip->GetFrame(nframes - 4, env);
		clip->GetFrame(nframes - 3, env);
		clip->GetFrame(nframes - 2, env);
		clip->GetFrame(nframes - 1, env);
		break;
	}
}

void TestBase::DeintTest(TEST_FRAMES tf)
{
	PEnv env;
	try {
		env = PEnv(CreateScriptEnvironment2());

		AVSValue result;
		std::string d3dvpPath = modulePath + "\\D3DVP.dll";
		env->LoadPlugin(d3dvpPath.c_str(), true, &result);

		std::string scriptpath = workDirPath + "\\script.avs";

		std::ofstream out(scriptpath);

		out << "src = LWLibavVideoSource(\"test.ts\")" << std::endl;
		out << "src.D3DVP(device=\"Intel\")" << std::endl;

		out.close();
		{
			PClip clip = env->Invoke("Import", scriptpath.c_str()).AsClip();
			GetFrames(clip, tf, env.get());
		}
	}
	catch (const AvisynthError& err) {
		printf("%s\n", err.msg);
		GTEST_FAIL();
	}
}

TEST_F(TestBase, DeintTest_)
{
	DeintTest(TF_BEGIN);
}

class ConvertTest : public ::testing::Test {
protected:
	ConvertTest() { }

	virtual ~ConvertTest() {
		// テスト毎に実行される，例外を投げない clean-up をここに書きます．
	}

	// コンストラクタとデストラクタでは不十分な場合．
	// 以下のメソッドを定義することができます：

	virtual void SetUp() {
		// このコードは，コンストラクタの直後（各テストの直前）
		// に呼び出されます．
	}

	virtual void TearDown() {
		// このコードは，各テストの直後（デストラクタの直前）
		// に呼び出されます．
	}
};

void CompareImageNV12(int height, int width, uint8_t* ref, uint8_t* test, int pitch)
{
	int heightUV = height >> 1;
	int widthUV = width >> 1;
	uint8_t* refUV = ref + height * pitch;
	uint8_t* testUV = test + height * pitch;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (ref[x + y * pitch] != test[x + y * pitch]) {
				printf("Error at Y(%d,%d): %d != %d\n", x, y, ref[x + y * pitch], test[x + y * pitch]);
				ASSERT_TRUE(0);
			}
		}
	}

	for (int y = 0; y < heightUV; ++y) {
		for (int x = 0; x < widthUV; ++x) {
			if (refUV[2 * x + 0 + y * pitch] != testUV[2 * x + 0 + y * pitch]) {
				printf("Error at U(%d,%d): %d != %d\n", x, y, refUV[2 * x + 0 + y * pitch], testUV[2 * x + 0 + y * pitch]);
				ASSERT_TRUE(0);
			}
			if (refUV[2 * x + 1 + y * pitch] != testUV[2 * x + 1 + y * pitch]) {
				printf("Error at V(%d,%d): %d != %d\n", x, y, refUV[2 * x + 1 + y * pitch], testUV[2 * x + 1 + y * pitch]);
				ASSERT_TRUE(0);
			}
		}
	}
}

void CompareImageYV12(int height, int width,
	uint8_t* refY, uint8_t* refU, uint8_t* refV, uint8_t* testY, uint8_t* testU, uint8_t* testV,
	int pitchY, int pitchUV)
{
	int heightUV = height >> 1;
	int widthUV = width >> 1;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (refY[x + y * pitchY] != testY[x + y * pitchY]) {
				printf("Error at Y(%d,%d): %d != %d\n", x, y, refY[x + y * pitchY], testY[x + y * pitchY]);
				ASSERT_TRUE(0);
			}
		}
	}

	for (int y = 0; y < heightUV; ++y) {
		for (int x = 0; x < widthUV; ++x) {
			if (refU[x + y * pitchUV] != testU[x + y * pitchUV]) {
				printf("Error at U(%d,%d): %d != %d\n", x, y, refU[x + y * pitchUV], testU[x + y * pitchUV]);
				ASSERT_TRUE(0);
			}
			if (refV[x + y * pitchUV] != testV[x + y * pitchUV]) {
				printf("Error at V(%d,%d): %d != %d\n", x, y, refV[x + y * pitchUV], testV[x + y * pitchUV]);
				ASSERT_TRUE(0);
			}
		}
	}
}

void CompareImageYUY2(int height, int width, uint8_t* ref, uint8_t* test, int pitch)
{
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (ref[2 * x + 0 + y * pitch] != test[2 * x + 0 + y * pitch]) {
				printf("Error at Y(%d,%d): %d != %d\n", x, y, ref[2 * x + 0 + y * pitch], test[2 * x + 0 + y * pitch]);
				ASSERT_TRUE(0);
			}
			if (ref[2 * x + 1 + y * pitch] != test[2 * x + 1 + y * pitch]) {
				printf("Error at UorV(%d,%d): %d != %d\n", x, y, ref[2 * x + 1 + y * pitch], test[2 * x + 1 + y * pitch]);
				ASSERT_TRUE(0);
			}
		}
	}
}

void CompareImageYC48(int height, int width, PIXEL_YC* ref, PIXEL_YC* test, int pitch)
{
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (ref[x + y * pitch].y != test[x + y * pitch].y) {
				printf("Error at Y(%d,%d): %d != %d\n", x, y, ref[x + y * pitch].y, test[x + y * pitch].y);
				ASSERT_TRUE(0);
			}
			if (ref[x + y * pitch].cb != test[x + y * pitch].cb) {
				printf("Error at U(%d,%d): %d != %d\n", x, y, ref[x + y * pitch].cb, test[x + y * pitch].cb);
				ASSERT_TRUE(0);
			}
			if (ref[x + y * pitch].cr != test[x + y * pitch].cr) {
				printf("Error at V(%d,%d): %d != %d\n", x, y, ref[x + y * pitch].cr, test[x + y * pitch].cr);
				ASSERT_TRUE(0);
			}
		}
	}
}

TEST_F(ConvertTest, yuv_to_nv12)
{
	int width = 1280;
	int height = 720;

	int widthUV = width >> 1;
	int heightUV = height >> 1;

	int pitchY = width + 16;
	int pitchUV = widthUV + 16;
	int dstPitch = width + 32;

	auto srcY = std::unique_ptr<uint8_t[]>(new uint8_t[pitchY * height]);
	auto srcU = std::unique_ptr<uint8_t[]>(new uint8_t[pitchUV * heightUV]);
	auto srcV = std::unique_ptr<uint8_t[]>(new uint8_t[pitchUV * heightUV]);
	auto ref = std::unique_ptr<uint8_t[]>(new uint8_t[dstPitch * (height + heightUV)]);
	auto test = std::unique_ptr<uint8_t[]>(new uint8_t[dstPitch * (height + heightUV)]);

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			srcY[x + y * pitchY] = rand() & 0xFF;
		}
	}
	for (int y = 0; y < heightUV; ++y) {
		for (int x = 0; x < widthUV; ++x) {
			srcU[x + y * pitchUV] = rand() & 0xFF;
			srcV[x + y * pitchUV] = rand() & 0xFF;
		}
	}

	yuv_to_nv12_c(height, width, ref.get(), dstPitch, srcY.get(), srcU.get(), srcV.get(), pitchY, pitchUV);
	yuv_to_nv12_avx2(height, width, test.get(), dstPitch, srcY.get(), srcU.get(), srcV.get(), pitchY, pitchUV);

	CompareImageNV12(height, width, ref.get(), test.get(), dstPitch);

	auto refY = std::unique_ptr<uint8_t[]>(new uint8_t[pitchY * height]);
	auto refU = std::unique_ptr<uint8_t[]>(new uint8_t[pitchUV * heightUV]);
	auto refV = std::unique_ptr<uint8_t[]>(new uint8_t[pitchUV * heightUV]);
	auto testY = std::unique_ptr<uint8_t[]>(new uint8_t[pitchY * height]);
	auto testU = std::unique_ptr<uint8_t[]>(new uint8_t[pitchUV * heightUV]);
	auto testV = std::unique_ptr<uint8_t[]>(new uint8_t[pitchUV * heightUV]);

	nv12_to_yuv_c(height, width, refY.get(), refU.get(), refV.get(), pitchY, pitchUV, ref.get(), dstPitch);
	nv12_to_yuv_avx2(height, width, testY.get(), testU.get(), testV.get(), pitchY, pitchUV, test.get(), dstPitch);

	CompareImageYV12(height, width, refY.get(), refU.get(), refV.get(), testY.get(), testU.get(), testV.get(), pitchY, pitchUV);
	CompareImageYV12(height, width, srcY.get(), srcU.get(), srcV.get(), testY.get(), testU.get(), testV.get(), pitchY, pitchUV);
}

TEST_F(ConvertTest, yc48_to_yuy2)
{
	int width = 1280;
	int height = 720;

	int widthUV = width >> 1;
	int heightUV = height >> 1;

	int pitchYC48 = width + 16;
	int pitchYUY2 = width * 2 + 32;

	auto src = std::unique_ptr<PIXEL_YC[]>(new PIXEL_YC[pitchYC48 * height]);
	auto ref = std::unique_ptr<uint8_t[]>(new uint8_t[pitchYUY2 * height]);
	auto test = std::unique_ptr<uint8_t[]>(new uint8_t[pitchYUY2 * height]);

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			src[x + y * pitchYC48].y = rand() & 0x7FFF;
			src[x + y * pitchYC48].cb = rand() & 0x7FFF - 16384;
			src[x + y * pitchYC48].cr = rand() & 0x7FFF - 16384;
		}
	}

	yc48_to_yuy2_c(ref.get(), pitchYUY2, src.get(), width, height, pitchYC48);
	yc48_to_yuy2_avx2(test.get(), pitchYUY2, src.get(), width, height, pitchYC48);

	CompareImageYUY2(height, width, ref.get(), test.get(), pitchYUY2);

	auto ref2 = std::unique_ptr<PIXEL_YC[]>(new PIXEL_YC[pitchYC48 * height]);
	auto test2 = std::unique_ptr<PIXEL_YC[]>(new PIXEL_YC[pitchYC48 * height]);

	yuy2_to_yc48_c(ref2.get(), ref.get(), pitchYUY2, width, height, pitchYC48);
	yuy2_to_yc48_avx2(test2.get(), test.get(), pitchYUY2, width, height, pitchYC48);

	CompareImageYC48(height, width, ref2.get(), test2.get(), pitchYC48);
}

int main(int argc, char **argv)
{
	::testing::GTEST_FLAG(filter) = "ConvertTest.*";
	::testing::InitGoogleTest(&argc, argv);
	int result = RUN_ALL_TESTS();

	getchar();

	return result;
}

