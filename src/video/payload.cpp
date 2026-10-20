#include "payload.h"

#include "headers.h"

#include <utility>

namespace clv
{
	namespace video
	{

		namespace
		{

			/* 载荷的形状判点：帧型必须是 I / P / B 之一、宏块序列的件数必须等于帧头的 mb_count、
			 * B 帧只带 bvops、非 B 帧只带 macroblocks。
			 * 写侧两条路都先过这一道：单片走 EncodeVopPayload、分片走 EncodeVopSlices 的逐片支 ——
			 * 判点只这一处，切片那侧不必先把整帧编一遍才拿到同样的结论。 */
			VideoErr CheckPayloadShape(uint32_t mb_width, const VopPayload& in, bool* is_b)
			{
				const bool b = (in.head.type == kVopTypeB);
				*is_b = b;
				if (! b && in.head.type != kVopTypeI && in.head.type != kVopTypeP) return VideoErr::kInvalidArgument;
				const size_t n = b ? in.bvops.size() : in.macroblocks.size();
				if (in.head.mb_count != n) return VideoErr::kInvalidArgument;
				if (b && ! in.macroblocks.empty()) return VideoErr::kInvalidArgument;
				if (! b && ! in.bvops.empty()) return VideoErr::kInvalidArgument;
				if (b && mb_width == 0u) return VideoErr::kInvalidArgument;
				return VideoErr::kOk;
			}

		}	 // namespace

		VideoErr EncodeVopPayload(uint8_t quant_precision, uint32_t mb_width, const VopPayload& in,
								  std::vector<uint8_t>* out)
		{
			if (out == nullptr) return VideoErr::kInvalidArgument;
			bool is_b = false;
			const VideoErr shape = CheckPayloadShape(mb_width, in, &is_b);
			if (shape != VideoErr::kOk) return shape;

			std::vector<uint8_t> bytes;
			if (! EncodeVopHeader(in.head, quant_precision, &bytes)) return VideoErr::kValueRange;

			std::vector<uint8_t> layer;
			const VideoErr e = is_b ? EncodeBvopLayer(mb_width, in.bvops, &layer) :
									  EncodeMacroblockLayer(in.head.type, mb_width, in.macroblocks, &layer);
			if (e != VideoErr::kOk) return e;

			bytes.insert(bytes.end(), layer.begin(), layer.end());
			*out = std::move(bytes);
			return VideoErr::kOk;
		}

