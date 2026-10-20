/* libFLAC 桥的实现。libFLAC 头只允许出现在本目录（后端隔离），库的其余部分不得 include 它。 */
#include "backends/flac/flac_codec.h"

#include <cstring>
#include <new>

#if defined(__GNUC__) && ! defined(__clang__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#if defined(_MSC_VER)
	#pragma warning(push)
	#pragma warning(disable: 4200 4201 4324)	// libFLAC 头里的零长数组与匿名联合体
#endif
extern "C"
{
#include "FLAC/stream_decoder.h"
#include "FLAC/stream_encoder.h"
}
#if defined(_MSC_VER)
	#pragma warning(pop)
#endif
#if defined(__GNUC__) && ! defined(__clang__)
	#pragma GCC diagnostic pop
#endif

namespace clv
{
	namespace audio
	{

		namespace
		{

			/* FLAC 帧同步码：11 bit 全 1 + 2 bit 保留 = 首字节 0xFF、次字节高 5 bit = 11111。 */
			bool LooksLikeFrameStart(const FLAC__byte* b, size_t n)
			{
				return n >= 2u && b[0] == 0xFFu && (b[1] & 0xF8u) == 0xF8u;
			}

			int32_t ReadSample(const uint8_t* at, uint16_t bytes_per_sample)
			{
				if (bytes_per_sample == 2u)
				{
					const uint32_t raw = static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8);
					return static_cast<int32_t>(static_cast<int16_t>(raw));
				}
				const uint32_t raw = static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
									 (static_cast<uint32_t>(at[2]) << 16);
				return static_cast<int32_t>(raw << 8) >> 8;	   // 24 bit 符号扩展
			}

			void WriteSample(uint8_t* at, int32_t v, uint16_t bytes_per_sample)
			{
				const uint32_t u = static_cast<uint32_t>(v);
				at[0] = static_cast<uint8_t>(u & 0xFFu);
				at[1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
				if (bytes_per_sample == 3u) at[2] = static_cast<uint8_t>((u >> 16) & 0xFFu);
			}

			/* 位流写入器：STREAMINFO 前 18 字节里有 20 / 3 / 5 / 36 bit 的混合字段。
			 * 每次写前核对是否越出可写区，越界返回 false；只保证这一条——
			 * 不校验值宽：比目标档宽的值，高位在逐位写入时被静默丢掉，宽度纪律由调用方守住。 */
			struct BitWriter
			{
				uint8_t* p = nullptr;
				size_t size = 0u;	 // 可写字节数
				size_t bits = 0u;

				bool Put(uint64_t value, size_t n)
				{
					const size_t cap_bits = size * 8u;
					if (bits > cap_bits || n > cap_bits - bits) return false;
					for (size_t i = n; i > 0u; --i)
					{
						const size_t bit = (value >> (i - 1u)) & 1u;
						if (bit != 0u) p[bits / 8u] = static_cast<uint8_t>(p[bits / 8u] | (1u << (7u - bits % 8u)));
						++bits;
					}
					return true;
				}
			};

		}	 // namespace

		bool PcmToPlanar(const uint8_t* pcm, size_t bytes, uint32_t samples_per_channel, uint16_t channels,
						 uint16_t bytes_per_sample, std::vector<std::vector<int32_t>>* planes)
		{
			if (pcm == nullptr || planes == nullptr || channels == 0u || samples_per_channel == 0u) return false;
			if (bytes_per_sample != 2u && bytes_per_sample != 3u) return false;
			const size_t want = static_cast<size_t>(samples_per_channel) * channels * bytes_per_sample;
			if (bytes != want) return false;

			planes->assign(channels, std::vector<int32_t>(samples_per_channel));
			for (uint32_t s = 0u; s < samples_per_channel; ++s)
			{
				for (uint16_t c = 0u; c < channels; ++c)
				{
					const size_t off = (static_cast<size_t>(s) * channels + c) * bytes_per_sample;
					(*planes)[c][s] = ReadSample(pcm + off, bytes_per_sample);
				}
			}
			return true;
		}

