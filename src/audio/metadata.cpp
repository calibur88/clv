#include "metadata.h"

#include "frame.h"

#include <cstring>

namespace clv
{
	namespace audio
	{

		namespace
		{

			// 掩码键名拼写固定，比对时键名不区分大小写（该键是注释块里的扩展声道掩码条目）
			constexpr const char* kMaskKey = "WAVEFORMATEXTENSIBLE_CHANNEL_MASK";

			uint32_t ReadLE32(const uint8_t* p)
			{
				return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
					   (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
			}

			void WriteLE32(uint32_t v, uint8_t* p)
			{
				p[0] = static_cast<uint8_t>(v & 0xFFu);
				p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
				p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
				p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
			}

			/* 图标块那一族长度前缀与尺寸字段是大端 32 位，与注释块的小端正反一趟。 */
			uint32_t ReadBE32(const uint8_t* p)
			{
				return (static_cast<uint32_t>(p[0]) << 24u) | (static_cast<uint32_t>(p[1]) << 16u) |
					   (static_cast<uint32_t>(p[2]) << 8u) | static_cast<uint32_t>(p[3]);
			}

			bool EqualsKeyIgnoreCase(const uint8_t* p, size_t n, const char* key)
			{
				const size_t klen = std::strlen(key);
				if (n != klen) return false;
				for (size_t i = 0; i < klen; ++i)
				{
					char a = static_cast<char>(p[i]);
					char b = key[i];
					if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
					if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
					if (a != b) return false;
				}
				return true;
			}

			/* 一条注释里 `=` 左侧的键长；没有 `=` 时返回 size（不是合法键值对，按不匹配处理）。 */
			size_t KeyLen(const uint8_t* p, size_t n)
			{
				for (size_t i = 0; i < n; ++i)
					if (p[i] == '=') return i;
				return n;
			}

		}	 // namespace

		bool ParseStreamInfo(const uint8_t* data, size_t size, StreamInfo* out)
		{
			if (data == nullptr || out == nullptr || size != kStreamInfoSize) return false;

			StreamInfo si;
			si.min_blocksize = static_cast<uint16_t>((data[0] << 8u) | data[1]);
			si.max_blocksize = static_cast<uint16_t>((data[2] << 8u) | data[3]);
			si.min_framesize =
				(static_cast<uint32_t>(data[4]) << 16u) | (static_cast<uint32_t>(data[5]) << 8u) | data[6];
			si.max_framesize =
				(static_cast<uint32_t>(data[7]) << 16u) | (static_cast<uint32_t>(data[8]) << 8u) | data[9];

			uint64_t v = 0u;
			for (int i = 0; i < 8; ++i) v = (v << 8u) | data[10 + i];
			si.sample_rate = static_cast<uint32_t>(v >> 44u);					// 20 位
			si.channels = static_cast<uint16_t>(((v >> 41u) & 0x7u) + 1u);		// 3 位，存的是声道数 - 1
			si.bit_depth = static_cast<uint16_t>(((v >> 36u) & 0x1Fu) + 1u);	// 5 位，存的是位深 - 1
			si.total_samples = v & 0xFFFFFFFFFu;								// 36 位
			/* 上面这组位移（44 / 41 / 36 等）是 STREAMINFO 的位域布局（载荷 34 字节的位序见 metadata.h 的头注释），
			 * 越出该表的位宽即解析失败；声道数与位深线上存的是「值 - 1」。 */
			std::memcpy(si.md5, data + 18, 16u);

			/* 值域判点（公开头「字段越出 FLAC 子集就返错误码」这句承诺落在这里）：
			 * 位深集合取合法域条声明的 {16, 24}；采样率在大于等于后端拒档的那一侧一律不受理
			 * （kSubsetSampleRateRejectFloor 的出处与口径见 frame.h）；块大小按位深分档判下界。
			 * 界值只引用 frame.h 的具名常量，本件不再手抄第二份。 */
			if (si.sample_rate == 0u) return false;
			if (si.sample_rate >= kSubsetSampleRateRejectFloor) return false;
			if (si.bit_depth != 16u && si.bit_depth != 24u) return false;
			if (si.min_blocksize == 0u || si.max_blocksize == 0u) return false;
			if (si.min_blocksize > si.max_blocksize) return false;
			if (si.min_framesize != 0u && si.max_framesize != 0u && si.min_framesize > si.max_framesize) return false;
			const uint32_t blocksize_floor = si.bit_depth == 16u ? kSubsetMinBlocksize16 : kSubsetMinBlocksize24;
			if (si.min_blocksize < blocksize_floor) return false;	 // 上面已判 min ≤ max，max 不必再判一次下界
			/* 此处不设块大小上界：min/max_blocksize 是线上 16 位字段（C 层 uint16_t），任何取值都不过
			 * 65535，写成 > 65535 的判式恒假。子集的上界界判不落在流信息核对里——读方接受域宽于
			 * 子集，越子集的 lax 流要照常读出；写方侧的上界判在编码入口做。 */
			*out = si;
			return true;
		}

