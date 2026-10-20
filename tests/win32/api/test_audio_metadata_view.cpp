/* 承载块内容的解读面（⑤ B 路）：作者等注释键值 + 封面图像字段。
 *
 * 输入字节一律在本文件里按 FLAC 原块形态手拼，**期望值取自手算的偏移与算式**，不取自被测实现。
 * 注释块（原块类型码 4）：u32 小端 vendor 长度 + vendor + u32 小端 条目数 + 每条 {u32 小端长度, "KEY=VALUE"}。
 * 图标块（原块类型码 6）：type / mime 长度 / mime / 描述长度 / 描述 / 宽 / 高 / 位深 / 调色板色数 /
 * 图像数据长度 / 图像数据 —— 这一族长度前缀是大端 u32，与注释块的小端正反一趟。
 *
 * 注释块视图必须由本头的解析函数产出，不得由调用方自拼：本件两路查询口（取第几条 / 按键名查）用的视图
 * 一律出自 `CLV_AudioParseVorbisComment`，因此这里量到的都是「块本身坏」那一族判点；视图与字节不同档
 * 那一族的入口自洽判点（起点不大于声明块长、声明块长不小于定长段、长度加减在 64 位域里做）见
 * `tests/win32/audio/test_audio_framing.cpp` 里名字以 `AudioFraming.CommentView` 起头的那几例。
 */

#include "CLV_Audio.h"
#include "CLV_Container.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace
{

	void put_le32(std::vector<uint8_t>& v, uint32_t x)
	{
		v.push_back(static_cast<uint8_t>(x & 0xFFu));
		v.push_back(static_cast<uint8_t>((x >> 8u) & 0xFFu));
		v.push_back(static_cast<uint8_t>((x >> 16u) & 0xFFu));
		v.push_back(static_cast<uint8_t>((x >> 24u) & 0xFFu));
	}

	void put_be32(std::vector<uint8_t>& v, uint32_t x)
	{
		v.push_back(static_cast<uint8_t>((x >> 24u) & 0xFFu));
		v.push_back(static_cast<uint8_t>((x >> 16u) & 0xFFu));
		v.push_back(static_cast<uint8_t>((x >> 8u) & 0xFFu));
		v.push_back(static_cast<uint8_t>(x & 0xFFu));
	}

	void put_text(std::vector<uint8_t>& v, const char* s)
	{
		for (const char* p = s; *p != '\0'; ++p) v.push_back(static_cast<uint8_t>(*p));
	}

	/* 一条注释 = u32 小端 (键长 + 1 + 值长) + "KEY=VALUE"。 */
	void put_comment(std::vector<uint8_t>& v, const char* key, const char* value)
	{
		const size_t n = std::strlen(key) + 1u + std::strlen(value);
		put_le32(v, static_cast<uint32_t>(n));
		put_text(v, key);
		v.push_back(static_cast<uint8_t>('='));
		put_text(v, value);
	}

	std::string as_string(const uint8_t* p, uint32_t n) { return std::string(reinterpret_cast<const char*>(p), n); }

	/* 本文件唯一的一份注释块样例，偏移逐处算过：
	 * [0,4) vendor 长度 = 8 → [4,12) "CLV test" → [12,16) 条目数 = 3
	 * → 第 0 条 [16,20) 长度 11 + [20,31) "TITLE=First"
	 * → 第 1 条 [31,35) 长度 10 + [35,45) "ARTIST=Ada"
	 * → 第 2 条 [45,49) 长度 12 + [49,61) "artist=Grace" ⇒ 整块 61 字节。 */
	std::vector<uint8_t> CommentBlock()
	{
		std::vector<uint8_t> v;
		put_le32(v, 8u);
		put_text(v, "CLV test");
		put_le32(v, 3u);
		put_comment(v, "TITLE", "First");
		put_comment(v, "ARTIST", "Ada");
		put_comment(v, "artist", "Grace");
		return v;
	}

	/* 图标块样例：type 3（前面封面）、MIME "image/png"、描述是三个汉字的 UTF-8（6 字节）、
	 * 320 × 200 × 24 位、无调色板、图像数据 12 字节。 */
	std::vector<uint8_t> PictureBlock()
	{
		std::vector<uint8_t> v;
		put_be32(v, 3u);
		put_be32(v, 9u);
		put_text(v, "image/png");
		const std::string desc = "\xE5\xB0\x81\xE9\x9D\xA2";	// 「封面」两字的 UTF-8
		put_be32(v, static_cast<uint32_t>(desc.size()));
		for (const unsigned char c: desc) v.push_back(static_cast<uint8_t>(c));
		put_be32(v, 320u);
		put_be32(v, 200u);
		put_be32(v, 24u);
		put_be32(v, 0u);
		put_be32(v, 12u);
		for (uint32_t i = 0u; i < 12u; ++i) v.push_back(static_cast<uint8_t>(0x10u + i));
		return v;
	}

	std::string TempPath(const std::string& stem)
	{
		std::error_code ec;
		const auto dir = std::filesystem::temp_directory_path(ec);
		std::string name = dir.generic_string();
		if (! name.empty() && name.back() != '/') name.push_back('/');
		name += stem + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".clv";
		return name;
	}

	struct ScopedFile
	{
		explicit ScopedFile(std::string path): path_(std::move(path)) {}

		~ScopedFile() { Discard(); }

		const char* c_str() const { return path_.c_str(); }

		private:

		void Discard()
		{
			std::error_code ec;
			std::filesystem::remove(std::filesystem::u8path(path_), ec);
		}

		std::string path_;
	};

	CLV_StreamDescC AudioDesc(uint8_t id)
	{
		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		d.stream_id = id;
		d.stream_type = 1;	  // 音频
		d.codec_id = 1;		  // FLAC 帧本体
		d.codec_version = 1;
		d.timebase_num = 1;
		d.timebase_den = 48000;
		d.max_packet_size = 1u << 20;
		d.first_dts = 0;
		d.sample_rate = 48000;
		d.channels = 2;
		d.bit_depth = 16;
		return d;
	}

}	 // namespace

