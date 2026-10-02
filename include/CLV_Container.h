/* CLV_Container.h
 *
 * 容器读写面的 C ABI 契约。**v0 实验性**：本头的形状（函数名、参数结构体、出参形态）在容器层
 * 开发期内可整体改；ABI 冻结后错误码只在本枚举尾部追加，既有码值语义不动。
 *
 * 面：不透明句柄 + 自由函数 + 整型错误码。写方一路 AddStream / AddExtBlock / WriteFrame 到 Finish；
 * 读方 NextFrame 拿重组好的整帧。句柄不跨线程使用，跨实例并发语义未定义。
 *
 * 错误码：容器面是**独立枚举** CLV_ContainerError，0 恒为成功。它与 CLV_File.h 的 CLV_FileError
 * 是两套编号，**不得跨头比对数值**——两边的 1 分别是「无效句柄」与「打开失败」，语义无关。
 * 文件面失败在容器口统一收敛为 CLV_CONTAINER_IO_FAILED，不回传文件面的码值。
 *
 * 时间轴：WriteFrame 收的是该流**原始时间轴上的绝对 dts / pts**，允许为负（B 帧前瞻）。
 * 写方按首帧 dts 把整条流平移到 0，读方不承担平移；pts_delta 恒等于「本帧 PTS - 本帧 DTS」，与平移量无关。
 * WriteFrame 必须按解码序（dts 非降）调用；乱序返回 CLV_CONTAINER_INVALID_ARGUMENT，不写坏文件。
 *
 * 帧大小：单帧载荷超过该流 max_packet_size 直接拒（CLV_CONTAINER_TOO_LARGE）；
 * 超过 fragment_chunk_size 时写方自动切分片，每片都自带完整的包外层头。
 *
 * 读方容错：CRC 不过 / 字段区不成立 / 片序跳变都是丢包或丢帧 + 计数 + 继续，
 * 只有包头不成立才进重同步；任何情况下都不判整文件损坏。
 * 观测口是 CLV_ContainerReaderStats，丢包与重同步计数都在那里。
 *
 * 载荷有效期：CLV_ContainerReaderNextFrame 出的 payload 指向句柄内部缓冲，
 * 只到下一次 NextFrame 或 Close 为止，要留内容自己拷。出参指针可为 NULL，表示不要该信息。
 *
 * 单文件上限 4 GiB（索引 file_offset 是 u32）：越限时写入返回 CLV_CONTAINER_TOO_LARGE，
 * 分段容器不在本期范围。
 */
#ifndef CLV_CONTAINER_H
#define CLV_CONTAINER_H

