#include "block.h"

#include "reconstruct.h"

#include <cstddef>

namespace clv
{
	namespace video
	{

		namespace
		{

			struct WidthRow
			{
				uint8_t start;	   // q' 起点（含）
				uint8_t bits;	   // 每系数位宽（含 1 bit 符号）
				int32_t qf_max;	   // |QF| 上界
			};

			/* 帧内 7 档：起点 1 / 3 / 5 / 9 / 17 / 33 / 65（探针实跑扫出）。
			 * QF_max 由 T_max = 8192 全域扫出，与 bits 算式 ceil(log2(QF_max+1)) + 1 自洽。 */
			constexpr WidthRow kIntraRows[7] = {
				{ 1u,  13u, 4095 },
				  { 3u,	 12u, 1365 },
				{ 5u,  11u, 819	 },
				 { 9u,	10u, 455  },
				{ 17u, 9u,	240	 },
				 { 33u, 8u,	 124  },
				  { 65u, 7u,  63   },
			};

			/* 帧间 7 档：起点 1 / 2 / 4 / 8 / 16 / 32 / 64，T_max = 16320。
			 * 与帧内同档相比位宽恰多 1（残差域是帧内域的 2 倍再补一点）。 */
			constexpr WidthRow kInterRows[7] = {
				{ 1u,  14u, 8159 },
				  { 2u,	 13u, 4080 },
				{ 4u,  12u, 2040 },
				  { 8u,	 11u, 1020 },
				{ 16u, 10u, 510	 },
				  { 32u, 9u,  255  },
				   { 64u, 8u,  127	},
			};

			const WidthRow* Rows(bool intra) noexcept { return intra ? kIntraRows : kInterRows; }

			/* 无损档：量化步长为 0，载体直载整数变换结果，界就是架构界本身
			 * ——帧内 64 × 128 = 8192、帧间 64 × 255 = 16320，与反量化第二级饱和域同源，
			 * 不在此另立字面量。位宽 = 1 bit 符号 + ⌈log2(界 + 1)⌉ bit 幅值 = 1 + 14 = 15，两档同宽。 */
			constexpr WidthRow kLosslessRows[2] = {
				{ kQuantLossless, 15u, kCoefClampMaxIntra8 },
				{ kQuantLossless, 15u, kCoefClampMaxInter8 },
			};
			static_assert((1 << 14) - 1 >= kCoefClampMaxInter8, "无损载体装不下帧间满幅系数");
			static_assert((1 << 14) - 1 >= kCoefClampMaxIntra8, "无损载体装不下帧内满幅系数");

			const WidthRow& Pick(uint8_t q, bool intra) noexcept
			{
				if (q == kQuantLossless) return kLosslessRows[intra ? 0 : 1];
				/* 到这里 q 恒 ≥ 1：域校验在码流读写与公开入口两处已经把 0 收进无损分支、把负域挡在外面。
				 * 末档起点 65（帧内）/ 64（帧间）覆盖 q 的整个可表达域，上界再大也落在末档，
				 * 故这里不按任何字面量截 q。 */
				const WidthRow* rows = Rows(intra);
				int i = 6;
				while (i > 0 && q < rows[i].start) --i;
				return rows[i];
			}

			/* 块序与合成位序同构：先 4 个亮度块（bit5 起由高到低），再 Cb、再 Cr。
			 * 返回第 k 个在场块对应的宏块内块号（0..5）。 */
			bool BlockIndexOrdered(uint8_t pattern, int k, int* out) noexcept
			{
				int seen = 0;
				for (int bit = 5; bit >= 0; --bit)
				{
					if ((pattern & static_cast<uint8_t>(1u << bit)) == 0u) continue;
					if (seen == k)
					{
						*out = 5 - bit;
						return true;
					}
					++seen;
				}
				return false;
			}

			/* 法 1 的 eob 字段宽：all_zero 已经吃掉「整块无非零」这一档，剩下的取值就是下标 0..63。
			 * 于是 eob ≥ 64 在字段宽度上结构性地不可能出现，解码侧不需要再补一条范围判据。 */
			inline constexpr uint32_t kEobFieldBits = 6u;
			static_assert((1u << kEobFieldBits) == static_cast<uint32_t>(kBlockCoefs),
						  "eob 字段必须正好铺满下标 0..63");