		bool SplitMetaBlock(const uint8_t* ext_payload, size_t size, uint8_t* block_type, const uint8_t** data,
							size_t* out_size)
		{
			if (ext_payload == nullptr || size < 1u) return false;
			if (block_type != nullptr) *block_type = ext_payload[0];
			if (data != nullptr) *data = ext_payload + 1;
			if (out_size != nullptr) *out_size = size - 1u;
			return true;
		}

		MetaAction ClassifyMetaBlock(uint8_t block_type)
		{
			if (block_type == 0u) return MetaAction::kRejectStreamInfo;
			if (block_type == 1u) return MetaAction::kDropPadding;
			if (block_type >= 2u && block_type <= 6u) return MetaAction::kStore;
			return MetaAction::kSkipUnknown;
		}

		bool ParseVComment(const uint8_t* block, size_t size, const uint8_t** vendor, size_t* vendor_len,
						   uint32_t* count, size_t* first_entry_off)
		{
			if (block == nullptr || size < 8u) return false;

			size_t pos = 0u;
			if (size - pos < 4u) return false;
			const uint32_t vlen = ReadLE32(block + pos);
			pos += 4u;
			if (vlen > size - pos) return false;
			const uint8_t* v = block + pos;
			pos += vlen;

			if (size - pos < 4u) return false;
			const uint32_t n = ReadLE32(block + pos);
			pos += 4u;
			const size_t entries_off = pos;	   // 条目区起点：4 字节厂商串长度 + 厂商串 + 4 字节条数

			/* 条目数不是声明值，必须与实有条数对得上：逐条走完，任何一条的长度越界即整块不成立。
			 * 每条还必须带 `=`（键值对的形态），否则同样按结构不成立处理 —— 这样查询口返回 false
			 * 就只表示「没查到」，不再和「块坏了」混成一个出口。 */
			for (uint32_t i = 0u; i < n; ++i)
			{
				if (size - pos < 4u) return false;
				const uint32_t len = ReadLE32(block + pos);
				pos += 4u;
				if (len > size - pos) return false;
				if (KeyLen(block + pos, len) == len) return false;
				pos += len;
			}

			if (vendor != nullptr) *vendor = v;
			if (vendor_len != nullptr) *vendor_len = vlen;
			if (count != nullptr) *count = n;
			if (first_entry_off != nullptr) *first_entry_off = entries_off;
			return true;
		}

		bool VCommentEntryAt(const uint8_t* block, size_t size, size_t first_entry_off, uint32_t count, uint32_t index,
							 CommentEntry* out)
		{
			if (block == nullptr || out == nullptr || index >= count) return false;

			size_t pos = first_entry_off;
			for (uint32_t i = 0u; i <= index; ++i)
			{
				if (size - pos < 4u) return false;
				const uint32_t len = ReadLE32(block + pos);
				pos += 4u;
				if (len > size - pos) return false;
				const uint8_t* item = block + pos;
				if (i == index)
				{
					const size_t klen = KeyLen(item, len);
					if (klen == len) return false;	  // 没有 `=`，不成其为键值对
					out->key = item;
					out->key_len = klen;
					out->value = item + klen + 1u;
					out->value_len = len - klen - 1u;
					return true;
				}
				pos += len;
			}
			return false;
		}

		bool VCommentFindKey(const uint8_t* block, size_t size, size_t first_entry_off, uint32_t count, const char* key,
							 uint32_t occurrence, CommentEntry* out)
		{
			if (block == nullptr || key == nullptr || out == nullptr) return false;

			/* 单趟扫描条目区：逐条就地比对，不借 VCommentEntryAt 每查一条从起点重扫一遍
			 * （条数是 32 位无符号，重扫即 O(n²)）。结构判与 VCommentEntryAt 同档：
			 * 长度越出块界或条目里没有 `=` 都判整次查询不成立。 */
			uint32_t seen = 0u;
			size_t pos = first_entry_off;
			for (uint32_t i = 0u; i < count; ++i)
			{
				if (size - pos < 4u) return false;
				const uint32_t len = ReadLE32(block + pos);
				pos += 4u;
				if (len > size - pos) return false;
				const uint8_t* item = block + pos;
				const size_t klen = KeyLen(item, len);
				if (klen == len) return false;	  // 没有 `=`，不成其为键值对
				if (EqualsKeyIgnoreCase(item, klen, key))
				{
					if (seen == occurrence)
					{
						out->key = item;
						out->key_len = klen;
						out->value = item + klen + 1u;
						out->value_len = len - klen - 1u;
						return true;
					}
					++seen;
				}
				pos += len;
			}
			return false;
		}