TEST(AudioMetadataView, VorbisCommentReadsBackVendorAndEveryEntry)
{
	const std::vector<uint8_t> block = CommentBlock();
	/* 上面那份拼法的总长：4 + 8 + 4 + (4+11) + (4+10) + (4+12) = 61。 */
	ASSERT_EQ(block.size(), 61u);

	CLV_AudioVorbisCommentC view {};
	ASSERT_EQ(CLV_AudioParseVorbisComment(block.data(), block.size(), &view), CLV_AUDIO_OK);
	EXPECT_EQ(view.vendor_length, 8u);
	EXPECT_EQ(as_string(view.vendor, view.vendor_length), "CLV test");
	EXPECT_EQ(view.comment_count, 3u);

	struct Expect
	{
		const char* key;
		const char* value;
	};

	const Expect want[3] = {
		{ "TITLE",	"First" },
		 { "ARTIST", "Ada"	 },
		 { "artist", "Grace" }
	};
	for (uint32_t i = 0u; i < 3u; ++i)
	{
		CLV_AudioCommentEntryC e {};
		ASSERT_EQ(CLV_AudioVorbisCommentEntryAt(&view, i, &e), CLV_AUDIO_OK) << "第 " << i << " 条";
		EXPECT_EQ(e.key_length, std::strlen(want[i].key)) << "第 " << i << " 条键长";
		EXPECT_EQ(as_string(e.key, e.key_length), want[i].key);
		EXPECT_EQ(as_string(e.value, e.value_length), want[i].value);
		/* 键与值之间确实只隔着一个 `=`：值指针 = 键指针 + 键长 + 1。 */
		EXPECT_EQ(e.value, e.key + e.key_length + 1u);
	}

	CLV_AudioCommentEntryC over {};
	EXPECT_EQ(CLV_AudioVorbisCommentEntryAt(&view, 3u, &over), CLV_AUDIO_VALUE_RANGE);

	std::cout << "[注释块读回] vendor 8 B、条目 3 条，逐条键值与手算偏移一致；块长 " << block.size() << " B\n";
}

TEST(AudioMetadataView, KeyLookupIsCaseInsensitiveAndWalksRepeatedKeys)
{
	const std::vector<uint8_t> block = CommentBlock();
	CLV_AudioVorbisCommentC view {};
	ASSERT_EQ(CLV_AudioParseVorbisComment(block.data(), block.size(), &view), CLV_AUDIO_OK);

	/* 同一份块里 ARTIST 出现两次（大小写不同），比对的是 `=` 左侧且不区分大小写。 */
	CLV_AudioCommentEntryC a {};
	ASSERT_EQ(CLV_AudioVorbisCommentFindKey(&view, "artist", 0u, &a), CLV_AUDIO_OK);
	EXPECT_EQ(as_string(a.value, a.value_length), "Ada");	 // 第 1 条命中 = 先出现的那条
	CLV_AudioCommentEntryC b {};
	ASSERT_EQ(CLV_AudioVorbisCommentFindKey(&view, "ARTIST", 1u, &b), CLV_AUDIO_OK);
	EXPECT_EQ(as_string(b.value, b.value_length), "Grace");
	EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, "Artist", 2u, &b), CLV_AUDIO_COMMENT_KEY_NOT_FOUND);
	EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, "ALBUM", 0u, &b), CLV_AUDIO_COMMENT_KEY_NOT_FOUND);
	/* 键名比到 `=` 为止：拿 "ARTIST=Ada" 整串去查不该命中。 */
	EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, "ARTIST=Ada", 0u, &b), CLV_AUDIO_COMMENT_KEY_NOT_FOUND);

	std::cout << "[注释块查键] 大小写不敏感命中两条同名键；越界 occurrence 与不存在的键都报未找到\n";
}

