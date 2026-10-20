#include "CLV_Container.h"
#include "CLV_File.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
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

	class ScopedFile
	{
		public:

		explicit ScopedFile(std::string path): path_(std::move(path)) {}

		~ScopedFile() { Discard(); }

		const char* c_str() const { return path_.c_str(); }

		void Discard()
		{
			std::error_code ec;
			std::filesystem::remove(std::filesystem::u8path(path_), ec);
		}

		private:

		std::string path_;
	};

	std::vector<uint8_t> Payload(size_t n, uint8_t seed)
	{
		std::vector<uint8_t> v(n);
		for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 37u + seed) & 0xFFu);
		return v;
	}

	CLV_StreamDescC MakeDesc(uint8_t type, uint8_t id, uint32_t max_packet)
	{
		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		d.stream_id = id;
		d.stream_type = type;
		d.codec_id = type == 1 ? 1 : 0;
		d.codec_version = 1;
		d.timebase_num = 1;
		d.timebase_den = type == 1 ? 48000 : 1081080000;
		d.max_packet_size = max_packet;
		d.first_dts = 0;
		d.sample_rate = type == 1 ? 48000 : 0;
		d.channels = type == 1 ? 2 : 0;
		d.bit_depth = type == 1 ? 16 : (type == 2 ? 0 : 8);
		d.layer_id = 0;
		return d;
	}

	/* 每条视频流必须挂 ext_type = 3 参数块（`docs/容器规范.md` 的扩展块一节，v2 起对视频流必选）：缺块的流被读方记
	 * video_stream_no_param_block 并判该流不可用。载荷 8 字节，字段布局照 src/video/headers.cpp 的
	 * ParseEncMeta：
	 *   [0] max_b_frames  [1] quant_precision  [2] ext_tools  [3] reserved  [4..5] width LE16  [6..7] height LE16
	 * 本件只验容器面的公开 ABI，画面尺寸取一个合法非零值（kDisplayW x kDisplayH）。 */
	constexpr uint16_t kDisplayW = 352u;
	constexpr uint16_t kDisplayH = 288u;

	CLV_ContainerError AddVideoParams(CLV_ContainerWriter* w, uint8_t stream_id, uint16_t width, uint16_t height)
	{
		const uint8_t meta[8] = { 0u,
								  7u,	 // quant_precision
								  0u,	 // ext_tools
								  0u,	 // reserved
								  static_cast<uint8_t>(width & 0xFFu),
								  static_cast<uint8_t>((width >> 8) & 0xFFu),
								  static_cast<uint8_t>(height & 0xFFu),
								  static_cast<uint8_t>((height >> 8) & 0xFFu) };
		return CLV_ContainerWriterAddExtBlock(w, stream_id, 3, 1, meta, sizeof(meta));
	}

}	 // namespace

