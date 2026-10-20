#include "macroblock.h"

#include "../core/varint.h"
#include "annex_b_tables_p.h"
#include "block.h"

#include <cstddef>

namespace clv
{
	namespace video
	{

		namespace
		{

			constexpr uint8_t kMaxMcbpcBits = 9u;
			constexpr uint8_t kMaxCbpyBits = 6u;
			constexpr uint8_t kDquantBits = 2u;
			constexpr size_t kMaxVarintScratch = 10u;

			const McbpcEntry* McbpcTable(uint8_t vop_type, size_t* count)
			{
				if (vop_type == kVopTypeI)
				{
					*count = std::size(kMcbpcIVop);
					return kMcbpcIVop;
				}
				if (vop_type == kVopTypeP)
				{
					*count = std::size(kMcbpcPVop);
					return kMcbpcPVop;
				}
				*count = 0u;
				return nullptr;
			}

			// 只在当前 VOP 类型那张表里找：表里的 code 是码字整数值，低 code_len 位有效
			const McbpcEntry* FindMcbpc(uint8_t vop_type, uint8_t len, uint16_t value)
			{
				size_t n = 0u;
				const McbpcEntry* t = McbpcTable(vop_type, &n);
				for (size_t i = 0; i < n; ++i)
					if (t[i].code_len == len && t[i].code == value) return &t[i];
				return nullptr;
			}

			const McbpcEntry* FindMcbpcByFields(uint8_t vop_type, int8_t mb_type, uint8_t cbpc)
			{
				size_t n = 0u;
				const McbpcEntry* t = McbpcTable(vop_type, &n);
				for (size_t i = 0; i < n; ++i)
					if (t[i].mb_type == mb_type && t[i].cbpc == cbpc) return &t[i];
				return nullptr;
			}

			const CbpyEntry* FindCbpyByValue(uint8_t len, uint16_t value)
			{
				for (size_t i = 0; i < std::size(kCbpyFourBlocks); ++i)
					if (kCbpyFourBlocks[i].code_len == len && kCbpyFourBlocks[i].code == value)
						return &kCbpyFourBlocks[i];
				return nullptr;
			}

			const CbpyEntry* FindCbpyByPattern(bool intra, uint8_t pattern)
			{
				for (size_t i = 0; i < std::size(kCbpyFourBlocks); ++i)
				{
					const uint8_t v = intra ? kCbpyFourBlocks[i].intra : kCbpyFourBlocks[i].inter;
					if (v == pattern) return &kCbpyFourBlocks[i];
				}
				return nullptr;
			}

			// 逐位读到表命中为止：码字集前缀自由，命中即定，不需要回溯
			template<typename Entry, typename Finder>
			VideoErr ReadVlc(BitReader* br, uint8_t max_bits, Finder finder, const Entry** out)
			{
				uint32_t value = 0u;
				for (uint8_t len = 1u; len <= max_bits; ++len)
				{
					uint8_t bit = 0u;
					if (! br->ReadBit(&bit)) return VideoErr::kPayloadTooSmall;
					value = (value << 1u) | bit;
					const Entry* e = finder(static_cast<uint8_t>(len), static_cast<uint16_t>(value));
					if (e != nullptr)
					{
						*out = e;
						return VideoErr::kOk;
					}
				}
				return VideoErr::kHeaderBad;
			}

			void WriteVarint(BitWriter* bw, uint64_t v)
			{
				uint8_t scratch[kMaxVarintScratch];
				const size_t n = clv::core::EncodeVarint(v, scratch);
				bw->WriteBytes(scratch, n);
			}

			void WriteZigZag(BitWriter* bw, int64_t v)
			{
				uint8_t scratch[kMaxVarintScratch];
				const size_t n = clv::core::EncodeZigZag(v, scratch);
				bw->WriteBytes(scratch, n);
			}

