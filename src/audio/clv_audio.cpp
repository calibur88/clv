/* 音频面 C ABI 的门面：把公开调用转成实现层调用。
 *
 * 这里不放任何格式判断——帧头、承载块、注释块的判定全在 audio 层里。
 * 无状态工具（帧头自查、承载块拆分与分类、掩码剥离、流信息解析）只做入参校验、转值与转调；
 * 编解码经 backends/flac/flac_codec.h 桥接 libFLAC，本文件不含 libFLAC 自己的头。
 *
 * 异常不出 C 边界：本层唯一的抛出源是分配（承载块 / PCM 的 std::vector、new 出的 Blob 与 Buffer），每个
 * 公开入口都在这一层收口成 CLV_AUDIO_OUT_OF_MEMORY —— 句柄口判空置 *err 后返 NULL，计数口返 0，出参守住
 * 入口那份清零纪律，已建而未交出的对象在 catch 里回收，不留半份结果。
 * catch 只接 std::bad_alloc，绝不接 std::exception 或 ...：实现层的逻辑错误（帧头不成立、字段失配、顺序
 * 错）一旦被吞成「内存不足」，调用方就分不清是该重试分配还是该丢这一帧，这种伪装比 terminate 更难查。
 * 两类入口不包：CLV_AudioStrError 一族只回字符串字面量、不做任何分配，包了反而要替它编一个 OOM 返回串；
 * 拆卸口（Close / Release）走的是 delete 与 vector 析构，都是非分配路径、没有抛出源，包 catch 反而给下
 * 一个读代码的人指出一条不存在的失败通道。
 */
#include "CLV_Audio.h"

#include "../core/log.h"
#include "backends/flac/flac_codec.h"
#include "frame.h"
#include "metadata.h"
#include "status.h"

#include <new>
#include <vector>

namespace
{

	using clv::audio::AudioErr;

	/* 公开枚举与内部枚举逐项同序，门面直接转值。顺序错位在这里编译期就断掉。 */
	static_assert(static_cast<int>(AudioErr::kOk) == static_cast<int>(CLV_AUDIO_OK), "ok order");
	static_assert(static_cast<int>(AudioErr::kInvalidHandle) == static_cast<int>(CLV_AUDIO_INVALID_HANDLE),
				  "handle order");
	static_assert(static_cast<int>(AudioErr::kInvalidArgument) == static_cast<int>(CLV_AUDIO_INVALID_ARGUMENT),
				  "argument order");
	static_assert(static_cast<int>(AudioErr::kValueRange) == static_cast<int>(CLV_AUDIO_VALUE_RANGE), "range order");
	static_assert(static_cast<int>(AudioErr::kFrameHeaderBad) == static_cast<int>(CLV_AUDIO_FRAME_HEADER_BAD),
				  "header order");
	static_assert(static_cast<int>(AudioErr::kFieldMismatch) == static_cast<int>(CLV_AUDIO_FIELD_MISMATCH),
				  "mismatch order");
	static_assert(static_cast<int>(AudioErr::kBlocksizeOutOfRange) ==
					  static_cast<int>(CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE),
				  "blocksize order");
	static_assert(static_cast<int>(AudioErr::kStreamInfoAbsent) == static_cast<int>(CLV_AUDIO_STREAMINFO_ABSENT),
				  "streaminfo order");
	static_assert(static_cast<int>(AudioErr::kMetadataBad) == static_cast<int>(CLV_AUDIO_METADATA_BAD), "meta order");
	static_assert(static_cast<int>(AudioErr::kBlockTypeUnsupported) ==
					  static_cast<int>(CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED),
				  "block type order");
	static_assert(static_cast<int>(AudioErr::kTooLarge) == static_cast<int>(CLV_AUDIO_TOO_LARGE), "large order");
	static_assert(static_cast<int>(AudioErr::kStateError) == static_cast<int>(CLV_AUDIO_STATE_ERROR), "state order");
	static_assert(static_cast<int>(AudioErr::kOutOfMemory) == static_cast<int>(CLV_AUDIO_OUT_OF_MEMORY), "oom order");
	static_assert(static_cast<int>(AudioErr::kBackendFailure) == static_cast<int>(CLV_AUDIO_BACKEND_FAILURE),
				  "backend order");
	static_assert(static_cast<int>(AudioErr::kStreamRejected) == static_cast<int>(CLV_AUDIO_STREAM_REJECTED),
				  "rejected order");
	/* 内部枚举止于 47；段尾两格只在门面产出，不经内部枚举，故只能在这里显式钉数值。
	 * 41 / 45 两个空位同样显式钉住——后续追加若把号位挤偏，这里先红。 */
	static_assert(static_cast<int>(CLV_AUDIO_TOO_LARGE) == 42, "gap 41 keeps TOO_LARGE at 42");
	static_assert(static_cast<int>(CLV_AUDIO_BACKEND_FAILURE) == 46, "gap 45 keeps BACKEND_FAILURE at 46");
	static_assert(static_cast<int>(CLV_AUDIO_METADATA_MALFORMED) == 48, "metadata malformed");
	static_assert(static_cast<int>(CLV_AUDIO_COMMENT_KEY_NOT_FOUND) == 49, "comment key not found");

	/* 投日志走核心层那处共用薄封装（src/core/log.h），本层不自己拼等级枚举值：
	 * 只影响本帧 / 本块的处置（丢帧、跳块）走 WARN 级；整条流拒解走 ERROR 级。
	 * 日志不改变处置结论：CLV_Log 无错误通道，越界等级与空串由它自行丢弃。 */
	using clv::core::LogWarn;