			/* Exp-Golomb order-0 的码字长：2L + 1，L = floor(log2(v + 1))。无参数、无表、自分隔。 */
			inline uint32_t EgBits(uint32_t v) noexcept
			{
				uint32_t L = 0u;
				for (uint32_t t = v + 1u; t > 1u; t >>= 1) ++L;
				return 2u * L + 1u;
			}

			void WriteEg(BitWriter* bw, uint32_t v)
			{
				const uint32_t x = v + 1u;
				uint8_t L = 0u;
				for (uint32_t t = x; t > 1u; t >>= 1) ++L;
				bw->Write(0u, L);
				bw->Write(1u, 1u);
				if (L > 0u) bw->Write(x & ((1u << L) - 1u), L);
			}

			/* 读侧的前导零上限：L 到 24 时 v 已接近 2^24，远超任何一档的 QF_max，
				 继续读只会把坏码流读成更大的坏值。到顶即判读不尽，这一条本身就是拒绝路径。 */
			inline constexpr uint8_t kEgMaxPrefix = 24u;

			bool ReadEg(BitReader* br, uint32_t* v)
			{
				uint8_t L = 0u;
				for (;;)
				{
					uint8_t bit = 0u;
					if (! br->ReadBit(&bit)) return false;
					if (bit == 1u) break;
					if (L >= kEgMaxPrefix) return false;
					++L;
				}
				if (L == 0u)
				{
					*v = 0u;
					return true;
				}
				uint32_t suf = 0u;
				if (! br->Read(&suf, L)) return false;
				*v = ((1u << L) | suf) - 1u;
				return true;
			}

		}	 // namespace

		uint8_t BlockBits(uint8_t q, bool intra) noexcept { return Pick(q, intra).bits; }

		int32_t BlockQfMax(uint8_t q, bool intra) noexcept { return Pick(q, intra).qf_max; }

		uint32_t BlockBytes(uint8_t q, bool intra) noexcept
		{
			return static_cast<uint32_t>(kBlockCoefs) * static_cast<uint32_t>(BlockBits(q, intra)) / 8u;
		}

		int PresentBlockCount(uint8_t pattern) noexcept
		{
			int n = 0;
			for (int bit = 5; bit >= 0; --bit)
				if ((pattern & static_cast<uint8_t>(1u << bit)) != 0u) ++n;
			return n;
		}

		uint32_t BlockBitsMax(uint8_t q, bool intra, CoefCoding coding) noexcept
		{
			const uint32_t bits = static_cast<uint32_t>(BlockBits(q, intra));
			switch (coding)
			{
			case CoefCoding::kEobEg:
				/* all_zero(1) + eob(6) + 64 槽各一个 EG(幅) + 非零槽的符号位。
				 * 按满幅系数算：EG(QF_max) 比定宽 bits 还长，所以这一支恒大于法 0 的 64 × bits。 */
				return 1u + kEobFieldBits +
					   static_cast<uint32_t>(kBlockCoefs) * (EgBits(static_cast<uint32_t>(BlockQfMax(q, intra))) + 1u);
			case CoefCoding::kFixed:
			default: return static_cast<uint32_t>(kBlockCoefs) * bits;
			}
		}

		uint32_t BlockBytesMax(uint8_t q, bool intra, CoefCoding coding) noexcept
		{
			return (BlockBitsMax(q, intra, coding) + 7u) / 8u;
		}