		bool PlanarToPcm(const int32_t* const* planes, uint32_t blocksize, uint16_t channels, uint16_t bytes_per_sample,
						 std::vector<uint8_t>* pcm)
		{
			if (planes == nullptr || pcm == nullptr || channels == 0u || blocksize == 0u) return false;
			if (bytes_per_sample != 2u && bytes_per_sample != 3u) return false;
			pcm->assign(static_cast<size_t>(blocksize) * channels * bytes_per_sample, 0u);
			for (uint32_t s = 0u; s < blocksize; ++s)
			{
				for (uint16_t c = 0u; c < channels; ++c)
				{
					const size_t off = (static_cast<size_t>(s) * channels + c) * bytes_per_sample;
					WriteSample(pcm->data() + off, planes[c][s], bytes_per_sample);
				}
			}
			return true;
		}

		/* ---------------- 编码 ---------------- */

		struct FlacEncoder::Handle
		{
			FLAC__StreamEncoder* enc = nullptr;
			FlacEncoder* self = nullptr;
			bool ok = false;
		};

		namespace
		{

			FLAC__StreamEncoderWriteStatus EncoderWriteCb(const FLAC__StreamEncoder*, const FLAC__byte buffer[],
														  size_t bytes, uint32_t samples, uint32_t, void* client)
			{
				auto* self = static_cast<FlacEncoder*>(client);
				/* samples == 0 表示这批字节是流头或元数据块；CLV 的 34 字节 STREAMINFO 由本层按实测统计
				 * 自己拼（容器 ext_type = 5），所以这里不收。 */
				if (samples == 0u) return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
				/* 原生 FLAC（非 Ogg）的写回调按帧对齐，samples > 0 的每批就是一整帧。
				 * 假设一旦被打破，收下等于把半帧交给容器，宁可让整条编码当场失败。 */
				if (! LooksLikeFrameStart(buffer, bytes)) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
				self->ObserveFrame(buffer, bytes, samples);
				return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
			}

			void EncoderMetaCb(const FLAC__StreamEncoder*, const FLAC__StreamMetadata*, void*)
			{
				/* 元数据由 CLV 自己按实测统计拼（ext_type = 5 的那 34 字节），这里不收。 */
			}

		}	 // namespace

		void FlacEncoder::ObserveFrame(const uint8_t* bytes, size_t size, uint32_t samples_per_channel)
		{
			std::vector<uint8_t> f(bytes, bytes + size);
			frames_.push_back(std::move(f));
			if (samples_per_channel > 0u)
			{
				total_samples_ += samples_per_channel;
				const uint32_t bs = samples_per_channel;
				if (min_blocksize_ == 0u || bs < min_blocksize_) min_blocksize_ = static_cast<uint16_t>(bs);
				if (bs > max_blocksize_) max_blocksize_ = static_cast<uint16_t>(bs);
				const uint32_t fs = static_cast<uint32_t>(size);
				if (min_framesize_ == 0u || fs < min_framesize_) min_framesize_ = fs;
				if (fs > max_framesize_) max_framesize_ = fs;
			}
		}

		FlacEncoder::FlacEncoder()
		{
			h_ = new (std::nothrow) Handle();
			if (h_ == nullptr) oom_ = true;
		}

		FlacEncoder::~FlacEncoder()
		{
			if (h_ == nullptr) return;
			if (h_->enc != nullptr)
			{
				/* Flush() 已 finish 过时这里不重复 finish；未 Flush 时把尾块收掉。 */
				if (! flushed_) FLAC__stream_encoder_finish(h_->enc);
				FLAC__stream_encoder_delete(h_->enc);
			}
			delete h_;
		}