	using clv::core::LogError;

	/* 对账的持续失配阈值：同一字段连续失配这么多帧即判流声明本身失真，整条音频流拒解。
	 * 只有整帧对账通过才打断连续段——被丢的帧既不算证据也不复位。 */
	inline constexpr uint32_t kFieldMismatchRunLimit = 8u;

	bool RunExhausted(uint32_t* run)
	{
		++*run;
		return *run >= kFieldMismatchRunLimit;
	}

	struct Blob
	{
		std::vector<uint8_t> data;
		uint32_t refs = 1u;
	};

	// 公开出参里的长度与计数字段是 uint32_t：入参尺寸超出它可表达域即在入口拒收，不做静默截断
	constexpr size_t kOutFieldMax = 0xFFFFFFFFull;

	/* 注释块视图的条目区起点：4 字节厂商串长度 + 厂商串 + 4 字节条目数。
	 * 视图里存了厂商串长度，这里重算而不是再存一个偏移；只在过了 VCommentViewIsSane 之后调用，
	 * 那时厂商长度与声明块长的关系已定，这一式在 size_t 域里既不下溢也不回绕。 */
	size_t VCommentEntriesOffset(const CLV_AudioVorbisCommentC* v) { return 4u + v->vendor_length + 4u; }

	/* 注释块视图的入口自洽校验（取第几条与按键查两个口共用）：
	 * ①块指针非空；②声明块长不小于头部必需字段（4 字节厂商串长度 + 4 字节条目数 = 8）；
	 * ③条目区起点不大于声明块长——起点按 64 位算式算（8 + 厂商串长度），长度加减在这一层就不存在
	 * 下溢与回绕两笔糊涂账。视图须由 CLV_AudioParseVorbisComment 产出，这条校验挡的是拿到的视图
	 * 与字节不同档的情形：校验不过直接判结构不成立，绝不带着不自洽的视图进条目扫描、从块外取字节。 */
	bool VCommentViewIsSane(const CLV_AudioVorbisCommentC* v)
	{
		if (v->block == nullptr) return false;
		if (v->block_size < 8u) return false;
		const uint64_t entries_off = 8ull + v->vendor_length;
		return entries_off <= static_cast<uint64_t>(v->block_size);
	}

}	 // namespace