		void EncodeBlock(const int32_t qf[kBlockCoefs], uint8_t q, bool intra, CoefCoding coding, BitWriter* bw)
		{
			switch (coding)
			{
			case CoefCoding::kEobEg:
			{
				/* 定位自然行序里最后一个非零系数，其后的槽一概不写；零槽照样占一格 EG(0) = 1 bit，
				 * 但不带符号位。不写零游程：游程那一格在「只有 DC」的块上是净亏
				 * （EG(游程 = 0) 那 1 bit 白花，而 DC 幅值本来就长）。 */
				int eob = -1;
				for (int i = 0; i < kBlockCoefs; ++i)
					if (qf[i] != 0) eob = i;
				if (eob < 0)
				{
					bw->Write(1u, 1u);	  // all_zero：整块只花这一 bit
					return;
				}
				bw->Write(0u, 1u);
				bw->Write(static_cast<uint32_t>(eob), kEobFieldBits);
				for (int i = 0; i <= eob; ++i)
				{
					const int32_t v = qf[i];
					const uint32_t mag = static_cast<uint32_t>(v < 0 ? -static_cast<int64_t>(v) : v);
					WriteEg(bw, mag);
					if (mag != 0u) bw->Write(v < 0 ? 1u : 0u, 1u);
				}
				return;
			}
			case CoefCoding::kFixed:
			default:
			{
				const uint8_t mag_bits = static_cast<uint8_t>(BlockBits(q, intra) - 1u);
				for (int i = 0; i < kBlockCoefs; ++i)
				{
					const int32_t v = qf[i];
					const uint32_t sign = (v < 0) ? 1u : 0u;
					const uint32_t mag = static_cast<uint32_t>(v < 0 ? -static_cast<int64_t>(v) : v);
					// 符号位在前，幅值位在后；|QF| = 0 时符号位仍写 0（法 0 不做零省略）
					bw->Write(sign, 1u);
					bw->Write(mag, mag_bits);
				}
				return;
			}
			}
		}

		VideoErr DecodeBlock(BitReader* br, uint8_t q, bool intra, CoefCoding coding, int32_t qf[kBlockCoefs])
		{
			if (br == nullptr) return VideoErr::kInvalidArgument;
			const int32_t cap = BlockQfMax(q, intra);
			for (int i = 0; i < kBlockCoefs; ++i) qf[i] = 0;
			switch (coding)
			{
			case CoefCoding::kEobEg:
			{
				uint32_t all_zero = 0u;
				if (! br->Read(&all_zero, 1u)) return VideoErr::kPayloadTooSmall;
				// 位图为 1 的块整块全零在本法里是写得出来的，语法合法 ⇒ 直接返回零系数
				if (all_zero != 0u) return VideoErr::kOk;
				uint32_t eob = 0u;
				if (! br->Read(&eob, kEobFieldBits)) return VideoErr::kPayloadTooSmall;
				for (uint32_t i = 0; i <= eob; ++i)
				{
					uint32_t mag = 0u;
					uint32_t sign = 0u;
					if (! ReadEg(br, &mag)) return VideoErr::kPayloadTooSmall;
					if (mag != 0u && ! br->Read(&sign, 1u)) return VideoErr::kPayloadTooSmall;
					if (static_cast<int64_t>(mag) > static_cast<int64_t>(cap)) return VideoErr::kValueRange;
					qf[i] = (sign != 0u) ? -static_cast<int32_t>(mag) : static_cast<int32_t>(mag);
				}
				// eob 的定义就是「最后一个非零系数的下标」，该位读回零即自相矛盾
				if (qf[eob] == 0) return VideoErr::kValueRange;
				return VideoErr::kOk;
			}
			case CoefCoding::kFixed:
			default:
			{
				const uint8_t mag_bits = static_cast<uint8_t>(BlockBits(q, intra) - 1u);
				bool nonzero = false;
				for (int i = 0; i < kBlockCoefs; ++i)
				{
					uint32_t sign = 0u;
					uint32_t mag = 0u;
					if (! br->Read(&sign, 1u)) return VideoErr::kPayloadTooSmall;
					if (! br->Read(&mag, mag_bits)) return VideoErr::kPayloadTooSmall;
					if (static_cast<int64_t>(mag) > static_cast<int64_t>(cap)) return VideoErr::kValueRange;
					qf[i] = (sign != 0u) ? -static_cast<int32_t>(mag) : static_cast<int32_t>(mag);
					if (qf[i] != 0) nonzero = true;
				}
				// 本函数只解位图里为 1 的块：全零块的对应位必为 0，位为 1 而 64 个系数全零不成立
				if (! nonzero) return VideoErr::kValueRange;
				return VideoErr::kOk;
			}
			}
		}