#include "CLV_Export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct CLV_ContainerWriter CLV_ContainerWriter;
	typedef struct CLV_ContainerReader CLV_ContainerReader;

	/* 值序即 ABI：与 src/container/status.h 的 ContainerErr 逐项同序，门面层直接转值，无映射表。
	 * 因此改序、插位、删位都是破坏性变更，新码只能追加在末尾。 */
	typedef enum CLV_ContainerError
	{
		CLV_CONTAINER_OK = 0,
		CLV_CONTAINER_INVALID_HANDLE,
		CLV_CONTAINER_INVALID_ARGUMENT,
		CLV_CONTAINER_IO_FAILED,
		CLV_CONTAINER_BAD_STRUCTURE,
		CLV_CONTAINER_VALUE_RANGE,
		CLV_CONTAINER_TOO_LARGE,
		CLV_CONTAINER_STREAM_NOT_FOUND,
		CLV_CONTAINER_STATE_ERROR,
		CLV_CONTAINER_OUT_OF_MEMORY,
		CLV_CONTAINER_UNSUPPORTED
	} CLV_ContainerError;

	/* 流描述符的线上字段一一对应。
	 * timebase_num / timebase_den 线上各 8 字节，但值域限 u32 非零；first_dts 在 v1 恒 0。 */
	typedef struct CLV_StreamDescC
	{
		uint8_t stream_id;
		uint8_t stream_type;	// 0 = 视频, 1 = 音频, 2 = 字幕
		uint8_t codec_id;
		uint8_t codec_version;
		uint64_t timebase_num;
		uint64_t timebase_den;
		uint32_t max_packet_size;	 // payload 字节上限，不含包外层
		uint64_t first_dts;
		uint64_t language;
		uint32_t sample_rate;
		uint16_t channels;
		uint16_t bit_depth;
		uint8_t codec_flags_0;
		uint8_t vlc_table_id;
		uint8_t codec_flags_1;
		uint8_t layer_id;
	} CLV_StreamDescC;

	typedef struct CLV_WriteParams
	{
		uint16_t version_minor;			 // v1 = 0
		uint8_t index_present;			 // 0 / 1
		uint8_t globally_sorted;		 // 0 / 1，只声明包序，不影响索引排序
		uint8_t streaming;				 // 0 / 1：1 时文件头 total_packets 写未知值
		uint32_t fragment_chunk_size;	 // 0 = 整帧一包
	} CLV_WriteParams;

	typedef struct CLV_FrameC
	{
		uint8_t stream_id;
		uint8_t is_keyframe;
		uint8_t complete;	   // 0 = 有片缺失或没等到末片
		uint64_t dts_delta;	   // 相对同流上一帧的增量；跨帧的绝对 dts 累加由调用方做
		int64_t pts_delta;
		uint64_t fragments_received;
		uint64_t fragments_expected;	// 0 = 末片未见，总片数未知
		const uint8_t* payload;
		size_t payload_size;
	} CLV_FrameC;

	typedef struct CLV_ReaderStatsC
	{
		uint64_t packets_ok;
		uint64_t dropped_crc;
		uint64_t dropped_field;
		uint64_t dropped_too_large;
		uint64_t dropped_unknown_stream;
		uint64_t resync_count;
		uint64_t resync_bytes_scanned;
		uint64_t resync_failed;
		uint64_t frames_ok;
		uint64_t frames_incomplete;
		uint64_t fragment_gaps;
		uint64_t missing_first_packets;
		uint64_t duplicate_fragments;
		uint64_t ext_blocks_bad;
		uint8_t truncated;
		uint8_t abandoned;
	} CLV_ReaderStatsC;

	/* 文件头的读回视图。flags：bit0 index_present / bit1 globally_sorted / bit2 streaming */
	typedef struct CLV_FileHeadC
	{
		uint16_t version_major;
		uint16_t version_minor;
		uint8_t stream_count;
		uint8_t flags;
		uint64_t duration_ticks;
		uint64_t total_packets;
		uint64_t index_offset;
	} CLV_FileHeadC;

	/* 索引条目。file_offset 是该帧首包 total_size 字段的绝对偏移 */
	typedef struct CLV_IndexEntryC
	{
		uint8_t stream_id;
		uint8_t is_keyframe;
		uint32_t file_offset;
		uint64_t dts;
	} CLV_IndexEntryC;

	/* 扩展块的读回视图。data 指向句柄内部缓冲，有效期到 Close */
	typedef struct CLV_ExtBlockC
	{
		uint8_t stream_id;
		uint8_t ext_type;
		uint8_t ext_version;
		uint64_t offset;
		const uint8_t* data;
		size_t size;
	} CLV_ExtBlockC;

	/* ---- 写方 ---- */

	/* 建文件并写占位头。params 可为 NULL（取 v1 默认：有索引、不分片、非流式）。
	 * 失败返回 NULL；err 出参可为 NULL。 */
	CLV_API CLV_ContainerWriter* CLV_ContainerWriterOpenFile(const char* path, const CLV_WriteParams* params,
															 CLV_ContainerError* err);

	/* 全部 AddStream / AddExtBlock 必须在第一帧之前。 */
	CLV_API CLV_ContainerError CLV_ContainerWriterAddStream(CLV_ContainerWriter* w, const CLV_StreamDescC* desc);

	/* 扩展块挂在指定流名下。ext_type / ext_version 是该流媒体层与调用方之间的约定，
	 * 容器层只搬运与校验 CRC，不解释载荷。 */
	CLV_API CLV_ContainerError CLV_ContainerWriterAddExtBlock(CLV_ContainerWriter* w, uint8_t stream_id,
															  uint8_t ext_type, uint8_t ext_version, const void* data,
															  size_t size);

	CLV_API CLV_ContainerError CLV_ContainerWriterWriteFrame(CLV_ContainerWriter* w, uint8_t stream_id,
															 uint8_t is_keyframe, int64_t dts, int64_t pts,
															 const void* payload, size_t size);

	/* 写索引区与文件尾并回填文件头。packets_out 可为 NULL。Finish 后句柄不可再写，只能 Close。 */
	CLV_API CLV_ContainerError CLV_ContainerWriterFinish(CLV_ContainerWriter* w, uint64_t* packets_out);

	CLV_API void CLV_ContainerWriterClose(CLV_ContainerWriter* w);

	/* ---- 读方 ---- */

	CLV_API CLV_ContainerReader* CLV_ContainerReaderOpenFile(const char* path, CLV_ContainerError* err);

	CLV_API CLV_ContainerError CLV_ContainerReaderStreamCount(const CLV_ContainerReader* r, uint32_t* out);
	CLV_API CLV_ContainerError CLV_ContainerReaderGetStream(const CLV_ContainerReader* r, uint32_t index,
															CLV_StreamDescC* out, uint8_t* usable);

	/* 取下一帧：有帧时 *got_frame = 1 且 *out 有效；包区走完时 *got_frame = 0（不视为错误）。
	 * got_frame 可为 NULL。句柄内部推进，取走的帧载荷在下一次调用前有效。 */
	CLV_API CLV_ContainerError CLV_ContainerReaderNextFrame(CLV_ContainerReader* r, CLV_FrameC* out,
															uint8_t* got_frame);

	CLV_API CLV_ContainerError CLV_ContainerReaderStats(const CLV_ContainerReader* r, CLV_ReaderStatsC* out);
	CLV_API CLV_ContainerError CLV_ContainerReaderHasIndex(const CLV_ContainerReader* r, uint8_t* out);
	CLV_API CLV_ContainerError CLV_ContainerReaderIndexCount(const CLV_ContainerReader* r, uint32_t* out);

	/* 读回视图：文件头字段、索引条目、扩展块。都是只读，句柄存活期内稳定 */
	CLV_API CLV_ContainerError CLV_ContainerReaderGetHead(const CLV_ContainerReader* r, CLV_FileHeadC* out);
	CLV_API CLV_ContainerError CLV_ContainerReaderGetIndexEntry(const CLV_ContainerReader* r, uint32_t index,
																CLV_IndexEntryC* out);
	CLV_API CLV_ContainerError CLV_ContainerReaderExtBlockCount(const CLV_ContainerReader* r, uint32_t* out);
	CLV_API CLV_ContainerError CLV_ContainerReaderGetExtBlock(const CLV_ContainerReader* r, uint32_t index,
															  CLV_ExtBlockC* out);

	CLV_API void CLV_ContainerReaderClose(CLV_ContainerReader* r);

	/* 每个枚举值都返回非空、静态存储期的 ASCII 串；未知值返回 "unknown"。 */
	CLV_API const char* CLV_ContainerStrError(CLV_ContainerError err);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_CONTAINER_H