CLV_AudioError CLV_AudioInspectFrame(const uint8_t* payload, size_t size, CLV_AudioFrameHeadC* out)
{
	try
	{
		if (out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		if (payload == nullptr || size == 0u) return CLV_AUDIO_INVALID_ARGUMENT;

		clv::audio::FrameHead h;
		if (! clv::audio::ParseFrameHead(payload, size, &h)) return CLV_AUDIO_FRAME_HEADER_BAD;
		if (! clv::audio::StreamableSubsetOk(h.blocksize, h.sample_rate)) return CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE;

		out->blocksize = h.blocksize;
		out->sample_rate = h.sample_rate;
		out->channels = h.channels;
		out->bit_depth = h.bit_depth;
		out->frame_or_sample_number = h.coded_number;
		out->blocking_strategy = h.blocking_strategy;
		out->header_size = h.header_size;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioParseStreamInfo(const uint8_t* data, size_t size, CLV_AudioStreamInfoC* out)
{
	try
	{
		if (out == nullptr || data == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		clv::audio::StreamInfo si;
		if (! clv::audio::ParseStreamInfo(data, size, &si)) return CLV_AUDIO_VALUE_RANGE;

		out->min_blocksize = si.min_blocksize;
		out->max_blocksize = si.max_blocksize;
		out->min_framesize = si.min_framesize;
		out->max_framesize = si.max_framesize;
		out->sample_rate = si.sample_rate;
		out->channels = si.channels;
		out->bit_depth = si.bit_depth;
		out->total_samples = si.total_samples;
		for (int i = 0; i < 16; ++i) out->md5[i] = si.md5[i];
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioSplitMetaBlock(const uint8_t* ext_payload, size_t size, uint8_t* flac_block_type,
									   const uint8_t** data, size_t* out_size)
{
	try
	{
		if (ext_payload == nullptr || size == 0u) return CLV_AUDIO_INVALID_ARGUMENT;
		if (flac_block_type == nullptr && data == nullptr && out_size == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		const uint8_t* p = nullptr;
		size_t n = 0u;
		uint8_t t = 0u;
		if (! clv::audio::SplitMetaBlock(ext_payload, size, &t, &p, &n)) return CLV_AUDIO_INVALID_ARGUMENT;
		if (flac_block_type != nullptr) *flac_block_type = t;
		if (data != nullptr) *data = p;
		if (out_size != nullptr) *out_size = n;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioClassifyMetaBlock(uint8_t flac_block_type, CLV_AudioMetaAction* out)
{
	try
	{
		if (out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		switch (clv::audio::ClassifyMetaBlock(flac_block_type))
		{
		case clv::audio::MetaAction::kStore: *out = CLV_AUDIO_META_STORE; return CLV_AUDIO_OK;
		case clv::audio::MetaAction::kDropPadding: *out = CLV_AUDIO_META_DROP_PADDING; return CLV_AUDIO_OK;
		case clv::audio::MetaAction::kRejectStreamInfo:
			*out = CLV_AUDIO_META_REJECT_STREAMINFO;
			return CLV_AUDIO_METADATA_BAD;
		default: *out = CLV_AUDIO_META_SKIP_UNKNOWN; return CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED;
		}
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioStripChannelMask(const uint8_t* block, size_t size, CLV_AudioBlob** out, uint32_t* removed)
{
	/* Blob 指针提到 try 之外只为让 catch 能回收它：*out 在最后一行才交出，
	 * 进了 catch 就说明 Blob 没交出去，delete 一次不会与调用方重复持有。 */
	Blob* blob = nullptr;
	try
	{
		if (out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		*out = nullptr;
		if (block == nullptr || size == 0u) return CLV_AUDIO_INVALID_ARGUMENT;

		blob = new (std::nothrow) Blob();
		if (blob == nullptr) return CLV_AUDIO_OUT_OF_MEMORY;
		uint32_t gone = 0u;
		if (! clv::audio::StripChannelMask(block, size, &blob->data, &gone))
		{
			delete blob;
			// 坏在块的内容层（长度前缀越界、条目数与实有条数不符、条目里没有 `=`），走 METADATA_MALFORMED；
			// METADATA_BAD 只留给「承载块首字节 = 0」那一档搬运层问题，两档不混用。
			return CLV_AUDIO_METADATA_MALFORMED;
		}
		if (removed != nullptr) *removed = gone;
		*out = reinterpret_cast<CLV_AudioBlob*>(blob);
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// StripChannelMask 填 blob->data 的 vector 就是本口的抛出源：Blob 建了却没交出去，回收干净
		delete blob;
		if (out != nullptr) *out = nullptr;
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioParseVorbisComment(const uint8_t* data, size_t size, CLV_AudioVorbisCommentC* out)
{
	try
	{
		if (out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		std::memset(out, 0, sizeof(*out));
		if (data == nullptr || size == 0u) return CLV_AUDIO_INVALID_ARGUMENT;
		if (size > kOutFieldMax) return CLV_AUDIO_VALUE_RANGE;

		const uint8_t* vendor = nullptr;
		size_t vendor_len = 0u;
		uint32_t count = 0u;
		if (! clv::audio::ParseVComment(data, size, &vendor, &vendor_len, &count, nullptr))
			return CLV_AUDIO_METADATA_MALFORMED;

		out->block = data;
		out->block_size = static_cast<uint32_t>(size);
		out->vendor = vendor;
		out->vendor_length = static_cast<uint32_t>(vendor_len);
		out->comment_count = count;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// 视图出参回到入口的 memset 清零态：不留半份视图
		if (out != nullptr) std::memset(out, 0, sizeof(*out));
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioVorbisCommentEntryAt(const CLV_AudioVorbisCommentC* view, uint32_t index,
											 CLV_AudioCommentEntryC* out)
{
	try
	{
		if (view == nullptr || out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		// 视图入口先做自洽校验：不自洽的视图不进扫描，不取块外字节
		if (! VCommentViewIsSane(view)) return CLV_AUDIO_METADATA_MALFORMED;
		if (index >= view->comment_count) return CLV_AUDIO_VALUE_RANGE;

		clv::audio::CommentEntry e;
		if (! clv::audio::VCommentEntryAt(view->block, view->block_size, VCommentEntriesOffset(view),
										  view->comment_count, index, &e))
		{
			return CLV_AUDIO_METADATA_MALFORMED;
		}

		out->key = e.key;
		out->key_length = static_cast<uint32_t>(e.key_len);
		out->value = e.value;
		out->value_length = static_cast<uint32_t>(e.value_len);
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioVorbisCommentFindKey(const CLV_AudioVorbisCommentC* view, const char* key, uint32_t occurrence,
											 CLV_AudioCommentEntryC* out)
{
	try
	{
		if (view == nullptr || out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		if (key == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		// 视图入口先做自洽校验：不自洽的视图不进扫描，不取块外字节；自洽视图查不到仍照实报「键不存在」
		if (! VCommentViewIsSane(view)) return CLV_AUDIO_METADATA_MALFORMED;

		clv::audio::CommentEntry e;
		if (! clv::audio::VCommentFindKey(view->block, view->block_size, VCommentEntriesOffset(view),
										  view->comment_count, key, occurrence, &e))
		{
			return CLV_AUDIO_COMMENT_KEY_NOT_FOUND;
		}

		out->key = e.key;
		out->key_length = static_cast<uint32_t>(e.key_len);
		out->value = e.value;
		out->value_length = static_cast<uint32_t>(e.value_len);
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioParsePicture(const uint8_t* data, size_t size, CLV_AudioPictureC* out)
{
	try
	{
		if (out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		std::memset(out, 0, sizeof(*out));
		if (data == nullptr || size == 0u) return CLV_AUDIO_INVALID_ARGUMENT;
		if (size > kOutFieldMax) return CLV_AUDIO_VALUE_RANGE;

		clv::audio::PictureFields f;
		if (! clv::audio::ParsePictureBlock(data, size, &f)) return CLV_AUDIO_METADATA_MALFORMED;

		out->picture_type = f.type;
		out->mime = f.mime;
		out->mime_length = static_cast<uint32_t>(f.mime_len);
		out->description = f.description;
		out->description_length = static_cast<uint32_t>(f.description_len);
		out->width = f.width;
		out->height = f.height;
		out->depth = f.depth;
		out->colors = f.colors;
		out->data = f.data;
		out->data_length = static_cast<uint32_t>(f.data_len);
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// 出参回到入口的 memset 清零态：不留半份图片描述
		if (out != nullptr) std::memset(out, 0, sizeof(*out));
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

uint32_t CLV_AudioBlobAddRef(CLV_AudioBlob* b)
{
	try
	{
		Blob* self = reinterpret_cast<Blob*>(b);
		if (self == nullptr) return 0u;
		return ++self->refs;
	}
	catch (const std::bad_alloc&)
	{
		return 0u;	  // 计数口没有 OOM 码可给，按空句柄同档返 0
	}
}

void CLV_AudioBlobRelease(CLV_AudioBlob* b)
{
	Blob* self = reinterpret_cast<Blob*>(b);
	if (self == nullptr) return;
	// 计数已归零的对象再 Release 不生效：不回收、也不回绕成一个巨大的计数
	if (self->refs == 0u) return;
	if (--self->refs == 0u) delete self;
}

CLV_AudioError CLV_AudioBlobGetBytes(const CLV_AudioBlob* b, const uint8_t** data, size_t* size)
{
	try
	{
		const Blob* self = reinterpret_cast<const Blob*>(b);
		if (self == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		if (data == nullptr && size == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		if (data != nullptr) *data = self->data.empty() ? nullptr : self->data.data();
		if (size != nullptr) *size = self->data.size();
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

/* ---- 句柄类函数：编解码闭环走 libFLAC，桥在 backends/flac ---- */

namespace
{

	struct Buffer
	{
		CLV_AudioBufferDescC desc {};
		std::vector<uint8_t> pcm;
		uint32_t refs = 1u;
	};

	uint16_t BytesPerSample(uint16_t bit_depth) { return static_cast<uint16_t>((bit_depth + 7u) / 8u); }

}	 // namespace

/* 两个不透明句柄的定义必须落在全局作用域：公开头里 `typedef struct CLV_AudioDecoder
 * CLV_AudioDecoder;` 指的是全局标签，放进匿名命名空间就成了另一个类型。 */
struct CLV_AudioDecoder
{
	CLV_AudioStreamParamsC params {};
	clv::audio::StreamInfo si {};
	bool rejected = false;	  // 流信息缺失或与流声明矛盾：整条音频流拒解
	/* 三个对账字段各自的连续失配段长；任一字段达到阈值即置 rejected。 */
	uint32_t run_sample_rate = 0u;
	uint32_t run_channels = 0u;
	uint32_t run_bit_depth = 0u;
	uint64_t next_sample = 0u;
	clv::audio::FlacDecoder codec;
	CLV_AudioStatsC stats {};
};

/* 失配掩码（bit0 = 采样率，bit1 = 声道数，bit2 = 位深）逐字段推进各自的连击计数，
 * 任一段到上限即判「同一字段持续失配」。帧头自核与解码产物核对两条路径共用这一处判定，
 * 免得两处的阈值语义各走各的。 */
namespace
{

	bool RunsExhausted(CLV_AudioDecoder* d, uint32_t contradicted)
	{
		bool exhausted = false;
		if (contradicted & 1u) exhausted = RunExhausted(&d->run_sample_rate) || exhausted;
		if (contradicted & 2u) exhausted = RunExhausted(&d->run_channels) || exhausted;
		if (contradicted & 4u) exhausted = RunExhausted(&d->run_bit_depth) || exhausted;
		return exhausted;
	}

}	 // namespace

struct CLV_AudioEncoder
{
	CLV_AudioEncParamsC params {};
	clv::audio::FlacEncoder codec;
	std::vector<uint8_t> current;	 // NextFrame 交出的指针要活到下一次取帧
	bool flushed = false;
};

CLV_AudioDecoder* CLV_AudioDecoderOpen(const CLV_AudioStreamParamsC* params, const uint8_t* streaminfo,
									   size_t streaminfo_size, CLV_AudioError* err)
{
	try
	{
		if (err != nullptr) *err = CLV_AUDIO_OK;
		if (params == nullptr)
		{
			if (err != nullptr) *err = CLV_AUDIO_INVALID_ARGUMENT;
			return nullptr;
		}
		/* 布局自报在先：非 0 又不等于本版本 sizeof 的对象一律显式拒，库不去猜它的字段落在哪。 */
		if (params->struct_size != 0u && params->struct_size != sizeof(CLV_AudioStreamParamsC))
		{
			if (err != nullptr) *err = CLV_AUDIO_VALUE_RANGE;
			return nullptr;
		}
		if (params->sample_rate == 0u || params->channels == 0u || params->channels > 8u ||
			params->max_packet_size == 0u)
		{
			if (err != nullptr) *err = CLV_AUDIO_VALUE_RANGE;
			return nullptr;
		}
		if (params->bit_depth != 16u && params->bit_depth != 24u)
		{
			if (err != nullptr) *err = CLV_AUDIO_VALUE_RANGE;
			return nullptr;
		}
		/* 参数过了才轮到「流」本身——坏流给句柄、分配失败给 null：下面两条致命路径都交出拒解态句柄
		 * （与视频侧同形：句柄可用，但除 Reset / Close 与 Stats 外的调用一律返 CLV_AUDIO_STREAM_REJECTED）。 */
		auto* d = new (std::nothrow) CLV_AudioDecoder();
		if (d == nullptr)
		{
			if (err != nullptr) *err = CLV_AUDIO_OUT_OF_MEMORY;
			return nullptr;
		}
		if (! d->codec.Ok())	// Handle 或后端 FLAC__StreamDecoder 分配失败
		{
			delete d;
			if (err != nullptr) *err = CLV_AUDIO_OUT_OF_MEMORY;
			return nullptr;
		}
		d->params = *params;

		/* 缺流信息（未给 / 长度不符 / 字段越出子集）：流本身不成立 → 置拒解态、交出句柄。 */
		if (streaminfo == nullptr || streaminfo_size != clv::audio::kStreamInfoSize ||
			! clv::audio::ParseStreamInfo(streaminfo, streaminfo_size, &d->si))
		{
			d->rejected = true;
			if (err != nullptr) *err = CLV_AUDIO_STREAMINFO_ABSENT;
			return d;
		}
		/* 流信息与流声明互相矛盾 → 拒解整条音频流（其余流不受影响，容器侧按流独立处置）。 */
		if (d->si.sample_rate != params->sample_rate || d->si.channels != params->channels ||
			d->si.bit_depth != params->bit_depth)
		{
			d->rejected = true;
			if (err != nullptr) *err = CLV_AUDIO_STREAM_REJECTED;
			return d;
		}
		return d;
	}
	catch (const std::bad_alloc&)
	{
		// 句柄口：分配失败交不出句柄，*err 置 OOM 后返 NULL（上面两条坏流档照旧交出拒解句柄，不属本口径）
		if (err != nullptr) *err = CLV_AUDIO_OUT_OF_MEMORY;
		return nullptr;
	}
}

CLV_AudioError CLV_AudioDecoderDecodeFrame(CLV_AudioDecoder* d, const uint8_t* payload, size_t size,
										   CLV_AudioBuffer** out, uint8_t* got, CLV_AudioFrameInfoC* info)
{
	try
	{
		if (out != nullptr) *out = nullptr;
		if (got != nullptr) *got = 0u;
		if (info != nullptr) std::memset(info, 0, sizeof(*info));
		if (d == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		if (d->rejected) return CLV_AUDIO_STREAM_REJECTED;	  // 与视频侧拒解态同形：拒解态一帧不出（*got 留 0）
		if (out == nullptr || got == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		if (payload == nullptr || size == 0u) return CLV_AUDIO_INVALID_ARGUMENT;
		if (size > d->params.max_packet_size) return CLV_AUDIO_TOO_LARGE;

		clv::audio::FrameHead h;
		/* 帧头一类原因的三档丢帧（自核不过 / 帧尾 CRC-16 不符 / 越出子集）口径统一为「丢帧 + warn」，
		 * 与对账两档同式：都只影响本帧、处置结论不变，观测面上不该有一档静默一档留声。 */
		if (! clv::audio::ParseFrameHead(payload, size, &h))
		{
			d->stats.frames_dropped_header++;
			LogWarn("clv audio: frame header self-check failed, frame dropped");
			return CLV_AUDIO_FRAME_HEADER_BAD;
		}
		/* 帧尾 CRC-16 属帧头自核义务，本层自己判：不把「后端会丢掉坏帧」当契约，也不依赖后端的内部处置。
		 * 覆盖除末两字节校验值以外的整帧，不符按丢帧处置。 */
		if (! clv::audio::FrameCrcOk(payload, size))
		{
			d->stats.frames_dropped_header++;
			LogWarn("clv audio: frame CRC-16 mismatch, frame dropped");
			return CLV_AUDIO_FRAME_HEADER_BAD;
		}
		if (! clv::audio::StreamableSubsetOk(h.blocksize, h.sample_rate))
		{
			d->stats.frames_dropped_header++;
			LogWarn("clv audio: frame outside streamable subset, frame dropped");
			return CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE;
		}

		/* 对账：帧头里带值的字段必须与流声明一致；块大小必须落在流信息给的区间内。
		 * 帧头写 0 的字段（「值只在流信息里给」）不参与对账，回落流声明。
		 * 单帧不一致 = 丢帧 + warn，继续解下一帧；块大小不在对账清单内，不计连续失配段。 */
		if (h.blocksize != 0u && (h.blocksize < d->si.min_blocksize || h.blocksize > d->si.max_blocksize))
		{
			d->stats.frames_dropped_mismatch++;
			LogWarn("clv audio: frame blocksize outside stream info range, frame dropped");
			return CLV_AUDIO_FIELD_MISMATCH;
		}
		uint32_t contradicted = 0u;	   // 位掩码：1 = 采样率，2 = 声道数，4 = 位深
		if (h.sample_rate != 0u && h.sample_rate != d->params.sample_rate) contradicted |= 1u;
		if (h.channels != 0u && h.channels != d->params.channels) contradicted |= 2u;
		if (h.bit_depth != 0u && h.bit_depth != d->params.bit_depth) contradicted |= 4u;
		if (contradicted != 0u)
		{
			d->stats.frames_dropped_mismatch++;
			LogWarn("clv audio: frame header contradicts stream declaration, frame dropped");
			if (RunsExhausted(d, contradicted))
			{
				/* 同一字段持续失配到这一步表明流声明本身失真：拒解整条音频流，其余流不受影响。 */
				d->rejected = true;
				LogError("clv audio: stream declaration contradicted by consecutive frames, stream rejected");
				return CLV_AUDIO_STREAM_REJECTED;
			}
			return CLV_AUDIO_FIELD_MISMATCH;
		}

		std::vector<uint8_t> pcm;
		uint32_t real_blocksize = 0u;
		uint16_t bits = 0u;
		uint16_t channels = 0u;
		if (! d->codec.DecodeFrame(payload, size, &pcm, &real_blocksize, &bits, &channels))
		{
			/* 帧头过了但对端解不出来 = 这一帧不成立，同样按丢帧处置，不判整条流。 */
			d->stats.frames_dropped_header++;
			return CLV_AUDIO_FRAME_HEADER_BAD;
		}
		if (bits != d->params.bit_depth || channels != d->params.channels)
		{
			d->stats.frames_dropped_mismatch++;
			LogWarn("clv audio: decoded frame contradicts stream declaration, frame dropped");
			const uint32_t mismatched =
				(bits != d->params.bit_depth ? 4u : 0u) | (channels != d->params.channels ? 2u : 0u);
			if (RunsExhausted(d, mismatched))
			{
				d->rejected = true;
				LogError("clv audio: stream declaration contradicted by consecutive frames, stream rejected");
				return CLV_AUDIO_STREAM_REJECTED;
			}
			return CLV_AUDIO_FIELD_MISMATCH;
		}
		/* 本帧字段全部对上：连续失配段就此打断，三个计数归零。 */
		d->run_sample_rate = 0u;
		d->run_channels = 0u;
		d->run_bit_depth = 0u;

		auto* b = new (std::nothrow) Buffer();
		if (b == nullptr) return CLV_AUDIO_OUT_OF_MEMORY;
		b->pcm = std::move(pcm);
		b->desc.format = CLV_AUDIO_FMT_INTERLEAVED;
		b->desc.sample_rate = d->params.sample_rate;
		b->desc.channels = d->params.channels;
		b->desc.bit_depth = d->params.bit_depth;
		b->desc.bytes_per_sample = BytesPerSample(d->params.bit_depth);
		b->desc.samples_per_channel = real_blocksize;
		b->desc.data = b->pcm.data();
		b->desc.size = b->pcm.size();
		*out = reinterpret_cast<CLV_AudioBuffer*>(b);
		*got = 1u;

		d->stats.frames_ok++;
		if (info != nullptr)
		{
			info->blocksize = real_blocksize;
			info->first_sample = d->next_sample;
			/* dts / pts / timebase 不经本口进来：时间基由容器层给出，本层不填。 */
		}
		d->next_sample += real_blocksize;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// PCM 缓冲与 Buffer 都在 try 里建，抛出点都在 *out 交出之前：出参回到入口的清零态
		if (out != nullptr) *out = nullptr;
		if (got != nullptr) *got = 0u;
		if (info != nullptr) std::memset(info, 0, sizeof(*info));
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioDecoderAddMetaBlock(CLV_AudioDecoder* d, const uint8_t* ext_payload, size_t size)
{
	try
	{
		if (d == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		if (d->rejected) return CLV_AUDIO_STREAM_REJECTED;	  // 与视频侧拒解态同形：拒解态下不收承载块
		if (ext_payload == nullptr || size == 0u) return CLV_AUDIO_INVALID_ARGUMENT;

		uint8_t type = 0u;
		const uint8_t* data = nullptr;
		size_t data_size = 0u;
		if (! clv::audio::SplitMetaBlock(ext_payload, size, &type, &data, &data_size))
		{
			/* 拆分只在载荷为空或零长时失败，那两档本入口已在上面拒掉，这一支实际到不了。
			 * 它是兜底档，不属于「流信息混进承载块」那一档，所以不记 blocks_rejected_streaminfo——
			 * 那个计数只记分类出的类型码 = 0（下面 kRejectStreamInfo 一支）。 */
			return CLV_AUDIO_METADATA_BAD;
		}
		switch (clv::audio::ClassifyMetaBlock(type))
		{
		case clv::audio::MetaAction::kStore:
		{
			/* 注释块进门前先尽力剥掉 WAVEFORMATEXTENSIBLE_CHANNEL_MASK：剥掉几条记进 mask_keys_removed；
			 * 剥不动（注释块结构本身不成立）就原样收录、照常继续——承载不留副本，字节本来由容器读方搬运；
			 * 内容不成立这件事由无状态解读口如实报 METADATA_MALFORMED，两条通道同判、不互相遮掩。 */
			if (type == 4u)
			{
				std::vector<uint8_t> stripped;
				uint32_t removed = 0u;
				if (clv::audio::StripChannelMask(data, data_size, &stripped, &removed))
					d->stats.mask_keys_removed += removed;
			}
			d->stats.blocks_stored++;
			return CLV_AUDIO_OK;
		}
		case clv::audio::MetaAction::kDropPadding: d->stats.blocks_dropped_padding++; return CLV_AUDIO_OK;
		case clv::audio::MetaAction::kRejectStreamInfo:
			d->stats.blocks_rejected_streaminfo++;
			return CLV_AUDIO_METADATA_BAD;
		case clv::audio::MetaAction::kSkipUnknown:
			/* 未定义块类型：warn + 跳过 + 计数，不判损坏。 */
			d->stats.blocks_skipped_unknown++;
			LogWarn("clv audio: unsupported metadata block type, block skipped");
			return CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED;
		}
		return CLV_AUDIO_METADATA_BAD;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioDecoderReset(CLV_AudioDecoder* d)
{
	try
	{
		if (d == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		d->codec.Reset();
		d->next_sample = 0u;
		d->rejected = false;	// 拒解态的唯一出口（与视频侧同形），故本入口不被拒解态拦截
		/* 连续失配段随在途状态一起清：复位后从无历史重新计。 */
		d->run_sample_rate = 0u;
		d->run_channels = 0u;
		d->run_bit_depth = 0u;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

/* 观测口：不被拒解态拦截 —— 拒解态本身就是从这里读的（`stream_rejected`）。 */
CLV_AudioError CLV_AudioDecoderStats(const CLV_AudioDecoder* d, CLV_AudioStatsC* out)
{
	try
	{
		if (out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		if (d == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		*out = d->stats;
		out->stream_rejected = d->rejected ? 1u : 0u;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

void CLV_AudioDecoderClose(CLV_AudioDecoder* d) { delete d; }

CLV_AudioEncoder* CLV_AudioEncoderOpen(const CLV_AudioEncParamsC* params, CLV_AudioError* err)
{
	/* 句柄指针提到 try 之外只为让 catch 能回收它：e 只在最后一行才交出，进了 catch 就说明句柄没交出去，
	 * delete 一次与下面 Open 返回 false 那一档同法，不会和调用方重复持有。 */
	CLV_AudioEncoder* e = nullptr;
	try
	{
		if (err != nullptr) *err = CLV_AUDIO_OK;
		if (params == nullptr)
		{
			if (err != nullptr) *err = CLV_AUDIO_INVALID_ARGUMENT;
			return nullptr;
		}
		/* 布局自报在先：非 0 又不等于本版本 sizeof 的对象一律显式拒，库不去猜它的字段落在哪。 */
		if (params->struct_size != 0u && params->struct_size != sizeof(CLV_AudioEncParamsC))
		{
			if (err != nullptr) *err = CLV_AUDIO_VALUE_RANGE;
			return nullptr;
		}
		if (params->sample_rate == 0u || params->channels == 0u || params->channels > 8u || params->reserved != 0u)
		{
			if (err != nullptr) *err = CLV_AUDIO_VALUE_RANGE;
			return nullptr;
		}
		if (params->bit_depth != 16u && params->bit_depth != 24u)
		{
			if (err != nullptr) *err = CLV_AUDIO_VALUE_RANGE;
			return nullptr;
		}
		/* 块大小的两道界都判在落后端之前：放到 libFLAC 里只会以 BACKEND_FAILURE 收场，
		 * 调用方分不出「参数不合法」与「后端故障」，而这两个码在公开头里是分开承诺的。
		 * 下界：16-bit 最小块 64、24-bit 最小块 72（界值即 frame.h 的具名常量，此处不另抄一份）；
		 * 0 = 由库按输入样本自选，不参与判界。
		 * 上界：写方产出的流要落在流式子集内——块大小 ≤ 16384，且采样率 ≤ 48000 时还须 ≤ 4608
		 *   （两值即 kSubsetMaxBlocksize / kSubsetMaxBlocksizeAtLowRate，判式见 StreamableSubsetOk）。
		 * 块大小上界只在写方按子集收：读方接受域在这一条上宽于子集，越上界的 lax 流照常解码，
		 * 拿上界去卡流信息会误拒这一类流。位深集合、采样率上界与块大小下界不同档——那是流信息
		 * 核对入口的值域判点（见 audio 层 ParseStreamInfo），公开头「字段越出子集就返错误码」由这三组守住。 */
		const uint32_t floor_bs =
			params->bit_depth == 16u ? clv::audio::kSubsetMinBlocksize16 : clv::audio::kSubsetMinBlocksize24;
		if (params->blocksize != 0u && params->blocksize < floor_bs)
		{
			if (err != nullptr) *err = CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE;
			return nullptr;
		}
		if (params->blocksize != 0u && ! clv::audio::StreamableSubsetOk(params->blocksize, params->sample_rate))
		{
			if (err != nullptr) *err = CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE;
			return nullptr;
		}
		if (params->level > 8u)
		{
			if (err != nullptr) *err = CLV_AUDIO_VALUE_RANGE;
			return nullptr;
		}

		e = new (std::nothrow) CLV_AudioEncoder();
		if (e == nullptr)
		{
			if (err != nullptr) *err = CLV_AUDIO_OUT_OF_MEMORY;
			return nullptr;
		}
		e->params = *params;
		if (! e->codec.Open(params->sample_rate, params->channels, params->bit_depth, params->blocksize, params->level))
		{
			const bool oom = e->codec.oom();	// 句柄 / 后端分配失败与其余后端故障分开报
			delete e;
			if (err != nullptr) *err = oom ? CLV_AUDIO_OUT_OF_MEMORY : CLV_AUDIO_BACKEND_FAILURE;
			return nullptr;
		}
		return e;
	}
	catch (const std::bad_alloc&)
	{
		// 落后端 Open 时的库内分配失败：句柄没交出去就回收，*err 置 OOM 后返 NULL
		delete e;
		if (err != nullptr) *err = CLV_AUDIO_OUT_OF_MEMORY;
		return nullptr;
	}
}

CLV_AudioError CLV_AudioEncoderWrite(CLV_AudioEncoder* e, const uint8_t* pcm, size_t size, uint32_t samples_per_channel)
{
	try
	{
		if (e == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		if (e->flushed) return CLV_AUDIO_STATE_ERROR;
		if (pcm == nullptr || size == 0u || samples_per_channel == 0u) return CLV_AUDIO_INVALID_ARGUMENT;
		const size_t bps = BytesPerSample(e->params.bit_depth);
		if (size != static_cast<size_t>(samples_per_channel) * e->params.channels * bps) return CLV_AUDIO_VALUE_RANGE;
		if (! e->codec.Write(pcm, size, samples_per_channel)) return CLV_AUDIO_BACKEND_FAILURE;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioEncoderNextFrame(CLV_AudioEncoder* e, const uint8_t** data, size_t* size, uint8_t* got)
{
	try
	{
		if (data != nullptr) *data = nullptr;
		if (size != nullptr) *size = 0u;
		if (got != nullptr) *got = 0u;
		if (e == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		if (data == nullptr || size == nullptr || got == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;

		if (! e->codec.NextFrame(&e->current)) return CLV_AUDIO_OK;	   // 没凑满一帧不是错误
		*data = e->current.data();
		*size = e->current.size();
		*got = 1u;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// 交帧的 vector 填充在 try 里：抛出点都在出参交出之前，三个出参回到入口的清零态
		if (data != nullptr) *data = nullptr;
		if (size != nullptr) *size = 0u;
		if (got != nullptr) *got = 0u;
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioEncoderFlush(CLV_AudioEncoder* e)
{
	try
	{
		if (e == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		if (e->flushed) return CLV_AUDIO_STATE_ERROR;
		if (! e->codec.Flush()) return CLV_AUDIO_BACKEND_FAILURE;
		e->flushed = true;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

CLV_AudioError CLV_AudioEncoderTakeStreamInfo(CLV_AudioEncoder* e, CLV_AudioBlob** out)
{
	try
	{
		if (out != nullptr) *out = nullptr;
		if (e == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		if (out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		if (! e->flushed) return CLV_AUDIO_STATE_ERROR;

		std::vector<uint8_t> si;
		/* 零样本时块大小与帧长统计全为 0，BuildStreamInfo 在零长判点就失败——公开头据此写明「须先喂过样本」。 */
		if (! clv::audio::BuildStreamInfo(e->codec.min_blocksize(), e->codec.max_blocksize(), e->codec.min_framesize(),
										  e->codec.max_framesize(), e->params.sample_rate, e->params.channels,
										  e->params.bit_depth, e->codec.total_samples(), &si))
		{
			return CLV_AUDIO_STATE_ERROR;
		}

		auto* blob = new (std::nothrow) Blob();
		if (blob == nullptr) return CLV_AUDIO_OUT_OF_MEMORY;
		/* MD5 留零：编码侧关掉了对端 MD5 计算，该字段恒为全零；读方按「MD5 全零 = 未给」处置。 */
		blob->data = std::move(si);
		*out = reinterpret_cast<CLV_AudioBlob*>(blob);
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		if (out != nullptr) *out = nullptr;	   // *out 只在最后一行交出，这里回到入口的清空态
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

void CLV_AudioEncoderClose(CLV_AudioEncoder* e) { delete e; }

uint32_t CLV_AudioBufferAddRef(CLV_AudioBuffer* b)
{
	try
	{
		if (b == nullptr) return 0u;
		auto* buf = reinterpret_cast<Buffer*>(b);
		return ++buf->refs;
	}
	catch (const std::bad_alloc&)
	{
		return 0u;	  // 计数口没有 OOM 码可给，按空句柄同档返 0
	}
}

void CLV_AudioBufferRelease(CLV_AudioBuffer* b)
{
	if (b == nullptr) return;
	auto* buf = reinterpret_cast<Buffer*>(b);
	// 计数已归零的对象再 Release 不生效：不回收、也不回绕成一个巨大的计数
	if (buf->refs == 0u) return;
	if (--buf->refs == 0u) delete buf;
}

CLV_AudioError CLV_AudioBufferGetDesc(const CLV_AudioBuffer* b, CLV_AudioBufferDescC* out)
{
	try
	{
		if (b == nullptr) return CLV_AUDIO_INVALID_HANDLE;
		if (out == nullptr) return CLV_AUDIO_INVALID_ARGUMENT;
		*out = reinterpret_cast<const Buffer*>(b)->desc;
		return CLV_AUDIO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_AUDIO_OUT_OF_MEMORY;
	}
}

/* StrError 一族不在收口口径内：它只做码 → 字符串字面量的映射，全程不分配，没有抛出源。
 * 给它套 catch 反而要为「OOM 时返回哪个串」编一个公开头里没承诺的值，故整族保持原样。 */
const char* CLV_AudioStrError(CLV_AudioError err)
{
	switch (err)
	{
	case CLV_AUDIO_OK: return "ok";
	case CLV_AUDIO_INVALID_HANDLE: return "invalid handle";
	case CLV_AUDIO_INVALID_ARGUMENT: return "invalid argument";
	case CLV_AUDIO_VALUE_RANGE: return "value range";
	case CLV_AUDIO_FRAME_HEADER_BAD: return "frame header";
	case CLV_AUDIO_FIELD_MISMATCH: return "field mismatch";
	case CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE: return "blocksize range";
	case CLV_AUDIO_STREAMINFO_ABSENT: return "stream info";
	case CLV_AUDIO_METADATA_BAD: return "metadata block";
	case CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED: return "block type";
	case CLV_AUDIO_TOO_LARGE: return "too large";
	case CLV_AUDIO_STATE_ERROR: return "state error";
	case CLV_AUDIO_OUT_OF_MEMORY: return "out of memory";
	case CLV_AUDIO_BACKEND_FAILURE: return "backend failure";
	case CLV_AUDIO_STREAM_REJECTED: return "stream rejected";
	case CLV_AUDIO_METADATA_MALFORMED: return "metadata malformed";
	case CLV_AUDIO_COMMENT_KEY_NOT_FOUND: return "comment key not found";
	}
	return "unknown";
}