TEST(ApiContainer, RoundTripsFragmentedFramesThroughFile)
{
	const std::string path = TempPath("clv_test_container_rt_");
	ScopedFile guard(path);

	const std::vector<uint8_t> big = Payload(1000, 3);
	const std::vector<uint8_t> small = Payload(40, 4);

	uint64_t packets = 0;
	{
		CLV_ContainerError err = CLV_CONTAINER_OK;
		CLV_WriteParams params;
		std::memset(&params, 0, sizeof(params));
		params.version_minor = 0;
		params.index_present = 1;
		params.globally_sorted = 0;
		params.streaming = 0;
		params.fragment_chunk_size = 300;

		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), &params, &err);
		ASSERT_NE(w, nullptr) << CLV_ContainerStrError(err);
		const CLV_StreamDescC video = MakeDesc(0, 0, 4096);
		const CLV_StreamDescC sub = MakeDesc(2, 1, 1024);
		ASSERT_EQ(CLV_ContainerWriterAddStream(w, &video), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterAddStream(w, &sub), CLV_CONTAINER_OK);
		ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), CLV_CONTAINER_OK);

		const uint8_t styles[] = { 'a', 'b', 'c' };
		ASSERT_EQ(CLV_ContainerWriterAddExtBlock(w, 1, 4, 1, styles, sizeof(styles)), CLV_CONTAINER_OK);

		ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 0, 1, -40, 80, big.data(), big.size()), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 1, 0, 0, 0, small.data(), small.size()), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 0, 0, 43243200, 43243200, small.data(), small.size()),
				  CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}
	EXPECT_EQ(packets, 6u);	   // 首帧四片 + 两帧各一片

	CLV_ContainerError err = CLV_CONTAINER_OK;
	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), &err);
	ASSERT_NE(r, nullptr) << CLV_ContainerStrError(err);

	uint32_t streams = 0;
	ASSERT_EQ(CLV_ContainerReaderStreamCount(r, &streams), CLV_CONTAINER_OK);
	EXPECT_EQ(streams, 2u);

	CLV_StreamDescC d0;
	uint8_t usable = 0;
	ASSERT_EQ(CLV_ContainerReaderGetStream(r, 0, &d0, &usable), CLV_CONTAINER_OK);
	EXPECT_EQ(usable, 1);
	EXPECT_EQ(d0.stream_type, 0);
	EXPECT_EQ(d0.timebase_den, 1081080000u);

	uint8_t has_index = 0;
	ASSERT_EQ(CLV_ContainerReaderHasIndex(r, &has_index), CLV_CONTAINER_OK);
	EXPECT_EQ(has_index, 1);
	uint32_t entries = 0;
	ASSERT_EQ(CLV_ContainerReaderIndexCount(r, &entries), CLV_CONTAINER_OK);
	EXPECT_EQ(entries, 3u);

	std::vector<CLV_FrameC> frames;
	std::vector<std::vector<uint8_t>> kept;
	for (;;)
	{
		CLV_FrameC f;
		std::memset(&f, 0, sizeof(f));
		uint8_t got = 0;
		ASSERT_EQ(CLV_ContainerReaderNextFrame(r, &f, &got), CLV_CONTAINER_OK);
		if (! got) break;
		frames.push_back(f);
		kept.emplace_back(f.payload, f.payload + f.payload_size);
		ASSERT_LE(frames.size(), 16u);
	}
	ASSERT_EQ(frames.size(), 3u);

	EXPECT_EQ(kept[0], big);
	EXPECT_EQ(frames[0].is_keyframe, 1);
	EXPECT_EQ(frames[0].complete, 1);
	EXPECT_EQ(frames[0].fragments_received, 4u);
	EXPECT_EQ(frames[0].fragments_expected, 4u);
	EXPECT_EQ(frames[0].pts_delta, 120);	// pts - dts = 80 - (-40)
	EXPECT_EQ(kept[1], small);
	EXPECT_EQ(frames[1].stream_id, 1);
	EXPECT_EQ(kept[2], small);
	EXPECT_EQ(frames[2].dts_delta, 43243240u);	  // 已平移：43243200 - (-40)

	CLV_ReaderStatsC st;
	std::memset(&st, 0, sizeof(st));
	ASSERT_EQ(CLV_ContainerReaderStats(r, &st), CLV_CONTAINER_OK);
	EXPECT_EQ(st.dropped_crc, 0u);
	EXPECT_EQ(st.dropped_field, 0u);
	EXPECT_EQ(st.dropped_unknown_stream, 0u);
	EXPECT_EQ(st.resync_count, 0u);
	EXPECT_EQ(st.resync_failed, 0u);
	EXPECT_EQ(st.fragment_gaps, 0u);
	EXPECT_EQ(st.abandoned, 0);
	EXPECT_EQ(st.truncated, 0);

	CLV_ContainerReaderClose(r);
}

TEST(ApiContainer, NullHandleIsRejectedEverywhere)
{
	CLV_FrameC f;
	CLV_StreamDescC d;
	CLV_ReaderStatsC st;
	uint32_t u32 = 0;
	uint8_t u8 = 0;
	uint64_t u64 = 0;
	std::memset(&f, 0, sizeof(f));
	std::memset(&d, 0, sizeof(d));
	std::memset(&st, 0, sizeof(st));

	EXPECT_EQ(CLV_ContainerWriterAddStream(nullptr, nullptr), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerWriterAddExtBlock(nullptr, 0, 4, 1, nullptr, 0), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerWriterWriteFrame(nullptr, 0, 0, 0, 0, nullptr, 0), CLV_CONTAINER_INVALID_HANDLE);
	// 分片写入的两条 NULL 路径：句柄为空判 INVALID_HANDLE，与整帧写入口同口径
	const CLV_Chunk one_chunk[1] = {
		{ nullptr, 0u }
	};
	EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(nullptr, 0, one_chunk, 1u, 0, 0, 0), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerWriterFinish(nullptr, &u64), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerReaderStreamCount(nullptr, &u32), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerReaderGetStream(nullptr, 0, &d, &u8), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerReaderNextFrame(nullptr, &f, &u8), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerReaderStats(nullptr, &st), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerReaderHasIndex(nullptr, &u8), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerReaderIndexCount(nullptr, &u32), CLV_CONTAINER_INVALID_HANDLE);
	CLV_ContainerWriterClose(nullptr);
	CLV_ContainerReaderClose(nullptr);
}

