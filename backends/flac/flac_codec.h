/* FLAC 帧的编解码桥：CLV 音频层里唯一碰 libFLAC 的地方。
 *
 * 两侧的接口形状都是「一次一帧」，与 CLV 的「一包一帧」口径对齐：
 *   编码 = 喂交错 PCM，产出若干完整 FLAC 帧（libFLAC 每编满一块给一帧）；
 *   解码 = 喂一帧载荷，产出该帧的交错 PCM。
 * 交错 PCM 的字节口径：小端、二进制补码，bytes_per_sample = 2（16-bit）/ 3（24-bit）。
 * 流式子集的块大小上下界与采样率拒档的具名常量不在本头：全部归 src/audio/frame.h 一处，适配层只引用那里。
 */
#ifndef CLV_AUDIO_FLAC_CODEC_H
#define CLV_AUDIO_FLAC_CODEC_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace audio
	{

		/* 交错 PCM 字节 ↔ libFLAC 的逐声道 int32（值按最低位对齐、带符号）。 */
		bool PcmToPlanar(const uint8_t* pcm, size_t bytes, uint32_t samples_per_channel, uint16_t channels,
						 uint16_t bytes_per_sample, std::vector<std::vector<int32_t>>* planes);
		bool PlanarToPcm(const int32_t* const* planes, uint32_t blocksize, uint16_t channels, uint16_t bytes_per_sample,
						 std::vector<uint8_t>* pcm);

		/* 编码侧。生命周期：Open → Write*（可多次）→ Flush（尾块编完，此后拒收样本）→ 取完帧 → 析构。
		 * 帧队列与流信息都由本对象持有；NextFrame 弹出即为拉取式（拉取式）。 */
		class FlacEncoder
		{
			public:

			FlacEncoder();
			~FlacEncoder();

			FlacEncoder(const FlacEncoder&) = delete;
			FlacEncoder& operator=(const FlacEncoder&) = delete;

			/* blocksize = 0 → 由库按素材自选；低于分档下界的值不归本层折档——块大小的界判只在门面一处，
			 * 门面「填 0 = 自选、其余值先判界再转来」是唯一口径。level 是压缩档（0..8，越界由调用方先判）。 */
			bool Open(uint32_t sample_rate, uint16_t channels, uint16_t bit_depth, uint16_t blocksize, uint8_t level);
			bool Write(const uint8_t* pcm, size_t bytes, uint32_t samples_per_channel);
			bool Flush();

			bool NextFrame(std::vector<uint8_t>* out);

			/* 由 libFLAC 的写回调调用：收一帧并累计块大小 / 帧长统计。 */
			void ObserveFrame(const uint8_t* bytes, size_t size, uint32_t samples_per_channel);

			uint64_t total_samples() const { return total_samples_; }

			uint32_t min_framesize() const { return min_framesize_; }

			uint32_t max_framesize() const { return max_framesize_; }

			uint16_t min_blocksize() const { return min_blocksize_; }

			uint16_t max_blocksize() const { return max_blocksize_; }

			/* 句柄或后端编码器分配失败（OOM）时为真；门面据此映射到 CLV_AUDIO_OUT_OF_MEMORY。 */
			bool oom() const { return oom_; }

			private:

			struct Handle;
			Handle* h_ = nullptr;
			std::vector<std::vector<uint8_t>> frames_;
			size_t next_ = 0u;
			uint64_t total_samples_ = 0u;
			uint32_t min_framesize_ = 0u;
			uint32_t max_framesize_ = 0u;
			uint16_t min_blocksize_ = 0u;
			uint16_t max_blocksize_ = 0u;
			bool flushed_ = false;
			bool oom_ = false;
		};

		/* 解码侧。一个实例服务一条流；Reset 清在途状态、不清统计。 */
		class FlacDecoder
		{
			public:

			FlacDecoder();
			~FlacDecoder();

			/* 解码回调是自由函数，只把 Handle 指针当 client_data 传，故 Handle 必须公开。 */
			struct Handle;

			FlacDecoder(const FlacDecoder&) = delete;
			FlacDecoder& operator=(const FlacDecoder&) = delete;

			/* 一帧完整载荷 → 交错 PCM。返回 false = 这帧没解出来（帧不成立 / 后端故障），
			 * 调用方按「丢这帧 + 计数」处置，不判整条流。 */
			bool DecodeFrame(const uint8_t* payload, size_t size, std::vector<uint8_t>* pcm, uint32_t* blocksize,
							 uint16_t* bit_depth, uint16_t* channels);
			void Reset();

			/* 后端解码器是否就绪：Handle 或 FLAC__StreamDecoder 分配失败时为假。
			 * 门面据此把 OOM 映射到 CLV_AUDIO_OUT_OF_MEMORY。 */
			bool Ok() const;

			private:

			Handle* h_ = nullptr;
		};

		/* 34 字节 STREAMINFO 载荷的拼装：字段由 FlacEncoder 的实测统计给出。
		 * total_samples 未知填全 1（FLAC 的「未知」编码）；MD5 由编码侧填零。 */
		bool BuildStreamInfo(uint16_t min_blocksize, uint16_t max_blocksize, uint32_t min_framesize,
							 uint32_t max_framesize, uint32_t sample_rate, uint16_t channels, uint16_t bit_depth,
							 uint64_t total_samples, std::vector<uint8_t>* out);

	}	 // namespace audio
}	 // namespace clv

#endif	  // CLV_AUDIO_FLAC_CODEC_H

