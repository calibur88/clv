/* test_audio_blob_refs.cpp
 *
 * 公开 C ABI 用例：只 include `include/CLV_Audio.h`（注释块素材取自同目录的
 * `audio_fixtures_p.h`），不经实现层头。
 *
 * `CLV_AudioBlob` 的两个产出出口是 `CLV_AudioStripChannelMask` 与
 * `CLV_AudioEncoderTakeStreamInfo`：两处都新建一个 `refs` 初值为 1 的对象交给调用方，
 * 出口本身不再 AddRef。三个引用口的现行口径见 `src/audio/clv_audio.cpp` 的
 * `CLV_AudioBlobAddRef` / `CLV_AudioBlobRelease` / `CLV_AudioBlobGetBytes`：
 *   AddRef —— 自增并返回**自增后**的计数；入参为 NULL 时返回 0。
 *   Release —— 自减；计数归零才回收对象；入参为 NULL 时什么都不做。
 *   GetBytes —— 先判句柄（NULL → INVALID_HANDLE），再判「两个出参都不要」（→ INVALID_ARGUMENT），
 *     只取指针或只取长度都算合法。
 * 回收本身在这套 ABI 上没有可观测出口：blob 由本层自己的 new/delete 管，不经任何分配统计钩子，
 * 「最后一次 Release 真把对象释放」只能由计数算术推出，断言里不假装看到它。
 * `GetBytes` 交出的指针指向 blob 自己的缓冲，没有任何公开口会改写它，故各次调用之间恒定；
 * 有效期到该 blob 的最后一个引用被 Release 为止。
 */

#include "CLV_Audio.h"
#include "audio_fixtures_p.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

	constexpr uint32_t kRate = 44100u;
	constexpr uint16_t kBlocksize = 1024u;

	CLV_AudioEncParamsC EncParams()
	{
		CLV_AudioEncParamsC p {};
		p.sample_rate = kRate;
		p.channels = 2u;
		p.bit_depth = 16u;
		p.blocksize = kBlocksize;
		p.level = 5u;
		return p;
	}

	// 带扩展声道掩码的注释块 → 剥掩码后的 blob（库拥有，refs 从 1 起）
	CLV_AudioBlob* MaskBlob(uint32_t* removed)
	{
		CLV_AudioBlob* blob = nullptr;
		const CLV_AudioError e =
			CLV_AudioStripChannelMask(clvtest::kVorbisMaskBytes, clvtest::kVorbisMaskLen, &blob, removed);
		EXPECT_EQ(e, CLV_AUDIO_OK) << CLV_AudioStrError(e);
		return blob;
	}

}	 // namespace

TEST(AudioBlobRefs, BlobAddRefCountsUpFromOneAndReleasePairsOneForOne)
{
	uint32_t removed = 0u;
	CLV_AudioBlob* blob = MaskBlob(&removed);
	ASSERT_NE(blob, nullptr);
	// 素材里掩码键共 1 条（audio_fixtures_p.h 的 kVorbisMaskEntries）
	EXPECT_EQ(removed, clvtest::kVorbisMaskEntries);

	const uint8_t* first = nullptr;
	size_t first_size = 0u;
	ASSERT_EQ(CLV_AudioBlobGetBytes(blob, &first, &first_size), CLV_AUDIO_OK);
	ASSERT_GT(first_size, 8u);
	ASSERT_LT(first_size, clvtest::kVorbisMaskLen);
	const std::vector<uint8_t> keep(first, first + first_size);

	// 产出口不加引用：交出来就是那一份初始引用，于是第一次 AddRef 报 1 + 1
	EXPECT_EQ(CLV_AudioBlobAddRef(blob), 2u);
	EXPECT_EQ(CLV_AudioBlobAddRef(blob), 3u);
	EXPECT_EQ(CLV_AudioBlobAddRef(blob), 4u);

	// 计数涨到 4 的这段时间里字节照旧可读，指针、长度、内容三者都不动
	const uint8_t* again = nullptr;
	size_t again_size = 0u;
	ASSERT_EQ(CLV_AudioBlobGetBytes(blob, &again, &again_size), CLV_AUDIO_OK);
	EXPECT_EQ(again, first);
	EXPECT_EQ(again_size, first_size);
	ASSERT_EQ(again_size, keep.size());
	EXPECT_EQ(std::memcmp(again, keep.data(), keep.size()), 0);

	// 三次 Release 抵掉三次 AddRef：4 → 3 → 2 → 1
	CLV_AudioBlobRelease(blob);
	CLV_AudioBlobRelease(blob);
	CLV_AudioBlobRelease(blob);
	// AddRef 的返回值是这套 ABI 上唯一能读到当前计数的口子：报 2 就说明加之前正是 1，
	// 即上三次各只减了 1，一次都没多减
	EXPECT_EQ(CLV_AudioBlobAddRef(blob), 2u);

	// 剩下的两份（初始那一份 + 刚才这一次）放完；归零即回收 —— 回收本身无可观测出口，
	// 这里按 Release 的口径把它当作「调用完成后指针不得再用」的约定线
	CLV_AudioBlobRelease(blob);
	CLV_AudioBlobRelease(blob);
}