TEST(ApiContainer, NullOutParamsAreRejected)
{
	const std::string path = TempPath("clv_test_container_arg_");
	ScopedFile guard(path);

	CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), nullptr, nullptr);
	ASSERT_NE(w, nullptr);
	CLV_StreamDescC d = MakeDesc(0, 0, 4096);
	ASSERT_EQ(CLV_ContainerWriterAddStream(w, &d), CLV_CONTAINER_OK);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), CLV_CONTAINER_OK);
	// 无流可写时先验参数：payload 非零但指针为空
	EXPECT_EQ(CLV_ContainerWriterWriteFrame(w, 0, 0, 0, 0, nullptr, 10), CLV_CONTAINER_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_ContainerWriterAddStream(w, nullptr), CLV_CONTAINER_INVALID_ARGUMENT);
	// 分片写入面：句柄有效但 chunks 为空 / chunk_count 为 0 都判入参不成立（判在落盘前，一片不写）
	const uint8_t payload[4] = { 1u, 2u, 3u, 4u };
	const CLV_Chunk chunk[1] = {
		{ payload, sizeof(payload) }
	};
	EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, nullptr, 1u, 0, 0, 0), CLV_CONTAINER_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, chunk, 0u, 0, 0, 0), CLV_CONTAINER_INVALID_ARGUMENT);
	CLV_ContainerWriterClose(w);

	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), nullptr);
	if (r != nullptr)
	{
		// 上面那个文件没写完（没有 Finish），打开应失败但不崩；成功时再验出参
		EXPECT_EQ(CLV_ContainerReaderStreamCount(r, nullptr), CLV_CONTAINER_INVALID_ARGUMENT);
		CLV_ContainerReaderClose(r);
	}
}

TEST(ApiContainer, BadStructureFileIsReportedNotCrashed)
{
	const std::string path = TempPath("clv_test_container_bad_");
	ScopedFile guard(path);
	{
		// 对抗性输入也走被测面写：测试不引平台或第三方文件流
		const char junk[] = "not a clv file at all, 64 bytes of noise to be sure!!!";
		CLV_File* f = CLV_OpenFile(path.c_str(), CLV_FILE_MODE_WRITE);
		ASSERT_NE(f, nullptr);
		size_t written = 0;
		ASSERT_EQ(CLV_WriteFile(f, junk, sizeof(junk), &written), CLV_FILE_OK);
		ASSERT_EQ(written, sizeof(junk));
		CLV_CloseFile(f);
	}

	CLV_ContainerError err = CLV_CONTAINER_OK;
	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), &err);
	EXPECT_EQ(r, nullptr);
	/* 依据注：这一档不许写成「BAD_STRUCTURE 或 VALUE_RANGE 都行」的二选一——那等于垃圾输入落哪档都没人在场。
	 * 本输入够不着值域档：定长文件头 64 + 定长文件尾 16 = 80 字节，而这块料连头带尾只有 65 字节
	 * （64 个可见字符 + 字面量的收尾 NUL，写入量上面已断言过），连凑齐一对定长件都不够 ⇒ 唯一落点就是结构档。
	 * 值域档要读满 64 字节的头、字段都取到了才可能判，本例到不了那一步。 */
	EXPECT_EQ(err, CLV_CONTAINER_BAD_STRUCTURE) << "不足定长头尾之和的输入不该落到别的档";
}

TEST(ApiContainer, BackslashPathIsRejectedByFileContract)
{
	CLV_ContainerError err = CLV_CONTAINER_OK;
	CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile("not\\a\\valid\\path.clv", nullptr, &err);
	EXPECT_EQ(w, nullptr);
	EXPECT_EQ(err, CLV_CONTAINER_IO_FAILED);
}

TEST(ApiContainer, StrErrorCoversEveryCode)
{
	const int known[] = { CLV_CONTAINER_OK,
						  CLV_CONTAINER_INVALID_HANDLE,
						  CLV_CONTAINER_INVALID_ARGUMENT,
						  CLV_CONTAINER_IO_FAILED,
						  CLV_CONTAINER_BAD_STRUCTURE,
						  CLV_CONTAINER_VALUE_RANGE,
						  CLV_CONTAINER_TOO_LARGE,
						  CLV_CONTAINER_STREAM_NOT_FOUND,
						  CLV_CONTAINER_STATE_ERROR,
						  CLV_CONTAINER_OUT_OF_MEMORY,
						  CLV_CONTAINER_UNSUPPORTED,
						  CLV_CONTAINER_EXT_CHAIN_TOO_LONG };
	for (const int code: known)
	{
		const char* s = CLV_ContainerStrError(static_cast<CLV_ContainerError>(code));
		ASSERT_NE(s, nullptr);
		EXPECT_GT(std::strlen(s), 0u);
		for (const char* p = s; *p != '\0'; ++p) EXPECT_EQ(*p & 0x80, 0) << "错误串应为 ASCII";
	}
	EXPECT_STREQ(CLV_ContainerStrError(static_cast<CLV_ContainerError>(999)), "unknown");
}
