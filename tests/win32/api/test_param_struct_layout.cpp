/* 参数结构体自报字节数在五处入口的验收（视频编 / 视频解 / 音频编 / 音频解 / 容器写）。
 * 钉三件事：0 = 存量 memset 写法照旧受理（字段名与各字段语义都没动，这类调用方不必改码）；
 * 显式填 sizeof 受理；其余任何值一律显式拒 —— 含「按旧布局填的对象，其首字段的值落进本布局首位」这一形状。
 * 节拍面不在本件的五处之内，也不套同一口径：那一面的自报字段只受理显式填入的本结构体长度、填 0 拒
 * （例外与理由写在 `include/CLV_Pace.h` 的参数结构体说明处），由节器件自己的用例分档断言。
 * 音频解码侧的「过了布局这一道」用可观察形式表达：流信息缺失走的是拒解态句柄 + STREAMINFO_ABSENT，
 * 而布局不符走的是 NULL + VALUE_RANGE，两条路径分得开，不必在这里拼一份真流信息。
 * 容器面多断一条：拒发生在开文件之前，磁盘上不留残件。 */

#include "CLV_Audio.h"
#include "CLV_Container.h"
#include "CLV_Video.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace
{

	std::string TempPath(const std::string& stem)
	{
		std::error_code ec;
		const auto dir = std::filesystem::temp_directory_path(ec);
		std::string name = dir.generic_string();
		if (! name.empty() && name.back() != '/') name.push_back('/');
		name += stem + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".clv";
		return name;
	}

	void Discard(const std::string& path)
	{
		std::error_code ec;
		std::filesystem::remove(std::filesystem::u8path(path), ec);
	}

	bool FileExists(const std::string& path)
	{
		std::error_code ec;
		return std::filesystem::exists(std::filesystem::u8path(path), ec);
	}

	struct ScopedFile
	{
		explicit ScopedFile(std::string path): path_(std::move(path)) {}

		~ScopedFile() { Discard(path_); }

		const char* c_str() const { return path_.c_str(); }

		private:

		std::string path_;
	};

	/* declared = true 时把自报尺寸填成 sizeof，false 时留 0（存量写法）。 */
	CLV_VideoEncParamsC VideoEnc(bool declared)
	{
		CLV_VideoEncParamsC p;
		std::memset(&p, 0, sizeof(p));
		if (declared) p.struct_size = static_cast<uint32_t>(sizeof(p));
		p.display_width = 176;
		p.display_height = 144;
		p.bit_depth = 8;
		p.quant_precision = 7;
		p.default_quant = 8;
		p.mode = CLV_VIDEO_MODE_LOSSY;
		return p;
	}

	CLV_VideoStreamParamsC VideoStream(bool declared)
	{
		CLV_VideoStreamParamsC p;
		std::memset(&p, 0, sizeof(p));
		if (declared) p.struct_size = static_cast<uint32_t>(sizeof(p));
		p.bit_depth = 8;
		p.quant_precision = 7;
		p.obmc_disable = 1;
		p.display_width = 176;
		p.display_height = 144;
		return p;
	}

	CLV_AudioEncParamsC AudioEnc(bool declared)
	{
		CLV_AudioEncParamsC p;
		std::memset(&p, 0, sizeof(p));
		if (declared) p.struct_size = static_cast<uint32_t>(sizeof(p));
		p.sample_rate = 8000;
		p.channels = 1;
		p.bit_depth = 16;
		p.blocksize = 1152;
		p.level = 5;
		return p;
	}

	CLV_AudioStreamParamsC AudioStream(bool declared)
	{
		CLV_AudioStreamParamsC p;
		std::memset(&p, 0, sizeof(p));
		if (declared) p.struct_size = static_cast<uint32_t>(sizeof(p));
		p.sample_rate = 8000;
		p.channels = 1;
		p.bit_depth = 16;
		p.max_packet_size = 65536;
		return p;
	}

}	 // namespace

