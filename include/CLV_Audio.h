/* CLV_Audio.h
 *
 * CLV 音频流（`stream_type = 1` / `codec_id = 1`，载荷为 FLAC 帧本体）编解码面的 C ABI 契约。
 * 编解码闭环在帧级：libFLAC 后端 + 帧头自核 + 流声明对账 + 一包一帧。函数名、结构体与出参形态
 * 都是对外 ABI 的一部分，改动即破坏调用方；错误码只在段尾追加，既有码值与语义不动。
 *
 * 面：不透明句柄 + 自由函数 + 整型错误码 + POD 描述符。零第三方类型、零平台类型、零文件接口——
 * 本层只吃内存里的帧字节，文件面失败在容器口收敛，本层不新立 IO 错误码。
 *
 * 与容器的分工：一包一帧（一个数据包承载一个完整 FLAC 帧，含帧头与帧尾校验），跨包的重组、
 * 片序与时间戳链都归容器层。本层承担两条独立义务：
 * **帧头自核**（同步码、块大小、采样率、声道、位深等字段自身自洽，并复核帧尾校验）与
 * **对账**（帧头的采样率 / 声道数 / 位深与流声明核对：单帧不一致丢帧 + warn，
 * 同一字段持续失配则拒解整条音频流）。块大小不在对账清单内，它的约束由帧头自洽
 * 与「不要求恒定块大小」共同承担。
 *
 * 时间轴：库不拥有播放时钟。音频的 DTS 由**逐帧实测的块大小**以样本为单位累加导出，
 * 容器带的 `dts_delta` 只作校验、不作时间来源。因此本层必须逐帧解帧头取块大小，
 * 不得以「首帧块大小恒定」推导后续帧的时间——变长块流上那样会把整条音轨推歪。
 * CLV_AudioInspectFrame 只读帧头、不解码，供逐帧取块大小。
 *
 * 合法域：v1 收 FLAC 流式子集——采样率位与位深位不得取保留值；块大小 ≤ 16384 样本，
 * 采样率 ≤ 48000 Hz 时 ≤ 4608 样本；采样率 ≤ 48000 Hz 时预测阶 ≤ 12；Rice 分块阶 ≤ 8；
 * 声道顺序不得依赖扩展掩码。位深收 16 / 24，声道 ≤ 8。越出子集不走单一码：编码侧按 `CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE`
 * 与 `CLV_AUDIO_VALUE_RANGE` 判，读侧走帧头自核（`FRAME_HEADER_BAD`）与流声明对账（`FIELD_MISMATCH`）两档；
 * 流信息块自身的字段越界（位深集合、采样率上界、块大小下界）判在流信息核对里（`CLV_AudioParseStreamInfo`，
 * 解码器建句柄时同一判点走 `STREAMINFO_ABSENT`）。
 *
 * 输出：一律整数 PCM、**交错**存放，字节序为**小端、二进制补码**；每样本字节数 = `ceil(bit_depth / 8)`（24 位取 3 字节，不取 4）。
 * 输出缓冲由库拥有、引用计数管理，调用方不接管所有权，要留内容自己拷。
 * 句柄与线程：所有句柄（含 `CLV_AudioBuffer` / `CLV_AudioBlob`）按**单线程使用**契约计，引用计数不是原子量，
 * 跨线程共享同一句柄的语义未定义。`AddRef` / `Release` 须配平；对已归零的对象再 `Release` 不生效（不回收、不回绕）。
 * 出参里的长度与计数字段都是 `uint32_t`：入参尺寸超出该域即判 `CLV_AUDIO_VALUE_RANGE`，不做静默截断。
 * 时间戳：每帧输出**不带**时间戳——`CLV_AudioFrameInfoC` 的 `dts_ticks` / `pts_ticks` / `timebase_den` 恒为 0。
 * 时间基由**容器层提供**，对齐在**消费侧（demo）**完成，本层不代填、不猜。
 *
 * 元数据：STREAMINFO 走 `ext_type = 5`，其余 FLAC 文件级块走 `ext_type = 6`（块内首字节 = 原块类型码，
 * 0 除外）。STREAMINFO 与流声明不一致时**拒解整条音频流**，其余流不受影响；
 * 承载块里读到 STREAMINFO 类型码按「字段不成立」处置：丢块 + 计数 + 继续，不判损坏；
 * 类型码 ≥ 7 的未定义块 warn + 跳过 + 计数，不判损坏；填充块丢弃不承载。
 * 搬运 Vorbis 注释时**尽力剥离**扩展声道掩码那一项键：剥得动就剥并记进计数，剥不掉（注释块结构本身
 * 不成立）原样收录、照常继续——掩码可以随注释块进容器，但帧头与声道顺序不依赖它。
 * 要确定性地剥并拿到结果块的，走 `CLV_AudioStripChannelMask`，那一口剥不动时如实返
 * `CLV_AUDIO_METADATA_MALFORMED`、不产出结果块。
 *
 * 写方义务：封装 FLAC 时必须保留未压缩帧兜底路径——某帧压缩结果不小于其 PCM 原始尺寸时，
 * 必须以未压缩帧落盘。该路径不可关（在用后端没有关掉它的公开接口），它是 payload 上限里那点余量成立的依据。
 *
 * 错误码：`CLV_AudioError` 是**独立枚举**，成功恒为 0（与既有公开面同值同义），
 * 失败码全部落在 32–63 段内。五面的错误码各自独立枚举：文件与容器从 1 顺排、音频 32 到 63、
 * 视频 64 到 95、节拍 96 起，不得跨头比对数值——各头的 1 语义无关。
 */