			// 逐字节读到终止位；非最短形式按码流不成立，与 VOP 头那两处同口径
			VideoErr ReadVarint(BitReader* br, uint64_t* out)
			{
				uint8_t scratch[kMaxVarintScratch];
				for (size_t i = 0; i < kMaxVarintScratch; ++i)
				{
					if (! br->ReadBytes(scratch + i, 1u)) return VideoErr::kPayloadTooSmall;
					if ((scratch[i] & 0x80u) == 0u)
					{
						uint64_t v = 0u;
						if (clv::core::DecodeVarint(scratch, i + 1u, &v) == 0u) return VideoErr::kHeaderBad;
						if (! clv::core::IsShortestVarint(scratch, i + 1u)) return VideoErr::kHeaderBad;
						*out = v;
						return VideoErr::kOk;
					}
				}
				return VideoErr::kHeaderBad;
			}

			VideoErr ReadZigZag(BitReader* br, int64_t* out)
			{
				uint64_t raw = 0u;
				const VideoErr e = ReadVarint(br, &raw);
				if (e != VideoErr::kOk) return e;
				*out = static_cast<int64_t>((raw >> 1u) ^ -static_cast<int64_t>(raw & 1u));
				return VideoErr::kOk;
			}

			int32_t Med3(int32_t a, int32_t b, int32_t c)
			{
				if (a <= b)
				{
					if (b <= c) return b;
					return a <= c ? c : a;
				}
				if (a <= c) return a;
				return b <= c ? c : b;
			}

			/* 预测值（int32）+ 线上残差（int64）→ 绝对矢量分量：先在 int64 里定界再落回 32 位，
			 * 全程不出现有符号溢出；界外按饱和收，口径同 SaturateVectorComponent。 */
			int32_t AddMvComponent(int32_t base, int64_t delta) noexcept
			{
				if (delta >= static_cast<int64_t>(INT32_MAX) - static_cast<int64_t>(base)) return INT32_MAX;
				if (delta <= static_cast<int64_t>(INT32_MIN) - static_cast<int64_t>(base)) return INT32_MIN;
				return static_cast<int32_t>(static_cast<int64_t>(base) + delta);
			}

			void WriteMvResidual(BitWriter* bw, MotionVector got, MotionVector want)
			{
				WriteZigZag(bw, static_cast<int64_t>(got.h) - want.h);
				WriteZigZag(bw, static_cast<int64_t>(got.v) - want.v);
			}

			VideoErr ReadMvResidual(BitReader* br, MotionVector want, MotionVector* got)
			{
				int64_t dh = 0, dv = 0;
				const VideoErr eh = ReadZigZag(br, &dh);
				if (eh != VideoErr::kOk) return eh;
				const VideoErr ev = ReadZigZag(br, &dv);
				if (ev != VideoErr::kOk) return ev;
				got->h = AddMvComponent(want.h, dh);
				got->v = AddMvComponent(want.v, dv);
				return VideoErr::kOk;
			}

		}	 // namespace

		bool IsIntraMacroblock(int8_t mb_type) noexcept { return mb_type == kMbTypeIntra; }

		int32_t SaturateVectorComponent(int64_t v) noexcept
		{
			if (v > static_cast<int64_t>(INT32_MAX)) return INT32_MAX;
			if (v < static_cast<int64_t>(INT32_MIN)) return INT32_MIN;
			return static_cast<int32_t>(v);
		}

		int8_t MbDquantFromBits(uint8_t two_bits) noexcept
		{
			const uint8_t b = two_bits & 0x03u;
			return static_cast<int8_t>(b < 2u ? b : b - 4u);
		}

		uint8_t MbDquantToBits(int8_t d_mb) noexcept
		{
			return static_cast<uint8_t>(static_cast<unsigned>(d_mb) & 0x03u);
		}

		MotionVector MedianPredictor(MotionVector a, MotionVector b, MotionVector c) noexcept
		{
			MotionVector p;
			p.h = Med3(a.h, b.h, c.h);
			p.v = Med3(a.v, b.v, c.v);
			return p;
		}