		bool FlacEncoder::Open(uint32_t sample_rate, uint16_t channels, uint16_t bit_depth, uint16_t blocksize,
							   uint8_t level)
		{
			if (h_ == nullptr)
			{
				oom_ = true;
				return false;
			}
			// 重入守卫：已开过就拒，二次调用会把 h_->enc 覆盖掉、前一个后端句柄永不释放
			if (h_->enc != nullptr) return false;
			if (sample_rate == 0u || channels == 0u || channels > 8u) return false;
			if (bit_depth != 16u && bit_depth != 24u) return false;
			/* 块大小的分档界判只在门面一处做（越界在那里就拒）；这里不再把低于下界的值折成 0——
			 * 那种折法在门面之后到不了。填 0 = 由库按素材自选，落到下面跳过 set_blocksize。 */

			h_->enc = FLAC__stream_encoder_new();
			if (h_->enc == nullptr)
			{
				oom_ = true;
				return false;
			}
			h_->self = this;

			/* 每个 set_* 都可能返回 FLAC__false（参数越界 / 后端已改状态）：任一失败即初始化不成立，
			 * 不让后端带半套参数进入 init_stream。 */
			if (! FLAC__stream_encoder_set_sample_rate(h_->enc, sample_rate)) return false;
			if (! FLAC__stream_encoder_set_channels(h_->enc, channels)) return false;
			if (! FLAC__stream_encoder_set_bits_per_sample(h_->enc, bit_depth)) return false;
			if (blocksize != 0u && ! FLAC__stream_encoder_set_blocksize(h_->enc, blocksize)) return false;
			if (! FLAC__stream_encoder_set_compression_level(h_->enc, level)) return false;
			if (! FLAC__stream_encoder_set_do_mid_side_stereo(h_->enc, false)) return false;
			if (! FLAC__stream_encoder_set_loose_mid_side_stereo(h_->enc, false)) return false;
			if (! FLAC__stream_encoder_set_total_samples_estimate(h_->enc, 0u)) return false;
			if (! FLAC__stream_encoder_set_streamable_subset(h_->enc, true)) return false;
			if (! FLAC__stream_encoder_set_metadata(h_->enc, nullptr, 0u)) return false;

			/* 未压缩（verbatim）子帧兜底：本层不设，也无处可设——随库安装的公开头里没有对应开关。
			 * libFLAC 把 verbatim 子帧当作衡量其余压缩子帧的基线，每个子帧都必然参与比较，压缩不划算
			 * 时自然落未压缩帧；关掉它只有 libFLAC 自己注明「非随库源码不得调用」的未公开调试口，
			 * 本桥不调。容器侧 payload 上限里那点余量就成立在这条路径上：换后端版本先复核这一段。 */

			const FLAC__StreamEncoderInitStatus st =
				FLAC__stream_encoder_init_stream(h_->enc, &EncoderWriteCb, nullptr, nullptr, &EncoderMetaCb, this);
			h_->ok = (st == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
			return h_->ok;
		}

		bool FlacEncoder::Write(const uint8_t* pcm, size_t bytes, uint32_t samples_per_channel)
		{
			if (h_ == nullptr || ! h_->ok || flushed_) return false;
			std::vector<std::vector<int32_t>> planes;
			const uint16_t bps = static_cast<uint16_t>((FLAC__stream_encoder_get_bits_per_sample(h_->enc) + 7u) / 8u);
			const uint16_t ch = static_cast<uint16_t>(FLAC__stream_encoder_get_channels(h_->enc));
			if (! PcmToPlanar(pcm, bytes, samples_per_channel, ch, bps, &planes)) return false;

			std::vector<int32_t> interleaved(static_cast<size_t>(samples_per_channel) * ch);
			for (uint32_t s = 0u; s < samples_per_channel; ++s)
				for (uint16_t c = 0u; c < ch; ++c) interleaved[static_cast<size_t>(s) * ch + c] = planes[c][s];

			return FLAC__stream_encoder_process_interleaved(h_->enc, interleaved.data(), samples_per_channel) ? true :
																												false;
		}

		bool FlacEncoder::Flush()
		{
			if (h_ == nullptr || ! h_->ok || flushed_) return false;
			/* libFLAC 没有「只冲缓冲、不改状态」的接口：尾块由 finish() 编完，
			 * 同时把填好的 STREAMINFO 交给元数据回调（本层忽略，见 EncoderMetaCb）。
			 * finish 之后不能再喂样本，本对象的统计值即为全流实测值。 */
			const bool ok = FLAC__stream_encoder_finish(h_->enc) ? true : false;
			if (ok) flushed_ = true;
			return ok;
		}

		bool FlacEncoder::NextFrame(std::vector<uint8_t>* out)
		{
			if (out == nullptr) return false;
			if (next_ >= frames_.size()) return false;
			*out = frames_[next_++];
			return true;
		}

		/* ---------------- 解码 ---------------- */

		struct FlacDecoder::Handle
		{
			FLAC__StreamDecoder* dec = nullptr;
			const uint8_t* payload = nullptr;
			size_t size = 0u;
			size_t pos = 0u;
			std::vector<uint8_t>* pcm = nullptr;
			std::vector<std::vector<int32_t>> planes;
			uint32_t blocksize = 0u;
			uint16_t bits = 0u;
			uint16_t channels = 0u;
			bool wrote = false;
			bool inited = false;
		};

		namespace
		{

			FLAC__StreamDecoderReadStatus DecoderReadCb(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes,
														void* client)
			{
				auto* h = static_cast<FlacDecoder::Handle*>(client);
				if (h->payload == nullptr || h->pos >= h->size)
				{
					*bytes = 0u;
					return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
				}
				size_t n = h->size - h->pos;
				if (n > *bytes) n = *bytes;
				std::memcpy(buffer, h->payload + h->pos, n);
				h->pos += n;
				*bytes = n;
				return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
			}

			FLAC__StreamDecoderWriteStatus DecoderWriteCb(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
														  const FLAC__int32* const buffer[], void* client)
			{
				auto* h = static_cast<FlacDecoder::Handle*>(client);
				const uint16_t ch = static_cast<uint16_t>(frame->header.channels);
				h->planes.assign(ch, std::vector<int32_t>(frame->header.blocksize));
				for (uint32_t s = 0u; s < frame->header.blocksize; ++s)
					for (uint16_t c = 0u; c < ch; ++c) h->planes[c][s] = buffer[c][s];
				h->blocksize = frame->header.blocksize;
				h->bits = static_cast<uint16_t>(frame->header.bits_per_sample);
				h->channels = ch;
				std::vector<const int32_t*> ptrs(ch);
				for (uint16_t c = 0u; c < ch; ++c) ptrs[c] = h->planes[c].data();
				if (! PlanarToPcm(ptrs.data(), frame->header.blocksize, ch,
								  static_cast<uint16_t>((frame->header.bits_per_sample + 7u) / 8u), h->pcm))
				{
					return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
				}
				h->wrote = true;
				return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
			}

			/* libFLAC 的错误回调：注册它接管后端默认的错误打印。帧级失败由 process_single 的
			 * 返回值与 wrote 标志判定，故此处不落任何诊断量。 */
			void DecoderErrorCb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* /*client*/) {}

		}	 // namespace