TEST(AudioMetadataView, CommentStructureFailuresAreRejectedWithoutPartialRead)
{
	/* 四种坏形状：vendor 长度越界 / 条目数多于实有 / 某条长度越界 / 条目里没有 `=`。 */
	const std::vector<uint8_t> good = CommentBlock();

	std::vector<uint8_t> bad = good;
	put_le32(bad, 0u);	  // 占位用不着，这里改第一处长度：直接写一个超块长的 vendor 长度
	bad[0] = 200u;
	bad[1] = 0u;
	bad[2] = 0u;
	bad[3] = 0u;
	/* 依据注：每一路拒判都走同一套三步——先把出参铺满哨兵 0xA5 → 调用 → 回查五个字段是不是全被清成零值。
	 * 铺哨兵是必要的：出参本就是零值时，「清零了」与「根本没动过」分不出来。
	 * 判据形态取自实现体在入口 memset 这一条契约（CLV_Audio.h 的出参口径），不是抄实现的返回值。 */
	int rejects = 0;
	auto reject_leaves_no_partial = [&rejects](const uint8_t* d, size_t n, CLV_AudioError want, const char* what)
	{
		CLV_AudioVorbisCommentC v;
		std::memset(&v, 0xA5, sizeof(v));
		EXPECT_EQ(CLV_AudioParseVorbisComment(d, n, &v), want) << what;
		++rejects;
		EXPECT_EQ(v.block, nullptr) << what << "：block 留了半份";
		EXPECT_EQ(v.block_size, 0u) << what << "：block_size 留了半份";
		EXPECT_EQ(v.vendor, nullptr) << what << "：vendor 留了半份";
		EXPECT_EQ(v.vendor_length, 0u) << what << "：vendor_length 留了半份";
		EXPECT_EQ(v.comment_count, 0u) << what << "：comment_count 留了半份";
	};

	reject_leaves_no_partial(bad.data(), bad.size(), CLV_AUDIO_METADATA_MALFORMED, "vendor 长度越界");

	bad = good;
	bad[12] = 4u;	 // 条目数声明成 4，实际只有 3 条
	reject_leaves_no_partial(bad.data(), bad.size(), CLV_AUDIO_METADATA_MALFORMED, "条目数多于实有");

	bad = good;
	bad[16] = 200u;	   // 第 0 条的长度越出块界
	bad[17] = 0u;
	bad[18] = 0u;
	bad[19] = 0u;
	reject_leaves_no_partial(bad.data(), bad.size(), CLV_AUDIO_METADATA_MALFORMED, "单条长度越界");

	bad = good;
	bad[20] = static_cast<uint8_t>('X');	// "TITLE" 换成 "XITLE"…
	bad[25] = static_cast<uint8_t>(' ');	// 再把 `=` 抹掉 ⇒ 不成其为键值对
	bad[21] = static_cast<uint8_t>('I');
	bad[22] = static_cast<uint8_t>('T');
	bad[23] = static_cast<uint8_t>('L');
	bad[24] = static_cast<uint8_t>('E');
	reject_leaves_no_partial(bad.data(), bad.size(), CLV_AUDIO_METADATA_MALFORMED, "条目里没有 `=`");

	/* 短于最小结构（两个长度前缀 = 8 字节）归结构不成立，空指针归参数不成立；两路都带着出参。 */
	reject_leaves_no_partial(good.data(), 7u, CLV_AUDIO_METADATA_MALFORMED, "短于最小结构");
	reject_leaves_no_partial(nullptr, 61u, CLV_AUDIO_INVALID_ARGUMENT, "载荷空指针");
	// 出参本身为空的那一路没有可回查的对象，只判返回码
	EXPECT_EQ(CLV_AudioParseVorbisComment(good.data(), good.size(), nullptr), CLV_AUDIO_INVALID_ARGUMENT);
	const std::vector<uint8_t> broken = []()
	{
		std::vector<uint8_t> v = CommentBlock();
		v[12] = 9u;
		return v;
	}();
	reject_leaves_no_partial(broken.data(), broken.size(), CLV_AUDIO_METADATA_MALFORMED, "条目数声明成 9");
	EXPECT_EQ(rejects, 7) << "坏形通道少走了几次，上面的回查就少钉几次";

	std::cout << "[注释块坏形] 四类越界与缺 `=` 全判结构不成立，出参不留半份\n";
}

TEST(AudioMetadataView, PictureFieldsReadBackEveryField)
{
	const std::vector<uint8_t> block = PictureBlock();
	/* 块长算式：4 + 4 + 9 + 4 + 6 + 4×4 + 4 + 12 = 59。 */
	ASSERT_EQ(block.size(), 59u);

	CLV_AudioPictureC pic {};
	ASSERT_EQ(CLV_AudioParsePicture(block.data(), block.size(), &pic), CLV_AUDIO_OK);
	EXPECT_EQ(pic.picture_type, 3u);
	EXPECT_EQ(pic.mime_length, 9u);
	EXPECT_EQ(as_string(pic.mime, pic.mime_length), "image/png");
	EXPECT_EQ(pic.description_length, 6u);
	/* 描述串按 UTF-8 交出，库不转码：两字汉字 = 6 字节。 */
	const std::string want_desc = "\xE5\xB0\x81\xE9\x9D\xA2";
	EXPECT_EQ(as_string(pic.description, pic.description_length), want_desc);
	EXPECT_EQ(pic.width, 320u);
	EXPECT_EQ(pic.height, 200u);
	EXPECT_EQ(pic.depth, 24u);
	EXPECT_EQ(pic.colors, 0u);
	EXPECT_EQ(pic.data_length, 12u);
	ASSERT_NE(pic.data, nullptr);
	for (uint32_t i = 0u; i < 12u; ++i) EXPECT_EQ(pic.data[i], static_cast<uint8_t>(0x10u + i));
	/* 图像数据末尾正好顶到块尾：47 + 12 = 59。 */
	EXPECT_EQ(pic.data - block.data() + pic.data_length, block.size());

	std::cout << "[图标块读回] type 3 / MIME image/png / 描述 6 B / 320x200x24 / 图像 12 B，字段逐项对上\n";
}