		size_t BvopMvdCount(uint8_t mb_type) noexcept
		{
			switch (mb_type)
			{
			case kBvopDirect: return 0u;
			case kBvopInterpolate: return 2u;
			case kBvopBackward:
			case kBvopForward: return 1u;
			default: return 0u;
			}
		}

		VideoErr EncodeMacroblockHeader(BitWriter* bw, uint8_t vop_type, const MacroblockHeader& mb,
										const MotionVector& pred)
		{
			if (bw == nullptr) return VideoErr::kInvalidArgument;
			if (vop_type == kVopTypeB) return VideoErr::kToolExcluded;
			if (vop_type != kVopTypeI && vop_type != kVopTypeP) return VideoErr::kInvalidArgument;

			if (mb.not_coded)
			{
				// I-VOP 没有 skip 语义：帧内全零宏块只能走块模式全零，不得改走 not_coded
				if (vop_type == kVopTypeI) return VideoErr::kValueRange;
				if (mb.block_size != 0u) return VideoErr::kValueRange;	  // 跳过全部字段，块数据无处安放
				bw->Write(1u, 1u);
				return VideoErr::kOk;
			}
			bw->Write(0u, 1u);

			if (mb.cbpc > 3u || mb.cbpy > 15u) return VideoErr::kValueRange;
			if (mb.dquant < kMbDquantMin || mb.dquant > kMbDquantMax) return VideoErr::kValueRange;
			// 块模式非全零时不许零长度；全零宏块的块数据本就不在场，零长度合法
			if (mb.block_size == 0u && (mb.cbpc | mb.cbpy) != 0u) return VideoErr::kValueRange;
			if (mb.mb_type == kMbTypeInter4V) return VideoErr::kToolExcluded;

			const McbpcEntry* m = FindMcbpcByFields(vop_type, mb.mb_type, mb.cbpc);
			if (m == nullptr) return VideoErr::kInvalidArgument;
			bw->Write(m->code, m->code_len);

			const CbpyEntry* c = FindCbpyByPattern(IsIntraMacroblock(mb.mb_type), mb.cbpy);
			if (c == nullptr) return VideoErr::kInvalidArgument;
			bw->Write(c->code, c->code_len);

			bw->Write(MbDquantToBits(mb.dquant), kDquantBits);
			// 帧间宏块在这里接 motion_vector()（两条 zigzag varint 残差）；
			// 帧内宏块与 not_coded 宏块都不带 —— 后者在这里已经提前 return 了。
			if (! IsIntraMacroblock(mb.mb_type)) WriteMvResidual(bw, mb.mv, pred);
			WriteVarint(bw, mb.block_size);
			return VideoErr::kOk;
		}