TEST(AudioBlobRefs, StreamInfoBlobKeepsBytesAcrossExtraRefs)
{
	const CLV_AudioEncParamsC ep = EncParams();
	CLV_AudioError err = CLV_AUDIO_OK;
	CLV_AudioEncoder* e = CLV_AudioEncoderOpen(&ep, &err);
	ASSERT_NE(e, nullptr) << CLV_AudioStrError(err);

	// 一整块静音 PCM：1024 样本 × 2 声道 × 2 字节 = 4096 B
	const std::vector<uint8_t> pcm(static_cast<size_t>(kBlocksize) * 2u * 2u, 0u);
	ASSERT_EQ(CLV_AudioEncoderWrite(e, pcm.data(), pcm.size(), kBlocksize), CLV_AUDIO_OK) << CLV_AudioStrError(err);
	ASSERT_EQ(CLV_AudioEncoderFlush(e), CLV_AUDIO_OK);

	CLV_AudioBlob* blob = nullptr;
	ASSERT_EQ(CLV_AudioEncoderTakeStreamInfo(e, &blob), CLV_AUDIO_OK);
	ASSERT_NE(blob, nullptr);

	const uint8_t* data = nullptr;
	size_t size = 0u;
	ASSERT_EQ(CLV_AudioBlobGetBytes(blob, &data, &size), CLV_AUDIO_OK);
	// 流信息载荷定长 34 字节（公开头的 TakeStreamInfo 条目）
	EXPECT_EQ(size, 34u);

	// 出口同样不额外加引用：第一声 AddRef 报 2
	EXPECT_EQ(CLV_AudioBlobAddRef(blob), 2u);

	// 载荷已被搬进 blob 自己的缓冲，编码器句柄关掉之后 blob 及其字节都还在
	CLV_AudioEncoderClose(e);
	EXPECT_EQ(CLV_AudioBlobAddRef(blob), 3u);

	const uint8_t* after = nullptr;
	size_t after_size = 0u;
	ASSERT_EQ(CLV_AudioBlobGetBytes(blob, &after, &after_size), CLV_AUDIO_OK);
	EXPECT_EQ(after, data);
	EXPECT_EQ(after_size, 34u);

	// 多挂了两次引用的 blob 仍解得出流信息，字段就是编码入参
	CLV_AudioStreamInfoC si {};
	ASSERT_EQ(CLV_AudioParseStreamInfo(after, after_size, &si), CLV_AUDIO_OK);
	EXPECT_EQ(si.sample_rate, kRate);
	EXPECT_EQ(si.channels, 2u);
	EXPECT_EQ(si.bit_depth, 16u);

	// 手里现在是三份（初始 + 两次 AddRef），三次 Release 放完；归零后的回收不可观测
	CLV_AudioBlobRelease(blob);
	CLV_AudioBlobRelease(blob);
	CLV_AudioBlobRelease(blob);
}

TEST(AudioBlobRefs, BlobRefApiIsInertOnNullHandleAndPartialOutParams)
{
	// NULL 档：AddRef 报 0 不崩，Release 是 no-op，GetBytes 走句柄档
	EXPECT_EQ(CLV_AudioBlobAddRef(nullptr), 0u);
	CLV_AudioBlobRelease(nullptr);
	const uint8_t* probe = nullptr;
	size_t probe_size = 0u;
	EXPECT_EQ(CLV_AudioBlobGetBytes(nullptr, &probe, &probe_size), CLV_AUDIO_INVALID_HANDLE);

	uint32_t removed = 0u;
	CLV_AudioBlob* blob = MaskBlob(&removed);
	ASSERT_NE(blob, nullptr);

	// 两个出参都不要 = 入参不成立；只给其中一个仍算合法
	EXPECT_EQ(CLV_AudioBlobGetBytes(blob, nullptr, nullptr), CLV_AUDIO_INVALID_ARGUMENT);
	const uint8_t* data = nullptr;
	size_t size = 0u;
	EXPECT_EQ(CLV_AudioBlobGetBytes(blob, nullptr, &size), CLV_AUDIO_OK);
	EXPECT_GT(size, 8u);
	EXPECT_EQ(CLV_AudioBlobGetBytes(blob, &data, nullptr), CLV_AUDIO_OK);
	EXPECT_NE(data, nullptr);

	// 一次 AddRef 一次 Release 走平，末尾那一次 Release 把初始引用放完
	EXPECT_EQ(CLV_AudioBlobAddRef(blob), 2u);
	CLV_AudioBlobRelease(blob);
	CLV_AudioBlobRelease(blob);
}