		bool ParsePictureBlock(const uint8_t* block, size_t size, PictureFields* out)
		{
			/* 定长部分 = 8 个 4 字节字段共 32 字节（类型、MIME 长度、描述长度、宽、高、位深、色数、图像长度）；
			 * 三个变长段都允许零长，全零长的退化块正好 32 字节，是合法结构。 */
			if (block == nullptr || out == nullptr || size < 4u * 8u) return false;

			PictureFields f;
			size_t pos = 0u;
			f.type = ReadBE32(block + pos);
			pos += 4u;

			if (size - pos < 4u) return false;
			const uint32_t mime_len = ReadBE32(block + pos);
			pos += 4u;
			if (mime_len > size - pos) return false;
			f.mime = block + pos;
			f.mime_len = mime_len;
			pos += mime_len;

			if (size - pos < 4u) return false;
			const uint32_t desc_len = ReadBE32(block + pos);
			pos += 4u;
			if (desc_len > size - pos) return false;
			f.description = block + pos;
			f.description_len = desc_len;
			pos += desc_len;

			/* 宽 / 高 / 位深 / 调色板色数四个 u32，再加图像数据长度。 */
			if (size - pos < 4u * 5u) return false;
			f.width = ReadBE32(block + pos);
			f.height = ReadBE32(block + pos + 4u);
			f.depth = ReadBE32(block + pos + 8u);
			f.colors = ReadBE32(block + pos + 12u);
			const uint32_t data_len = ReadBE32(block + pos + 16u);
			pos += 4u * 5u;
			if (data_len > size - pos) return false;
			f.data = block + pos;
			f.data_len = data_len;

			*out = f;
			return true;
		}

		bool StripChannelMask(const uint8_t* block, size_t size, std::vector<uint8_t>* out, uint32_t* removed)
		{
			if (block == nullptr || out == nullptr || size < 8u) return false;
			if (removed != nullptr) *removed = 0u;

			size_t pos = 0u;
			if (size - pos < 4u) return false;
			const uint32_t vendor_len = ReadLE32(block + pos);
			pos += 4u;
			if (vendor_len > size - pos) return false;
			pos += vendor_len;

			if (size - pos < 4u) return false;
			const uint32_t count = ReadLE32(block + pos);
			pos += 4u;

			std::vector<uint8_t> kept;
			kept.resize(4u + vendor_len + 4u);
			WriteLE32(vendor_len, kept.data());
			std::memcpy(kept.data() + 4u, block + 4u, vendor_len);
			const size_t count_off = 4u + vendor_len;	 // 条数字段的偏移：kept 会扩容，不能存裸指针
			WriteLE32(0u, kept.data() + count_off);

			uint32_t kept_count = 0u;
			uint32_t gone = 0u;
			for (uint32_t i = 0u; i < count; ++i)
			{
				if (size - pos < 4u) return false;
				const uint32_t len = ReadLE32(block + pos);
				pos += 4u;
				if (len > size - pos) return false;
				const uint8_t* item = block + pos;
				pos += len;

				const size_t klen = KeyLen(item, len);
				/* 条目里没有 '=' 就判整块不成立：注释面的解析口径对无键条目一律判坏，
				 * 从这条通道原样搬出去只会产出必然解析失败的块，两侧必须同判。 */
				if (klen == len) return false;
				if (EqualsKeyIgnoreCase(item, klen, kMaskKey))
				{
					++gone;	   // 掩码条目剥掉：帧头与声道顺序不依赖它
					continue;
				}
				const size_t base = kept.size();
				kept.resize(base + 4u + len);
				WriteLE32(len, kept.data() + base);
				std::memcpy(kept.data() + base + 4u, item, len);
				++kept_count;
			}
			WriteLE32(kept_count, kept.data() + count_off);
			if (removed != nullptr) *removed = gone;
			out->swap(kept);
			return true;
		}

	}	 // namespace audio
}	 // namespace clv