TEST(AudioMetadataView, PictureLengthOverrunsAreRejected)
{
	CLV_AudioPictureC pic {};

	std::vector<uint8_t> bad = PictureBlock();
	/* 图像数据长度字段在 [43,47)（数据体从 47 起）：写成 60，块尾只剩 12 字节。 */
	bad[43] = 0u;
	bad[44] = 0u;
	bad[45] = 0u;
	bad[46] = 60u;
	EXPECT_EQ(CLV_AudioParsePicture(bad.data(), bad.size(), &pic), CLV_AUDIO_METADATA_MALFORMED);

	bad = PictureBlock();
	bad[4] = 0u;
	bad[5] = 0u;
	bad[6] = 0u;
	bad[7] = 200u;	  // MIME 长度越界
	EXPECT_EQ(CLV_AudioParsePicture(bad.data(), bad.size(), &pic), CLV_AUDIO_METADATA_MALFORMED);

	/* 截断两档，各落在一条不同的判点上：
	 *   31 B 不足定长段。图标块的定长部分是 8 个 4 字节字段 = 32 B（类型、MIME 长、描述长、宽、高、
	 *     位深、色数、图像数据长），两个变长段都允许零长 ⇒ 32 B 是合法下界，入口判式就是 `size < 4 × 8`
	 *     （src/audio/metadata.cpp 的 ParsePictureBlock）。
	 *   35 B 的定长段放得下，但块被切在变长段中间 ⇒ 某条长度前缀声明的段长越出剩余字节，同一枚码。 */
	EXPECT_EQ(CLV_AudioParsePicture(bad.data(), 31u, &pic), CLV_AUDIO_METADATA_MALFORMED);
	EXPECT_EQ(CLV_AudioParsePicture(bad.data(), 35u, &pic), CLV_AUDIO_METADATA_MALFORMED);
	EXPECT_EQ(CLV_AudioParsePicture(nullptr, 59u, &pic), CLV_AUDIO_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_AudioParsePicture(bad.data(), bad.size(), nullptr), CLV_AUDIO_INVALID_ARGUMENT);

	std::cout << "[图标块坏形] 两处长度前缀越界与两档截断全判结构不成立\n";
}

TEST(AudioMetadataView, AuthorAndCoverSurviveContainerRoundTrip)
{
	/* 端到端：两块元数据按容器层的承载形态（首字节 = 原块类型码）写进 ext_type = 6，
	 * 再从容器读回、拆首字节、解析 —— 作者与封面字节与写进去的逐字节相同。 */
	const std::vector<uint8_t> comment = CommentBlock();
	const std::vector<uint8_t> picture = PictureBlock();

	std::vector<uint8_t> carrier_comment;
	carrier_comment.push_back(4u);
	carrier_comment.insert(carrier_comment.end(), comment.begin(), comment.end());
	std::vector<uint8_t> carrier_picture;
	carrier_picture.push_back(6u);
	carrier_picture.insert(carrier_picture.end(), picture.begin(), picture.end());

	const std::string path = TempPath("clv_meta_roundtrip");
	ScopedFile guard(path);
	{
		CLV_ContainerError err = CLV_CONTAINER_OK;
		CLV_WriteParams wp;
		std::memset(&wp, 0, sizeof(wp));
		wp.index_present = 1;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(guard.c_str(), &wp, &err);
		ASSERT_NE(w, nullptr) << CLV_ContainerStrError(err);
		const CLV_StreamDescC au = AudioDesc(0);
		ASSERT_EQ(CLV_ContainerWriterAddStream(w, &au), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterAddExtBlock(w, 0, 6, 1, carrier_comment.data(), carrier_comment.size()),
				  CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterAddExtBlock(w, 0, 6, 1, carrier_picture.data(), carrier_picture.size()),
				  CLV_CONTAINER_OK);
		const std::vector<uint8_t> frame(64u, 0xABu);
		ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 0, 0, 0, 0, frame.data(), frame.size()), CLV_CONTAINER_OK);
		uint64_t packets = 0u;
		ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}

	CLV_ContainerError err = CLV_CONTAINER_OK;
	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(guard.c_str(), &err);
	ASSERT_NE(r, nullptr) << CLV_ContainerStrError(err);
	uint32_t ext_count = 0u;
	ASSERT_EQ(CLV_ContainerReaderExtBlockCount(r, &ext_count), CLV_CONTAINER_OK);
	ASSERT_EQ(ext_count, 2u);

	std::string author;
	std::vector<uint8_t> cover;
	for (uint32_t i = 0u; i < ext_count; ++i)
	{
		CLV_ExtBlockC b {};
		ASSERT_EQ(CLV_ContainerReaderGetExtBlock(r, i, &b), CLV_CONTAINER_OK);
		EXPECT_EQ(b.ext_type, 6u);
		uint8_t kind = 0u;
		const uint8_t* payload = nullptr;
		size_t payload_size = 0u;
		ASSERT_EQ(CLV_AudioSplitMetaBlock(b.data, b.size, &kind, &payload, &payload_size), CLV_AUDIO_OK);
		if (kind == 4u)
		{
			CLV_AudioVorbisCommentC view {};
			ASSERT_EQ(CLV_AudioParseVorbisComment(payload, payload_size, &view), CLV_AUDIO_OK);
			CLV_AudioCommentEntryC e {};
			ASSERT_EQ(CLV_AudioVorbisCommentFindKey(&view, "ARTIST", 0u, &e), CLV_AUDIO_OK);
			author = as_string(e.value, e.value_length);
		}
		else if (kind == 6u)
		{
			CLV_AudioPictureC pic {};
			ASSERT_EQ(CLV_AudioParsePicture(payload, payload_size, &pic), CLV_AUDIO_OK);
			EXPECT_EQ(pic.width, 320u);
			EXPECT_EQ(pic.height, 200u);
			ASSERT_NE(pic.data, nullptr);
			cover.assign(pic.data, pic.data + pic.data_length);
		}
		else
		{
			FAIL() << "承载块类型码不是 4 / 6：" << static_cast<int>(kind);
		}
	}
	CLV_ContainerReaderClose(r);

	EXPECT_EQ(author, "Ada");
	ASSERT_EQ(cover.size(), 12u);
	for (uint32_t i = 0u; i < cover.size(); ++i) EXPECT_EQ(cover[i], static_cast<uint8_t>(0x10u + i));
	/* 封面字节与写进去的那一段逐字节相同（不是「长度相同」）。 */
	const std::vector<uint8_t> want_cover(picture.end() - 12, picture.end());
	EXPECT_EQ(cover, want_cover);

	std::cout << "[端到端] 容器 → 拆承载块 → 解析：作者 = " << author << "，封面 " << cover.size()
			  << " B 与源逐字节相同\n";
}

