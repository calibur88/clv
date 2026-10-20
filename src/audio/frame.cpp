#include "frame.h"

namespace clv
{
	namespace audio
	{

		namespace
		{

			/* 两张 CRC 表在首次调用处就地建好。写成命名作用域上的对象要参与静态初始化顺序，
			 * 而本仓对这类一次性初始化的既定形态是函数内 static（取源口见 src/core/logger.cpp）。 */
			const uint8_t (&Crc8Table())[256]
			{
				static uint8_t table[256];
				static const bool built = []
				{
					for (unsigned i = 0; i < 256u; ++i)
					{
						unsigned c = i;
						for (int k = 0; k < 8; ++k) c = (c & 0x80u) ? ((c << 1u) ^ 0x07u) & 0xFFu : (c << 1u) & 0xFFu;
						table[i] = static_cast<uint8_t>(c);
					}
					return true;
				}();
				(void) built;
				return table;
			}

			const uint16_t (&Crc16Table())[256]
			{
				static uint16_t table[256];
				static const bool built = []
				{
					for (unsigned i = 0; i < 256u; ++i)
					{
						unsigned c = i << 8u;
						for (int k = 0; k < 8; ++k)
							c = (c & 0x8000u) ? ((c << 1u) ^ 0x8005u) & 0xFFFFu : (c << 1u) & 0xFFFFu;
						table[i] = static_cast<uint16_t>(c);
					}
					return true;
				}();
				(void) built;
				return table;
			}

			// 采样率码表的定值档；0b0000 与 0b1100..0b1110 走特殊路径
			uint32_t RateFromCode(uint8_t code)
			{
				static const uint32_t kMap[12] = { 88200u, 176400u, 192000u, 8000u,	 16000u, 22050u,
												   24000u, 32000u,	44100u,	 48000u, 96000u };
				if (code == 0u) return 0u;	  // 值只在流信息里给
				if (code >= 1u && code <= 11u) return kMap[code - 1u];
				return 0u;	  // 12..14 由 extra 字节给，15 禁用
			}

			/* 扩展 UTF-8 编码号某一档（lead 个字节）可载的最大值。lead 取 0 时返回 0，
			 * 供下界判直接比较：任何值都大于 0，故单字节档没有「更短的一档」。 */
			constexpr uint64_t ExtUtf8Cap(unsigned lead)
			{
				if (lead <= 1u) return lead == 1u ? 0x7Fu : 0u;
				return (1ull << ((7u - lead) + 6u * (lead - 1u))) - 1ull;
			}

			static_assert(ExtUtf8Cap(0u) == 0u, "无字节档没有可载值");
			static_assert(ExtUtf8Cap(1u) == 0x7Fu, "单字节 7 位");
			static_assert(ExtUtf8Cap(2u) == 0x7FFu, "两字节 11 位");
			static_assert(ExtUtf8Cap(3u) == 0xFFFFu, "三字节 16 位");
			static_assert(ExtUtf8Cap(4u) == 0x1FFFFFu, "四字节 21 位");
			static_assert(ExtUtf8Cap(5u) == 0x3FFFFFFu, "五字节 26 位");
			static_assert(ExtUtf8Cap(6u) == 0x7FFFFFFFu, "六字节 31 位");
			static_assert(ExtUtf8Cap(7u) == 0xFFFFFFFFFu, "七字节 36 位，与流信息总样本数的 36 位域同宽");

			/* 扩展 UTF-8 编码号（附加信息里的长度与值都用它写）：
			 * 首字节前导 1 的个数即总字节数，1 = 单字节 7 位载荷，2..7 = 每续字节 6 位载荷。 */
			bool ReadCodedNumber(const uint8_t* p, size_t avail, uint64_t* out, size_t* used)
			{
				if (avail == 0u) return false;
				const uint8_t b0 = p[0];
				if (b0 < 0x80u)
				{
					*out = b0;
					*used = 1u;
					return true;
				}
				unsigned lead = 0u;
				for (int bit = 7; bit >= 0; --bit)
				{
					if ((b0 & (1u << bit)) == 0u) break;
					++lead;
				}
				if (lead < 2u || lead > 7u) return false;	 // 10xxxxxx 与 11111110 以上都非法
				if (avail < lead) return false;
				const unsigned first_bits = 7u - lead;
				uint64_t v = b0 & ((1u << first_bits) - 1u);
				for (unsigned k = 1u; k < lead; ++k)
				{
					if ((p[k] & 0xC0u) != 0x80u) return false;
					v = (v << 6u) | (p[k] & 0x3Fu);
				}
				/* 最短形式两侧都判：值越出本档可表达域即不成立，落得进更短一档也不成立。
				 * 每档可载上界按位宽派生、不手抄魔数 —— 单字节形态是 0xxxxxxx 共 7 位；
				 * lead >= 2 时首字节贡献 (7 - lead) 位、每个续字节贡献 6 位，
				 * 故位宽 = (7 - lead) + 6 * (lead - 1)，逐档为 11 / 16 / 21 / 26 / 31 / 36 位。 */
				if (v > ExtUtf8Cap(lead)) return false;			 // 越出本档可表达域
				if (v <= ExtUtf8Cap(lead - 1u)) return false;	 // 更短一档装得下 => 多装了字节
				*out = v;
				*used = lead;
				return true;
			}

		}	 // namespace

		uint8_t Crc8(const uint8_t* data, size_t size)
		{
			const uint8_t* const table = Crc8Table();
			uint8_t crc = 0u;
			for (size_t i = 0; i < size; ++i) crc = table[crc ^ data[i]];
			return crc;
		}