		VideoErr DecodeMacroblockHeader(BitReader* br, uint8_t vop_type, MacroblockHeader* out,
										const MotionVector& pred)
		{
			if (br == nullptr || out == nullptr) return VideoErr::kInvalidArgument;
			if (vop_type == kVopTypeB) return VideoErr::kToolExcluded;
			if (vop_type != kVopTypeI && vop_type != kVopTypeP) return VideoErr::kInvalidArgument;

			uint8_t flag = 0u;
			if (! br->ReadBit(&flag)) return VideoErr::kPayloadTooSmall;
			MacroblockHeader mb;
			if (flag != 0u)
			{
				// I-VOP 侧不存在的位型：读到即码流不成立（写方不会产出，见 EncodeMacroblockHeader 同处判据）
				if (vop_type == kVopTypeI) return VideoErr::kHeaderBad;
				*out = mb;
				out->not_coded = true;
				return VideoErr::kOk;
			}

			const McbpcEntry* m = nullptr;
			const VideoErr me = ReadVlc(
				br, kMaxMcbpcBits, [vop_type](uint8_t len, uint16_t v) { return FindMcbpc(vop_type, len, v); }, &m);
			if (me != VideoErr::kOk) return me;
			// Stuffing 行（mb_type = -1）与 4MV 行都是表里留着专供识别的排除集
			if (m->mb_type < 0 || m->mb_type == kMbTypeInter4V) return VideoErr::kToolExcluded;

			const CbpyEntry* c = nullptr;
			const VideoErr ce = ReadVlc(br, kMaxCbpyBits, FindCbpyByValue, &c);
			if (ce != VideoErr::kOk) return ce;

			uint32_t dq = 0u;
			if (! br->Read(&dq, kDquantBits)) return VideoErr::kPayloadTooSmall;

			// 帧间宏块的 motion_vector() 就在 dquant 之后（not_coded 已提前 return）
			if (! IsIntraMacroblock(m->mb_type))
			{
				MotionVector got {};
				const VideoErr mv_err = ReadMvResidual(br, pred, &got);
				if (mv_err != VideoErr::kOk) return mv_err;
				mb.mv = got;
			}

			uint64_t bs = 0u;
			const VideoErr be = ReadVarint(br, &bs);
			if (be != VideoErr::kOk) return be;
			// 剩余不足 bs 字节：块数据越出载荷
			if (bs > br->BitsLeft() / 8u) return VideoErr::kPayloadTooSmall;

			mb.mb_type = m->mb_type;
			mb.cbpc = m->cbpc;
			mb.cbpy = IsIntraMacroblock(m->mb_type) ? c->intra : c->inter;
			// 零长度只在块模式全零时合法，判定要等块模式解出来
			if (bs == 0u && (mb.cbpc | mb.cbpy) != 0u) return VideoErr::kValueRange;
			mb.dquant = MbDquantFromBits(static_cast<uint8_t>(dq));
			mb.block_size = static_cast<uint32_t>(bs);
			*out = mb;
			return VideoErr::kOk;
		}

		VideoErr EncodeBvopMacroblockHeader(BitWriter* bw, const BvopMacroblockHeader& mb, const BvopPrediction& pred)
		{
			if (bw == nullptr) return VideoErr::kInvalidArgument;
			if (mb.mb_type > kBvopMbTypeMax) return VideoErr::kToolExcluded;
			if (mb.cbpy > kPatternMask6)
				return VideoErr::kValueRange;	 // 6 bit 全块模式：bit5..2 亮度、bit1 Cb、bit0 Cr
			if (mb.not_coded && mb.cbpy != 0u) return VideoErr::kValueRange;
			if (mb.not_coded && mb.block_size != 0u) return VideoErr::kValueRange;
			if (! mb.not_coded && mb.block_size == 0u && mb.cbpy != 0u) return VideoErr::kValueRange;

			bw->Write(mb.not_coded ? 1u : 0u, 8u);
			bw->Write(mb.mb_type, 8u);

			const size_t n = BvopMvdCount(mb.mb_type);
			if (n >= 2u)
			{
				WriteMvResidual(bw, mb.forward, pred.forward);
				WriteMvResidual(bw, mb.backward, pred.backward);
			}
			else if (n == 1u && mb.mb_type == kBvopBackward)
			{
				WriteMvResidual(bw, mb.backward, pred.backward);
			}
			else if (n == 1u)
			{
				WriteMvResidual(bw, mb.forward, pred.forward);
			}
			WriteVarint(bw, mb.cbpy);
			// block_size 只在有块数据时在场
			if (! mb.not_coded) WriteVarint(bw, mb.block_size);
			return VideoErr::kOk;
		}