		FlacDecoder::FlacDecoder()
		{
			h_ = new (std::nothrow) Handle();
			if (h_ == nullptr) return;
			/* dec 可能分配失败；此时 Ok() 为假，门面据此报 OOM。 */
			h_->dec = FLAC__stream_decoder_new();
		}

		bool FlacDecoder::Ok() const { return h_ != nullptr && h_->dec != nullptr; }

		FlacDecoder::~FlacDecoder()
		{
			if (h_ == nullptr) return;
			if (h_->dec != nullptr)
			{
				FLAC__stream_decoder_finish(h_->dec);
				FLAC__stream_decoder_delete(h_->dec);
			}
			delete h_;
		}

		bool FlacDecoder::DecodeFrame(const uint8_t* payload, size_t size, std::vector<uint8_t>* pcm,
									  uint32_t* blocksize, uint16_t* bit_depth, uint16_t* channels)
		{
			if (h_ == nullptr || h_->dec == nullptr || payload == nullptr || size == 0u || pcm == nullptr) return false;
			if (! LooksLikeFrameStart(payload, size)) return false;

			h_->payload = payload;
			h_->size = size;
			h_->pos = 0u;
			h_->pcm = pcm;
			h_->wrote = false;
			pcm->clear();

			if (! h_->inited)
			{
				/* 逐帧喂时 MD5 校验必然对不上（那是对整条码流的承诺），这里关掉，帧的对账由 CLV 容器层负责。 */
				FLAC__stream_decoder_set_md5_checking(h_->dec, false);
				const FLAC__StreamDecoderInitStatus st =
					FLAC__stream_decoder_init_stream(h_->dec, &DecoderReadCb, nullptr, nullptr, nullptr, nullptr,
													 &DecoderWriteCb, nullptr, &DecoderErrorCb, h_);
				h_->inited = (st == FLAC__STREAM_DECODER_INIT_STATUS_OK);
				if (! h_->inited) return false;
			}
			else
			{
				/* 读回调按解码器要的容量整批给，一帧解完时输入缓冲里常留着本帧之后的字节；
				 * 本层一包只交一帧，那些字节不能留给下一包，所以每帧开解前先冲掉输入缓冲。 */
				FLAC__stream_decoder_flush(h_->dec);
			}

			const bool ran = FLAC__stream_decoder_process_single(h_->dec) ? true : false;
			if (! ran || ! h_->wrote)
			{
				/* 后端进错误态后必须回 IDLE 才能再收帧。 */
				FLAC__stream_decoder_reset(h_->dec);
				return false;
			}
			if (blocksize != nullptr) *blocksize = h_->blocksize;
			if (bit_depth != nullptr) *bit_depth = h_->bits;
			if (channels != nullptr) *channels = h_->channels;
			return true;
		}