TEST(ParamStructLayout, ZeroFieldKeepsLegacyCallShapeOnAllFaces)
{
	CLV_VideoError verr = CLV_VIDEO_OK;
	CLV_VideoEncParamsC ep = VideoEnc(false);
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &verr);
	ASSERT_EQ(verr, CLV_VIDEO_OK);
	ASSERT_NE(e, nullptr);
	CLV_VideoStreamParamsC sp = VideoStream(false);
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&sp, &verr);
	ASSERT_EQ(verr, CLV_VIDEO_OK);
	ASSERT_NE(d, nullptr);

	CLV_AudioError aerr = CLV_AUDIO_OK;
	CLV_AudioEncParamsC aep = AudioEnc(false);
	CLV_AudioEncoder* ae = CLV_AudioEncoderOpen(&aep, &aerr);
	ASSERT_EQ(aerr, CLV_AUDIO_OK);
	ASSERT_NE(ae, nullptr);
	CLV_AudioStreamParamsC asp = AudioStream(false);
	CLV_AudioDecoder* ad = CLV_AudioDecoderOpen(&asp, nullptr, 0u, &aerr);
	/* 留 0 ⇒ 布局这一道过了；流信息没给才落到缺流信息那一档。 */
	EXPECT_NE(ad, nullptr);
	EXPECT_EQ(aerr, CLV_AUDIO_STREAMINFO_ABSENT) << "布局被受理的证据 = 拿得到句柄，只报缺流信息";

	std::string path = TempPath("layout_zero");
	ScopedFile guard(path);
	CLV_WriteParams wp;
	std::memset(&wp, 0, sizeof(wp));
	wp.index_present = 1;
	CLV_ContainerError cerr = CLV_CONTAINER_OK;
	CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(guard.c_str(), &wp, &cerr);
	ASSERT_EQ(cerr, CLV_CONTAINER_OK);
	ASSERT_NE(w, nullptr);
	CLV_ContainerWriterClose(w);

	std::cout << "[自报尺寸] 留 0（存量 memset 写法）五处全过：视频编 / 视频解 / 音频编 / 音频解 / 容器写\n";

	CLV_VideoEncoderClose(e);
	CLV_VideoDecoderClose(d);
	CLV_AudioEncoderClose(ae);
	CLV_AudioDecoderClose(ad);
}

TEST(ParamStructLayout, DeclaredSizeEqualsSizeofIsAcceptedOnAllFaces)
{
	CLV_VideoError verr = CLV_VIDEO_OK;
	CLV_VideoEncParamsC ep = VideoEnc(true);
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &verr);
	ASSERT_EQ(verr, CLV_VIDEO_OK);
	ASSERT_NE(e, nullptr);
	CLV_VideoStreamParamsC sp = VideoStream(true);
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&sp, &verr);
	ASSERT_EQ(verr, CLV_VIDEO_OK);
	ASSERT_NE(d, nullptr);

	CLV_AudioError aerr = CLV_AUDIO_OK;
	CLV_AudioEncParamsC aep = AudioEnc(true);
	CLV_AudioEncoder* ae = CLV_AudioEncoderOpen(&aep, &aerr);
	ASSERT_EQ(aerr, CLV_AUDIO_OK);
	ASSERT_NE(ae, nullptr);
	CLV_AudioStreamParamsC asp = AudioStream(true);
	CLV_AudioDecoder* ad = CLV_AudioDecoderOpen(&asp, nullptr, 0u, &aerr);
	ASSERT_NE(ad, nullptr);
	EXPECT_EQ(aerr, CLV_AUDIO_STREAMINFO_ABSENT);

	std::string path = TempPath("layout_declared");
	ScopedFile guard(path);
	CLV_WriteParams wp;
	std::memset(&wp, 0, sizeof(wp));
	wp.struct_size = static_cast<uint32_t>(sizeof(wp));
	wp.index_present = 1;
	CLV_ContainerError cerr = CLV_CONTAINER_OK;
	CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(guard.c_str(), &wp, &cerr);
	ASSERT_EQ(cerr, CLV_CONTAINER_OK);
	ASSERT_NE(w, nullptr);
	CLV_ContainerWriterClose(w);

	std::cout << "[自报尺寸] 显式 sizeof 五处全过，尺寸 = enc " << sizeof(CLV_VideoEncParamsC) << " / stream "
			  << sizeof(CLV_VideoStreamParamsC) << " / audio_enc " << sizeof(CLV_AudioEncParamsC) << " / audio_stream "
			  << sizeof(CLV_AudioStreamParamsC) << " / write " << sizeof(CLV_WriteParams) << "\n";

	CLV_VideoEncoderClose(e);
	CLV_VideoDecoderClose(d);
	CLV_AudioEncoderClose(ae);
	CLV_AudioDecoderClose(ad);
}