		VideoErr DecodeBvopMacroblockHeader(BitReader* br, BvopMacroblockHeader* out, const BvopPrediction& pred)
		{
			if (br == nullptr || out == nullptr) return VideoErr::kInvalidArgument;

			uint32_t nc = 0u;
			if (! br->Read(&nc, 8u)) return VideoErr::kPayloadTooSmall;
			if (nc > 1u) return VideoErr::kHeaderBad;

			uint32_t mt = 0u;
			if (! br->Read(&mt, 8u)) return VideoErr::kPayloadTooSmall;
			if (mt > kBvopMbTypeMax) return VideoErr::kToolExcluded;

			BvopMacroblockHeader mb;
			mb.not_coded = nc != 0u;
			mb.mb_type = static_cast<uint8_t>(mt);

			const size_t n = BvopMvdCount(mb.mb_type);
			if (n >= 2u)
			{
				const VideoErr ef = ReadMvResidual(br, pred.forward, &mb.forward);
				if (ef != VideoErr::kOk) return ef;
				const VideoErr eb = ReadMvResidual(br, pred.backward, &mb.backward);
				if (eb != VideoErr::kOk) return eb;
			}
			else if (n == 1u && mb.mb_type == kBvopBackward)
			{
				const VideoErr e = ReadMvResidual(br, pred.backward, &mb.backward);
				if (e != VideoErr::kOk) return e;
			}
			else if (n == 1u)
			{
				const VideoErr e = ReadMvResidual(br, pred.forward, &mb.forward);
				if (e != VideoErr::kOk) return e;
			}

			uint64_t cbpy = 0u;
			const VideoErr e = ReadVarint(br, &cbpy);
			if (e != VideoErr::kOk) return e;
			if (cbpy > kPatternMask6) return VideoErr::kValueRange;
			if (mb.not_coded && cbpy != 0u) return VideoErr::kHeaderBad;
			mb.cbpy = static_cast<uint8_t>(cbpy);

			if (! mb.not_coded)
			{
				uint64_t bs = 0u;
				const VideoErr be = ReadVarint(br, &bs);
				if (be != VideoErr::kOk) return be;
				if (bs > br->BitsLeft() / 8u) return VideoErr::kPayloadTooSmall;
				// 零长度只在位图全零时合法（cbpy 已在上面解出）
				if (bs == 0u && mb.cbpy != 0u) return VideoErr::kValueRange;
				mb.block_size = static_cast<uint32_t>(bs);
			}

			*out = mb;
			return VideoErr::kOk;
		}

		namespace
		{

			/* B-VOP 的预测器状态：已解码宏块的绝对矢量 + 该宏块带了哪个方向。
			 * 无效候选（越界、缺位、邻居不带该方向）按零矢量参与中值，I / P 侧同一套规则。 */
			struct BvopState
			{
				std::vector<MotionVector> forward;
				std::vector<MotionVector> backward;
				std::vector<uint8_t> has;	 // bit0 = 带前向，bit1 = 带后向
			};

			MotionVector BvopNeighbor(const BvopState& st, int64_t idx, bool fwd)
			{
				if (idx < 0 || static_cast<size_t>(idx) >= st.has.size()) return MotionVector {};
				const size_t i = static_cast<size_t>(idx);
				if ((st.has[i] & (fwd ? 1u : 2u)) == 0u) return MotionVector {};
				return fwd ? st.forward[i] : st.backward[i];
			}

			BvopPrediction BvopPredictAt(const BvopState& st, uint32_t mb_width, size_t i)
			{
				const int64_t col = static_cast<int64_t>(i % mb_width);
				const bool above = (i >= mb_width);
				const int64_t up = above ? static_cast<int64_t>(i) - mb_width : -1;
				const int64_t left = col > 0 ? static_cast<int64_t>(i) - 1 : -1;
				const int64_t up_right = (above && col + 1 < mb_width) ? up + 1 : -1;
				BvopPrediction p;
				p.forward = MedianPredictor(BvopNeighbor(st, left, true), BvopNeighbor(st, up, true),
											BvopNeighbor(st, up_right, true));
				p.backward = MedianPredictor(BvopNeighbor(st, left, false), BvopNeighbor(st, up, false),
											 BvopNeighbor(st, up_right, false));
				return p;
			}

			void BvopPush(BvopState& st, const BvopMacroblockHeader& h)
			{
				st.forward.push_back(h.forward);
				st.backward.push_back(h.backward);
				uint8_t mask = 0u;
				if (h.mb_type == kBvopInterpolate || h.mb_type == kBvopForward) mask |= 1u;
				if (h.mb_type == kBvopInterpolate || h.mb_type == kBvopBackward) mask |= 2u;
				st.has.push_back(mask);
			}

		}	 // namespace