		uint16_t Crc16(const uint8_t* data, size_t size)
		{
			const uint16_t* const table = Crc16Table();
			uint16_t crc = 0u;
			for (size_t i = 0; i < size; ++i)
				crc = static_cast<uint16_t>((crc << 8u) ^ table[((crc >> 8u) ^ data[i]) & 0xFFu]);
			return crc;
		}

		bool ParseFrameHead(const uint8_t* data, size_t size, FrameHead* out)
		{
			if (data == nullptr || out == nullptr || size < 7u) return false;
			// byte1 只许 0xF8 / 0xF9：同步码低 6 位（含 bit2）、保留位 bit1 一并钉死，块策略位留 0 / 1 两值
			if (data[0] != 0xFFu || (data[1] != 0xF8u && data[1] != 0xF9u)) return false;

			const uint8_t bs_code = static_cast<uint8_t>(data[2] >> 4u);
			const uint8_t sr_code = static_cast<uint8_t>(data[2] & 0x0Fu);
			const uint8_t ch_code = static_cast<uint8_t>(data[3] >> 4u);
			const uint8_t bits_code = static_cast<uint8_t>((data[3] >> 1u) & 0x07u);
			if ((data[3] & 1u) != 0u) return false;	   // 位深后的保留位
			if (bs_code == 0u) return false;		   // 块大小码 0b0000 为保留值
			if (sr_code == 15u) return false;		   // 采样率码 0b1111 为禁用值
			if (bits_code == 3u) return false;		   // 位深码 0b011 为保留值
			if (ch_code >= 11u) return false;		   // 声道码 0b1011..0b1111 为保留值

			size_t pos = 4u;
			uint64_t number = 0u;
			size_t used = 0u;
			if (! ReadCodedNumber(data + pos, size - pos, &number, &used)) return false;
			pos += used;

			uint32_t blocksize = 0u;
			if (bs_code == 6u || bs_code == 7u)
			{
				const size_t extra = (bs_code == 6u) ? 1u : 2u;
				if (size - pos < extra) return false;
				uint32_t stored = 0u;
				for (size_t i = 0; i < extra; ++i) stored = (stored << 8u) | data[pos + i];
				blocksize = stored + 1u;	// 块大小码表：存储值 = 块大小 - 1
				pos += extra;
			}
			else if (bs_code == 1u)
			{
				blocksize = 192u;
			}
			else if (bs_code >= 2u && bs_code <= 5u)
			{
				blocksize = 144u << bs_code;	// 576 / 1152 / 2304 / 4608
			}
			else
			{
				blocksize =
					1u
					<< bs_code;	   // 256 .. 32768（块大小码表的 0b1000..0b1111 都是定值码；越子集由 StreamableSubsetOk 判）
			}

			uint32_t rate = RateFromCode(sr_code);
			if (sr_code >= 12u && sr_code <= 14u)
			{
				const size_t extra = (sr_code == 12u) ? 1u : 2u;
				if (size - pos < extra) return false;
				uint32_t stored = 0u;
				for (size_t i = 0; i < extra; ++i) stored = (stored << 8u) | data[pos + i];
				pos += extra;
				/* 192u、144u << bs_code、1000u 与本表都是帧头两张码表（块大小码表、采样率码表）里的码值。 */
				if (sr_code == 12u) rate = stored * 1000u;	  // 非常规采样率，单位 kHz
				else if (sr_code == 13u) rate = stored;		  // 单位 Hz
				else rate = stored * 10u;					  // 单位 Hz ÷ 10
				if (rate == 0u) return false;
			}

			if (pos >= size) return false;
			const uint8_t want = data[pos];
			if (Crc8(data, pos) != want) return false;
			++pos;

			uint16_t channels = 0u;
			if (ch_code <= 7u) channels = static_cast<uint16_t>(ch_code + 1u);
			else channels = 2u;	   // 三种立体声去相关都是 2 声道

			uint16_t depth = 0u;
			switch (bits_code)
			{
			case 0u: depth = 0u; break;	   // 值只在流信息里给
			case 1u: depth = 8u; break;
			case 2u: depth = 12u; break;
			case 4u: depth = 16u; break;
			case 5u: depth = 20u; break;
			case 6u: depth = 24u; break;
			default: depth = 32u; break;
			}

			FrameHead h;
			h.blocksize = blocksize;
			h.sample_rate = rate;
			h.channels = channels;
			h.bit_depth = depth;
			h.coded_number = number;
			h.blocking_strategy = static_cast<uint8_t>(data[1] & 1u);
			h.header_size = static_cast<uint8_t>(pos);
			// 帧头之后的样本区至少要容得下 2 字节的帧尾校验
			if (h.header_size > size - 2u) return false;
			*out = h;
			return true;
		}

		bool FrameCrcOk(const uint8_t* data, size_t frame_size)
		{
			if (data == nullptr || frame_size < 3u) return false;
			const uint16_t want = static_cast<uint16_t>((data[frame_size - 2u] << 8u) | data[frame_size - 1u]);
			return Crc16(data, frame_size - 2u) == want;
		}

		bool StreamableSubsetOk(uint32_t blocksize, uint32_t sample_rate)
		{
			if (blocksize == 0u) return true;	 // 未知：交给流信息那一路判
			if (blocksize > kSubsetMaxBlocksize) return false;
			if (sample_rate != 0u && sample_rate <= kSubsetRateThresholdForShortBlock &&
				blocksize > kSubsetMaxBlocksizeAtLowRate)
			{
				return false;
			}
			return true;
		}

	}	 // namespace audio
}	 // namespace clv