/* ---- 真件输入：一支带注释块与封面的 FLAC（入库件，由外部示例音频转换而来）--------------------
 *
 * `assets/fixtures/sample-15s.flac`，来源 / 转换命令 / 两条逐字节对平都登记在
 * `assets/fixtures/provenance.txt`。下面三个用例的期望值取自一份**不依赖 CLV** 的独立块表走查，
 * 不取自被测实现。本件注释块有 11 条真键值 ⇒ 「查键」这一形第一次落在真件上；
 * 而它没有「零条目注释块」与「载荷大过两字节域顶的大块」两形，那两形在本组里由真块**派生**出来量：
 * 条目数就地改成 0（其余字节一字不动）、大块用真封面块复制拼接 —— 载体换了，覆面不退。
 */

#ifndef CLV_ASSETS_DIR
	#define CLV_ASSETS_DIR "assets/fixtures"
#endif

namespace
{

	struct FlacBlock
	{
		uint8_t type;
		size_t offset;	  // 载荷起点（块头 4 字节之后）
		size_t size;
	};

	std::vector<uint8_t> ReadWholeFile(const std::string& path)
	{
		std::ifstream f(path, std::ios::binary);
		if (! f) return {};
		return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}

	/* FLAC 的元数据链：`fLaC` 之后逐块 {首字节 = 类型 | last<<7, 3 字节大端长度, 载荷}，最后一块之后是音频帧。 */
	std::vector<FlacBlock> WalkFlacBlocks(const std::vector<uint8_t>& d)
	{
		std::vector<FlacBlock> out;
		if (d.size() < 4u || std::memcmp(d.data(), "fLaC", 4u) != 0) return out;
		size_t i = 4u;
		while (i + 4u <= d.size())
		{
			const uint8_t head = d[i];
			const size_t len = (static_cast<size_t>(d[i + 1u]) << 16u) | (static_cast<size_t>(d[i + 2u]) << 8u) |
							   static_cast<size_t>(d[i + 3u]);
			if (i + 4u + len > d.size()) break;
			out.push_back({ static_cast<uint8_t>(head & 0x7Fu), i + 4u, len });
			i += 4u + len;
			if ((head & 0x80u) != 0u) break;
		}
		return out;
	}

	std::vector<uint8_t> Slice(const std::vector<uint8_t>& d, const FlacBlock& b)
	{
		return std::vector<uint8_t>(d.begin() + static_cast<ptrdiff_t>(b.offset),
									d.begin() + static_cast<ptrdiff_t>(b.offset + b.size));
	}

	const FlacBlock* FindBlock(const std::vector<FlacBlock>& v, uint8_t type)
	{
		for (const FlacBlock& b: v)
			if (b.type == type) return &b;
		return nullptr;
	}

}	 // namespace