		VideoErr EncodeVopSlices(uint8_t quant_precision, uint32_t mb_width, uint32_t frame_id, uint64_t slice_mb_count,
								 const VopPayload& in, std::vector<std::vector<uint8_t>>* slices)
		{
			if (slices == nullptr) return VideoErr::kInvalidArgument;
			slices->clear();

			bool is_b = false;
			const VideoErr shape = CheckPayloadShape(mb_width, in, &is_b);
			if (shape != VideoErr::kOk) return shape;

			std::vector<uint8_t> vopb;
			if (! EncodeVopHeader(in.head, quant_precision, &vopb)) return VideoErr::kValueRange;

			const uint64_t total = in.head.mb_count;
			if (slice_mb_count == 0u || slice_mb_count >= total)
			{
				// 整帧一片：帧头之后直接接整个宏块层，不发片头（与 EncodeVopPayload 同一条式子）
				std::vector<uint8_t> layer;
				const VideoErr e = is_b ? EncodeBvopLayer(mb_width, in.bvops, &layer) :
										  EncodeMacroblockLayer(in.head.type, mb_width, in.macroblocks, &layer);
				if (e != VideoErr::kOk) return e;
				vopb.insert(vopb.end(), layer.begin(), layer.end());
				slices->push_back(std::move(vopb));
				return VideoErr::kOk;
			}
			const uint64_t n = (total + slice_mb_count - 1u) / slice_mb_count;
			if (n > kMaxFragmentCount) return VideoErr::kValueRange;	// 分片数上限

			for (uint64_t i = 0; i < n; ++i)
			{
				const uint64_t begin = i * slice_mb_count;
				const uint64_t cnt = (total - begin < slice_mb_count) ? (total - begin) : slice_mb_count;

				std::vector<uint8_t> layer;
				if (is_b)
				{
					const std::vector<BvopMacroblock> g(in.bvops.begin() + static_cast<long>(begin),
														in.bvops.begin() + static_cast<long>(begin + cnt));
					const VideoErr e = EncodeBvopLayer(mb_width, g, &layer);
					if (e != VideoErr::kOk) return e;
				}
				else
				{
					const std::vector<Macroblock> g(in.macroblocks.begin() + static_cast<long>(begin),
													in.macroblocks.begin() + static_cast<long>(begin + cnt));
					const VideoErr e = EncodeMacroblockLayer(in.head.type, mb_width, g, &layer);
					if (e != VideoErr::kOk) return e;
				}

				SliceHeader sh;
				sh.frame_id = frame_id;
				sh.fragment_count = static_cast<uint16_t>(n);
				sh.is_first = (i == 0u);
				sh.is_last = (i + 1u == n);
				sh.has_vop_quant = sh.is_first;
				sh.vop_type = in.head.type;
				sh.vop_quant = in.head.quant;
				sh.first_mb = begin;
				sh.mb_run = cnt;
				std::vector<uint8_t> hdr;
				if (! EncodeSliceHeader(sh, quant_precision, &hdr)) return VideoErr::kValueRange;

				std::vector<uint8_t> piece = hdr;
				if (i == 0u) piece.insert(piece.end(), vopb.begin(), vopb.end());	 // 完整 VOP 头只落在首片
				piece.insert(piece.end(), layer.begin(), layer.end());
				slices->push_back(std::move(piece));
			}
			return VideoErr::kOk;
		}

		namespace
		{

			/* 按帧类型解 count 个宏块，回报吃掉的字节数。 */
			VideoErr DecodeRunByType(const uint8_t* data, size_t size, uint8_t vop_type, uint32_t mb_width,
									 uint64_t count, VopPayload* body, size_t* consumed)
			{
				if (vop_type == kVopTypeB)
				{
					std::vector<BvopMacroblock> part;
					const VideoErr e = DecodeBvopRun(data, size, mb_width, count, &part, consumed);
					if (e == VideoErr::kOk) body->bvops.insert(body->bvops.end(), part.begin(), part.end());
					return e;
				}
				std::vector<Macroblock> part;
				const VideoErr e = DecodeMacroblockRun(data, size, vop_type, mb_width, count, &part, consumed);
				if (e == VideoErr::kOk) body->macroblocks.insert(body->macroblocks.end(), part.begin(), part.end());
				return e;
			}

		}	 // namespace