		void FlacDecoder::Reset()
		{
			if (h_ == nullptr || h_->dec == nullptr) return;
			FLAC__stream_decoder_reset(h_->dec);
		}

		bool BuildStreamInfo(uint16_t min_blocksize, uint16_t max_blocksize, uint32_t min_framesize,
							 uint32_t max_framesize, uint32_t sample_rate, uint16_t channels, uint16_t bit_depth,
							 uint64_t total_samples, std::vector<uint8_t>* out)
		{
			if (out == nullptr) return false;
			if (min_blocksize == 0u || max_blocksize == 0u || min_framesize == 0u || max_framesize == 0u) return false;
			if (sample_rate == 0u || channels == 0u || channels > 8u) return false;
			if (bit_depth < 4u || bit_depth > 32u) return false;

			out->assign(34u, 0u);
			(*out)[0] = static_cast<uint8_t>(min_blocksize >> 8);
			(*out)[1] = static_cast<uint8_t>(min_blocksize & 0xFFu);
			(*out)[2] = static_cast<uint8_t>(max_blocksize >> 8);
			(*out)[3] = static_cast<uint8_t>(max_blocksize & 0xFFu);
			for (int i = 0; i < 3; ++i)
				(*out)[4u + static_cast<size_t>(i)] = static_cast<uint8_t>((min_framesize >> (16 - i * 8)) & 0xFFu);
			for (int i = 0; i < 3; ++i)
				(*out)[7u + static_cast<size_t>(i)] = static_cast<uint8_t>((max_framesize >> (16 - i * 8)) & 0xFFu);

			BitWriter bw;
			bw.p = out->data() + 10u;
			bw.size = 24u;	  // 34 字节 STREAMINFO 的前 10 字节是块大小 / 帧长，可写区 = [10, 34)
			if (! bw.Put(sample_rate, 20u)) return false;
			if (! bw.Put(channels - 1u, 3u)) return false;	  // 入参是 1..8，字段存的是「声道数 − 1」
			if (! bw.Put(bit_depth - 1u, 5u)) return false;
			/* 全 1 = 「总样本数未知」，FLAC 的合法编码。 */
			if (! bw.Put(total_samples == 0u ? ((1ull << 36) - 1ull) : total_samples, 36u)) return false;
			/* 18..33 = MD5：本层不重算（编码侧关掉了 MD5），16 字节留零。 */
			return true;
		}

	}	 // namespace audio
}	 // namespace clv
