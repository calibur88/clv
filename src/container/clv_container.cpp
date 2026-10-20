/* clv_container.cpp
 *
 * 容器面 C ABI 的门面：把公开文件面（CLV_File）适配成容器层的字节源/汇，
 * 再把 container 的 C++ 类型包成不透明句柄。这里不放任何格式判断——判定全在 container 里。
 *
 * 句柄与 CLV_File 同法：公开名只是标签，实体是内部类型，靠 reinterpret_cast 互转。
 *
 * 异常不出 C 边界：本层唯一的抛出源是分配（容器内部用 std::vector / std::string），
 * 每个可能分配的入口都在这一层收口成公开码 CLV_CONTAINER_OUT_OF_MEMORY；指针口返回 NULL，
 * 且返回前把已建的对象与已开的文件收干净，不留半开句柄。
 */
#include "CLV_Container.h"

#include "CLV_File.h"
#include "reader.h"
#include "writer.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace
{

	using clv::container::ByteSinkIf;
	using clv::container::ByteSourceIf;
	using clv::container::ChunkFrameInput;
	using clv::container::ChunkInput;
	using clv::container::ContainerErr;
	using clv::container::ContainerReader;
	using clv::container::ContainerWriter;
	using clv::container::ExtRecord;
	using clv::container::FrameInput;
	using clv::container::kUnknownSegmentTotal;
	using clv::container::ReaderStats;
	using clv::container::ReassembledFrame;
	using clv::container::SegBlock;
	using clv::container::StreamDesc;
	using clv::container::WriterConfig;
	using clv::container::WriteSummary;

	/* 公开枚举与内部枚举逐项同序，门面直接转值。顺序错位在这里编译期就断掉。
	 * 内部面只到 kStateError，故下面两项没有内部对应项、也不在下面钉序号：
	 * CLV_CONTAINER_OUT_OF_MEMORY 由门面自己在各入口把分配失败收口成这个公开码；
	 * CLV_CONTAINER_UNSUPPORTED 门面与容器实现层都不产出，全库只剩 StrError 的文案分支认它
	 * （文件面不可用不走这一码，走内部项 kIoFailed 收敛）。 */
	static_assert(static_cast<int>(ContainerErr::kOk) == static_cast<int>(CLV_CONTAINER_OK), "ok order");
	static_assert(static_cast<int>(ContainerErr::kInvalidHandle) == static_cast<int>(CLV_CONTAINER_INVALID_HANDLE),
				  "handle order");
	static_assert(static_cast<int>(ContainerErr::kInvalidArgument) == static_cast<int>(CLV_CONTAINER_INVALID_ARGUMENT),
				  "argument order");
	static_assert(static_cast<int>(ContainerErr::kIoFailed) == static_cast<int>(CLV_CONTAINER_IO_FAILED), "io order");
	static_assert(static_cast<int>(ContainerErr::kBadStructure) == static_cast<int>(CLV_CONTAINER_BAD_STRUCTURE),
				  "structure order");
	static_assert(static_cast<int>(ContainerErr::kValueRange) == static_cast<int>(CLV_CONTAINER_VALUE_RANGE),
				  "range order");
	static_assert(static_cast<int>(ContainerErr::kTooLarge) == static_cast<int>(CLV_CONTAINER_TOO_LARGE),
				  "large order");
	static_assert(static_cast<int>(ContainerErr::kStreamNotFound) == static_cast<int>(CLV_CONTAINER_STREAM_NOT_FOUND),
				  "not found order");
	// 内部尾项与公开尾项的序号刻意不同：各自钉一次绝对位置，谁在中间插位都会在这里编译失败。
	static_assert(static_cast<int>(ContainerErr::kExtChainTooLong) == 9, "internal ext-chain code ABI position");
	static_assert(static_cast<int>(CLV_CONTAINER_EXT_CHAIN_TOO_LONG) == 11, "public ext-chain code ABI position");
	static_assert(static_cast<int>(ContainerErr::kStateError) == static_cast<int>(CLV_CONTAINER_STATE_ERROR),
				  "state order");
	// 分段两码只在公开枚举末尾追加，内部面没有对应项：门面直接给公开码，位置在这里钉住。
	static_assert(static_cast<int>(CLV_CONTAINER_SEGMENT_MISMATCH) == 12, "segment mismatch ABI position");
	static_assert(static_cast<int>(CLV_CONTAINER_SEGMENT_ORDER) == 13, "segment order ABI position");

	// 位置自己记账：容器写方只在收尾回跳一次改写文件头，其余一律顺序追加
	class FileSink final: public ByteSinkIf
	{
		public:

		void Init(CLV_File* h) noexcept
		{
			h_ = h;
			pos_ = 0;
		}

		bool Write(const uint8_t* data, size_t n) override
		{
			size_t written = 0;
			if (CLV_WriteFile(h_, data, n, &written) != CLV_FILE_OK || written != n) return false;
			pos_ += written;
			return true;
		}

		bool Seek(uint64_t pos) override
		{
			if (CLV_SeekFile(h_, pos, nullptr) != CLV_FILE_OK) return false;
			pos_ = pos;
			return true;
		}

		uint64_t Tell() override { return pos_; }

		bool Flush() { return CLV_FlushFile(h_) == CLV_FILE_OK; }

		private:

		CLV_File* h_ = nullptr;
		uint64_t pos_ = 0;
	};

	class FileSource final: public ByteSourceIf
	{
		public:

		void Init(CLV_File* h) noexcept { h_ = h; }

		bool Seek(uint64_t pos) override { return CLV_SeekFile(h_, pos, nullptr) == CLV_FILE_OK; }

		bool Read(uint8_t* data, size_t n, size_t* got) override
		{
			return CLV_ReadFile(h_, data, n, got) == CLV_FILE_OK;
		}

		uint64_t Size() override
		{
			uint64_t size = 0;
			if (CLV_FileSize(h_, &size) != CLV_FILE_OK) return 0;
			return size;
		}

		private:

		CLV_File* h_ = nullptr;
	};

	struct WriterHandle
	{
		CLV_File* file = nullptr;
		FileSink sink;
		WriterConfig cfg;
		std::unique_ptr<ContainerWriter> w;
	};

	struct ReaderHandle
	{
		/* 当前段的三件套。ContainerReader 持字节源的引用，所以源必须比 reader 活得久且地址稳定：
		 * 每段的源都堆分配、留在句柄里直到 Close，轮换段时从不搬动被引用的对象。 */
		std::vector<std::unique_ptr<FileSource>> srcs;
		CLV_File* file = nullptr;
		std::unique_ptr<ContainerReader> r;
		ReassembledFrame current;

		// 段清单（走 OpenFile 时 paths 为空、auto_advance 为假，单段语义与改动前一致）
		std::vector<std::string> paths;
		size_t cursor = 0;	  // 当前段在清单里的下标
		bool auto_advance = false;
		std::vector<StreamDesc> baseline;	 // 首段的描述符表，用作各段一致性的比对基准

		// 观测结转：段的 reader 被拆掉前把它的段计数并进来，否则清单走完后就查不到前几段的数了
		ContainerReader::SegmentStats acc;
		uint64_t segments_read = 0;
		uint64_t segments_missing = 0;
		uint64_t segments_mismatched = 0;	 // 描述符与首段对不上而整段拒接的段数
		uint64_t segments_reordered = 0;	 // 段块 seg_index 与清单位置不符、按头里段号记账的段数
		uint64_t last_seg_index = 0;
		/* 段间扩展块一致性的主动查询读数（本版不据此拒接任何段，只供调用方查）。
		 * scanned 记**扫过的段数（含基准段）**，0 = 从没查过 ⇒ 不会被读成"查了且一致"。 */
		uint64_t segments_ext_scanned = 0;
		uint64_t segments_ext_inconsistent = 0;
		// 最后一个接缝事件的档位，清单走完那一刻由 NextFrame 回报；句柄存活期内不清
		bool seam_mismatch = false;
		bool seam_order = false;
	};

	inline WriterHandle* AsWriter(CLV_ContainerWriter* h) noexcept { return reinterpret_cast<WriterHandle*>(h); }

	inline ReaderHandle* AsReader(CLV_ContainerReader* h) noexcept { return reinterpret_cast<ReaderHandle*>(h); }

	inline const ReaderHandle* AsReader(const CLV_ContainerReader* h) noexcept
	{
		return reinterpret_cast<const ReaderHandle*>(h);
	}

	inline CLV_ContainerWriter* WrapWriter(WriterHandle* p) noexcept
	{
		return reinterpret_cast<CLV_ContainerWriter*>(p);
	}

	inline CLV_ContainerReader* WrapReader(ReaderHandle* p) noexcept
	{
		return reinterpret_cast<CLV_ContainerReader*>(p);
	}

	inline CLV_ContainerError ToApi(ContainerErr e) noexcept
	{
		// 内部 0..8 与公开 0..8 逐项同序，直接转值；内部尾项 kExtChainTooLong 落在公开枚举 UNSUPPORTED
		// 之后一位（公开侧只能在末尾追加），两者序号对不上，只能在这里显式映射。
		if (e == ContainerErr::kExtChainTooLong) return CLV_CONTAINER_EXT_CHAIN_TOO_LONG;
		return static_cast<CLV_ContainerError>(static_cast<int>(e));
	}

	inline StreamDesc ToDesc(const CLV_StreamDescC& c) noexcept
	{
		StreamDesc d;
		d.stream_id = c.stream_id;
		d.stream_type = c.stream_type;
		d.codec_id = c.codec_id;
		d.codec_version = c.codec_version;
		d.timebase_num = c.timebase_num;
		d.timebase_den = c.timebase_den;
		d.max_packet_size = c.max_packet_size;
		d.first_dts = c.first_dts;
		d.language = c.language;
		d.sample_rate = c.sample_rate;
		d.channels = c.channels;
		d.bit_depth = c.bit_depth;
		d.codec_flags_0 = c.codec_flags_0;
		d.vlc_table_id = c.vlc_table_id;
		// ext_offset 是布局算出来的，不由调用方给，故公开面没有这个字段
		d.ext_offset = 0;
		d.codec_flags_1 = c.codec_flags_1;
		d.layer_id = c.layer_id;
		return d;
	}

	inline void FromDesc(const StreamDesc& d, CLV_StreamDescC* out) noexcept
	{
		out->stream_id = d.stream_id;
		out->stream_type = d.stream_type;
		out->codec_id = d.codec_id;
		out->codec_version = d.codec_version;
		out->timebase_num = d.timebase_num;
		out->timebase_den = d.timebase_den;
		out->max_packet_size = d.max_packet_size;
		out->first_dts = d.first_dts;
		out->language = d.language;
		out->sample_rate = d.sample_rate;
		out->channels = d.channels;
		out->bit_depth = d.bit_depth;
		out->codec_flags_0 = d.codec_flags_0;
		out->vlc_table_id = d.vlc_table_id;
		out->codec_flags_1 = d.codec_flags_1;
		out->layer_id = d.layer_id;
	}

	/* ---- 分段门面 ----
	 * 一个句柄 = 一条段清单。段界落在帧边界上，所以续读一段只是「拆掉这一段的 reader、装上下一段的
	 * reader」：不存在跨段重组，也不引入跨段字节源。清单外的入口（OpenFile）语义一字不动。
	 * 接缝上的失败只计数不接入：拿不出可解析结构的段计 missing，描述符对不上的段整段拒接、
	 * 连它的包都不试解。 */

	/* 建文件参数的向后承接：本结构在尾部追加了分段身份一段，只报旧尺寸的递交照样受理，
	 * 但拿不到那一段 ⇒ 按不分段处理，既不会把新字段当垃圾读，也不会悄悄产出自称分段的文件。 */
	constexpr uint32_t kWriteParamsSizeV1 = static_cast<uint32_t>(offsetof(CLV_WriteParams, segmented));
	constexpr uint32_t kWriteParamsSizeV2 = static_cast<uint32_t>(sizeof(CLV_WriteParams));

	/* 各段流描述符的逐字段比对。允许取不同的只有 first_dts（本段该流首帧的绝对 dts）与
	 * ext_offset（本文件内的偏移），其余任一项对不上就判这一段不可接入。 */
	bool SameDesc(const StreamDesc& a, const StreamDesc& b) noexcept
	{
		return a.stream_id == b.stream_id && a.stream_type == b.stream_type && a.codec_id == b.codec_id &&
			   a.codec_version == b.codec_version && a.timebase_num == b.timebase_num &&
			   a.timebase_den == b.timebase_den && a.max_packet_size == b.max_packet_size && a.language == b.language &&
			   a.sample_rate == b.sample_rate && a.channels == b.channels && a.bit_depth == b.bit_depth &&
			   a.codec_flags_0 == b.codec_flags_0 && a.vlc_table_id == b.vlc_table_id &&
			   a.codec_flags_1 == b.codec_flags_1 && a.layer_id == b.layer_id;
	}

	bool SameDescTable(const std::vector<StreamDesc>& a, const std::vector<StreamDesc>& b) noexcept
	{
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (! SameDesc(a[i], b[i])) return false;
		return true;
	}

	/* 段间扩展块比对用的最小形状：键 = (流号, 块类型)，值 = (版本号, 载荷字节)。
	 * 描述符表里没有画面尺寸（尺寸只在 ext_type = 3 的参数块里），所以 SameDesc 那套段身份对账
	 * 结构上碰不到它 —— 这一族比对是补在扩展块这一层的，不替代描述符对账。 */
	struct ExtFingerprint
	{
		uint8_t stream_id = 0;
		uint8_t type = 0;
		uint8_t version = 0;
		std::vector<uint8_t> data;
	};

	void CollectExtFingerprints(const ContainerReader& rd, std::vector<ExtFingerprint>& out_v)
	{
		out_v.clear();
		for (const ExtRecord& e: rd.ExtBlocks())
		{
			ExtFingerprint f;
			f.stream_id = e.stream_id;
			f.type = e.type;
			f.version = e.version;
			f.data = e.data;
			out_v.push_back(std::move(f));
		}
	}

	/* 本段与基准段之间有没有不一致；首处不一致只在第一次命中时登记，之后只累计数。
	 * 键在同一段里按出现序配对（同 (流号, 类型) 出现多条时逐条对齐，不多不少都算缺块）。 */
	bool CompareExtFingerprints(const std::vector<ExtFingerprint>& base, const std::vector<ExtFingerprint>& cur,
								uint64_t seg_index, CLV_ReaderSegExtCheckC& res)
	{
		std::vector<char> matched(cur.size(), 0);
		bool bad = false;
		auto note = [&](uint8_t reason, uint8_t stream_id, uint8_t type)
		{
			if (res.first_diff_reason != 0u) return;
			res.first_diff_reason = reason;
			res.first_diff_seg_index = seg_index;
			res.first_diff_stream_id = stream_id;
			res.first_diff_ext_type = type;
		};
		for (const ExtFingerprint& b: base)
		{
			size_t hit = cur.size();
			for (size_t i = 0; i < cur.size(); ++i)
			{
				if (matched[i] == 0 && cur[i].stream_id == b.stream_id && cur[i].type == b.type)
				{
					hit = i;
					break;
				}
			}
			if (hit == cur.size())
			{
				++res.blocks_missing;
				note(2, b.stream_id, b.type);
				bad = true;
				continue;
			}
			matched[hit] = 1;
			++res.blocks_compared;
			if (cur[hit].version != b.version || cur[hit].data != b.data)
			{
				++res.blocks_differing;
				note(1, b.stream_id, b.type);
				bad = true;
			}
		}
		for (size_t i = 0; i < cur.size(); ++i)
		{
			if (matched[i] != 0u) continue;
			++res.blocks_missing;
			note(2, cur[i].stream_id, cur[i].type);
			bad = true;
		}
		return bad;
	}

	/* 开一个文件并把结构解析跑完，产物交给调用方接管；失败路径在这里自己收干净，一个对象都不留。
	 * 分配失败按 std::bad_alloc 抛出，由各入口收口成 CLV_CONTAINER_OUT_OF_MEMORY。 */
	ContainerErr ProbeSegment(const char* path, CLV_File** file, std::unique_ptr<FileSource>* src,
							  std::unique_ptr<ContainerReader>* reader)
	{
		CLV_FileError fe = CLV_FILE_OK;
		CLV_File* h = CLV_OpenFileEx(path, CLV_FILE_MODE_READ, &fe);
		if (h == nullptr) return ContainerErr::kIoFailed;
		std::unique_ptr<FileSource> s(new (std::nothrow) FileSource());
		if (s == nullptr)
		{
			CLV_CloseFile(h);
			throw std::bad_alloc();
		}
		s->Init(h);
		std::unique_ptr<ContainerReader> r(new (std::nothrow) ContainerReader(*s));
		if (r == nullptr)
		{
			CLV_CloseFile(h);
			throw std::bad_alloc();
		}
		const ContainerErr e = r->Open();
		if (e != ContainerErr::kOk)
		{
			CLV_CloseFile(h);
			return e;
		}
		*file = h;
		src->reset(s.release());
		reader->reset(r.release());
		return ContainerErr::kOk;
	}

	// 拆掉当前段：段观测先结转再销毁 reader，然后关文件（源对象留着不回收，它的地址不能作古）
	// 段观测四档的结转动作：拆段与被拒段共用，保证「结转过」这件事只有一条路径。
	void FoldSegmentStats(ReaderHandle* h, const ContainerReader::SegmentStats& s) noexcept
	{
		h->acc.block_missing += s.block_missing;
		h->acc.block_bad += s.block_bad;
		h->acc.discontinuities += s.discontinuities;
		h->acc.video_stream_no_param_block += s.video_stream_no_param_block;
	}

	void RetireSegment(ReaderHandle* h) noexcept
	{
		if (h->r != nullptr)
		{
			FoldSegmentStats(h, h->r->SegStats());
			h->r.reset();
		}
		if (h->file != nullptr)
		{
			CLV_CloseFile(h->file);
			h->file = nullptr;
		}
	}

	/* 装上一段：旧段先拆，新段的源落进句柄的持有表。首段也走这里，所以句柄只有一条装载路径。 */
	void CommitSegment(ReaderHandle* h, CLV_File* file, std::unique_ptr<FileSource>&& src,
					   std::unique_ptr<ContainerReader>&& reader)
	{
		RetireSegment(h);
		h->file = file;
		h->srcs.push_back(std::move(src));
		h->r = std::move(reader);
	}

	/* 段号与清单位置的核对：以段块里的 seg_index 为权威（清单序与文件名只是线索），
	 * 对上就记段号，对不上不改接、只记事件 —— 段号错位不判损坏。 */
	void NoteSegIdentity(ReaderHandle* h, const SegBlock* sb, size_t position) noexcept
	{
		if (sb == nullptr) return;
		h->last_seg_index = sb->index;
		if (sb->index != static_cast<uint64_t>(position))
		{
			h->segments_reordered++;
			h->seam_order = true;
		}
	}

	/* 清单往后走：自 cursor+1 起逐条试开，接不上的整段不接入、只计数并把游标让过去。
	 * 返回 true = 已装上新段（调用方重取一帧），false = 清单走完。 */
	bool AdvanceSegment(ReaderHandle* h)
	{
		for (size_t next = h->cursor + 1u; next < h->paths.size(); ++next)
		{
			h->cursor = next;
			CLV_File* file = nullptr;
			std::unique_ptr<FileSource> src;
			std::unique_ptr<ContainerReader> reader;
			const ContainerErr e = ProbeSegment(h->paths[next].c_str(), &file, &src, &reader);
			if (e != ContainerErr::kOk)
			{
				h->segments_missing++;	  // 这一段拿不出可解析的结构，按缺段跳过
				continue;
			}
			if (! SameDescTable(h->baseline, reader->Descs()))
			{
				// 被拒的段也要把它自己的段观测数结转进来：否则「因为缺参数块而被拒」只存在于探测的那一瞬间，
				// 事后查 video_stream_no_param_block 会读成 0 —— 看得到拒了、看不出为什么拒。只补可归因：
				// 接不上的段照样不接，返回码、帧数、segments_mismatched 一律不变。
				FoldSegmentStats(h, reader->SegStats());
				CLV_CloseFile(file);
				h->segments_mismatched++;
				h->seam_mismatch = true;
				continue;
			}
			CommitSegment(h, file, std::move(src), std::move(reader));
			h->segments_read++;
			NoteSegIdentity(h, h->r->Seg(), next);
			return true;
		}
		return false;
	}

}	 // namespace