#ifndef CLV_AUDIO_H
#define CLV_AUDIO_H

#include "CLV_Export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct CLV_AudioDecoder CLV_AudioDecoder;
	typedef struct CLV_AudioEncoder CLV_AudioEncoder;
	typedef struct CLV_AudioBuffer CLV_AudioBuffer;
	typedef struct CLV_AudioBlob CLV_AudioBlob;

	/* 音频面错误码。0 恒为成功；失败码从 32 起，段内只追加不改序、不复用；退场码的号位留白，不再投给任何新码，
	 * 因此下列数值不因任何退场而前移。
	 * 标了「拒解整条流」的码是致命的：句柄进入拒解态，此后除 Reset / Close 与观测口
	 * CLV_AudioDecoderStats 外的调用一律返回 CLV_AUDIO_STREAM_REJECTED，其余流不受影响。
	 * 建句柄时即命中的（缺流信息，或流信息与流声明矛盾），Open 照常交出这个拒解态句柄，不返 NULL。
	 * 标了「丢帧」的码只影响本帧。 */
	typedef enum CLV_AudioError
	{
		CLV_AUDIO_OK = 0,

		CLV_AUDIO_INVALID_HANDLE = 32,	  /* 句柄为空或已关闭 */
		CLV_AUDIO_INVALID_ARGUMENT,		  /* 出参指针为空、数组与计数不匹配、载荷为空等 */
		CLV_AUDIO_VALUE_RANGE,			  /* 参数值域不成立：采样率为 0、声道越 [1,8]、位深非 16/24 */
		CLV_AUDIO_FRAME_HEADER_BAD,		  /* 帧头自核不过：同步码不符、字段取保留值、帧尾校验不符 → 丢帧 */
		CLV_AUDIO_FIELD_MISMATCH,		  /* 对账不一致：采样率 / 声道数 / 位深与流声明不符 → 丢帧 */
		CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE, /* 块大小越出流式子集上界或与 STREAMINFO 不符 → 丢帧 */
		CLV_AUDIO_STREAMINFO_ABSENT, /* 缺流信息：未给 / 长度不符 / 字段越出 FLAC 子集（位深集合、采样率上界、块大小下界）→ 拒解整条流 */
		CLV_AUDIO_METADATA_BAD,			  /* 承载块首字节 = 0（STREAMINFO 不得出现在此）→ 丢块 + 计数 + 继续 */
		CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED, /* 承载块类型码 ≥ 7 → 跳过 + 计数，不判损坏 */
		/* 41 为留白号位（不复用）：这一格对应的两类情形已分别由 TOO_LARGE 与 STATE_ERROR 覆盖，实现里无产出点。 */
		CLV_AUDIO_TOO_LARGE = 42, /* 单帧超过该流 payload 上限 */
		CLV_AUDIO_STATE_ERROR,	  /* 调用顺序不成立：已冲刷后继续喂、未喂样本就取帧等 */
		CLV_AUDIO_OUT_OF_MEMORY,  /* 库内分配失败 */
		/* 45 为留白号位（不复用）：非子集 / 能力外请求一律走上列具体码，本层不投「不支持」这一格。 */
		CLV_AUDIO_BACKEND_FAILURE = 46, /* 底层编解码器返回失败但结构上看不出问题 */
		CLV_AUDIO_STREAM_REJECTED, /* 流信息与流声明矛盾：Open 交出拒解态句柄并返回本码；此后除 Reset / Close 与 Stats 外统一返回本码 */
		CLV_AUDIO_METADATA_MALFORMED, /* 注释块 / 图标块的字节结构不成立：长度前缀越出块界、条目数与实有条数不符、条目里没有 `=` */
		CLV_AUDIO_COMMENT_KEY_NOT_FOUND /* 注释块里查不到该键（或该键的命中数不够 occurrence） */
	} CLV_AudioError;

	/* v1 只有交错整数 PCM。扩格式时在段尾追加新值，位深不进枚举（走描述符字段）。 */
	typedef enum CLV_AudioSampleFormat
	{
		CLV_AUDIO_FMT_INTERLEAVED = 0
	} CLV_AudioSampleFormat;

	/* 承载块的去向。判定只看块类型码，不解释块载荷内容。
	 * 注意 `STORE` 的实际含义 = **计入承载并计数**，句柄内**不留字节副本**：
	 * 注释块与图标块的字节本来就由容器读方稳定交出（`CLV_ExtBlockC.data` 有效期到 Close），
	 * 解读走下面那组无状态函数即可，句柄内再存一份只会多出一次所有权与失效口径。 */
	typedef enum CLV_AudioMetaAction
	{
		CLV_AUDIO_META_STORE = 0,		  /* 计入承载 + 计数，不留存副本 */
		CLV_AUDIO_META_DROP_PADDING,	  /* 丢弃不承载 */
		CLV_AUDIO_META_REJECT_STREAMINFO, /* 不该出现在承载块里：丢块 + 计数 + 继续 */
		CLV_AUDIO_META_SKIP_UNKNOWN		  /* 类型码 ≥ 7：跳过 + 计数，不判损坏 */
	} CLV_AudioMetaAction;

	/* 建解码器需要的流侧参数，来源是流描述符。块大小不在此声明：它逐帧从帧头取。
	 * `struct_size` = 0 或 sizeof(本结构)，其余值判 `CLV_AUDIO_VALUE_RANGE`；
	 * 两档语义与残余风险见 `CLV_Video.h` 的参数结构体注释（三头同式）。本结构原首字段 `sample_rate` 非 0，
	 * 所以按旧布局递交进来的对象头四字节必不等于本 sizeof ⇒ 走显式拒那一支。 */
	typedef struct CLV_AudioStreamParamsC
	{
		uint32_t struct_size;
		uint32_t sample_rate;
		uint16_t channels;
		uint16_t bit_depth;			 // 16 / 24
		uint32_t max_packet_size;	 // 该流 payload 字节上限
	} CLV_AudioStreamParamsC;

	/* 交错 PCM 输出的描述符。data 指向库拥有的缓冲，只到最后一个引用被 Release 为止有效。
	 * 样本按小端、二进制补码存放。
	 * bytes_per_sample = ceil(bit_depth / 8)：16 位 = 2，24 位 = 3。
	 * size = samples_per_channel * channels * bytes_per_sample。 */
	typedef struct CLV_AudioBufferDescC
	{
		CLV_AudioSampleFormat format;
		uint32_t sample_rate;
		uint16_t channels;
		uint16_t bit_depth;
		uint16_t bytes_per_sample;
		uint32_t samples_per_channel;
		const uint8_t* data;
		size_t size;
	} CLV_AudioBufferDescC;

	/* 一帧解码的结论。first_sample 是本帧首样本在该流样本轴上的序号（由逐帧块大小累加得出，
	 * 不是容器给的）。`dts_ticks` / `pts_ticks` / `timebase_den` 恒为 0：时间基由容器层提供。 */
	typedef struct CLV_AudioFrameInfoC
	{
		uint32_t blocksize;
		uint64_t first_sample;
		int64_t dts_ticks;
		int64_t pts_ticks;
		uint64_t timebase_den;
	} CLV_AudioFrameInfoC;

	/* 帧头自核的结果，字段全部来自帧头本身。
	 * 采样率与位深可能取「值只在流信息里给」的编码：此时对应字段为 0，
	 * 由调用方回落流声明，本头不猜。 */
	typedef struct CLV_AudioFrameHeadC
	{
		uint32_t blocksize;
		uint32_t sample_rate;
		uint16_t channels;
		uint16_t bit_depth;
		uint64_t frame_or_sample_number;
		uint8_t blocking_strategy;	  // 0 = 按帧号，1 = 按样本号（变长块流）
		uint8_t header_size;
	} CLV_AudioFrameHeadC;

	/* STREAMINFO 载荷（34 字节）的读回视图。编码侧产出的流信息里，校验摘要字段：本版本该字段恒为零，
	 * 不参与校验；读回视图如实搬运线上字节，外部素材带的非零摘要原样交出、库不比对。 */
	typedef struct CLV_AudioStreamInfoC
	{
		uint16_t min_blocksize;
		uint16_t max_blocksize;
		uint32_t min_framesize;
		uint32_t max_framesize;
		uint32_t sample_rate;
		uint16_t channels;
		uint16_t bit_depth;
		uint64_t total_samples;
		uint8_t md5[16];	// 编码侧产出：本版本该字段恒为零，不参与校验
	} CLV_AudioStreamInfoC;

	/* 观测口。计数只增不清零，Reset 后保留。 */
	typedef struct CLV_AudioStatsC
	{
		uint64_t frames_ok;
		uint64_t
			frames_dropped_header;	  // 凡按帧头一类原因丢的帧都进这里：自核不过、帧尾 CRC-16 不符、越出子集，以及帧头过了但对端解不出
		uint64_t frames_dropped_mismatch;	 // 对账不一致
		uint64_t blocks_stored;
		uint64_t blocks_dropped_padding;
		uint64_t blocks_rejected_streaminfo;	// 只记分类出的承载块类型码 = 0（流信息混入）那一档
		uint64_t blocks_skipped_unknown;
		uint64_t mask_keys_removed;
		uint8_t stream_rejected;
	} CLV_AudioStatsC;

	/* 建编码器参数。blocksize = 0 表示由库按子集自选；level 是压缩档，不承诺跨版本稳定；
	 * `struct_size` = 0（按本布局受理）或 sizeof(本结构)，其余任何值判 `CLV_AUDIO_VALUE_RANGE`；口径与本文件 `CLV_AudioStreamParamsC` 同式。
	 * 采样率的实际可接受区间由后端决定，超出区间在打开编码器时被拒。 */
	typedef struct CLV_AudioEncParamsC
	{
		uint32_t struct_size;
		uint32_t sample_rate;
		uint16_t channels;
		uint16_t bit_depth;	   // 16 / 24
		uint16_t blocksize;
		uint8_t level;
		uint8_t reserved;
	} CLV_AudioEncParamsC;

	/* 注释块（原块类型码 4）的读回视图。每个指针都落在调用方递交进来的那块字节上——
	 * 库不复制、不持有、不改变它们的归属，视图在源字节存活期内可读。
	 * 注释块视图必须由本头的解析函数产出，不得由调用方自拼。 */
	typedef struct CLV_AudioVorbisCommentC
	{
		const uint8_t* block;	 // 块载荷起点（不含承载块的那个类型码字节）
		uint32_t block_size;
		const uint8_t* vendor;	  // 厂商串：UTF-8 字节，不保证 NUL 结尾
		uint32_t vendor_length;
		uint32_t comment_count;	   // 键值条目条数，解析时已与块内实有条数对过
	} CLV_AudioVorbisCommentC;

	/* 一条注释。键 = `=` 左侧、值 = `=` 右侧，两段都不含 `=` 本身；键名比对不区分大小写。 */
	typedef struct CLV_AudioCommentEntryC
	{
		const uint8_t* key;
		uint32_t key_length;
		const uint8_t* value;
		uint32_t value_length;
	} CLV_AudioCommentEntryC;

	/* 图标块（原块类型码 6）的字段视图：封面一类的嵌入图像。
	 * MIME 串与描述串同为 UTF-8 字节、不保证 NUL 结尾；图像数据原样交出，库不解码也不裁剪。 */
	typedef struct CLV_AudioPictureC
	{
		uint32_t picture_type;	  // 图标类型码（3 = 前面封面，6 = 作者照片……取值属原块自带）
		const uint8_t* mime;
		uint32_t mime_length;
		const uint8_t* description;
		uint32_t description_length;
		uint32_t width;
		uint32_t height;
		uint32_t depth;
		uint32_t colors;
		const uint8_t* data;
		uint32_t data_length;
	} CLV_AudioPictureC;

	/* ---- 无状态工具（句柄外可用）---- */

	/* 只解帧头，不解码：逐帧取块大小、供 DTS 以样本为单位累加导出。
	 * 帧头不成立返回 CLV_AUDIO_FRAME_HEADER_BAD，且* out 不被写入。 */
	CLV_EXPORT CLV_AudioError CLV_AudioInspectFrame(const uint8_t* payload, size_t size, CLV_AudioFrameHeadC* out);

	/* 解析流信息载荷（34 字节）。长度不符或字段越出 FLAC 子集（位深集合、采样率上界、块大小下界）
	 * 返回相应错误码。 */
	CLV_EXPORT CLV_AudioError CLV_AudioParseStreamInfo(const uint8_t* data, size_t size, CLV_AudioStreamInfoC* out);

	/* 拆分承载块：首字节 = 原块类型码，其后 = 原块载荷。 */
	CLV_EXPORT CLV_AudioError CLV_AudioSplitMetaBlock(const uint8_t* ext_payload, size_t size, uint8_t* flac_block_type,
													  const uint8_t** data, size_t* out_size);

	/* 块类型码 → 去向。0 = 流信息（不得进承载块），1 = 填充（丢弃），2..6 = 原样收，≥ 7 = 跳过。 */
	CLV_EXPORT CLV_AudioError CLV_AudioClassifyMetaBlock(uint8_t flac_block_type, CLV_AudioMetaAction* out);

	/* 搬运 Vorbis 注释块时剥掉扩展声道掩码那一项键，产出库拥有的结果块（引用计数）。
	 * removed 报剥掉的键数；一块都没剥掉时结果块与输入逐字节相同。
	 * 注释块内容不成立（长度前缀越出块界、条目数与实有条数不符、条目里没有 `=`）返回 CLV_AUDIO_METADATA_MALFORMED，
 * 不产出结果块；CLV_AUDIO_METADATA_BAD 不从这里出——那一档只管承载块首字节 = 0（STREAMINFO 混进承载块）。 */
	CLV_EXPORT CLV_AudioError CLV_AudioStripChannelMask(const uint8_t* block, size_t size, CLV_AudioBlob** out,
														uint32_t* removed);

	/* ---- 承载块内容的解读（只读，不动格式）----
	 * 输入是 `CLV_AudioSplitMetaBlock` 给出的**原块载荷**（即去掉承载块首字节之后那段），
	 * 字节由容器读方持有并原样搬运：注释块与图标块的内容层字段在这里读出来，
	 * 本面对这些字节**不复制、不持有、不改写**，取到的指针跟着源字节走（容器 Close 前稳定）。 */

	/* 读回注释块（原块类型码 4）的厂商串与条目数。
	 * data / size 为空或 0 判 INVALID_ARGUMENT；块内长度前缀越界、条目数与实有条数不符判 METADATA_MALFORMED。 */
	CLV_EXPORT CLV_AudioError CLV_AudioParseVorbisComment(const uint8_t* data, size_t size,
														  CLV_AudioVorbisCommentC* out);

	/* 取第 index 条注释（0 起）。index 越出 comment_count 判 VALUE_RANGE。 */
	CLV_EXPORT CLV_AudioError CLV_AudioVorbisCommentEntryAt(const CLV_AudioVorbisCommentC* view, uint32_t index,
															CLV_AudioCommentEntryC* out);

	/* 按键名查注释：键名比对不区分大小写、比的是 `=` 左侧；同名可以有多条，occurrence 取第 occurrence + 1 次命中（0 起）。
	 * 没有这一条命中返回 CLV_AUDIO_COMMENT_KEY_NOT_FOUND，*out 不写。 */
	CLV_EXPORT CLV_AudioError CLV_AudioVorbisCommentFindKey(const CLV_AudioVorbisCommentC* view, const char* key,
															uint32_t occurrence, CLV_AudioCommentEntryC* out);

	/* 读回图标块（原块类型码 6）的字段：图标类型码、MIME 串、描述串、宽 / 高 / 位深 / 调色板色数、图像数据。
	 * 图像数据原样交出，本面不解码、不裁剪；任一长度前缀越界判 METADATA_MALFORMED。 */
	CLV_EXPORT CLV_AudioError CLV_AudioParsePicture(const uint8_t* data, size_t size, CLV_AudioPictureC* out);

	/* ---- 解码 ---- */

	/* 建解码器。streaminfo 为该流的流信息载荷（34 字节）；NULL 或长度不符即按缺流信息处置。
	 * **坏流给句柄、分配失败给 null**：流本身不成立（缺流信息，或流信息与流声明矛盾）时不返回 NULL，
	 * 而是交出一个已置拒解态的句柄，`*err` 写对应的致命码（STREAMINFO_ABSENT / STREAM_REJECTED）；
	 * 该句柄除 Reset / Close 与 Stats 外每个调用都返回 CLV_AUDIO_STREAM_REJECTED。
	 * 只有环境故障（库内分配失败）才返回 NULL + OUT_OF_MEMORY；参数不成立返回 NULL + 相应值域码。 */
	CLV_EXPORT CLV_AudioDecoder* CLV_AudioDecoderOpen(const CLV_AudioStreamParamsC* params, const uint8_t* streaminfo,
													  size_t streaminfo_size, CLV_AudioError* err);

	/* 解一帧（一包一帧）：先自核帧头、再与流声明对账，两项都过才解码。
	 * 出缓冲为库拥有的交错 PCM。* got = 0 表示本帧被丢（原因见返回码与统计口），不视为崩溃。
	 * info 可为 NULL。句柄在拒解态时直接返 CLV_AUDIO_STREAM_REJECTED、一帧不出。 */
	CLV_EXPORT CLV_AudioError CLV_AudioDecoderDecodeFrame(CLV_AudioDecoder* d, const uint8_t* payload, size_t size,
														  CLV_AudioBuffer** out, uint8_t* got,
														  CLV_AudioFrameInfoC* info);

	/* 把一个承载块交给解码器分类并计数（载荷来自 `ext_type = 6`）。
	 * 返回该块的去向对应码：收 = OK，填充 = OK 且计数，流信息 = METADATA_BAD，≥ 7 = BLOCK_TYPE_UNSUPPORTED。
	 * 三种都不判损坏，调用方继续下一块。句柄在拒解态时直接返 CLV_AUDIO_STREAM_REJECTED。 */
	CLV_EXPORT CLV_AudioError CLV_AudioDecoderAddMetaBlock(CLV_AudioDecoder* d, const uint8_t* ext_payload,
														   size_t size);

	/* 定位 / 丢包重置：清空本流的样本累加基准与在途状态。统计计数保留。
	 * 它也是**拒解态的唯一出口**（复位即离开拒解态），故本入口不被拒解态拦截。 */
	CLV_EXPORT CLV_AudioError CLV_AudioDecoderReset(CLV_AudioDecoder* d);

	CLV_EXPORT CLV_AudioError CLV_AudioDecoderStats(const CLV_AudioDecoder* d, CLV_AudioStatsC* out);
	CLV_EXPORT void CLV_AudioDecoderClose(CLV_AudioDecoder* d);

	/* ---- 编码 ---- */

	/* 建编码器。采样率的实际可接受区间由后端决定，超出区间在打开编码器时被拒。 */
	CLV_EXPORT CLV_AudioEncoder* CLV_AudioEncoderOpen(const CLV_AudioEncParamsC* params, CLV_AudioError* err);

	/* 喂交错 PCM，字节布局与输出描述符一致：小端、二进制补码、每样本 ceil(bit_depth ÷ 8) 字节。
	 * samples_per_channel 是本批每声道样本数。
	 * 越出子集或值域返回相应错误码，本批不消费。 */
	CLV_EXPORT CLV_AudioError CLV_AudioEncoderWrite(CLV_AudioEncoder* e, const uint8_t* pcm, size_t size,
													uint32_t samples_per_channel);

	/* 取下一帧编码结果（一包一帧的那一个 FLAC 帧本体）。* got = 0 表示当前喂入的样本
	 * 还没凑满一帧，不是错误。 */
	CLV_EXPORT CLV_AudioError CLV_AudioEncoderNextFrame(CLV_AudioEncoder* e, const uint8_t** data, size_t* size,
														uint8_t* got);

	/* 冲刷尾帧。之后不能再喂样本。 */
	CLV_EXPORT CLV_AudioError CLV_AudioEncoderFlush(CLV_AudioEncoder* e);

	/* 取本次编码的流信息载荷（34 字节），供写方作为 `ext_type = 5` 落盘。
	 * Flush 之后才有意义；且须先喂过样本——一个样本都没喂时块大小与帧长统计为空，本口返回
	 * `CLV_AUDIO_STATE_ERROR`。载荷里的校验摘要字段：本版本该字段恒为零，不参与校验。
	 * blob 由库拥有、引用计数管理。 */
	CLV_EXPORT CLV_AudioError CLV_AudioEncoderTakeStreamInfo(CLV_AudioEncoder* e, CLV_AudioBlob** out);

	CLV_EXPORT void CLV_AudioEncoderClose(CLV_AudioEncoder* e);

	/* ---- 缓冲与引用计数 ---- */

	CLV_EXPORT uint32_t CLV_AudioBufferAddRef(CLV_AudioBuffer* b);
	CLV_EXPORT void CLV_AudioBufferRelease(CLV_AudioBuffer* b);
	CLV_EXPORT CLV_AudioError CLV_AudioBufferGetDesc(const CLV_AudioBuffer* b, CLV_AudioBufferDescC* out);

	CLV_EXPORT uint32_t CLV_AudioBlobAddRef(CLV_AudioBlob* b);
	CLV_EXPORT void CLV_AudioBlobRelease(CLV_AudioBlob* b);
	CLV_EXPORT CLV_AudioError CLV_AudioBlobGetBytes(const CLV_AudioBlob* b, const uint8_t** data, size_t* size);

	/* 每个枚举值都返回非空、静态存储期的 ASCII 串；未知值返回 "unknown"。 */
	CLV_EXPORT const char* CLV_AudioStrError(CLV_AudioError err);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_AUDIO_H
