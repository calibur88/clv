/* CLV_Container.h
 *
 * 容器读写面的 C ABI 契约。函数名、参数结构体与出参形态都是对外 ABI 的一部分，改动即破坏调用方；
 * 错误码只在本枚举尾部追加，既有码值语义不动。
 *
 * 面：不透明句柄 + 自由函数 + 整型错误码。写方一路 AddStream / AddExtBlock / WriteFrame 到 Finish；
 * 读方 NextFrame 拿重组好的整帧。句柄不跨线程使用，跨实例并发语义未定义。
 *
 * 错误码：容器面是**独立枚举** CLV_ContainerError，0 恒为成功。五面的错误码各自独立枚举：文件与容器从 1 顺排、
 * 音频 32 到 63、视频 64 到 95、节拍 96 起，不得跨头比对数值——各头的 1 语义无关。
 * 本头的 1 是无效句柄，CLV_File.h 的 1 是打开失败。
 * 文件面失败在容器口统一收敛为 CLV_CONTAINER_IO_FAILED，不回传文件面的码值。
 *
 * 时间轴：WriteFrame 收的是该流**原始时间轴上的绝对 dts / pts**，允许为负（B 帧前瞻）。
 * 写方按首帧 dts 把整条流平移到 0，读方不承担平移；pts_delta 恒等于「本帧 PTS - 本帧 DTS」，与平移量无关。
 * WriteFrame 必须按解码序（dts 非降）调用；乱序返回 CLV_CONTAINER_INVALID_ARGUMENT，不写坏文件。
 *
 * 帧大小：单帧载荷超过该流 `max_packet_size` 直接拒（CLV_CONTAINER_TOO_LARGE）；
 * 超过 fragment_chunk_size 时写方自动切分片，每片都自带完整的包外层头。
 * CLV_ContainerWriterWriteFrameChunks 的分片边界由调用方给定，容器不按 fragment_chunk_size 二次切。
 * 两条写帧口还各有一道早失败判，都判在落盘之前：单包载荷不得超过读方能读回的上界（声明的
 * `max_packet_size` 越界即在 AddStream 拒收），一帧的载荷合计不得超过 1 073 741 824 字节。
 * 两档都回 CLV_CONTAINER_TOO_LARGE。
 *
 * 读方容错：CRC 不过 / 字段区不成立 / 片序跳变都是丢包或丢帧 + 计数 + 继续，
 * 只有包头不成立才进重同步；任何情况下都不判整文件损坏。
 * 观测口是 CLV_ContainerReaderStats，丢包与重同步计数都在那里。
 *
 * 载荷有效期：CLV_ContainerReaderNextFrame 出的 payload 指向句柄内部缓冲，
 * 只到下一次 NextFrame 或 Close 为止，要留内容自己拷。
 * 出参中仅辅助项（错误码 `err`、统计等）允许传 NULL 表示不要该项；帧、描述符等主出参不得为 NULL，传 NULL 判参数不成立。
 *
 * 单文件不设 4 GiB 上限：索引 `file_offset`、描述符 `ext_offset`、扩展块 `next_ext_offset` 线上均为 u64。
 *
 * 分段容器（多个物理文件拼回一条逻辑序列）**已在 v2 落地**，公开面与格式侧同时到位：
 * - 格式侧：文件头 `flags` bit3 = `segmented`，偏移 44 起的 8 字节 `file_ext_offset` 指向**文件级**扩展块链，
 *   段身份落在该链首块的 `ext_type = 7` 段描述块（载荷定长 48 字节，整块 70 = 前缀 18 + 48 + CRC 4）；
 *   `ext_type = 8`（段索引表）只留值、本版未实现。`first_dts` 在非分段文件仍钉零起点，
 *   分段容器的后续段则取本段该流首帧的**绝对 dts**（后续段不平移，段与段的条目直接可比）。
 * - 写侧：`CLV_WriteParams` 尾部六项（`segmented` / `seg_flags` / `seg_index` / `seg_total` /
 *   `seg_start_ticks` / `seg_end_ticks`），只报上一版尺寸的递交拿不到这一段 ⇒ 按不分段处理。
 * - 读侧：`CLV_ContainerReaderOpenSegments` 吃一份段清单（库不猜文件名、不列目录），`auto_advance = 1` 时
 *   段界自动续读成一条连续帧序列；段号与总段数走 `SegmentIndex` / `SegmentTotal`，
 *   段级观测走 `CLV_ReaderSegStatsC`（段标识落在文件头 CRC 覆盖之外，失效的实测主形态是静默丢失 ⇒ 必须是可查的数）。
 *   接缝上打不开的段计 `segments_missing` 跳过、描述符与前段对不上的段整段拒接（`SEGMENT_MISMATCH`），
 *   段号与清单位置不符只按头里的段号重排并回报 `SEGMENT_ORDER`，两者都不判损坏、不回收已交出的帧。
 * - 段界落在帧边界上 ⇒ 读方不存在跨段重组；一段一个编码器实例（实例边界即段边界），
 *   段首必须是 I 帧且不得跨段回引，那是视频编码侧的约束，容器层不验证、只看声明。
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

	/* 值序即 ABI：前段（0..8）与 src/container/status.h 的 ContainerErr 逐项同序，门面层直接转值，无映射表；
	 * 末尾追加的 CLV_CONTAINER_EXT_CHAIN_TOO_LONG 有内部对应项但序号对不上（内部项排在本项之前好几位），
	 * 由门面侧 ToApi() 显式映射，门面侧用 static_assert 钉住这一对。OUT_OF_MEMORY / UNSUPPORTED 无内部对应项。
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
		CLV_CONTAINER_UNSUPPORTED,
		/* 读方走某条流的扩展块链，逐块解析都成立、链路一直向前，却在跳数上限处仍没走到链尾：
		 * 判该链超出读方接受的跳数上限。此时包区起点无法确定，故不判「跳过该链继续读」，
		 * 而是整次打开失败：CLV_ContainerReaderOpenFile 返回 NULL 并把本码写入 err。
		 * 上限是读方实现的护栏常量，不是格式约束，不写进 docs/容器规范.md 正文。 */
		CLV_CONTAINER_EXT_CHAIN_TOO_LONG,
		/* 分段容器：这一段自己的流描述符与前段逐字段比不下来（各段一致性条见 `docs/容器规范.md`）。
	 * 判该段不可接入：不试解它的包，已经交出去的帧不回收，其余段照常。 */
		CLV_CONTAINER_SEGMENT_MISMATCH,
		/* 清单里给的位置与该段头里 seg_index 对不上。以头里的段号为权威放置该段（文件名只是线索），
	 * 本码走观测口回报并计数，不判损坏、不停读。 */
		CLV_CONTAINER_SEGMENT_ORDER
	} CLV_ContainerError;

	/* 流描述符的线上字段一一对应。
	 * timebase_num / timebase_den 线上各 8 字节，但值域限 u32 非零；
	 * first_dts 在非分段文件恒 0（零起点），分段容器的后续段取本段该流首帧的绝对 dts。
	 * 分段容器的首段（seg_index = 0）与非分段文件同样钉 0：这一道只在写方判，AddStream 拒非零递交，
	 * 读方不据此拒文件。 */
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

	/* 建文件参数。传 NULL 取默认（有索引、不分片、非流式、不分段）。
	 * `struct_size` = 0、上一版尺寸或 sizeof(CLV_WriteParams) 三档之一，其余值判 CLV_CONTAINER_INVALID_ARGUMENT；
	 * 两档语义与残余风险见 `CLV_Video.h` 参数结构体那条注释（三头同式）。
	 * 本结构的残余风险比另两头顶：原首字段 `version_minor` 在旧布局里合法取 0，
	 * 所以「旧布局 + 全默认值」的递交会被当成 0 而按本布局受理，这类调用方必须显式填 sizeof。 */
	typedef struct CLV_WriteParams
	{
		uint32_t struct_size;
		uint16_t version_minor;			 // 本版 = 0
		uint8_t index_present;			 // 0 / 1
		uint8_t globally_sorted;		 // 0 / 1，只声明包序，不影响索引排序
		uint8_t streaming;				 // 0 / 1：1 时文件头 total_packets 写未知值，真实包数只落文件尾
		uint32_t fragment_chunk_size;	 // 0 = 整帧一包
		/* 分段身份（v2 尾部追加）。segmented = 1 时下面这些才落字节，且必须落在合法域内
	 * （seg_total 不得为 0，未知填全 1；段描述块各字段见 `docs/容器规范.md`）。
	 * 向后承接靠 struct_size：只报旧尺寸的调用方拿不到这一段 ⇒ 写方按不分段处理，
	 * 既不会把新字段当垃圾读，也不会悄悄产出一个自称分段的文件。 */
		uint8_t segmented;	  // 0 / 1
		uint8_t seg_flags;	  // bit0 seg_total 已知 / bit1 段首帧在各视频流上都是关键帧 / bit2 与本段上一段之间有时间断
		uint64_t seg_index;			 // 自 0 起
		uint64_t seg_total;			 // 全 1 = 未知
		uint64_t seg_start_ticks;	 // 本段最早一帧的全局 tick
		uint64_t seg_end_ticks;		 // 本段最后一条包的解码序时间（按解码序，不按显示序），与总时长不同口径
	} CLV_WriteParams;

	/* CLV_ContainerWriterWriteFrameChunks 的一片输入，数组下标即片序。
	 * data 只需在调用期间有效，容器当场落盘。 */
	typedef struct CLV_Chunk
	{
		const void* data;
		size_t size;
	} CLV_Chunk;

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

	/* 分段观测口（新结构，不动 CLV_ReaderStatsC —— 它的 sizeof 是 ABI）。
	 * 段标识本身在文件头 CRC 覆盖之外，失效的实测主形态是静默丢失 ⇒ 这三类都必须是可查的数。 */
	typedef struct CLV_ReaderSegStatsC
	{
		uint64_t block_missing;					 // 声明了 segmented 却没拿到可用段描述块的次数
		uint64_t block_bad;						 // 段块读得进但解析不成立的次数
		uint64_t discontinuities;				 // 段块 bit2 声明「与上一段之间有时间断」的次数
		uint64_t video_stream_no_param_block;	 // 视频流缺 ext_type = 3 而在打开时被丢弃的条数
		uint64_t segments_read;					 // 本句柄已经走过的段数（含当前段）
		uint64_t segments_missing;				 // 清单里打不开、因而被跳过的段数
		uint64_t segments_mismatched;			 // 描述符与首段对不上而被整段拒接的段数（清单往后走时不接入该段）
		uint64_t segments_reordered;	  // 段块 seg_index 与清单位置不符、按头里段号记账的段数（不判损坏，接照常走）
		uint64_t last_seg_index;		  // 最后一次读到的段号（segments_read = 0 时为 0）
		uint64_t segments_ext_scanned;	  // 段间参数块一致性查过的段数（含基准段）；0 = 从未查，别读成"一致"
		uint64_t segments_ext_inconsistent;	   // 其中与基准段扩展块内容不一致的段数（不含基准段本身）
	} CLV_ReaderSegStatsC;

	/* 段间扩展块一致性的主动查询出参，由 `CLV_ContainerReaderSegExtCheck` 填写。
	 * 本版**不因段间参数块内容不一致而拒接**：段照交、返回码仍是 OK —— 要不要把这种不一致当错误，
	 * 由调用方按本结构的读数自己定。基准段 = 按清单里给出的位置顺序逐段试开，第一个结构读得通的段；
	 * 段号本身以段描述块里的 `seg_index` 为权威，清单顺序与文件名只是线索。 */
	typedef struct CLV_ReaderSegExtCheckC
	{
		uint32_t segments_scanned;		   // 实际打开并走过扩展链的段数（含基准段）
		uint32_t segments_unreadable;	   // 打不开或解析不成立、因而不参与比对的段数（不静默丢）
		uint64_t baseline_seg_index;	   // 作基准的那一段的段号
		uint64_t blocks_compared;		   // 按 (stream_id, ext_type) 参与逐字节比对的块条数
		uint64_t blocks_missing;		   // 一侧有该键、另一侧没有的条数
		uint64_t blocks_differing;		   // 两侧都有但版本号或载荷不同的条数
		uint64_t segments_inconsistent;	   // 至少有一处不一致的段数（不含基准段）
		uint64_t first_diff_seg_index;	   // 首处不一致落在哪一段；无不一致时为全 1
		uint8_t first_diff_stream_id;	   // 首处不一致的流号
		uint8_t first_diff_ext_type;	   // 首处不一致的扩展块类型
		uint8_t first_diff_reason;		   // 0 = 无不一致，1 = 内容不同，2 = 一侧缺该块
	} CLV_ReaderSegExtCheckC;

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
		uint64_t
			total_packets;	  // 文件头里的原值。`flags` bit2 = 1 时它是未知值，真实包数以文件尾为准；本出参透传文件头，读方不拿文件尾改写它
		uint64_t index_offset;
	} CLV_FileHeadC;

	/* 索引条目。file_offset 是该帧首包 total_size 字段的绝对偏移 */
	typedef struct CLV_IndexEntryC
	{
		uint8_t stream_id;
		uint8_t is_keyframe;
		uint64_t file_offset;
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

	/* 建文件并写占位头。params 可为 NULL（取默认：有索引、不分片、非流式、不分段）。
	 * 失败返回 NULL；err 出参可为 NULL。 */
	CLV_EXPORT CLV_ContainerWriter* CLV_ContainerWriterOpenFile(const char* path, const CLV_WriteParams* params,
																CLV_ContainerError* err);

	/* 全部 AddStream / AddExtBlock 必须在第一帧之前。 */
	CLV_EXPORT CLV_ContainerError CLV_ContainerWriterAddStream(CLV_ContainerWriter* w, const CLV_StreamDescC* desc);

	/* 扩展块挂在指定流名下。ext_type / ext_version 是该流媒体层与调用方之间的约定，
	 * 容器层只搬运与校验 CRC，不解释载荷。 */
	CLV_EXPORT CLV_ContainerError CLV_ContainerWriterAddExtBlock(CLV_ContainerWriter* w, uint8_t stream_id,
																 uint8_t ext_type, uint8_t ext_version,
																 const void* data, size_t size);

	CLV_EXPORT CLV_ContainerError CLV_ContainerWriterWriteFrame(CLV_ContainerWriter* w, uint8_t stream_id,
																uint8_t is_keyframe, int64_t dts, int64_t pts,
																const void* payload, size_t size);

	/* 按调用方给定的分片边界写一帧：chunks 的下标即片序，容器按数组顺序一包一片落盘。
	 * chunk_count = 1：单包帧路径，包外层 is_fragment = 0（不标首末片），chunks[0] 即该帧的完整载荷。
	 * chunk_count ≥ 2：容器自动置 is_fragment = 1，第 0 片为帧首包并携带 pts_delta，末片置 is_last_fragment；
	 *   同一帧各片带同一 dts_delta。片内载荷是什么结构容器不认识，也不按 fragment_chunk_size 二次切。
	 * flags：本版只用 bit0 = is_keyframe，bit1-7 必须为 0。
	 * dts / pts：该流原始时间轴上的绝对值，允许为负，语义与 WriteFrame 相同。
	 * 返回 CLV_CONTAINER_INVALID_ARGUMENT：chunks 为 NULL 且 chunk_count > 0、chunk_count 为 0、
	 *   flags 的 bit1-7 不为 0、或任一片 size 为 0 / data 为 NULL。返回 CLV_CONTAINER_TOO_LARGE：任一片超过该流 max_packet_size。
	 * 以上两类失败都在落盘之前判完，出错时一片都不写。 */
	CLV_EXPORT CLV_ContainerError CLV_ContainerWriterWriteFrameChunks(CLV_ContainerWriter* w, uint8_t stream_id,
																	  const CLV_Chunk* chunks, uint32_t chunk_count,
																	  int64_t dts, int64_t pts, uint8_t flags);

	/* 写索引区与文件尾并回填文件头。`packets_out` 回填本次写入的真实包数（同一值写进文件尾，
	 * 流式写时文件头那个字段是未知值，所以它是唯一可对账的数），可为 NULL。
	 * Finish 后句柄不可再写，只能 Close。 */
	CLV_EXPORT CLV_ContainerError CLV_ContainerWriterFinish(CLV_ContainerWriter* w, uint64_t* packets_out);

	CLV_EXPORT void CLV_ContainerWriterClose(CLV_ContainerWriter* w);

	/* ---- 读方 ---- */

	CLV_EXPORT CLV_ContainerReader* CLV_ContainerReaderOpenFile(const char* path, CLV_ContainerError* err);

	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderStreamCount(const CLV_ContainerReader* r, uint32_t* out);

	/* 只列出可用的流；描述符校验不通过的流在打开时被丢弃，不回报数量，也不占用编号。
	 * index 是可用流列表的下标（不是 stream_id），越出列表判 CLV_CONTAINER_INVALID_ARGUMENT；
	 * 表内每条都通过了校验，usable 因此恒取 1，传 NULL 表示不要该项。 */
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderGetStream(const CLV_ContainerReader* r, uint32_t index,
															   CLV_StreamDescC* out, uint8_t* usable);

	/* 取下一帧：有帧时 *got_frame = 1 且 *out 有效；包区走完时 *got_frame = 0（不视为错误）。
	 * got_frame 可为 NULL。句柄内部推进，取走的帧载荷在下一次调用前有效。
	 * 多段句柄（OpenSegments + auto_advance）下，清单走完而最后一个接缝上有事件时，返回
	 * CLV_CONTAINER_SEGMENT_MISMATCH 或 CLV_CONTAINER_SEGMENT_ORDER 且 *got_frame 仍为 0：
	 * 已经交出去的帧不回收，段级计数查 SegStats。 */
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderNextFrame(CLV_ContainerReader* r, CLV_FrameC* out,
															   uint8_t* got_frame);

	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderStats(const CLV_ContainerReader* r, CLV_ReaderStatsC* out);
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderHasIndex(const CLV_ContainerReader* r, uint8_t* out);
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderIndexCount(const CLV_ContainerReader* r, uint32_t* out);

	/* 打开一组段（分段容器的多段读）。paths[i] 按段序给，path_count >= 1；库不猜文件名、不列目录，
	 * 段与文件的对应关系全在调用方手里。auto_advance = 1 时本段包区走完即自动接下一段，
	 * 对调用方呈现一条连续帧序列（CLV_FrameC 形状不变）；= 0 时本段读完就 got_frame = 0，与单段同形。
	 * paths / path_count 不成立判 CLV_CONTAINER_INVALID_ARGUMENT；paths[0] 打不开与 OpenFile 同档。
	 * 时间轴：段界不落在帧中间，但段首帧的 dts_delta 按**该段自己的起点**算（后续段不平移 ⇒
	 * 那个增量就是它的绝对 dts），跨段一个累加器加到底不成立 —— 连续时间轴要用 first_dts 逐段起算。
	 * 句柄轮转只换 reader：Stats / GetHead / 索引与扩展块视图反映**当前段**，跨段累计只由 SegStats 承担。 */
	CLV_EXPORT CLV_ContainerReader* CLV_ContainerReaderOpenSegments(const char* const* paths, uint32_t path_count,
																	uint8_t auto_advance, CLV_ContainerError* err);

	/* 当前段的段号与声明的总段数（头里 seg_total 未知时 SegmentTotal 返回全 1）。
	 * 本文件不是分段容器时：SegmentIndex 返 0、SegmentTotal 返全 1（= 未知），并回报 OK，不当错误。 */
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderSegmentIndex(const CLV_ContainerReader* r, uint64_t* out);
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderSegmentTotal(const CLV_ContainerReader* r, uint64_t* out);

	// 分段观测计数；句柄为空判 CLV_CONTAINER_INVALID_HANDLE，out 为空判 CLV_CONTAINER_INVALID_ARGUMENT
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderSegStats(const CLV_ContainerReader* r, CLV_ReaderSegStatsC* out);

	/* 段间扩展块（参数块）一致性的主动查询：把清单里每一段的扩展块按 (stream_id, ext_type) 与基准段逐字节比对，
	 * 读数同时写进 SegStats 的 `segments_ext_scanned` / `segments_ext_inconsistent` 两档。
	 * 本调用**不改变任何读行为**：不动段游标、不回收已交出的帧、不把不一致判成拒接（不一致时返回码仍是 OK）。
	 * 成本 = 每段一次「文件头 + 描述符表 + 扩展链」的读取；它是按需口，不在 NextFrame 通路上。
	 * 句柄为空判 CLV_CONTAINER_INVALID_HANDLE，out 为空判 CLV_CONTAINER_INVALID_ARGUMENT；
	 * 清单里每一段都打不开判 CLV_CONTAINER_IO_FAILED，此时不写 out、两档计数也不动；
	 * 库内分配失败判 CLV_CONTAINER_OUT_OF_MEMORY，此时两档计数不动。 */
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderSegExtCheck(CLV_ContainerReader* r, CLV_ReaderSegExtCheckC* out);

	/* 读回视图：文件头字段、索引条目、扩展块。都是只读，句柄存活期内稳定 */
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderGetHead(const CLV_ContainerReader* r, CLV_FileHeadC* out);
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderGetIndexEntry(const CLV_ContainerReader* r, uint32_t index,
																   CLV_IndexEntryC* out);
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderExtBlockCount(const CLV_ContainerReader* r, uint32_t* out);
	CLV_EXPORT CLV_ContainerError CLV_ContainerReaderGetExtBlock(const CLV_ContainerReader* r, uint32_t index,
																 CLV_ExtBlockC* out);

	CLV_EXPORT void CLV_ContainerReaderClose(CLV_ContainerReader* r);

	/* 每个枚举值都返回非空、静态存储期的 ASCII 串；未知值返回 "unknown"。 */
	CLV_EXPORT const char* CLV_ContainerStrError(CLV_ContainerError err);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_CONTAINER_H