TEST(AudioMetadataView, SampleTrackBlockTableAndEntryViews)
{
	const std::vector<uint8_t> file = ReadWholeFile(std::string(CLV_ASSETS_DIR) + "/sample-15s.flac");
	if (file.empty()) GTEST_SKIP() << "真件不在位，本组真件用例跳过：assets/fixtures/sample-15s.flac";
	EXPECT_EQ(file.size(), 2080768u);

	/* 块表 = STREAMINFO(34) → PICTURE(7835) → VORBIS_COMMENT(306) → PADDING(8192, last)，
	 * 之后第一字节起是音频帧（16387）。顺序、条数、PADDING 占 last 位三处都是走查实测。 */
	const std::vector<FlacBlock> blocks = WalkFlacBlocks(file);
	ASSERT_EQ(blocks.size(), 4u);
	EXPECT_EQ(blocks[0].type, 0u);
	EXPECT_EQ(blocks[0].size, 34u);
	EXPECT_EQ(blocks[1].type, 6u);
	EXPECT_EQ(blocks[1].size, 7835u);
	EXPECT_EQ(blocks[2].type, 4u);
	EXPECT_EQ(blocks[2].size, 306u);
	EXPECT_EQ(blocks[3].type, 1u);
	EXPECT_EQ(blocks[3].size, 8192u);
	EXPECT_EQ(blocks[3].offset + blocks[3].size, 16387u);

	/* STREAMINFO 的位域（全大端）：块大小 47 / 帧长 89..164 / 44100 Hz / 2 ch / 24 bit / 661500 样本。
	 * 47 既不是 2 的幂、也低于 24 bit 那一档的子集下限 72 ⇒ 本件是一个非规整块大小的真流。 */
	const std::vector<uint8_t> si = Slice(file, blocks[0]);
	ASSERT_EQ(si.size(), 34u);
	const auto be16 = [&si](size_t o) { return (static_cast<size_t>(si[o]) << 8u) | si[o + 1u]; };
	const auto be24 = [&si](size_t o)
	{ return (static_cast<size_t>(si[o]) << 16u) | (static_cast<size_t>(si[o + 1u]) << 8u) | si[o + 2u]; };
	EXPECT_EQ(be16(0u), 47u);
	EXPECT_EQ(be16(2u), 47u);
	EXPECT_EQ(be24(4u), 89u);
	EXPECT_EQ(be24(7u), 164u);
	uint64_t w = 0u;
	for (size_t i = 10u; i < 18u; ++i) w = (w << 8u) | si[i];
	EXPECT_EQ(w >> 44u, 44100u);
	EXPECT_EQ(((w >> 41u) & 0x7u) + 1u, 2u);
	EXPECT_EQ(((w >> 36u) & 0x1Fu) + 1u, 24u);
	EXPECT_EQ(w & ((1ull << 36u) - 1ull), 661500ull);

	const std::vector<uint8_t> comment = Slice(file, blocks[2]);
	CLV_AudioVorbisCommentC view {};
	ASSERT_EQ(CLV_AudioParseVorbisComment(comment.data(), comment.size(), &view), CLV_AUDIO_OK);
	EXPECT_EQ(view.vendor_length, 12u);
	EXPECT_EQ(as_string(view.vendor, view.vendor_length), "Lavf63.7.100");
	ASSERT_EQ(view.comment_count, 11u);

	/* 11 条按下标逐条取回，键与值各自对平（顺序 = 块内顺序，值里有空格与 `.` 都不许截断）。 */
	static const char* const kWantKey[] = { "title", "artist",		"album",	"ALBUMARTIST", "date",	 "TRACKNUMBER",
											"genre", "DESCRIPTION", "composer", "publisher",   "encoder" };
	static const char* const kWantVal[] = { "Sample Track",
											"Samplelib",
											"Test Files",
											"Samplelib",
											"2026",
											"3/12",
											"Electronic",
											"Sample MP3 with full ID3v2.3 tags and cover art",
											"Samplelib Generator",
											"samplelib.com",
											"Lavf63.7.100" };
	for (uint32_t i = 0u; i < view.comment_count; ++i)
	{
		CLV_AudioCommentEntryC e {};
		ASSERT_EQ(CLV_AudioVorbisCommentEntryAt(&view, i, &e), CLV_AUDIO_OK) << "第 " << i << " 条取不回";
		EXPECT_EQ(as_string(e.key, e.key_length), kWantKey[i]) << "第 " << i << " 条的键";
		EXPECT_EQ(as_string(e.value, e.value_length), kWantVal[i]) << "第 " << i << " 条的值";
	}

	/* 查键第一次落在真件上：块里写的是小写 `artist`，按大写查也要中 ⇒ 大小写不敏感这条不只在手拼上成立。 */
	CLV_AudioCommentEntryC hit {};
	ASSERT_EQ(CLV_AudioVorbisCommentFindKey(&view, "ARTIST", 0u, &hit), CLV_AUDIO_OK);
	EXPECT_EQ(as_string(hit.value, hit.value_length), "Samplelib");

	CLV_AudioCommentEntryC e {};
	std::memset(&e, 0xA5, sizeof(e));
	const std::vector<uint8_t> sentinel(sizeof(e), 0xA5u);
	/* 越界下标与未命中两路都判拒，且**不写不碰出参** —— 铺了哨兵才分得清「清零」与「没动过」。 */
	EXPECT_EQ(CLV_AudioVorbisCommentEntryAt(&view, 11u, &e), CLV_AUDIO_VALUE_RANGE);
	EXPECT_EQ(std::memcmp(&e, sentinel.data(), sizeof(e)), 0) << "越界下标那次写了出参";
	EXPECT_EQ(e.key_length, 0xA5A5A5A5u) << "key_length 被写";
	EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, "NOSUCHKEY", 0u, &e), CLV_AUDIO_COMMENT_KEY_NOT_FOUND);
	EXPECT_EQ(std::memcmp(&e, sentinel.data(), sizeof(e)), 0) << "未命中那次写了出参";
	EXPECT_EQ(e.value_length, 0xA5A5A5A5u) << "value_length 被写";

	/* 「零条目注释块」这一形本件给不了 ⇒ 就地真块的条目数改成 0（其余字节一字不动）派生出来：
	 * vendor 长 12 ⇒ 条目数字段在 [16,20)。派生块仍要能解，且两路 getter 照旧拒、照旧不碰出参。 */
	std::vector<uint8_t> zero = comment;
	zero[16] = 0u;
	zero[17] = 0u;
	zero[18] = 0u;
	zero[19] = 0u;
	CLV_AudioVorbisCommentC zv {};
	ASSERT_EQ(CLV_AudioParseVorbisComment(zero.data(), zero.size(), &zv), CLV_AUDIO_OK);
	EXPECT_EQ(zv.vendor_length, 12u);
	EXPECT_EQ(zv.comment_count, 0u);
	std::memset(&e, 0xA5, sizeof(e));
	EXPECT_EQ(CLV_AudioVorbisCommentEntryAt(&zv, 0u, &e), CLV_AUDIO_VALUE_RANGE);
	EXPECT_EQ(std::memcmp(&e, sentinel.data(), sizeof(e)), 0) << "零条目按下标那次写了出参";
	EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&zv, "ARTIST", 0u, &e), CLV_AUDIO_COMMENT_KEY_NOT_FOUND);
	EXPECT_EQ(std::memcmp(&e, sentinel.data(), sizeof(e)), 0) << "零条目按键那次写了出参";

	std::cout << "[真件] 块表 = STREAMINFO(34) + PICTURE(7835) + VORBIS_COMMENT(306) + PADDING(8192)，"
				 "vendor 12 B、条目 11 条逐条对平；零条目那一形由真块改条目数派生\n";
}