TEST(ParamStructLayout, ForeignSizeIsRejectedEverywhereAndContainerWritesNoFile)
{
	// 形状一：比本版长度少一个 u32 的自报值 —— 按更窄旧布局填的调用方落进的正就是这个值，必须显式拒。
	uint32_t foreign[2];
	foreign[0] = static_cast<uint32_t>(sizeof(CLV_VideoEncParamsC)) - 4u;
	foreign[1] = 176u;	  // 形状二：旧首字段（display_width）的值落进本布局首位

	for (int k = 0; k < 2; ++k)
	{
		const uint32_t s = foreign[k];

		CLV_VideoError verr = CLV_VIDEO_OK;
		CLV_VideoEncParamsC ep = VideoEnc(true);
		ep.struct_size = s;
		CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &verr);
		EXPECT_EQ(e, nullptr) << "外来尺寸 " << s;
		EXPECT_EQ(verr, CLV_VIDEO_VALUE_RANGE) << "外来尺寸 " << s;

		CLV_VideoStreamParamsC sp = VideoStream(true);
		sp.struct_size = s;
		CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&sp, &verr);
		EXPECT_EQ(d, nullptr) << "外来尺寸 " << s;
		EXPECT_EQ(verr, CLV_VIDEO_VALUE_RANGE) << "外来尺寸 " << s;

		CLV_AudioError aerr = CLV_AUDIO_OK;
		CLV_AudioEncParamsC aep = AudioEnc(true);
		aep.struct_size = s;
		CLV_AudioEncoder* ae = CLV_AudioEncoderOpen(&aep, &aerr);
		EXPECT_EQ(ae, nullptr) << "外来尺寸 " << s;
		EXPECT_EQ(aerr, CLV_AUDIO_VALUE_RANGE) << "外来尺寸 " << s;

		CLV_AudioStreamParamsC asp = AudioStream(true);
		asp.struct_size = s;
		CLV_AudioDecoder* ad = CLV_AudioDecoderOpen(&asp, nullptr, 0u, &aerr);
		EXPECT_EQ(ad, nullptr) << "外来尺寸 " << s;
		EXPECT_EQ(aerr, CLV_AUDIO_VALUE_RANGE) << "外来尺寸 " << s;

		std::string path = TempPath("layout_foreign");
		Discard(path);
		CLV_WriteParams wp;
		std::memset(&wp, 0, sizeof(wp));
		wp.struct_size = s;
		wp.index_present = 1;
		CLV_ContainerError cerr = CLV_CONTAINER_OK;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), &wp, &cerr);
		EXPECT_EQ(w, nullptr) << "外来尺寸 " << s;
		EXPECT_EQ(cerr, CLV_CONTAINER_INVALID_ARGUMENT) << "外来尺寸 " << s;
		EXPECT_FALSE(FileExists(path)) << "外来尺寸 " << s << " 应当判在落盘之前";
		Discard(path);

		std::cout << "[自报尺寸] 外来形状 " << s << "：五处全拒，容器面磁盘无残件\n";
	}
}