		/* I / P 侧的预测器状态：每宏块 1 个矢量（帧间宏块各带一个），故只有一份。
		 * 失效规则与 B-VOP 那套同源：越界、缺位、邻居是帧内 → 按零参与中值。
		 * not_coded 的宏块**参与**，值是零 —— 码流里它不带 motion_vector()，按零矢量处理，
		 * 与「帧内宏块压根没有矢量」是两件事，故这里 has 位给了 1。 */
		namespace
		{

			struct IpState
			{
				std::vector<MotionVector> mv;
				std::vector<uint8_t> has;
			};

			MotionVector IpNeighbor(const IpState& st, int64_t idx)
			{
				if (idx < 0 || static_cast<size_t>(idx) >= st.has.size()) return MotionVector {};
				if (st.has[static_cast<size_t>(idx)] == 0u) return MotionVector {};
				return st.mv[static_cast<size_t>(idx)];
			}

			MotionVector IpPredictAt(const IpState& st, uint32_t mb_width, size_t i)
			{
				const int64_t col = static_cast<int64_t>(i % mb_width);
				const bool above = (i >= mb_width);
				const int64_t up = above ? static_cast<int64_t>(i) - mb_width : -1;
				const int64_t left = col > 0 ? static_cast<int64_t>(i) - 1 : -1;
				const int64_t up_right = (above && col + 1 < mb_width) ? up + 1 : -1;
				return MedianPredictor(IpNeighbor(st, left), IpNeighbor(st, up), IpNeighbor(st, up_right));
			}

			void IpPush(IpState& st, const MacroblockHeader& h)
			{
				const bool inter = ! IsIntraMacroblock(h.mb_type);
				st.mv.push_back((inter && ! h.not_coded) ? h.mv : MotionVector {});
				st.has.push_back(inter ? 1u : 0u);
			}

		}	 // namespace

		VideoErr EncodeMacroblockLayer(uint8_t vop_type, uint32_t mb_width, const std::vector<Macroblock>& mbs,
									   std::vector<uint8_t>* out)
		{
			if (out == nullptr) return VideoErr::kInvalidArgument;
			if (mb_width == 0u) return VideoErr::kInvalidArgument;
			BitWriter bw;
			IpState st;
			for (size_t i = 0u; i < mbs.size(); ++i)
			{
				const Macroblock& m = mbs[i];
				if (m.head.block_size != m.block.size()) return VideoErr::kInvalidArgument;
				if (m.head.not_coded && ! m.block.empty()) return VideoErr::kValueRange;
				const VideoErr e = EncodeMacroblockHeader(&bw, vop_type, m.head, IpPredictAt(st, mb_width, i));
				if (e != VideoErr::kOk) return e;
				if (! m.head.not_coded) bw.WriteBytes(m.block.data(), m.block.size());
				IpPush(st, m.head);
			}
			bw.AlignToByte(0u);
			*out = bw.Bytes();
			return VideoErr::kOk;
		}

		VideoErr DecodeMacroblockRun(const uint8_t* data, size_t size, uint8_t vop_type, uint32_t mb_width,
									 uint64_t count, std::vector<Macroblock>* out, size_t* consumed)
		{
			if (out == nullptr) return VideoErr::kInvalidArgument;
			if (data == nullptr && size != 0u) return VideoErr::kInvalidArgument;
			if (mb_width == 0u) return VideoErr::kInvalidArgument;
			BitReader br(data, size);
			IpState st;
			std::vector<Macroblock> mbs;
			for (uint64_t i = 0; i < count; ++i)
			{
				Macroblock m;
				const VideoErr e = DecodeMacroblockHeader(&br, vop_type, &m.head, IpPredictAt(st, mb_width, i));
				if (e != VideoErr::kOk) return e;
				if (! m.head.not_coded)
				{
					m.block.resize(m.head.block_size);
					if (! br.ReadBytes(m.block.data(), m.block.size())) return VideoErr::kPayloadTooSmall;
				}
				IpPush(st, m.head);
				mbs.push_back(std::move(m));
			}
			const size_t bits = size * 8u - br.BitsLeft();
			if (consumed != nullptr) *consumed = (bits + 7u) / 8u;
			*out = std::move(mbs);
			return VideoErr::kOk;
		}