		/* 整帧入口：片数由调用方给出，因此「解完宏块序列后还剩整字节」是有意义的判点 ——
		 * 剩下了就是片数报错了。与下面的 DecodeVopPayload 是两档用途，不是同一件事写两遍：
		 * 那一档不带片数，只能按载荷本身判，也就没有这条对账。 */
		VideoErr DecodeVopFrame(const uint8_t* data, size_t size, uint8_t quant_precision, uint32_t mb_width,
								uint64_t fragment_count, DecodedVop* out, size_t* consumed)
		{
			if (out == nullptr) return VideoErr::kInvalidArgument;
			if (data == nullptr && size != 0u) return VideoErr::kInvalidArgument;
			// 一帧至少一片是物理事实；0 = 调用方没等到末片就喂，属违约而非「未知」
			if (fragment_count == 0u || fragment_count > kMaxFragmentCount) return VideoErr::kValueRange;

			DecodedVop d;

			if (fragment_count == 1u)
			{
				VopHeader head;
				size_t used = 0u;
				if (! DecodeVopHeader(data, size, quant_precision, &head, &used)) return VideoErr::kHeaderBad;
				d.body.head = head;
				size_t run_used = 0u;
				const VideoErr e =
					DecodeRunByType(data + used, size - used, head.type, mb_width, head.mb_count, &d.body, &run_used);
				if (e != VideoErr::kOk) return e;
				// 还剩下整字节 = 实际不止一片而调用方报了一片：传错片数
				if (used + run_used != size) return VideoErr::kValueRange;
				d.slice_count = 1u;
				if (consumed != nullptr) *consumed = size;
				*out = std::move(d);
				return VideoErr::kOk;
			}

			size_t pos = 0u;
			VopHeader head;
			uint64_t runs = 0u;
			for (uint64_t i = 0; i < fragment_count; ++i)
			{
				SliceHeader sh;
				sh.mb_run = ~0ull;	  // 哨兵：片头没解到这一段就保持非 0，别把「读不尽」误分派成「空片」
				size_t hdr_used = 0u;
				// 首末片位的权威 = 传入的片数本身（容器已按 fragment_index 升序拼好，见调用方契约）
				if (! DecodeSliceHeader(data + pos, size - pos, quant_precision, i == 0u, i + 1u == fragment_count, &sh,
										&hdr_used))
				{
					// 空片（该片宏块数 = 0）是值域不成立；读不尽或携带位矛盾才是片头不成立
					if (sh.mb_run == 0u) return VideoErr::kValueRange;
					return VideoErr::kSliceHeaderBad;
				}
				if (static_cast<uint64_t>(sh.fragment_count) != fragment_count) return VideoErr::kValueRange;
				pos += hdr_used;

				if (i == 0u)
				{
					size_t vop_used = 0u;
					if (! DecodeVopHeader(data + pos, size - pos, quant_precision, &head, &vop_used))
						return VideoErr::kHeaderBad;
					d.body.head = head;
					pos += vop_used;
				}
				if (sh.first_mb != runs) return VideoErr::kSliceSequence;	 // 片必须按宏块光栅序首尾相接
				runs += sh.mb_run;

				size_t run_used = 0u;
				const VideoErr e =
					DecodeRunByType(data + pos, size - pos, head.type, mb_width, sh.mb_run, &d.body, &run_used);
				if (e != VideoErr::kOk) return e;
				pos += run_used;
			}

			// 各片 mb_run 累加必须等于首片 VOP 头的全帧 mb_count；不等 = 传错片数或片头说谎
			if (runs != head.mb_count) return VideoErr::kValueRange;
			if (pos != size) return VideoErr::kValueRange;

			d.slice_count = static_cast<uint32_t>(fragment_count);
			if (consumed != nullptr) *consumed = size;
			*out = std::move(d);
			return VideoErr::kOk;
		}

		/* 一段不带片头的载荷：没有片数可对账，因此「解完还剩整字节」不构成判点
		 * （调用方给的就是这一段的全部字节）。门面走的是上面的 DecodeVopFrame，两档用途不同。 */
		VideoErr DecodeVopPayload(const uint8_t* data, size_t size, uint8_t quant_precision, uint32_t mb_width,
								  VopPayload* out, size_t* consumed)
		{
			if (out == nullptr) return VideoErr::kInvalidArgument;
			if (data == nullptr && size != 0u) return VideoErr::kInvalidArgument;

			VopHeader head;
			size_t used = 0u;
			if (! DecodeVopHeader(data, size, quant_precision, &head, &used)) return VideoErr::kHeaderBad;

			VopPayload p;
			p.head = head;
			const uint8_t* rest = data + used;
			const size_t rest_size = size - used;
			const VideoErr e =
				(head.type == kVopTypeB) ?
					DecodeBvopLayer(rest, rest_size, mb_width, head.mb_count, &p.bvops) :
					DecodeMacroblockLayer(rest, rest_size, head.type, mb_width, head.mb_count, &p.macroblocks);
			if (e != VideoErr::kOk) return e;

			if (consumed != nullptr) *consumed = size;
			*out = std::move(p);
			return VideoErr::kOk;
		}

	}	 // namespace video
}	 // namespace clv
