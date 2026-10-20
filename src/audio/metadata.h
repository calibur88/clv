/* metadata.h
 *
 * FLAC 文件级元数据块在 CLV 里的落位。两条通道：
 * 流信息块只走容器层的 `ext_type = 5`；其余块原样搬进 `ext_type = 6`，块内首字节 = FLAC 原块类型码。
 *
 * 流信息块载荷 34 字节的位域（STREAMINFO 的字段序与位宽，逐条写在下面）：
 *   16 最小块大小 / 16 最大块大小 / 24 最小帧大小 / 24 最大帧大小 /
 *   20 采样率 / 3 声道数-1 / 5 位深-1 / 36 总样本数 / 128 MD5
 *
 * Vorbis 注释块（V-comment 形态：长度前缀 + 键值对，键名不区分大小写）：
 *   u32 小端 vendor 长度 + vendor 串 / u32 小端 注释条数 + 每条 {u32 小端长度, "KEY=VALUE"}
 * 键名按不区分大小写比较，比对的是 `=` 左侧。
 */
#ifndef CLV_AUDIO_METADATA_H
#define CLV_AUDIO_METADATA_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace audio
	{

		/* STREAMINFO 元数据块的长度：34 字节（字段清单见本件头注释）。
		 * 元数据块解析与「流信息对账」两处都用它，硬写两处则改长度必漏一处。 */
		inline constexpr size_t kStreamInfoSize = 34u;

		enum class MetaAction : unsigned char
		{
			kStore,			   // 计入承载（分类 + 计数），**不在句柄内另存字节副本**
			kDropPadding,	   // 丢弃不承载
			kRejectStreamInfo, /* 不该出现在承载块里 */
			kSkipUnknown	   // 未定义区
		};

		/* 注释块（类型码 4）的条目视图：键与值都指向调用方给的那块字节。 */
		struct CommentEntry
		{
			const uint8_t* key;
			size_t key_len;
			const uint8_t* value;
			size_t value_len;
		};

		/* 图标块（类型码 6）的字段视图：同样只指向原字节。 */
		struct PictureFields
		{
			uint32_t type = 0;
			const uint8_t* mime = nullptr;
			size_t mime_len = 0;
			const uint8_t* description = nullptr;
			size_t description_len = 0;
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t depth = 0;
			uint32_t colors = 0;
			const uint8_t* data = nullptr;
			size_t data_len = 0;
		};

		struct StreamInfo
		{
			uint16_t min_blocksize = 0;
			uint16_t max_blocksize = 0;
			uint32_t min_framesize = 0;
			uint32_t max_framesize = 0;
			uint32_t sample_rate = 0;
			uint16_t channels = 0;
			uint16_t bit_depth = 0;
			uint64_t total_samples = 0;
			uint8_t md5[16] = { 0 };
		};

		/* STREAMINFO 载荷解析 + 值域核对：长度不符，或位深、采样率、块大小越出 FLAC 子集即返回 false。
		 * 值域三组判点的界值引用 frame.h 的具名常量（位深集合 {16, 24} 见公开头合法域条）。 */
		bool ParseStreamInfo(const uint8_t* data, size_t size, StreamInfo* out);

		/* 注释块结构核对：vendor 串 + 条目数 + 第一条目的偏移。任一长度越出块界即返回 false（不改出参）。
		 * 出参指针都可以为 NULL（只判结构时不必取内容）。 */
		bool ParseVComment(const uint8_t* block, size_t size, const uint8_t** vendor, size_t* vendor_len,
						   uint32_t* count, size_t* first_entry_off);

		/* 按序号取一条：越界或该条长度越出块界返回 false。 */
		bool VCommentEntryAt(const uint8_t* block, size_t size, size_t first_entry_off, uint32_t count, uint32_t index,
							 CommentEntry* out);

		/* 大小写不敏感查键（比对的是 `=` 左侧），occurrence 取第 occurrence + 1 次命中，从 0 起。
		 * 没有这一条命中返回 false。 */
		bool VCommentFindKey(const uint8_t* block, size_t size, size_t first_entry_off, uint32_t count, const char* key,
							 uint32_t occurrence, CommentEntry* out);

		/* 图标块字段解析（一族大端 32 位长度前缀）。任一长度越出块界即返回 false。 */
		bool ParsePictureBlock(const uint8_t* block, size_t size, PictureFields* out);

		/* 拆分承载块：首字节 = 原块类型码，其后 = 原块载荷（逐字节照搬，不解释）。 */
		bool SplitMetaBlock(const uint8_t* ext_payload, size_t size, uint8_t* block_type, const uint8_t** data,
							size_t* out_size);

		MetaAction ClassifyMetaBlock(uint8_t block_type);

		/* 剥掉 Vorbis 注释块里的扩展声道掩码那一条键值：
		 * 掩码可以随注释块进容器，但帧头与声道顺序不依赖它，所以搬运时尽力剥离——
		 * 剥得动记条数，注释块结构本身不成立时剥不动，由调用方原样搬运（不拦）。
		 * 产出的新块写进 out（调用方给的容器，函数自己按最短结构算好所需字节）。
		 * removed 报剥掉的条数。返回 false = 注释块结构不成立。 */
		bool StripChannelMask(const uint8_t* block, size_t size, std::vector<uint8_t>* out, uint32_t* removed);

	}	 // namespace audio
}	 // namespace clv

#endif	  // CLV_AUDIO_METADATA_H