		VideoErr DecodeMacroblockLayer(const uint8_t* data, size_t size, uint8_t vop_type, uint32_t mb_width,
									   uint64_t mb_count, std::vector<Macroblock>* out)
		{
			std::vector<Macroblock> mbs;
			size_t used = 0u;
			const VideoErr e = DecodeMacroblockRun(data, size, vop_type, mb_width, mb_count, &mbs, &used);
			if (e != VideoErr::kOk) return e;
			// 解满 mb_count 后还有整字节剩余 = 码流不成立；不足一字节的是补齐位
			if (used < size) return VideoErr::kHeaderBad;
			*out = std::move(mbs);
			return VideoErr::kOk;
		}

		VideoErr EncodeBvopLayer(uint32_t mb_width, const std::vector<BvopMacroblock>& mbs, std::vector<uint8_t>* out)
		{
			if (out == nullptr) return VideoErr::kInvalidArgument;
			if (mb_width == 0u) return VideoErr::kInvalidArgument;
			BitWriter bw;
			BvopState st;
			for (size_t i = 0; i < mbs.size(); ++i)
			{
				const BvopMacroblock& m = mbs[i];
				if (m.head.block_size != m.block.size()) return VideoErr::kInvalidArgument;
				if (m.head.not_coded && ! m.block.empty()) return VideoErr::kValueRange;
				const VideoErr e = EncodeBvopMacroblockHeader(&bw, m.head, BvopPredictAt(st, mb_width, i));
				if (e != VideoErr::kOk) return e;
				if (! m.head.not_coded) bw.WriteBytes(m.block.data(), m.block.size());
				BvopPush(st, m.head);
			}
			bw.AlignToByte(0u);
			*out = bw.Bytes();
			return VideoErr::kOk;
		}

		VideoErr DecodeBvopRun(const uint8_t* data, size_t size, uint32_t mb_width, uint64_t count,
							   std::vector<BvopMacroblock>* out, size_t* consumed)
		{
			if (out == nullptr) return VideoErr::kInvalidArgument;
			if (data == nullptr && size != 0u) return VideoErr::kInvalidArgument;
			if (mb_width == 0u) return VideoErr::kInvalidArgument;
			BitReader br(data, size);
			BvopState st;
			std::vector<BvopMacroblock> mbs;
			for (uint64_t i = 0; i < count; ++i)
			{
				BvopMacroblock m;
				const VideoErr e = DecodeBvopMacroblockHeader(&br, &m.head, BvopPredictAt(st, mb_width, i));
				if (e != VideoErr::kOk) return e;
				if (! m.head.not_coded)
				{
					m.block.resize(m.head.block_size);
					if (! br.ReadBytes(m.block.data(), m.block.size())) return VideoErr::kPayloadTooSmall;
				}
				BvopPush(st, m.head);
				mbs.push_back(std::move(m));
			}
			const size_t bits = size * 8u - br.BitsLeft();
			if (consumed != nullptr) *consumed = (bits + 7u) / 8u;
			*out = std::move(mbs);
			return VideoErr::kOk;
		}

		VideoErr DecodeBvopLayer(const uint8_t* data, size_t size, uint32_t mb_width, uint64_t mb_count,
								 std::vector<BvopMacroblock>* out)
		{
			std::vector<BvopMacroblock> mbs;
			size_t used = 0u;
			const VideoErr e = DecodeBvopRun(data, size, mb_width, mb_count, &mbs, &used);
			if (e != VideoErr::kOk) return e;
			if (used < size) return VideoErr::kHeaderBad;
			*out = std::move(mbs);
			return VideoErr::kOk;
		}

	}	 // namespace video
}	 // namespace clv

