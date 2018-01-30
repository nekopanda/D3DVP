
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
	DeintTest(TF_MID);
}

int main(int argc, char **argv)
{
	::testing::GTEST_FLAG(filter) = "TestBase.*";
	::testing::InitGoogleTest(&argc, argv);
	int result = RUN_ALL_TESTS();

	getchar();

	return result;
}