TEST(AudioMetadataView, SampleTrackPictureFieldsMatchIndependentWalkthrough)
{
	const std::vector<uint8_t> file = ReadWholeFile(std::string(CLV_ASSETS_DIR) + "/sample-15s.flac");
	if (file.empty()) GTEST_SKIP() << "真件不在位，本组真件用例跳过：assets/fixtures/sample-15s.flac";
	const std::vector<FlacBlock> blocks = WalkFlacBlocks(file);
	const FlacBlock* pic = FindBlock(blocks, 6u);
	ASSERT_NE(pic, nullptr);
	const std::vector<uint8_t> payload = Slice(file, *pic);

	CLV_AudioPictureC p {};
	ASSERT_EQ(CLV_AudioParsePicture(payload.data(), payload.size(), &p), CLV_AUDIO_OK);
	EXPECT_EQ(p.picture_type, 0u);	  // 0 = Other：这支件没标「前面封面」
	EXPECT_EQ(p.mime_length, 10u);	  // "image/jpeg"
	EXPECT_EQ(as_string(p.mime, p.mime_length), "image/jpeg");
	EXPECT_EQ(p.description_length, 0u);
	EXPECT_EQ(p.width, 300u);
	EXPECT_EQ(p.height, 300u);
	EXPECT_EQ(p.depth, 12u);	// 块里写的就是 12 而非 24：这一格由写方自定，库不校正也不解释
	EXPECT_EQ(p.colors, 0u);
	ASSERT_EQ(p.data_length, 7793u);
	ASSERT_NE(p.data, nullptr);
	/* 字段串自身对平：type 4 + mime 长 4 + mime 10 + 描述长 4 + 描述 0 + 宽高深色 16 + 数据长 4 + 数据 = 块长。 */
	EXPECT_EQ(4u + 4u + p.mime_length + 4u + p.description_length + 16u + 4u + p.data_length, payload.size());
	EXPECT_EQ(p.data[0], 0xFFu);
	EXPECT_EQ(p.data[1], 0xD8u);
	EXPECT_EQ(p.data[2], 0xFFu);	// JFIF 段起始
	EXPECT_EQ(p.data[p.data_length - 2u], 0xFFu);
	EXPECT_EQ(p.data[p.data_length - 1u], 0xD9u);	 // JPEG 结束标记
	/* 交出的图像字节与独立走查切出来的那一段**整块**逐字节相同（不是「长度相同」）。 */
	ASSERT_EQ(payload.size(), 7835u);
	EXPECT_EQ(std::memcmp(p.data, payload.data() + (payload.size() - p.data_length), p.data_length), 0);

	std::cout << "[真件] PICTURE：type=0 image/jpeg 300x300/12bit，图像数据 " << p.data_length << " B 整块对平\n";
}

/* 真封面块与真注释块经容器整块往返，外加一档「载荷大过两字节域顶」的大块。
 *
 * 扩展块头里 `ext_len` 是 4 字节 ⇒ 单块载荷的可表达上界 = 该字段全 1 = 4 294 967 295 B。
 * 本件那张 300×300 的 JPEG 封面块 7 835 B 加 1 字节 FLAC 原块类型码 = 7 836 B 载荷，落在域内；
 * 但 7 836 B 离两字节域顶（65 535）还差一个数量级，那一格外侧没人量 ⇒
 * 另把真封面块**复制九遍**拼成 70 516 B 载荷（9 × 7 835 + 1 = 70 516，已越出 65 535），
 * 量的正是「声明长度越出两字节可表达域仍被受理」这一侧，字节全部来自真块。
 * 上界外侧只有 2^32 那一档可测：写方把上界判排在 data 判之前，所以声明越界的载荷不必真备得出字节。
 */