		VideoErr EncodeBlockData(uint8_t pattern, const int32_t qf[kBlocksPerMacroblock][kBlockCoefs], uint8_t q,
								 bool intra, CoefCoding coding, std::vector<uint8_t>* out)
		{
			if (out == nullptr) return VideoErr::kInvalidArgument;
			if ((pattern & ~kPatternMask6) != 0u) return VideoErr::kValueRange;
			BitWriter bw;
			const int present = PresentBlockCount(pattern);
			for (int k = 0; k < present; ++k)
			{
				int idx = 0;
				if (! BlockIndexOrdered(pattern, k, &idx)) return VideoErr::kValueRange;
				EncodeBlock(qf[idx], q, intra, coding, &bw);
			}
			// 每个宏块的块数据都以字节边界结束：位状态不跨宏块、不跨片
			bw.AlignToByte(0u);
			*out = bw.Bytes();
			return VideoErr::kOk;
		}

		VideoErr DecodeBlockData(uint8_t pattern, const uint8_t* data, size_t size, uint8_t q, bool intra,
								 CoefCoding coding, int32_t qf[kBlocksPerMacroblock][kBlockCoefs])
		{
			if (data == nullptr && size != 0u) return VideoErr::kInvalidArgument;
			if ((pattern & ~kPatternMask6) != 0u) return VideoErr::kValueRange;
			// 块模式位为 0 的块不占字节、出参恒为 0（按零残差处置）
			for (int b = 0; b < kBlocksPerMacroblock; ++b)
				for (int i = 0; i < kBlockCoefs; ++i) qf[b][i] = 0;
			const int present = PresentBlockCount(pattern);
			const uint32_t want = static_cast<uint32_t>(present) * BlockBytesMax(q, intra, coding);
			if (coding == CoefCoding::kFixed)
			{
				if (want != size) return VideoErr::kValueRange;
			}
			else
			{
				/* 变长：块数据只保证不超过上界。位图全 0 的 MB（整块跳过）合法长度就是 0，
				 * 定宽那侧的 want != size 天然接受它，改成上界时必须把这一支单独留着，
				 * 否则 P 帧的跳过 MB 会被判成码流不成立。 */
				if (want == 0u)
				{
					if (size != 0u) return VideoErr::kValueRange;
				}
				else if (size == 0u || size > want)
				{
					return VideoErr::kValueRange;
				}
			}
			BitReader br(data, size);
			for (int k = 0; k < present; ++k)
			{
				int idx = 0;
				if (! BlockIndexOrdered(pattern, k, &idx)) return VideoErr::kValueRange;
				const VideoErr e = DecodeBlock(&br, q, intra, coding, qf[idx]);
				if (e != VideoErr::kOk) return e;
			}
			/* 定宽直写下不该有剩余位；变长时块边界自终止，末尾只允许 0..7 位的字节对齐填充。
			 * 剩下整字节 = 与本法解出的块界对不上，与 block_size 说谎同档。 */
			if (coding == CoefCoding::kFixed)
			{
				if (br.BitsLeft() != 0u) return VideoErr::kValueRange;
			}
			else if (br.BitsLeft() >= 8u)
			{
				return VideoErr::kValueRange;
			}
			return VideoErr::kOk;
		}

		bool BlockSizeMatches(uint8_t pattern, uint32_t block_size, uint8_t q, bool intra, CoefCoding coding) noexcept
		{
			const uint32_t want = static_cast<uint32_t>(PresentBlockCount(pattern)) * BlockBytesMax(q, intra, coding);
			if (coding == CoefCoding::kFixed) return block_size == want;
			// present = 0 时块数据就是零长度；其余只卡上界
			if (want == 0u) return block_size == 0u;
			return block_size >= 1u && block_size <= want;
		}

	}	 // namespace video
}	 // namespace clv
