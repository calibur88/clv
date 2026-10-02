/* clv_container.cpp
 *
 * 容器面 C ABI 的门面：把公开文件面（CLV_File）适配成容器层的字节源/汇，
 * 再把 container 的 C++ 类型包成不透明句柄。这里不放任何格式判断——判定全在 container 里。
 *
 * 句柄与 CLV_File 同法：公开名只是标签，实体是内部类型，靠 reinterpret_cast 互转。
 */
#include "CLV_Container.h"

#include "CLV_File.h"
#include "reader.h"
#include "writer.h"

#include <memory>
#include <new>

namespace
{

	using clv::container::ByteSinkIf;
	using clv::container::ByteSourceIf;
	using clv::container::ContainerErr;
	using clv::container::ContainerReader;
	using clv::container::ContainerWriter;
	using clv::container::FrameInput;
	using clv::container::ReaderStats;
	using clv::container::ReassembledFrame;
	using clv::container::StreamDesc;
	using clv::container::WriterConfig;
	using clv::container::WriteSummary;

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
		CLV_File* file = nullptr;
		FileSource src;
		std::unique_ptr<ContainerReader> r;
		ReassembledFrame current;
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
		}
		self->w.reset(new (std::nothrow) ContainerWriter(self->sink, self->cfg));
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
		return ToApi(h->w->AddStream(ToDesc(*desc)));
	}

	CLV_ContainerError CLV_ContainerWriterAddExtBlock(CLV_ContainerWriter* w, uint8_t stream_id, uint8_t ext_type,
													  uint8_t ext_version, const void* data, size_t size)
	{
		WriterHandle* h = AsWriter(w);
		if (h == nullptr || ! h->w) return CLV_CONTAINER_INVALID_HANDLE;
		return ToApi(h->w->AddExtBlock(stream_id, ext_type, ext_version, static_cast<const uint8_t*>(data), size));
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
		return ToApi(h->w->WriteFrame(in));
	}

	CLV_ContainerError CLV_ContainerWriterFinish(CLV_ContainerWriter* w, uint64_t* packets_out)
	{
		WriterHandle* h = AsWriter(w);
		if (h == nullptr || ! h->w) return CLV_CONTAINER_INVALID_HANDLE;

		WriteSummary sum;
		const ContainerErr e = h->w->Finish(&sum);
		if (e != ContainerErr::Ok) return ToApi(e);
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

		CLV_FileError fe = CLV_FILE_OK;
		CLV_File* h = CLV_OpenFileEx(path, CLV_FILE_MODE_READ, &fe);
		if (h == nullptr) return fail(CLV_CONTAINER_IO_FAILED);

		ReaderHandle* self = new (std::nothrow) ReaderHandle();
		if (self == nullptr)
		{
			CLV_CloseFile(h);
			return fail(CLV_CONTAINER_OUT_OF_MEMORY);
		}
		self->file = h;
		self->src.Init(h);
		self->r.reset(new (std::nothrow) ContainerReader(self->src));
		if (! self->r)
		{
			CLV_CloseFile(self->file);
			delete self;
			return fail(CLV_CONTAINER_OUT_OF_MEMORY);
		}

		const ContainerErr e = self->r->Open();
		if (e != ContainerErr::Ok)
		{
			CLV_CloseFile(self->file);
			delete self;
			return fail(ToApi(e));
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
		if (usable != nullptr) *usable = h->r->StreamUsable(descs[index].stream_id) ? 1u : 0u;
		return CLV_CONTAINER_OK;
	}

	CLV_ContainerError CLV_ContainerReaderNextFrame(CLV_ContainerReader* r, CLV_FrameC* out, uint8_t* got_frame)
	{
		ReaderHandle* h = AsReader(r);
		if (h == nullptr || ! h->r) return CLV_CONTAINER_INVALID_HANDLE;
		if (out == nullptr) return CLV_CONTAINER_INVALID_ARGUMENT;

		ReassembledFrame f;
		if (! h->r->NextFrame(&f))
		{
			h->current = ReassembledFrame {};
			*out = CLV_FrameC {};
			if (got_frame != nullptr) *got_frame = 0;
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

	void CLV_ContainerReaderClose(CLV_ContainerReader* r)
	{
		ReaderHandle* h = AsReader(r);
		if (h == nullptr) return;
		CLV_CloseFile(h->file);
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
		default: return "unknown";
		}
	}

}	 // extern "C"