TEST(AudioMetadataView, SampleTrackCoverSurvivesContainerRoundTrip)
{
	const std::vector<uint8_t> file = ReadWholeFile(std::string(CLV_ASSETS_DIR) + "/sample-15s.flac");
	if (file.empty()) GTEST_SKIP() << "真件不在位，本组真件用例跳过：assets/fixtures/sample-15s.flac";
	const std::vector<FlacBlock> blocks = WalkFlacBlocks(file);
	const FlacBlock* c = FindBlock(blocks, 4u);
	const FlacBlock* p = FindBlock(blocks, 6u);
	ASSERT_NE(c, nullptr);
	ASSERT_NE(p, nullptr);
	const std::vector<uint8_t> comment = Slice(file, *c);
	const std::vector<uint8_t> picture = Slice(file, *p);
	ASSERT_EQ(picture.size(), 7835u);
	ASSERT_EQ(comment.size(), 306u);

	std::vector<uint8_t> carrier_comment;
	carrier_comment.push_back(4u);
	carrier_comment.insert(carrier_comment.end(), comment.begin(), comment.end());
	std::vector<uint8_t> carrier_picture;
	carrier_picture.push_back(6u);
	carrier_picture.insert(carrier_picture.end(), picture.begin(), picture.end());
	ASSERT_EQ(carrier_picture.size(), 7836u);

	std::vector<uint8_t> carrier_big;
	carrier_big.push_back(6u);
	for (int i = 0; i < 9; ++i) carrier_big.insert(carrier_big.end(), picture.begin(), picture.end());
	ASSERT_EQ(carrier_big.size(), 70516u);	  // 9 × 7835 + 1 ⇒ 越过 65 535 那一档上限

	const std::string path = TempPath("clv_meta_cap");
	ScopedFile guard(path);
	{
		CLV_ContainerError err = CLV_CONTAINER_OK;
		CLV_WriteParams wp;
		std::memset(&wp, 0, sizeof(wp));
		wp.index_present = 1;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(guard.c_str(), &wp, &err);
		ASSERT_NE(w, nullptr) << CLV_ContainerStrError(err);
		const CLV_StreamDescC au = AudioDesc(0);
		ASSERT_EQ(CLV_ContainerWriterAddStream(w, &au), CLV_CONTAINER_OK);

		// 真封面整块收
		ASSERT_EQ(CLV_ContainerWriterAddExtBlock(w, 0, 6, 1, carrier_picture.data(), carrier_picture.size()),
				  CLV_CONTAINER_OK);
		// 上界外一档：声明 2^32 B 载荷。上界判先于 data 判，故 nullptr 也拿得到 TOO_LARGE，且这块不落盘
		EXPECT_EQ(CLV_ContainerWriterAddExtBlock(w, 0, 6, 1, nullptr, 0x100000000ull), CLV_CONTAINER_TOO_LARGE);
		// 越界那一笔没带走句柄：注释块照常收
		ASSERT_EQ(CLV_ContainerWriterAddExtBlock(w, 0, 6, 1, carrier_comment.data(), carrier_comment.size()),
				  CLV_CONTAINER_OK);
		// 派生大块（真封面块 × 9 + 1 字节类型码 = 70 516 B，已越出两字节域顶 65 535）照常收
		ASSERT_EQ(CLV_ContainerWriterAddExtBlock(w, 0, 6, 1, carrier_big.data(), carrier_big.size()), CLV_CONTAINER_OK);
		const std::vector<uint8_t> frame(64u, 0xABu);
		ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 0, 0, 0, 0, frame.data(), frame.size()), CLV_CONTAINER_OK);
		uint64_t packets = 0u;
		ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}

	CLV_ContainerError err = CLV_CONTAINER_OK;
	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(guard.c_str(), &err);
	ASSERT_NE(r, nullptr) << CLV_ContainerStrError(err);
	uint32_t ext_count = 0u;
	ASSERT_EQ(CLV_ContainerReaderExtBlockCount(r, &ext_count), CLV_CONTAINER_OK);
	ASSERT_EQ(ext_count, 3u);

	size_t back_comment = 0u;
	size_t back_picture = 0u;
	size_t back_big = 0u;
	for (uint32_t i = 0u; i < ext_count; ++i)
	{
		CLV_ExtBlockC b {};
		ASSERT_EQ(CLV_ContainerReaderGetExtBlock(r, i, &b), CLV_CONTAINER_OK);
		uint8_t kind = 0u;
		const uint8_t* payload = nullptr;
		size_t payload_size = 0u;
		ASSERT_EQ(CLV_AudioSplitMetaBlock(b.data, b.size, &kind, &payload, &payload_size), CLV_AUDIO_OK);
		if (kind == 4u)
		{
			/* 真注释块读回来逐字节相同，并且还能解析。 */
			ASSERT_EQ(payload_size, comment.size());
			EXPECT_EQ(std::memcmp(payload, comment.data(), comment.size()), 0);
			CLV_AudioVorbisCommentC view {};
			ASSERT_EQ(CLV_AudioParseVorbisComment(payload, payload_size, &view), CLV_AUDIO_OK);
			EXPECT_EQ(as_string(view.vendor, view.vendor_length), "Lavf63.7.100");
			EXPECT_EQ(view.comment_count, 11u);
			back_comment = payload_size;
		}
		else
		{
			ASSERT_EQ(kind, 6u);
			if (payload_size == carrier_big.size() - 1u)
			{
				// 派生大块：九份真封面块首尾相接，拆掉类型码后逐字节回来 —— 载荷长越出两字节域顶仍整块收
				EXPECT_EQ(std::memcmp(payload, carrier_big.data() + 1u, payload_size), 0);
				back_big = payload_size;
				continue;
			}
			// 拆掉 1 字节 FLAC 原块类型码之后，整块载荷逐字节回来
			ASSERT_EQ(payload_size, picture.size());
			EXPECT_EQ(std::memcmp(payload, picture.data(), payload_size), 0);
			// 回来的字节还能按 FLAC 封面块解，字段与上一用例的独立走查同值
			CLV_AudioPictureC pic {};
			ASSERT_EQ(CLV_AudioParsePicture(payload, payload_size, &pic), CLV_AUDIO_OK);
			ASSERT_EQ(pic.data_length, 7793u);
			back_picture = payload_size;
		}
	}
	CLV_ContainerReaderClose(r);
	EXPECT_EQ(back_comment, comment.size());
	EXPECT_EQ(back_picture, picture.size());
	EXPECT_EQ(back_big, carrier_big.size() - 1u);

	std::cout << "[封面往返] 真封面块 " << carrier_picture.size() << " B 与派生大块 " << carrier_big.size()
			  << " B 都整块收、读回逐字节相同；声明 4294967296 B 判 TOO_LARGE；真注释块 " << back_comment
			  << " B 往返逐字节相同\n";
}