extern "C"
{

	CLV_ContainerWriter* CLV_ContainerWriterOpenFile(const char* path, const CLV_WriteParams* params,
													 CLV_ContainerError* err)
	{
		auto fail = [err](CLV_ContainerError code) -> CLV_ContainerWriter*
		{
			if (err != nullptr) *err = code;
			return nullptr;
		};

		/* 布局自报判在开文件之前：递交的对象不是本版本布局就一个字节都不落盘。 */
		if (params != nullptr && params->struct_size != 0u && params->struct_size != kWriteParamsSizeV1 &&
			params->struct_size != kWriteParamsSizeV2)
		{
			return fail(CLV_CONTAINER_INVALID_ARGUMENT);
		}

		CLV_FileError fe = CLV_FILE_OK;
		CLV_File* h = CLV_OpenFileEx(path, CLV_FILE_MODE_WRITE, &fe);
		if (h == nullptr) return fail(CLV_CONTAINER_IO_FAILED);

		WriterHandle* self = new (std::nothrow) WriterHandle();
		if (self == nullptr)
		{
			CLV_CloseFile(h);
			return fail(CLV_CONTAINER_OUT_OF_MEMORY);
		}
		self->file = h;
		self->sink.Init(h);
		if (params != nullptr)
		{
			self->cfg.version_minor = params->version_minor;
			self->cfg.index_present = params->index_present != 0;
			self->cfg.globally_sorted = params->globally_sorted != 0;
			self->cfg.streaming = params->streaming != 0;
			self->cfg.fragment_chunk_size = params->fragment_chunk_size;
			// 分段身份只在递交了本尺寸的调用方那里才读得到；旧尺寸按不分段处理
			if (params->struct_size == kWriteParamsSizeV2)
			{
				self->cfg.segmented = params->segmented != 0;
				self->cfg.seg.flags = params->seg_flags;
				self->cfg.seg.index = params->seg_index;
				self->cfg.seg.total = params->seg_total;
				self->cfg.seg.start_ticks = params->seg_start_ticks;
				self->cfg.seg.end_ticks = params->seg_end_ticks;
			}
		}
		try
		{
			self->w.reset(new (std::nothrow) ContainerWriter(self->sink, self->cfg));
		}
		catch (const std::bad_alloc&)
		{
			self->w.reset();
		}
		if (! self->w)
		{
			CLV_CloseFile(self->file);
			delete self;
			return fail(CLV_CONTAINER_OUT_OF_MEMORY);
		}

		if (err != nullptr) *err = CLV_CONTAINER_OK;
		return WrapWriter(self);
	}

	CLV_ContainerError CLV_ContainerWriterAddStream(CLV_ContainerWriter* w, const CLV_StreamDescC* desc)
	{
		WriterHandle* h = AsWriter(w);
		if (h == nullptr || ! h->w) return CLV_CONTAINER_INVALID_HANDLE;
		if (desc == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		try
		{
			return ToApi(h->w->AddStream(ToDesc(*desc)));
		}
		catch (const std::bad_alloc&)
		{
			return CLV_CONTAINER_OUT_OF_MEMORY;
		}
	}

	CLV_ContainerError CLV_ContainerWriterAddExtBlock(CLV_ContainerWriter* w, uint8_t stream_id, uint8_t ext_type,
													  uint8_t ext_version, const void* data, size_t size)
	{
		WriterHandle* h = AsWriter(w);
		if (h == nullptr || ! h->w) return CLV_CONTAINER_INVALID_HANDLE;
		try
		{
			return ToApi(h->w->AddExtBlock(stream_id, ext_type, ext_version, static_cast<const uint8_t*>(data), size));
		}
		catch (const std::bad_alloc&)
		{
			return CLV_CONTAINER_OUT_OF_MEMORY;
		}
	}

	CLV_ContainerError CLV_ContainerWriterWriteFrame(CLV_ContainerWriter* w, uint8_t stream_id, uint8_t is_keyframe,
													 int64_t dts, int64_t pts, const void* payload, size_t size)
	{
		WriterHandle* h = AsWriter(w);
		if (h == nullptr || ! h->w) return CLV_CONTAINER_INVALID_HANDLE;
		if (size != 0 && payload == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;

		FrameInput in;
		in.stream_id = stream_id;
		in.is_keyframe = is_keyframe != 0;
		in.dts = dts;
		in.pts = pts;
		in.payload = static_cast<const uint8_t*>(payload);
		in.payload_size = size;
		try
		{
			return ToApi(h->w->WriteFrame(in));
		}
		catch (const std::bad_alloc&)
		{
			return CLV_CONTAINER_OUT_OF_MEMORY;
		}
	}

	CLV_ContainerError CLV_ContainerWriterWriteFrameChunks(CLV_ContainerWriter* w, uint8_t stream_id,
														   const CLV_Chunk* chunks, uint32_t chunk_count, int64_t dts,
														   int64_t pts, uint8_t flags)
	{
		WriterHandle* h = AsWriter(w);
		if (h == nullptr || ! h->w) return CLV_CONTAINER_INVALID_HANDLE;
		if (chunk_count == 0 || chunks == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		// 本版只用 bit0 = is_keyframe，bit1-7 必须为 0：非 0 的递交当场拒，不静默丢掉调用方的位
		if ((flags & 0xFEu) != 0u) return CLV_CONTAINER_INVALID_ARGUMENT;

		std::vector<ChunkInput> prepared;
		try
		{
			prepared.resize(chunk_count);
			for (uint32_t i = 0; i < chunk_count; ++i)
			{
				prepared[i].data = static_cast<const uint8_t*>(chunks[i].data);
				prepared[i].size = chunks[i].size;
			}
		}
		catch (const std::bad_alloc&)
		{
			return CLV_CONTAINER_OUT_OF_MEMORY;
		}

		ChunkFrameInput in;
		in.stream_id = stream_id;
		in.is_keyframe = (flags & 1u) != 0;
		in.dts = dts;
		in.pts = pts;
		in.chunks = prepared.data();
		in.chunk_count = chunk_count;
		try
		{
			return ToApi(h->w->WriteFrameChunks(in));
		}
		catch (const std::bad_alloc&)
		{
			return CLV_CONTAINER_OUT_OF_MEMORY;
		}
	}

	CLV_ContainerError CLV_ContainerWriterFinish(CLV_ContainerWriter* w, uint64_t* packets_out)
	{
		WriterHandle* h = AsWriter(w);
		if (h == nullptr || ! h->w) return CLV_CONTAINER_INVALID_HANDLE;

		WriteSummary sum;
		ContainerErr e = ContainerErr::kOk;
		try
		{
			e = h->w->Finish(&sum);
		}
		catch (const std::bad_alloc&)
		{
			return CLV_CONTAINER_OUT_OF_MEMORY;
		}
		if (e != ContainerErr::kOk) return ToApi(e);
		if (! h->sink.Flush()) return CLV_CONTAINER_IO_FAILED;
		if (packets_out != nullptr) *packets_out = sum.packets;
		return CLV_CONTAINER_OK;
	}

	void CLV_ContainerWriterClose(CLV_ContainerWriter* w)
	{
		WriterHandle* h = AsWriter(w);
		if (h == nullptr) return;
		CLV_CloseFile(h->file);
		delete h;
	}

	CLV_ContainerReader* CLV_ContainerReaderOpenFile(const char* path, CLV_ContainerError* err)
	{
		auto fail = [err](CLV_ContainerError code) -> CLV_ContainerReader*
		{
			if (err != nullptr) *err = code;
			return nullptr;
		};

		ReaderHandle* self = new (std::nothrow) ReaderHandle();
		if (self == nullptr) return fail(CLV_CONTAINER_OUT_OF_MEMORY);

		CLV_File* file = nullptr;
		std::unique_ptr<FileSource> src;
		std::unique_ptr<ContainerReader> reader;
		ContainerErr e = ContainerErr::kOk;
		try
		{
			e = ProbeSegment(path, &file, &src, &reader);
			if (e == ContainerErr::kOk)
			{
				CommitSegment(self, file, std::move(src), std::move(reader));
				self->baseline = self->r->Descs();
				self->cursor = 0;
				self->segments_read = 1;
				const SegBlock* sb = self->r->Seg();
				if (sb != nullptr) self->last_seg_index = sb->index;	// 单段视野：不比对清单位置
			}
		}
		catch (const std::bad_alloc&)
		{
			RetireSegment(self);
			delete self;
			return fail(CLV_CONTAINER_OUT_OF_MEMORY);
		}
		if (e != ContainerErr::kOk)
		{
			delete self;
			return fail(ToApi(e));
		}

		if (err != nullptr) *err = CLV_CONTAINER_OK;
		return WrapReader(self);
	}

	CLV_ContainerReader* CLV_ContainerReaderOpenSegments(const char* const* paths, uint32_t path_count,
														 uint8_t auto_advance, CLV_ContainerError* err)
	{
		auto fail = [err](CLV_ContainerError code) -> CLV_ContainerReader*
		{
			if (err != nullptr) *err = code;
			return nullptr;
		};

		if (paths == nullptr || path_count == 0u) return fail(CLV_CONTAINER_INVALID_ARGUMENT);
		for (uint32_t i = 0; i < path_count; ++i)
			if (paths[i] == nullptr) return fail(CLV_CONTAINER_INVALID_ARGUMENT);

		ReaderHandle* self = new (std::nothrow) ReaderHandle();
		if (self == nullptr) return fail(CLV_CONTAINER_OUT_OF_MEMORY);
		self->auto_advance = auto_advance != 0;

		CLV_File* file = nullptr;
		std::unique_ptr<FileSource> src;
		std::unique_ptr<ContainerReader> reader;
		ContainerErr e = ContainerErr::kOk;
		try
		{
			// 清单整份抄进句柄：调用方那个数组只在这次调用期间有效
			self->paths.assign(paths, paths + path_count);
			e = ProbeSegment(self->paths[0].c_str(), &file, &src, &reader);
			if (e == ContainerErr::kOk)
			{
				CommitSegment(self, file, std::move(src), std::move(reader));
				self->baseline = self->r->Descs();
				self->cursor = 0;
				self->segments_read = 1;
				NoteSegIdentity(self, self->r->Seg(), 0);
			}
		}
		catch (const std::bad_alloc&)
		{
			RetireSegment(self);
			delete self;
			return fail(CLV_CONTAINER_OUT_OF_MEMORY);
		}
		if (e != ContainerErr::kOk)
		{
			delete self;
			return fail(ToApi(e));	  // paths[0] 打不开与 OpenFile 同档：CLV_CONTAINER_IO_FAILED
		}

		if (err != nullptr) *err = CLV_CONTAINER_OK;
		return WrapReader(self);
	}

	CLV_ContainerError CLV_ContainerReaderStreamCount(const CLV_ContainerReader* r, uint32_t* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		*out = static_cast<uint32_t>(h->r->Descs().size());
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderGetStream(const CLV_ContainerReader* r, uint32_t index, CLV_StreamDescC* out,
													uint8_t* usable)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		const std::vector<StreamDesc>& descs = h->r->Descs();
		if (index >= descs.size()) return CLV_CONTAINER_INVALID_ARGUMENT;

		FromDesc(descs[index], out);
		// Descs() 只收校验通过的条目，坏条目在 Open 就被丢弃，所以 usable 对表内每条都取到 1
		if (usable != nullptr) *usable = h->r->StreamUsable(descs[index].stream_id) ? 1u : 0u;
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderNextFrame(CLV_ContainerReader* r, CLV_FrameC* out, uint8_t* got_frame)
	{
		ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;

		ReassembledFrame f;
		bool got_one = false;
		try
		{
			/* 段清单在这里续读：本段包区走完且开着自动续接，就装下一段再来一轮。
			 * 调用方看到的仍是一条连续帧序列，帧的形状与单段完全一致。 */
			for (;;)
			{
				got_one = h->r->NextFrame(&f);
				if (got_one) break;
				if (! h->auto_advance || h->cursor + 1u >= h->paths.size()) break;
				if (! AdvanceSegment(h)) break;
			}
		}
		catch (const std::bad_alloc&)
		{
			return CLV_CONTAINER_OUT_OF_MEMORY;
		}
		if (! got_one)
		{
			h->current = ReassembledFrame {};
			*out = CLV_FrameC {};
			if (got_frame != nullptr) *got_frame = 0;
			/* 清单走完那一刻回报最后一个接缝事件：段接不上是因，取不到帧是果。
			 * 单段句柄与关了自动续接的句柄两个事件位都是假，照旧回 OK。 */
			if (h->seam_mismatch) return CLV_CONTAINER_SEGMENT_MISMATCH;
			if (h->seam_order) return CLV_CONTAINER_SEGMENT_ORDER;
			return CLV_CONTAINER_OK;
		}

		h->current = std::move(f);
		out->stream_id = h->current.stream_id;
		out->is_keyframe = h->current.is_keyframe ? 1u : 0u;
		out->complete = h->current.complete ? 1u : 0u;
		out->dts_delta = h->current.dts_delta;
		out->pts_delta = h->current.pts_delta;
		out->fragments_received = h->current.fragments_received;
		out->fragments_expected = h->current.fragments_expected;
		out->payload = h->current.payload.empty() ? nullptr : h->current.payload.data();
		out->payload_size = h->current.payload.size();
		if (got_frame != nullptr) *got_frame = 1;
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderStats(const CLV_ContainerReader* r, CLV_ReaderStatsC* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;

		const ReaderStats& s = h->r->Stats();
		out->packets_ok = s.packets_ok;
		out->dropped_crc = s.dropped_crc;
		out->dropped_field = s.dropped_field;
		out->dropped_too_large = s.dropped_too_large;
		out->dropped_unknown_stream = s.dropped_unknown_stream;
		out->resync_count = s.resync_count;
		out->resync_bytes_scanned = s.resync_bytes_scanned;
		out->resync_failed = s.resync_failed;
		out->frames_ok = s.frames_ok;
		out->frames_incomplete = s.frames_incomplete;
		out->fragment_gaps = s.fragment_gaps;
		out->missing_first_packets = s.missing_first_packets;
		out->duplicate_fragments = s.duplicate_fragments;
		out->ext_blocks_bad = s.ext_blocks_bad;
		out->truncated = s.truncated ? 1u : 0u;
		out->abandoned = s.abandoned ? 1u : 0u;
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderHasIndex(const CLV_ContainerReader* r, uint8_t* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		*out = h->r->HasIndex() ? 1u : 0u;
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderIndexCount(const CLV_ContainerReader* r, uint32_t* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		*out = h->r->IndexEntryCount();
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderGetHead(const CLV_ContainerReader* r, CLV_FileHeadC* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;

		const clv::container::FileHead& head = h->r->Head();
		out->version_major = head.version_major;
		out->version_minor = head.version_minor;
		out->stream_count = head.stream_count;
		out->flags = head.flags;
		out->duration_ticks = head.duration_ticks;
		out->total_packets = head.total_packets;
		out->index_offset = head.index_offset;
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderGetIndexEntry(const CLV_ContainerReader* r, uint32_t index,
														CLV_IndexEntryC* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;

		const std::vector<clv::container::IndexEntry>& entries = h->r->IndexEntries();
		if (index >= entries.size()) return CLV_CONTAINER_INVALID_ARGUMENT;

		const clv::container::IndexEntry& e = entries[index];
		out->stream_id = e.stream_id;
		out->is_keyframe = e.is_keyframe ? 1u : 0u;
		out->file_offset = e.file_offset;
		out->dts = e.dts;
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderExtBlockCount(const CLV_ContainerReader* r, uint32_t* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		*out = static_cast<uint32_t>(h->r->ExtBlocks().size());
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderGetExtBlock(const CLV_ContainerReader* r, uint32_t index, CLV_ExtBlockC* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;

		const std::vector<clv::container::ExtRecord>& blocks = h->r->ExtBlocks();
		if (index >= blocks.size()) return CLV_CONTAINER_INVALID_ARGUMENT;

		const clv::container::ExtRecord& b = blocks[index];
		out->stream_id = b.stream_id;
		out->ext_type = b.type;
		out->ext_version = b.version;
		out->offset = b.offset;
		out->data = b.data.empty() ? nullptr : b.data.data();
		out->size = b.data.size();
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderSegmentIndex(const CLV_ContainerReader* r, uint64_t* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		const SegBlock* sb = h->r->Seg();
		*out = sb != nullptr ? sb->index : 0u;
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderSegmentTotal(const CLV_ContainerReader* r, uint64_t* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		const SegBlock* sb = h->r->Seg();
		*out = sb != nullptr ? sb->total : kUnknownSegmentTotal;
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderSegStats(const CLV_ContainerReader* r, CLV_ReaderSegStatsC* out)
	{
		const ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;
		// 前四档 = 已走过各段结转 + 当前段现值；后七档本来就在句柄上，跨段存活期内不重置
		const ContainerReader::SegmentStats& cur = h->r->SegStats();
		out->block_missing = h->acc.block_missing + cur.block_missing;
		out->block_bad = h->acc.block_bad + cur.block_bad;
		out->discontinuities = h->acc.discontinuities + cur.discontinuities;
		out->video_stream_no_param_block = h->acc.video_stream_no_param_block + cur.video_stream_no_param_block;
		out->segments_read = h->segments_read;
		out->segments_missing = h->segments_missing;
		out->segments_mismatched = h->segments_mismatched;
		out->segments_reordered = h->segments_reordered;
		out->last_seg_index = h->last_seg_index;
		out->segments_ext_scanned = h->segments_ext_scanned;
		out->segments_ext_inconsistent = h->segments_ext_inconsistent;
		return CLV_CONTAINER_OK;
	}

	/* 段间扩展块（参数块）一致性的主动查询。基准 = 清单里第一个读得开的段，与 OpenSegments 建句柄时
	 * 定下的描述符基准同源（同一份"首段"口径，不另起一套）。比对键 = (stream_id, ext_type)，
	 * 命中的块做版本号 + 载荷的逐字节比对；容器不认识块内容，所以这里也只比字节 ——
	 * 尺寸那类语义由调用方用公开解读口（如 `CLV_VideoParseEncMeta`）自己去取。
	 * 本调用不动段游标、不回收已交出的帧、不把不一致判成拒接。 */
	CLV_ContainerError CLV_ContainerReaderSegExtCheck(CLV_ContainerReader* r, CLV_ReaderSegExtCheckC* out)
	{
		ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;

		CLV_ReaderSegExtCheckC res;
		std::memset(&res, 0, sizeof(res));
		res.first_diff_seg_index = ~0ull;

		try
		{
			std::vector<ExtFingerprint> base;
			std::vector<std::string> list = h->paths;	 // 拷一份：探测不碰句柄自己的三件套
			size_t base_at = list.size();
			if (list.empty())
			{
				// 单段句柄：基准就是自己，没有对照段；scanned = 1 的读法是"查过了"，不是"没得查"
				CollectExtFingerprints(*h->r, base);
				res.baseline_seg_index = h->r->Seg() != nullptr ? h->r->Seg()->index : 0ull;
				res.segments_scanned = 1u;
			}
			else
			{
				for (size_t i = 0; i < list.size(); ++i)
				{
					CLV_File* f = nullptr;
					std::unique_ptr<FileSource> s;
					std::unique_ptr<ContainerReader> rd;
					if (ProbeSegment(list[i].c_str(), &f, &s, &rd) != ContainerErr::kOk)
					{
						++res.segments_unreadable;
						continue;
					}
					CollectExtFingerprints(*rd, base);
					res.baseline_seg_index = rd->Seg() != nullptr ? rd->Seg()->index : 0ull;
					rd.reset();
					s.reset();
					CLV_CloseFile(f);
					base_at = i;
					res.segments_scanned = 1u;
					break;
				}
			}
			if (base_at == list.size() && res.segments_scanned == 0u) return CLV_CONTAINER_IO_FAILED;

			for (size_t i = 0; i < list.size(); ++i)
			{
				if (i == base_at) continue;
				CLV_File* f = nullptr;
				std::unique_ptr<FileSource> s;
				std::unique_ptr<ContainerReader> rd;
				if (ProbeSegment(list[i].c_str(), &f, &s, &rd) != ContainerErr::kOk)
				{
					++res.segments_unreadable;
					continue;
				}
				std::vector<ExtFingerprint> cur;
				CollectExtFingerprints(*rd, cur);
				const uint64_t seg_index = rd->Seg() != nullptr ? rd->Seg()->index : 0ull;
				rd.reset();
				s.reset();
				CLV_CloseFile(f);
				++res.segments_scanned;
				if (CompareExtFingerprints(base, cur, seg_index, res)) ++res.segments_inconsistent;
			}

			h->segments_ext_scanned = res.segments_scanned;
			h->segments_ext_inconsistent = res.segments_inconsistent;
			*out = res;
			return CLV_CONTAINER_OK;
		}
		catch (const std::bad_alloc&)
		{
			return CLV_CONTAINER_OUT_OF_MEMORY;
		}
	}

	void CLV_ContainerReaderClose(CLV_ContainerReader* r)
	{
		ReaderHandle* h = AsReader(r);
		if (h == nullptr) return;
		// 只关当前段的文件：其余段的源只占对象内存，没挂文件
		if (h->file != nullptr) CLV_CloseFile(h->file);
		delete h;
	}

	const char* CLV_ContainerStrError(CLV_ContainerError err)
	{
		switch (err)
		{
		case CLV_CONTAINER_OK: return "ok";
		case CLV_CONTAINER_INVALID_HANDLE: return "invalid handle";
		case CLV_CONTAINER_INVALID_ARGUMENT: return "invalid argument";
		case CLV_CONTAINER_IO_FAILED: return "io failed";
		case CLV_CONTAINER_BAD_STRUCTURE: return "bad structure";
		case CLV_CONTAINER_VALUE_RANGE: return "value range";
		case CLV_CONTAINER_TOO_LARGE: return "too large";
		case CLV_CONTAINER_STREAM_NOT_FOUND: return "stream not found";
		case CLV_CONTAINER_STATE_ERROR: return "state error";
		case CLV_CONTAINER_OUT_OF_MEMORY: return "out of memory";
		case CLV_CONTAINER_UNSUPPORTED: return "unsupported";
		case CLV_CONTAINER_EXT_CHAIN_TOO_LONG: return "ext chain too long";
		case CLV_CONTAINER_SEGMENT_MISMATCH: return "segment mismatch";
		case CLV_CONTAINER_SEGMENT_ORDER: return "segment order";
		default: return "unknown";
		}
	}

}	 // extern "C"
