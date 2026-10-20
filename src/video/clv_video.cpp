/* 视频面 C ABI 的门面：公开枚举与内部枚举逐项同序，直接转值。
 * 无状态工具（补后尺寸推导、`ext_type = 3` 解析）与编解码器句柄都在这一层实现：
 * 编码器产 VOP 载荷（帧头 + 宏块序列），并做**编码器内部重建**
 * （反量化 + 反变换 + 加预测 + 夹取）写进自己的参考帧；帧间预测走 motion.h 的运动补偿。
 * 系数载体有两种写法：定长档与法 1 档（先写结束标志 EOB，再用位数递增的 Exp-Golomb 编码写幅值）；
 * 具体用哪一档由视频帧头第 5 到第 7 位声明，本库默认法 1。
 * 解码器句柄吃载荷、出帧：帧池 + 参考缓冲（DPB）+ 显示序输出队列都在这层，
 * 单片与多片载荷都走它；逐块走与编码器同一套 ReconstructBlockToPlane，not_coded 的宏块从参考帧整块搬。
 *
 * 异常不出 C 边界：正常负载下本层的抛出源只有分配（帧池 / 矢量场的 std::vector、new 出的帧与载荷），
 * 每个公开入口都在这一层收口成 CLV_VIDEO_OUT_OF_MEMORY —— 句柄口判空置 *err 后返 NULL，计数口返 0，
 * 出参守住入口那份清零纪律，已建而未交出的对象在 catch 里回收，不留半份结果。
 * 分带线程创建失败那一类抛出在本版本判为不可达：并行度是编译期常量、出厂档在本机实测过，
 * 造不出该档的沙盒不构成可验收的路径，故 catch 不为它另开一档回报。
 * catch 只接 std::bad_alloc，绝不接 std::exception 或 ...：实现层的逻辑错误（码流不成立、顺序错）一旦被
 * 吞成「内存不足」，调用方就分不清是该重试分配还是该丢这一帧，这种伪装比 terminate 更难查。
 * CLV_VideoStrError 一族不包：它只回字符串字面量、不做任何分配，包了反而要替它编一个 OOM 返回串。
 */
#include "CLV_Video.h"

#include "block.h"
#include "direct_scale.h"
#include "dpb.h"
#include "encoder_hook.h"
#include "headers.h"
#include "macroblock.h"
#include "motion.h"
#include "padding.h"
#include "payload.h"
#include "reconstruct.h"
#include "status.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace
{

	/* 帧内分带的并行度 = 编译期事实，与系数编码法那一档同款口径（`CLV_COEF_CODING`）：
	 * 由构建面给值，**不进公开入参、不接受环境变量改行为** —— 分带是读方内部实现，
	 * 出厂形态必须是确定的。1 = 单线程一条路，与未启用分带的实现同序同结果。
	 * 取值口径由实测定：并行度到某一档后收益被内存带宽吃掉，故缺省档随实测走。 */
#ifndef CLV_DEC_MB_THREADS
	#define CLV_DEC_MB_THREADS 16
#endif
	inline constexpr size_t kMbThreads = static_cast<size_t>(CLV_DEC_MB_THREADS);
	static_assert(kMbThreads >= 1u, "分带并行度至少 1 档（1 = 不切带）");

	using clv::video::VideoErr;

	static_assert(static_cast<int>(VideoErr::kOk) == static_cast<int>(CLV_VIDEO_OK), "ok order");
	static_assert(static_cast<int>(VideoErr::kInvalidHandle) == static_cast<int>(CLV_VIDEO_INVALID_HANDLE),
				  "handle order");
	static_assert(static_cast<int>(VideoErr::kInvalidArgument) == static_cast<int>(CLV_VIDEO_INVALID_ARGUMENT),
				  "argument order");
	static_assert(static_cast<int>(VideoErr::kValueRange) == static_cast<int>(CLV_VIDEO_VALUE_RANGE), "range order");
	static_assert(static_cast<int>(VideoErr::kPayloadTooSmall) == static_cast<int>(CLV_VIDEO_PAYLOAD_TOO_SMALL),
				  "small order");
	static_assert(static_cast<int>(VideoErr::kHeaderBad) == static_cast<int>(CLV_VIDEO_HEADER_BAD), "header order");
	static_assert(static_cast<int>(VideoErr::kQuantOutOfRange) == static_cast<int>(CLV_VIDEO_QUANT_OUT_OF_RANGE),
				  "quant order");
	static_assert(static_cast<int>(VideoErr::kBitDepthUnsupported) == static_cast<int>(CLV_VIDEO_BIT_DEPTH_UNSUPPORTED),
				  "depth order");
	static_assert(static_cast<int>(VideoErr::kToolExcluded) == static_cast<int>(CLV_VIDEO_TOOL_EXCLUDED), "tool order");
	static_assert(static_cast<int>(VideoErr::kToolListAbsent) == static_cast<int>(CLV_VIDEO_TOOL_LIST_ABSENT),
				  "list order");
	static_assert(static_cast<int>(VideoErr::kSliceHeaderBad) == static_cast<int>(CLV_VIDEO_SLICE_HEADER_BAD),
				  "slice header order");
	static_assert(static_cast<int>(VideoErr::kSliceSequence) == static_cast<int>(CLV_VIDEO_SLICE_SEQUENCE),
				  "slice seq order");
	static_assert(static_cast<int>(VideoErr::kRefFrameMissing) == static_cast<int>(CLV_VIDEO_REF_FRAME_MISSING),
				  "ref order");
	static_assert(static_cast<int>(VideoErr::kOutputTooSmall) == static_cast<int>(CLV_VIDEO_OUTPUT_TOO_SMALL),
				  "output order");
	static_assert(static_cast<int>(VideoErr::kStateError) == static_cast<int>(CLV_VIDEO_STATE_ERROR), "state order");
	static_assert(static_cast<int>(VideoErr::kOutOfMemory) == static_cast<int>(CLV_VIDEO_OUT_OF_MEMORY), "oom order");
	static_assert(static_cast<int>(VideoErr::kStreamRejected) == static_cast<int>(CLV_VIDEO_STREAM_REJECTED),
				  "rejected order");
	static_assert(static_cast<int>(VideoErr::kQuantOutOfRangeCall) ==
					  static_cast<int>(CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL),
				  "per-call quant order");
	static_assert(static_cast<int>(VideoErr::kPayloadLengthMismatch) ==
					  static_cast<int>(CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH),
				  "payload length mismatch order");
	/* 号位 73 / 78 / 81 / 82 是留白位，不复用；后继条目的数值靠显式赋值钉住，
	 * 这里再把「空位确实空着、后继没被挤偏」显式断言一遍：段首 64、段尾 83，
	 * 空位四格在 StrError 里落到 default 分支（返回 "unknown"），后继四格按下面的值钉死。 */
	static_assert(static_cast<int>(CLV_VIDEO_SLICE_HEADER_BAD) == 74, "gap 73 keeps SLICE_HEADER_BAD at 74");
	static_assert(static_cast<int>(CLV_VIDEO_STATE_ERROR) == 79, "gap 78 keeps STATE_ERROR at 79");
	static_assert(static_cast<int>(CLV_VIDEO_OUT_OF_MEMORY) == 80, "gap 81 / 82 keeps OOM at 80");
	static_assert(static_cast<int>(CLV_VIDEO_STREAM_REJECTED) == 83, "segment tail before 84");
	/* 84 与 85 都紧跟段尾追加，两侧都显式钉值：号位一旦写进码流外的契约，靠枚举自增接不上就是事故。 */
	static_assert(static_cast<int>(CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL) == 84, "tail append at 84");
	static_assert(CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL != CLV_VIDEO_QUANT_OUT_OF_RANGE, "声明侧与逐调用侧必须是两个码");
	static_assert(static_cast<int>(CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH) == 85, "tail append at 85");
	static_assert(CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH != CLV_VIDEO_PAYLOAD_TOO_SMALL, "过长与过短必须是两个码");

}	 // namespace

CLV_VideoError CLV_VideoCodedSize(uint16_t display_width, uint16_t display_height, uint32_t* coded_width,
								  uint32_t* coded_height)
{
	try
	{
		if (coded_width == nullptr || coded_height == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
		if (display_width == 0u || display_height == 0u) return CLV_VIDEO_VALUE_RANGE;
		const uint32_t w = clv::video::CodedSize(display_width);
		const uint32_t h = clv::video::CodedSize(display_height);
		/* 补后尺寸积越 reconstruct.h 的像素上界即拒：界外的尺寸在任何帧缓冲分配入口都分配不出来，
		 * 把它写给出参只是把失败推迟到分配点。
		 * 拒时不写出参 —— 出参停在调用方传进来的值，不留半套尺寸。 */
		if (! clv::video::CodedPixelsWithinBound(w, h)) return CLV_VIDEO_VALUE_RANGE;
		*coded_width = w;
		*coded_height = h;
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// 尺寸不写出参，与上面「拒时不写出参」同一条纪律：不留半套尺寸
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

CLV_VideoError CLV_VideoParseEncMeta(const uint8_t* data, size_t size, CLV_VideoEncMetaC* out)
{
	try
	{
		if (out == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
		if (data == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
		// 定长 8 字节：短了是「读不出字段」，长了是「装进了规定之外的多余字节」，两档不是一个码
		if (size < 8u) return CLV_VIDEO_PAYLOAD_TOO_SMALL;
		if (size > 8u) return CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH;

		clv::video::EncMeta m;
		if (clv::video::ParseEncMeta(data, size, &m))
		{
			out->max_b_frames = m.max_b_frames;
			out->quant_precision = m.quant_precision;
			out->ext_tools = m.ext_tools;
			out->reserved = m.reserved;
			out->display_width = m.display_width;
			out->display_height = m.display_height;
			return CLV_VIDEO_OK;
		}
		// 解析不过时区分两档：保留位与工具位非 0 是字段不成立，量化精度与尺寸越界是值域不成立
		if (data[3] != 0u || (data[2] & clv::video::kExtToolsReservedMask) != 0u) return CLV_VIDEO_HEADER_BAD;
		if (data[1] < clv::video::kQuantPrecisionMin || data[1] > clv::video::kQuantPrecisionMax)
			return CLV_VIDEO_VALUE_RANGE;
		// 小端 u16 的两个字节同时为 0 才算 0：尺寸写 0 是值域不成立，不是「未声明」
		if ((data[4] | data[5]) == 0u || (data[6] | data[7]) == 0u) return CLV_VIDEO_VALUE_RANGE;
		return CLV_VIDEO_HEADER_BAD;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

using clv::video::BlockGeom;
using clv::video::BlockSizeMatches;
using clv::video::BvopMacroblock;
using clv::video::CoefCoding;
using clv::video::DecodeBlockData;
using clv::video::DecodedVop;
using clv::video::DirectScale;
using clv::video::DirectScaled;
using clv::video::EncodeBlockData;
using clv::video::EstimateMotion;
using clv::video::FillBlock;
using clv::video::FrameHolders;
using clv::video::FrameQuant;
using clv::video::Hadamard8;
using clv::video::kBlockCoefs;
using clv::video::kBlocksPerMacroblock;
using clv::video::kEncoderCoefCoding;
using clv::video::kIntraPredLevel8;
using clv::video::kMaxFragmentCount;
using clv::video::kMbSize;
using clv::video::kPatternMask6;
using clv::video::kQuantLossless;
using clv::video::kQuantMin;
using clv::video::kQuantPrecisionMax;
using clv::video::kQuantPrecisionMin;
using clv::video::kVopTypeB;
using clv::video::kVopTypeI;
using clv::video::kVopTypeP;
using clv::video::Macroblock;
using clv::video::MacroblockBlockAt;
using clv::video::MotionVector;
using clv::video::OutputItem;
using clv::video::PlaneView;
using clv::video::PredictDirectMacroblock;
using clv::video::PredictMacroblock;
using clv::video::QuantCeiling;
using clv::video::ReconstructBlockToPlane;
using clv::video::ResidualBlock8x8;
using clv::video::ScatterBlock8x8;
using clv::video::Scheduler;
using clv::video::SliceHeader;
using clv::video::VopHeader;
using clv::video::VopPayload;

/* 帧对象。计数只有这一份，落在帧对象上：
 *   架上持有 —— 建帧时那一次，跟着这帧走完「待输出队列 → 待取队列」；取帧口交出指针时交接给调用方
 *               那一份（同一趟里一加一放，净值不变），失败早退由看门人放掉，不留死槽。
 *   参考窗持有 —— I / P 进参考缓冲时由调度器经 `FrameHolders` 加，逐出 / 冲刷 / 重置时放。
 *   调用方持有 —— 取帧口交出时加，`CLV_VideoFrameFree` / `CLV_VideoFrameRelease` 减，AddRef 可另加。
 *               交出时置 `handed`，归还只受理一次：落在参考窗里的计数本来就大于 1，
 *               只判「计数未归零」挡不住第二次归还，第二次会把还被引用的帧从池里收走。
 * 计数归零的槽位就是空闲槽：对象与像素缓冲都留着复用，所以常驻量看同时在场的帧数，不看解出过多少帧。
 * 在参考窗里的帧自然计数不为零，不另设参考窗保护分支。编码器申请的帧不落池（pooled = false），
 * 计数归零即释放该缓冲。 */
struct CLV_VideoFrame
{
	std::vector<uint8_t> buf[3];
	uint32_t stride[3] = { 0u, 0u, 0u };
	uint32_t coded_width = 0u;
	uint32_t coded_height = 0u;
	uint32_t display_width = 0u;
	uint32_t display_height = 0u;
	uint16_t bit_depth = 8u;
	uint32_t refs = 1u;
	bool handed = false;	// true = 交出去的那一次还没还：归还的状态闸，只受理一次
	bool pooled = false;	// true = 归零后回收进解码器空闲槽（对象留着复用），不是释放
	bool shelf = false;		// true = 架上那一次持有还在本帧名下
	bool live = true;		// false = 重置或关闭已作废：指针失效，归还只回报参数不成立
	/* 帧自带的元信息：出帧后调用方只能从 FrameGetDesc 拿 PTS / 帧型，所以解码时逐项落到这里。
	 * disposition 不存 —— 它是「这一刻的处置」，取帧时才填 OUTPUT。 */
	CLV_VideoFrameInfoC info {};
};

namespace
{

	constexpr uint32_t kPlaneAlignBytes = 32u;

	uint32_t Align32(uint32_t v) { return (v + (kPlaneAlignBytes - 1u)) & ~(kPlaneAlignBytes - 1u); }

	/* 平面缓冲的唯一定法：行距按 32 字节对齐，亮度全幅、两色度各半高。
	 * 编码器输入帧与解码器输出帧共用同一布局，两侧都经这里分配。
	 * 分配之前先比 reconstruct.h 的像素上界：字节数在本处是 size_t 量，32 位构建下 size_t 只有 32 位，
	 * 宽 × 高的积按取模回绕会得到一个小得离谱的字节数（65 536 × 65 536 正好回绕成 0），
	 * 届时分配出一块装不下任何一行的缓冲，而寻址算式照算 —— 那是越界写，不是报错。
	 * 界内时单平面字节数的上界 = 像素上界 + 31 × 65 536（行距对齐的每行余量 × 补后高的上限），
	 * 远小于 2^31，故上面那两个 64 位积转回 size_t 不丢量。
	 * 越界返回 false 且三平面一个也不分配（行距与缓冲都停在初值）：不钳到界内继续跑，也不留半帧。 */
	bool AllocPlanes(CLV_VideoFrame& f)
	{
		if (! clv::video::CodedPixelsWithinBound(f.coded_width, f.coded_height)) return false;
		f.stride[0] = Align32(f.coded_width);
		f.stride[1] = Align32(f.coded_width / 2u);
		f.stride[2] = f.stride[1];
		const uint64_t luma_bytes = static_cast<uint64_t>(f.stride[0]) * f.coded_height;
		const uint64_t chroma_bytes = static_cast<uint64_t>(f.stride[1]) * (f.coded_height / 2u);
		f.buf[0].assign(static_cast<size_t>(luma_bytes), 0u);
		f.buf[1].assign(static_cast<size_t>(chroma_bytes), 0u);
		f.buf[2].assign(static_cast<size_t>(chroma_bytes), 0u);
		return true;
	}

	/* 把三平面的像素缓冲全部退还（帧对象留着）：重置与关闭那一步的「回落基线」就是这里发生的。
	 * 用 swap 而不是 clear —— clear 只把 size 归零，容量还占着，回落就不彻底。 */
	void FreePlanes(CLV_VideoFrame& f)
	{
		for (uint32_t p = 0u; p < 3u; ++p)
		{
			std::vector<uint8_t>().swap(f.buf[p]);
			f.stride[p] = 0u;
		}
	}

	/* 帧句柄 ↔ 平面视图：搬运规则本身在 reconstruct，编码端与解码端共用一份。 */
	PlaneView ViewOf(CLV_VideoFrame& f)
	{
		PlaneView v;
		v.width = f.coded_width;
		v.height = f.coded_height;
		for (uint32_t p = 0u; p < 3u; ++p)
		{
			v.p[p] = f.buf[p].data();
			v.stride[p] = f.stride[p];
		}
		return v;
	}

	// 六块预测的共用口（定义在本文件后面）：帧内填黑电平、帧间走运动补偿，编码端不再各写一遍
	void PredictFor(const PlaneView* ref, bool intra, MotionVector mv, uint32_t mb_x, uint32_t mb_y,
					uint8_t out[kBlocksPerMacroblock][kBlockCoefs]);

	/* 编码端一个宏块：预测（读 ref = 上一帧的重建）→ 残差 → 变换量化 → 系数载体打包 →
	 * 内部重建写进 dst（本帧的重建缓冲）；dst 要到帧末与本函数的 ref 交换，本帧才成为下一帧的参考。
	 * intra = true 时预测块恒 kIntraPredLevel8；否则按宏块矢量走运动补偿预测。
	 *
	 * **先把整个宏块的六块预测取齐，再逐块写重建**：矢量可能跨块指向本宏块内「还没轮到」的位置，
	 * 一边取一边写会让自己读到刚写进去的值，而解码端读的是完整的参考帧 —— 两侧就此岔开，不再逐字节全等。
	 * 返回合成块模式（bit5..2 亮度、bit1 Cb、bit0 Cr）与块数据字节。 */
	VideoErr EncodeMacroblockBlocks(const PlaneView& src, const PlaneView& ref, const PlaneView& dst, bool intra,
									uint32_t mb_x, uint32_t mb_y, uint8_t q, MotionVector mv, uint8_t* pattern_out,
									std::vector<uint8_t>* block_out)
	{
		int32_t qf[kBlocksPerMacroblock][kBlockCoefs];
		uint8_t pattern = 0u;
		uint8_t pred6[kBlocksPerMacroblock][kBlockCoefs];
		PredictFor(intra ? nullptr : &ref, intra, mv, mb_x, mb_y, pred6);
		for (int idx = 0; idx < kBlocksPerMacroblock; ++idx)
		{
			const BlockGeom g = MacroblockBlockAt(mb_x, mb_y, idx);
			int32_t x[kBlockCoefs];
			ResidualBlock8x8(src, g.plane, g.px, g.py, pred6[idx], x);
			int32_t t[kBlockCoefs];
			Hadamard8(x, t);
			clv::video::QuantizeBlock(t, q, qf[idx]);
			bool nonzero = false;
			for (int i = 0; i < kBlockCoefs; ++i)
			{
				if (qf[idx][i] != 0)
				{
					nonzero = true;
					break;
				}
			}
			if (nonzero) pattern |= static_cast<uint8_t>(1u << (5 - idx));
			ReconstructBlockToPlane(qf[idx], q, intra, pred6[idx], dst, g.plane, g.px, g.py);
		}
		*pattern_out = pattern;
		return EncodeBlockData(pattern, qf, q, intra, kEncoderCoefCoding, block_out);
	}

	/* 解码端一个宏块：按块模式解出的系数逐块重建写进输出帧。
	 * pred6 是**调用方已经算好的六个预测块**（帧内恒 128、帧间按宏块矢量取样、B 的 direct 走双向平均）
	 * —— 预测一律由调用方从 motion.h 取，这里只做「解系数 + 重建 + 回写」。
	 * 位为 0 的块按零残差处置 —— 重建即预测块，与编码端「全零块写预测」同式。 */
	VideoErr DecodeMacroblockBlocks(uint8_t pattern, const std::vector<uint8_t>& block, uint8_t q, bool intra,
									CoefCoding coding, const uint8_t pred6[kBlocksPerMacroblock][kBlockCoefs],
									PlaneView& dst, uint32_t mb_x, uint32_t mb_y)
	{
		int32_t qf[kBlocksPerMacroblock][kBlockCoefs];
		// 位为 0 的块由 DecodeBlockData 清零（按零残差处置），这里无需预置
		const VideoErr de = DecodeBlockData(pattern, block.data(), block.size(), q, intra, coding, qf);
		if (de != VideoErr::kOk) return de;
		for (int idx = 0; idx < kBlocksPerMacroblock; ++idx)
		{
			const BlockGeom g = MacroblockBlockAt(mb_x, mb_y, idx);
			ReconstructBlockToPlane(qf[idx], q, intra, pred6[idx], dst, g.plane, g.px, g.py);
		}
		return VideoErr::kOk;
	}

	/* 帧内 / 帧间（含 B 的 forward / backward）两种情形的六块预测 —— 一句话交给 motion.h。 */
	void PredictFor(const PlaneView* ref, bool intra, MotionVector mv, uint32_t mb_x, uint32_t mb_y,
					uint8_t out[kBlocksPerMacroblock][kBlockCoefs])
	{
		if (intra)
		{
			for (int idx = 0; idx < kBlocksPerMacroblock; ++idx) FillBlock(kIntraPredLevel8, out[idx]);
			return;
		}
		PredictMacroblock(*ref, mb_x, mb_y, mv, out);
	}

}	 // namespace

/* ---- 解码器句柄：吃码流、吐帧 ---- */

/* 帧池：槽号 = 下标，池只持有对象，持有计数落在帧对象上（三处来源见 `CLV_VideoFrame` 上方）。
 * 回收口径：引用归零方可复用 —— 归零的槽位连同像素缓冲一起留在池里当下一次的空闲位，
 * 所以常驻量看「同时在场的帧数」，不看解出过多少帧。
 * 上界：占用 ≤（参考缓冲 maxlen + 调用方未释放数）× 每帧持有字节 + 基线（算式与实测常量在 dpb.h）。
 * 池饿时（没有空闲位可取）允许槽位数超过 maxlen，超出部分由未归还数封顶。
 * 驱逐触发：DPB 到顶即逐出解码序最旧的一帧并放掉参考窗那一次持有；计数归零进空闲池，
 * 计数非零（调用方还持着）留到那一次归还落地。 */
struct CLV_VideoDecoder: clv::video::FrameHolders
{
	CLV_VideoStreamParamsC params {};
	uint32_t coded_width = 0u;
	uint32_t coded_height = 0u;
	uint32_t mb_width = 0u;
	uint32_t mb_height = 0u;
	/* 拒解态：置位点唯一 = CLV_VideoDecoderOpen 的三条致命声明档；置位后除 Reset / Close 与
	 * StreamRejected 观测口外，每个入口一律返 STREAM_REJECTED。Reset 是唯一出口。 */
	bool rejected = false;
	Scheduler sched;
	std::vector<CLV_VideoFrame*> slots;	   // 帧池，id = 下标；计数归零的那一格就是空闲位
	std::vector<int64_t> slot_pts;		   // 与 slots 同序：按 PTS 找前向 / 后向参考
	/* 与 slots 同序的**逐宏块矢量场**，B 帧 direct 要靠它取后向参考的同位矢量。
	 * 帧内宏块与 not_coded 宏块都存零矢量 —— 取不到同位矢量时按零矢量处理，
	 * 故这里不需要再带一个「有没有矢量」的标志位；B 帧不进参考缓冲，它的矢量场不被任何帧取用。 */
	std::vector<std::vector<MotionVector>> slot_mv;
	std::deque<CLV_VideoFrame*> ready;	  // 待调用方取的帧，已按显示序（PTS 升序）排好
	/* 重置作废时还带着调用方持有的帧：像素缓冲已退还、计数已清零，只剩一个对象壳挂在句柄名下。
	 * 不能就地释放 —— 失效后那一次归还仍要读出「已作废」并回报参数不成立，访问已释放的内存
	 * 就不是回报而是崩。壳的回收点是关闭。 */
	std::vector<CLV_VideoFrame*> voided;
	bool flushed = false;
	/* 是否已经向调用方出过至少一帧：出过即「池已起完」，起池段的策略开关从此不再起作用。 */
	bool primed = false;

	CLV_VideoDecoder(uint8_t max_b_frames, uint8_t preroll_frames): sched(max_b_frames, preroll_frames, this) {}

	~CLV_VideoDecoder()
	{
		for (CLV_VideoFrame* f: voided) delete f;
	}

	/* 槽号 → 帧；越界与空槽都给 NULL，调用侧一律判空再用。 */
	CLV_VideoFrame* FrameAt(int id)
	{
		if (id < 0 || static_cast<size_t>(id) >= slots.size()) return nullptr;
		return slots[static_cast<size_t>(id)];
	}

	/* 放一次持有。归零的池内帧留在原槽当空闲位（对象与像素缓冲都留着复用），
	 * 归零的池外帧（编码器申请的、或作废后剩下的壳）就地释放。 */
	void Drop(CLV_VideoFrame* f)
	{
		if (f == nullptr || f->refs == 0u) return;
		if (--f->refs != 0u) return;
		if (f->pooled) return;
		delete f;
	}

	/* 架上那一次持有交回池：标志先落，重复放不掉第二次。 */
	void DropShelf(CLV_VideoFrame* f)
	{
		if (f == nullptr || ! f->shelf) return;
		f->shelf = false;
		Drop(f);
	}

	/* 参考窗持有的两个落点（由 Scheduler 经 `FrameHolders` 调用）：加减的都是帧对象那一份计数。 */
	void Acquire(int id) override
	{
		CLV_VideoFrame* f = FrameAt(id);
		if (f != nullptr) f->refs += 1u;
	}

	void Release(int id) override { Drop(FrameAt(id)); }

	/* 取一格槽位：先把计数归零的空闲位（含上一帧解码失败留下的空槽）拿来复用，
	 * 没有空闲位才让池长一格 —— 池饿时槽位数可以超过参考缓冲 maxlen，超出部分由未归还数封顶。 */
	int AcquireSlot()
	{
		for (size_t i = 0u; i < slots.size(); ++i)
		{
			CLV_VideoFrame* f = slots[i];
			if (f == nullptr) return static_cast<int>(i);
			if (f->live && f->refs == 0u) return static_cast<int>(i);
		}
		slots.push_back(nullptr);
		slot_pts.push_back(0);
		slot_mv.emplace_back();
		return static_cast<int>(slots.size()) - 1;
	}

	/* 重置与关闭共用的作废步：池侧的账清零、像素缓冲全部回收、已交出的指针失效。
	 * 没人持着的格子就地回收；还带着调用方持有的只剩一个对象壳（像素缓冲已退还、计数清零），
	 * 挂进 `voided` 由句柄兜到关闭 —— 壳不能就地 delete：失效后那一次归还仍要读出「已作废」
	 * 并回报参数不成立，读已释放的内存就不是回报而是崩。归还永远不来，欠的也只是这一个壳。 */
	void VoidAllFrames()
	{
		for (CLV_VideoFrame*& f: slots)
		{
			if (f == nullptr) continue;
			DropShelf(f);	 // 架上那一次是池的持有，作废时放掉（参考窗那一次由 sched.Reset 放过）
			f->live = false;
			f->pooled = false;
			f->handed = false;	  // 状态闸落下：此后再归还只回报参数不成立
			if (f->refs == 0u)
			{
				delete f;
				f = nullptr;
				continue;
			}
			f->refs = 0u;	 // 计数账清零：作废之后这帧名下不再有任何持有
			FreePlanes(*f);
			voided.push_back(f);
		}
		slots.clear();
		slot_pts.clear();
		slot_mv.clear();	// 三个向量同序是不变量，漏清会让下次 AcquireSlot 把矢量场接到旧长度上
	}

	/* 建一帧，或复用池里那一个计数已归零的空闲位。两类失败分档交回，都不是「凑合着建」：
	 *   kOutOfMemory  = 帧对象本身拿不到；
	 *   kValueRange   = 补后尺寸积越像素上界，AllocPlanes 拒绝分配，三平面一个也不建。
	 * 失败时这一格槽位交回空槽（与 new 失败时的处置同一条路），不留半帧。 */
	VideoErr NewFrame(int id, CLV_VideoFrame** out)
	{
		*out = nullptr;
		CLV_VideoFrame* f = FrameAt(id);	// 空闲位：对象与像素缓冲都留着复用
		if (f == nullptr)
		{
			f = new (std::nothrow) CLV_VideoFrame();
			if (f == nullptr) return VideoErr::kOutOfMemory;
			f->pooled = true;
			f->coded_width = coded_width;
			f->coded_height = coded_height;
			f->display_width = params.display_width;
			f->display_height = params.display_height;
			f->bit_depth = params.bit_depth;
			if (! AllocPlanes(*f))
			{
				delete f;
				slots[static_cast<size_t>(id)] = nullptr;
				return VideoErr::kValueRange;
			}
		}
		else if (f->buf[0].empty() && ! AllocPlanes(*f))
		{
			// 缓冲被退还过的槽位（尺寸由本句柄定死，正常路径走不到这一支）：重开失败就整格弃掉
			delete f;
			slots[static_cast<size_t>(id)] = nullptr;
			return VideoErr::kValueRange;
		}
		f->refs = 1u;	 // 架上那一次
		f->shelf = true;
		f->handed = false;	  // 还没交出：归还的状态闸是关的
		f->live = true;
		f->info = CLV_VideoFrameInfoC {};
		PlaneView v = ViewOf(*f);
		FillPlanesBlack(v);
		slots[static_cast<size_t>(id)] = f;
		slot_pts[static_cast<size_t>(id)] = 0;
		*out = f;
		return VideoErr::kOk;
	}
};

namespace
{

	/* 架上持有的看门人：本帧从建好到交进队列之间任何早退（宏块判不成立、越界、分配抛）都要把架上
	 * 那一次放掉，否则那格槽位再也回不了空闲池 —— 一次失败的解码就永久占着一整帧的缓冲。 */
	struct ShelfGuard
	{
		CLV_VideoDecoder* d = nullptr;
		CLV_VideoFrame* f = nullptr;
		bool armed = true;

		~ShelfGuard()
		{
			if (armed) d->DropShelf(f);
		}
	};

}	 // namespace

CLV_VideoDecoder* CLV_VideoDecoderOpen(const CLV_VideoStreamParamsC* params, CLV_VideoError* err)
{
	try
	{
		if (err != nullptr) *err = CLV_VIDEO_OK;
		/* 入参档：调用方给的东西本身不成立 → 交不出句柄，NULL + err。 */
		if (params == nullptr)
		{
			if (err != nullptr) *err = CLV_VIDEO_INVALID_ARGUMENT;
			return nullptr;
		}
		/* 布局自报在先：非 0 又不等于本版本 sizeof 的对象一律显式拒，库不去猜它的字段落在哪。 */
		if (params->struct_size != 0u && params->struct_size != sizeof(CLV_VideoStreamParamsC))
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}
		if (params->display_width == 0u || params->display_height == 0u)
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}
		if (params->quant_precision < kQuantPrecisionMin || params->quant_precision > kQuantPrecisionMax)
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}
		/* 补后尺寸积先比像素上界再建句柄：帧缓冲要到解码途中才分配，界判落在一切分配之前。
		 * 句柄一旦交出去，之后每一帧都按这两个尺寸定行距与字节数，界外尺寸没有可退的余地。
		 * 尺寸是调用方递进来的声明，走交不出句柄这一档（与上面的尺寸为 0 同一档），不进拒解态。 */
		const uint32_t coded_w = clv::video::CodedSize(params->display_width);
		const uint32_t coded_h = clv::video::CodedSize(params->display_height);
		if (! clv::video::CodedPixelsWithinBound(coded_w, coded_h))
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}

		/* 起池档只有三档，其余值不成立：这一档改的是「第一帧什么时候能出来」，
		 * 静默按缺省受理会让调用方以为自己的选择生效了。
		 * 出帧阈值不校验取值域（类型域 0..255 即合法域）——低于重排地板的由 Scheduler 钳到地板，
		 * 高于地板的是调用方自愿加深队列，代价（首帧延迟与一整帧缓冲）写在公开头字段注释里。 */
		if (params->prefill_mode != CLV_PREFILL_DEFAULT && params->prefill_mode != CLV_PREFILL_SERIAL &&
			params->prefill_mode != CLV_PREFILL_PARALLEL)
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}

		auto* d = new (std::nothrow) CLV_VideoDecoder(params->max_b_frames, params->preroll_frames);
		if (d == nullptr)
		{
			if (err != nullptr) *err = CLV_VIDEO_OUT_OF_MEMORY;
			return nullptr;
		}
		d->params = *params;
		d->coded_width = coded_w;
		d->coded_height = coded_h;
		d->mb_width = d->coded_width / kMbSize;
		d->mb_height = d->coded_height / kMbSize;

		/* 声明档：流自己声明成 v1 不收的样子 → 句柄照交，但进拒解态，*err 写命中的致命码。
		 * 拒解态下除 Reset / Close 与 StreamRejected 外每个入口都返 CLV_VIDEO_STREAM_REJECTED，
		 * Reset 是唯一出口（定位 / 丢包重置带着新声明再来）。
		 * v1 只做 8-bit：10 / 12-bit 的像素电平未定义（本层只做 8-bit），16 是结构预留；
		 * extended_esc = 0 而位深 10 / 12 也落 bit_depth 这一档 —— 逃生口没开，解不出来就是拒解。 */
		if (params->bit_depth != 8u)
		{
			d->rejected = true;
			if (err != nullptr) *err = CLV_VIDEO_BIT_DEPTH_UNSUPPORTED;
			return d;
		}
		// v1 只允许默认 VLC 表、无透明、不隔行、layer 0、色度恒定块补偿开着：任一不符即拒解整条流
		if (params->vlc_table_id != 0u || params->alpha_enabled != 0u || params->obmc_disable != 1u ||
			params->layer_id != 0u)
		{
			d->rejected = true;
			if (err != nullptr) *err = CLV_VIDEO_TOOL_EXCLUDED;
			return d;
		}
		// 声明了工具清单却没给内容，是字段不成立而不是「没工具」
		if (params->tool_list == nullptr && params->tool_list_size != 0u)
		{
			d->rejected = true;
			if (err != nullptr) *err = CLV_VIDEO_TOOL_LIST_ABSENT;
			return d;
		}
		/* tool_list 的内容这里不解释：清单只声明有哪些扩展工具，v1 不据它放宽任何判定，
		 * 清单点名的其它工具（GMC / 隔行 / 任意形状 / N-bit 等）一律不实现。 */
		return d;
	}
	catch (const std::bad_alloc&)
	{
		if (err != nullptr) *err = CLV_VIDEO_OUT_OF_MEMORY;
		return nullptr;
	}
}

namespace
{

	/* 内部码 → 公开码：两套枚举逐项同序，转值即可。 */
	CLV_VideoError ToPublic(clv::video::VideoErr e) { return static_cast<CLV_VideoError>(e); }

	/* 显示序上离当前帧最近的驻留参考帧：dir < 0 只要前向（PTS ≤ 当前），dir > 0 只要后向。 */
	int PickReference(CLV_VideoDecoder* d, int64_t pts, int dir)
	{
		int best = -1;
		int64_t best_gap = INT64_MAX;
		for (size_t i = 0u; i < d->slots.size(); ++i)
		{
			const int id = static_cast<int>(i);
			if (d->slots[i] == nullptr || ! d->sched.InDpb(id)) continue;
			const int64_t t = d->slot_pts[i];
			if (dir < 0 && t > pts) continue;
			if (dir > 0 && t < pts) continue;
			const int64_t gap = (t > pts) ? (t - pts) : (pts - t);
			if (gap < best_gap)
			{
				best_gap = gap;
				best = id;
			}
		}
		return best;
	}

	/* 归还的唯一落点。两扇门走同一份计数，判不成立的一律不动计数：空指针、已被重置或关闭作废、
	 * 名下没有可还的持有（计数已归零）。减到零的池内帧留在原槽当空闲位（对象与像素缓冲都留着复用），
	 * 池外帧（编码器申请的）就地释放。
	 * `only_handout` = true 是带回报的那扇门（Free）：还的只能是取帧口交出的那一次，状态闸 `handed`
	 * 已经落下就判参数不成立 —— 落在参考窗里的帧计数本来就大于 1，只判「计数未归零」挡不住第二次，
	 * 第二次会把还被别的帧引用的帧当空闲位收走。
	 * `only_handout` = false 是不回报的那扇门（Release）：与 AddRef 配对，减调用方名下的任意一次持有；
	 * 交出的那一次还没还时先消耗它，闸落下后 Free 不再受理，两条路合计只还得掉一次。 */
	bool GiveBackFrame(CLV_VideoFrame* f, bool only_handout)
	{
		if (f == nullptr || ! f->live || f->refs == 0u) return false;
		if (only_handout && ! f->handed) return false;
		f->handed = false;
		--f->refs;
		if (f->refs == 0u && ! f->pooled) delete f;
		return true;
	}

	/* 出帧队列的公共出口：交出指针时调用方那一份计数加一，架上那一份同时交回池。
	 * 一加一放在同一趟里完成，所以计数的净值不变，但持有的来源换了 —— 全程只有一份账。 */
	CLV_VideoError TakeNext(CLV_VideoDecoder* d, CLV_VideoFrame** out, uint8_t* got)
	{
		if (d->ready.empty()) return CLV_VIDEO_OK;
		CLV_VideoFrame* f = d->ready.front();
		d->ready.pop_front();
		f->refs += 1u;
		f->handed = true;	 // 这一次持有落在调用方名下，归还只受理到这里
		d->DropShelf(f);
		*out = f;
		if (got != nullptr) *got = 1u;
		return CLV_VIDEO_OK;
	}

}	 // namespace

CLV_VideoError CLV_VideoDecoderDecode(CLV_VideoDecoder* d, const uint8_t* payload, size_t size, uint64_t fragment_count,
									  int64_t dts_ticks, int64_t pts_ticks, CLV_VideoFrameInfoC* info)
{
	try
	{
		if (info != nullptr) std::memset(info, 0, sizeof(*info));
		if (d == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		if (d->rejected) return CLV_VIDEO_STREAM_REJECTED;
		if (payload == nullptr || size == 0u) return CLV_VIDEO_INVALID_ARGUMENT;
		// 片数契约：0 = 没等到末片就喂，> 65 535 = 越分片数上限，两条都在入口判，不进解析
		if (fragment_count == 0u || fragment_count > kMaxFragmentCount) return CLV_VIDEO_VALUE_RANGE;
		// 已冲刷就是句柄进了 EOS 态：此时载荷吃进去既不报错也不出帧，帧被静默吞掉。
		// 出口是 Reset（拒解态同理走 Reset），故这里给显式的顺序错误，不让调用方拿不到帧却看不出为什么。
		if (d->flushed) return CLV_VIDEO_STATE_ERROR;

		DecodedVop fp;
		const clv::video::VideoErr r = clv::video::DecodeVopFrame(payload, size, d->params.quant_precision, d->mb_width,
																  fragment_count, &fp, nullptr);
		// 载荷自身不成立 = 丢这一帧（丢帧即隐藏动作），不判整条流；只有致命工具位才是拒解
		if (r != clv::video::VideoErr::kOk) return ToPublic(r);

		const VopHeader& head = fp.body.head;
		if (head.mb_count != static_cast<uint64_t>(d->mb_width) * d->mb_height) return CLV_VIDEO_VALUE_RANGE;

		const uint8_t type = head.type;
		const int forward = PickReference(d, pts_ticks, -1);
		const int backward = PickReference(d, pts_ticks, 1);
		const bool refs_ok = (type == kVopTypeI) || (type == kVopTypeP && forward >= 0) ||
							 (type == kVopTypeB && forward >= 0 && backward >= 0);
		if (! refs_ok)
		{
			if (info != nullptr)
			{
				info->type = (type == kVopTypeP) ? CLV_VIDEO_FRAME_P : CLV_VIDEO_FRAME_B;
				info->disposition = CLV_VIDEO_DISP_DROP_REF_MISSING;
				info->macroblock_count = static_cast<uint32_t>(head.mb_count);
				info->quant_used = head.quant;
				info->slice_count = fp.slice_count;
				info->dts_ticks = dts_ticks;
				info->pts_ticks = pts_ticks;
			}
			return CLV_VIDEO_OK;	// 坏帧跳过、不产出，但接口要说清为什么没产出
		}

		/* B 帧的帧级时间基准：TRB = 本帧 PTS − 前向参考 PTS（time_pb），
		 * TRD = 后向参考 PTS − 前向参考 PTS（time_pp），单位都是容器 tick。
		 * 判据 `TRD > 0 && 0 < TRB < TRD` 不满足即丢这一帧：不建帧、不入队、不进参考缓冲。
		 * direct 缩放入口那句 `TRB > TRD` 是宏块式的另一层检查，与本判据分属两层、互不替代。 */
		int64_t trb = 0;
		int64_t trd = 0;
		if (type == kVopTypeB)
		{
			trb = pts_ticks - d->slot_pts[static_cast<size_t>(forward)];
			trd = d->slot_pts[static_cast<size_t>(backward)] - d->slot_pts[static_cast<size_t>(forward)];
			if (trd <= 0 || trb <= 0 || trb >= trd)
			{
				if (info != nullptr)
				{
					info->type = CLV_VIDEO_FRAME_B;
					info->disposition = CLV_VIDEO_DISP_DROP_DIRECT_TIME;
					info->macroblock_count = static_cast<uint32_t>(head.mb_count);
					info->quant_used = head.quant;
					info->slice_count = fp.slice_count;
					info->dts_ticks = dts_ticks;
					info->pts_ticks = pts_ticks;
				}
				return CLV_VIDEO_OK;	// 丢帧不是错误：句柄仍可用，时间轴照常推进
			}
		}

		const int id = d->AcquireSlot();
		CLV_VideoFrame* f = nullptr;
		const VideoErr fe = d->NewFrame(id, &f);
		if (fe != VideoErr::kOk) return ToPublic(fe);
		d->slot_pts[static_cast<size_t>(id)] = pts_ticks;
		/* 从这里到交进调度器之间还有多条早退路，架上那一次持有由看门人兜着：
		 * 漏放就是那一格槽位永久占着一整帧的缓冲，池再收不回来。 */
		ShelfGuard shelf { d, f };

		PlaneView dstview = ViewOf(*f);
		if (type == kVopTypeB)
		{
			const PlaneView fwdview_b = ViewOf(*d->slots[static_cast<size_t>(forward)]);
			const PlaneView backview = ViewOf(*d->slots[static_cast<size_t>(backward)]);
			// trb / trd 在进这一支之前已按帧级判据算好并验过（本宏块头不带任何时间字段）
			const std::vector<MotionVector>& back_mv = d->slot_mv[static_cast<size_t>(backward)];

			for (size_t i = 0u; i < fp.body.bvops.size(); ++i)
			{
				const BvopMacroblock& m = fp.body.bvops[i];
				const uint32_t mx = static_cast<uint32_t>(i % d->mb_width), my = static_cast<uint32_t>(i / d->mb_width);
				uint8_t pred6[kBlocksPerMacroblock][kBlockCoefs];

				if (m.head.mb_type == clv::video::kBvopDirect)
				{
					/* 同位块矢量：取后向参考的同位宏块。帧内宏块与 not_coded 宏块在场里存的就是零矢量，
					 * 无须另判「取不到」。下标也不用再比一次界：每一帧的宏块数、后向参考的矢量场长
					 * 度、本帧的 bvops 件数三者都等于入口对账过的 `mb_width × mb_height`。 */
					const MotionVector& mv_ref = back_mv[i];
					DirectScaled ds {};
					// 矢量域：向零截断的整数除法，禁 >>（式子只在 direct_scale.cpp 一处）
					const VideoErr se = DirectScale(mv_ref, trb, trd, &ds);
					if (se != clv::video::VideoErr::kOk) return static_cast<CLV_VideoError>(se);
					// 像素域：两向各取一套六块，逐像素双向平均
					const VideoErr pe = PredictDirectMacroblock(&fwdview_b, &backview, mx, my, ds, pred6);
					if (pe != clv::video::VideoErr::kOk) return static_cast<CLV_VideoError>(pe);
				}
				else if (m.head.mb_type == clv::video::kBvopBackward)
				{
					PredictFor(&backview, false, m.head.backward, mx, my, pred6);
				}
				else if (m.head.mb_type == clv::video::kBvopForward)
				{
					PredictFor(&fwdview_b, false, m.head.forward, mx, my, pred6);
				}
				else
				{
					// interpolate：前向一套、后向一套，逐像素双向平均（合成式与 direct 路径同一处）
					uint8_t pf[kBlocksPerMacroblock][kBlockCoefs];
					uint8_t pb[kBlocksPerMacroblock][kBlockCoefs];
					PredictFor(&fwdview_b, false, m.head.forward, mx, my, pf);
					PredictFor(&backview, false, m.head.backward, mx, my, pb);
					clv::video::BlendMacroblocks(pf, pb, pred6);
				}

				// direct 无残差：重建就是预测块本身
				if (m.head.not_coded)
				{
					for (int idx = 0; idx < kBlocksPerMacroblock; ++idx)
					{
						const BlockGeom g = MacroblockBlockAt(mx, my, idx);
						ScatterBlock8x8(dstview, g.plane, g.px, g.py, pred6[idx]);
					}
					continue;
				}
				const uint8_t qq = FrameQuant(head.quant, head.dquant, 0, d->params.quant_precision);
				const uint8_t pattern = static_cast<uint8_t>(m.head.cbpy & kPatternMask6);	  // B 侧是 6 bit 全位图
				if (! BlockSizeMatches(pattern, m.head.block_size, qq, false, head.coef_coding))
					return CLV_VIDEO_VALUE_RANGE;
				const VideoErr de =
					DecodeMacroblockBlocks(pattern, m.block, qq, false, head.coef_coding, pred6, dstview, mx, my);
				if (de != VideoErr::kOk) return static_cast<CLV_VideoError>(de);
			}

			/* B 不进参考缓冲，矢量场按零填满、与 bvops 同序；它不会被任何帧取用。 */
			std::vector<MotionVector>& my_mv = d->slot_mv[static_cast<size_t>(id)];
			my_mv.assign(fp.body.bvops.size(), MotionVector {});
		}
		else
		{
			const PlaneView fwdview = (forward >= 0) ? ViewOf(*d->slots[static_cast<size_t>(forward)]) : PlaneView {};
			std::vector<MotionVector>& my_mv = d->slot_mv[static_cast<size_t>(id)];
			my_mv.assign(fp.body.macroblocks.size(), MotionVector {});

			/* 帧内宏块级并行：读方内部实现，不改码流、不要求写方切带 —— 第三方按整帧单线程解同一份
			 * 字节，得到同一份像素（并行度不进码流、不进公开入参）。可以并行的依据是这一段的读写集，
			 * 四条都与素材无关：
			 *   ① 进到这里的只有已解析完的语法（整帧在 DecodeVopFrame 一次解析完），本循环不读比特；
			 *   ② 帧内预测取常量电平（`PredictFor` 的 intra 分支），不读邻块重建；
			 *   ③ 帧间预测只读上一帧的重建视图，本帧的写侧全在 `dstview` 上；
			 *   ④ v1 无环路滤波与 OBMC（两件都在工具清单里不收），宏块之间没有滤波耦合。
			 * ⇒ 写侧只有本宏块自己的六个 8×8 区域与 `my_mv[i]`，按下标互斥，按宏块行区间切给多个
			 *   线程是数据无竞争的。并行度是编译期常量（见 `kMbThreads`），1 档 = 与未启用分带时
			 *   同一条路径、同一顺序。 */
			const size_t nmb = fp.body.macroblocks.size();
			auto run_mb_range = [&](size_t lo, size_t hi) -> CLV_VideoError
			{
				for (size_t i = lo; i < hi; ++i)
				{
					const Macroblock& m = fp.body.macroblocks[i];
					const uint32_t mx = static_cast<uint32_t>(i % d->mb_width),
								   my = static_cast<uint32_t>(i / d->mb_width);
					const uint8_t qq = FrameQuant(head.quant, head.dquant, m.head.dquant, d->params.quant_precision);
					const bool intra = clv::video::IsIntraMacroblock(m.head.mb_type);

					uint8_t pred6[kBlocksPerMacroblock][kBlockCoefs];
					if (intra)
					{
						PredictFor(nullptr, true, MotionVector {}, mx, my, pred6);
					}
					else
					{
						/* 走到这一支的只有 P-VOP 的帧间宏块：I-VOP 的 mcbpc 表只留帧内行与专供识别的排除行，
						 * 而 P 的前向参考已在入口按 refs_ok 判过 ⇒ 这里不再判 forward。
						 * not_coded 的宏块不带 motion_vector()，矢量按零处理。 */
						PredictFor(&fwdview, false, m.head.not_coded ? MotionVector {} : m.head.mv, mx, my, pred6);
					}
					my_mv[i] = intra ? MotionVector {} : m.head.mv;

					// 无残差 ⇒ 重建就是预测块（帧内是 128 常量块，帧间是零矢量的同位块）
					if (m.head.not_coded)
					{
						for (int idx = 0; idx < kBlocksPerMacroblock; ++idx)
						{
							const BlockGeom g = MacroblockBlockAt(mx, my, idx);
							ScatterBlock8x8(dstview, g.plane, g.px, g.py, pred6[idx]);
						}
						continue;
					}
					/* I / P 侧合成块模式 = cbpy（4 bit 亮度）<< 2 | cbpc（2 bit 色度），合成后位序：
					 * bit5..bit2 = 亮度块 0..3（左上起、行优先）、bit1 = Cb、bit0 = Cr、00 = 该块不在场。
					 * mcbpc 表里 cbpc 列的取值按这一位序读，不取外部 4:2:0 色度块模式码的原列语义
					 * （该列 00 = 两块都在场、11 = 无）：搬过来的是码字行，列语义随合成位序定。 */
					const uint8_t pattern = static_cast<uint8_t>((m.head.cbpy << 2) | (m.head.cbpc & 0x03u));
					if (! BlockSizeMatches(pattern, m.head.block_size, qq, intra, head.coef_coding))
						return CLV_VIDEO_VALUE_RANGE;
					const VideoErr de =
						DecodeMacroblockBlocks(pattern, m.block, qq, intra, head.coef_coding, pred6, dstview, mx, my);
					if (de != VideoErr::kOk) return static_cast<CLV_VideoError>(de);
				}
				return CLV_VIDEO_OK;
			};

			/* 起池段是否也走分带：`prefill_mode` 取串行档时，第一帧出队之前整段按单线程跑 ——
			 * 这一档赌的是「起池那几帧里，建线程池的开销比省下的解码时间更贵」。 */
			size_t threads = static_cast<size_t>(kMbThreads);
			if (d->params.prefill_mode == CLV_PREFILL_SERIAL && ! d->primed) threads = 1u;
			/* 不足两行宏块时切带没有意义（切不开），与并行度为 1 同路。 */
			if (threads <= 1u || d->mb_width == 0u || nmb < 2u * static_cast<size_t>(d->mb_width))
			{
				const CLV_VideoError rc = run_mb_range(0, nmb);
				if (rc != CLV_VIDEO_OK) return rc;
			}
			else
			{
				const size_t rows = (nmb + d->mb_width - 1u) / d->mb_width;
				size_t band = (rows + threads - 1u) / threads;
				if (band < 1u) band = 1u;
				std::vector<std::thread> pool;
				std::vector<CLV_VideoError> rcs;
				for (size_t lo = 0u; lo < nmb; lo += band * d->mb_width)
				{
					const size_t hi = (lo + band * d->mb_width > nmb) ? nmb : lo + band * d->mb_width;
					rcs.push_back(CLV_VIDEO_OK);
					const size_t slot = rcs.size() - 1u;
					pool.emplace_back([&run_mb_range, &rcs, lo, hi, slot] { rcs[slot] = run_mb_range(lo, hi); });
				}
				for (auto& x: pool) x.join();
				// 逐带结论按带序判，第一个非 OK 即本帧结论（与单线程版的「先到先返」同序）
				for (const CLV_VideoError rc: rcs)
					if (rc != CLV_VIDEO_OK) return rc;
			}
		}

		f->info.type = (type == kVopTypeI) ? CLV_VIDEO_FRAME_I :
					   (type == kVopTypeP) ? CLV_VIDEO_FRAME_P :
											 CLV_VIDEO_FRAME_B;
		f->info.is_keyframe = (type == kVopTypeI) ? 1u : 0u;
		f->info.quant_used = head.quant;
		f->info.macroblock_count = static_cast<uint32_t>(head.mb_count);
		f->info.slice_count = fp.slice_count;
		f->info.dts_ticks = dts_ticks;
		f->info.pts_ticks = pts_ticks;
		// timebase_den 留 0：解码侧参数里没有这一项，时间基是容器侧事实，库不替调用方填
		const std::vector<OutputItem> emitted = d->sched.PushDecoded(id, type, pts_ticks);
		// 交进队列即由架上那一次持有负责（弹出交给待取队列时也不换手），看门人从此不管这帧
		shelf.armed = false;
		bool delayed = true;
		for (const OutputItem& o: emitted)
		{
			d->ready.push_back(d->slots[static_cast<size_t>(o.id)]);
			d->primed = true;	 // 池已起完：起池段的策略开关从此不再起作用
			if (o.id == id) delayed = false;
		}

		if (info != nullptr)
		{
			*info = f->info;
			info->disposition = delayed ? CLV_VIDEO_DISP_DELAYED : CLV_VIDEO_DISP_OUTPUT;
		}
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// 出参回到入口的清零态：不留半份帧元信息
		if (info != nullptr) std::memset(info, 0, sizeof(*info));
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

CLV_VideoError CLV_VideoDecoderNextOutput(CLV_VideoDecoder* d, CLV_VideoFrame** out, uint8_t* got)
{
	try
	{
		if (out != nullptr) *out = nullptr;
		if (got != nullptr) *got = 0u;
		if (out == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;	  // 帧出参不可为空：给了 NULL 就没人接这支帧
		if (d == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		if (d->rejected) return CLV_VIDEO_STREAM_REJECTED;
		return TakeNext(d, out, got);	 // 队列空是 OK + got = 0，不是错误
	}
	catch (const std::bad_alloc&)
	{
		if (out != nullptr) *out = nullptr;
		if (got != nullptr) *got = 0u;
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

CLV_VideoError CLV_VideoDecoderFlush(CLV_VideoDecoder* d, CLV_VideoFrame** out, uint8_t* got)
{
	try
	{
		if (out != nullptr) *out = nullptr;
		if (got != nullptr) *got = 0u;
		if (out == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;	  // 与取帧口同一档：帧出参不可为空
		if (d == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		if (d->rejected) return CLV_VIDEO_STREAM_REJECTED;
		if (d->ready.empty() && ! d->flushed)
		{
			// 放出的帧仍带架上那一次持有（队列换成待取队列，持有不换手），取走时才交接给调用方
			for (const OutputItem& o: d->sched.Flush()) d->ready.push_back(d->slots[static_cast<size_t>(o.id)]);
			d->flushed = true;
		}
		return TakeNext(d, out, got);
	}
	catch (const std::bad_alloc&)
	{
		if (out != nullptr) *out = nullptr;
		if (got != nullptr) *got = 0u;
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

CLV_VideoError CLV_VideoDecoderReset(CLV_VideoDecoder* d)
{
	try
	{
		if (d == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		/* 顺序固定：先放掉调度器那两侧的持有（架上 / 参考窗），再作废整池 ——
		 * 反过来会让 VoidAllFrames 看见还挂着参考窗的帧。 */
		d->sched.Reset();
		d->ready.clear();
		d->flushed = false;
		d->primed = false;		// 池重新空了：起池段的策略开关重新起作用
		d->rejected = false;	// 拒解态的唯一出口
		d->VoidAllFrames();
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

CLV_VideoError CLV_VideoDecoderStreamRejected(const CLV_VideoDecoder* d, uint8_t* out)
{
	try
	{
		if (out == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
		if (d == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		*out = d->rejected ? 1u : 0u;
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// *out 只在成功路径写，失败一律不写出参：与入口的判定纪律一致
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

void CLV_VideoDecoderClose(CLV_VideoDecoder* d)
{
	try
	{
		if (d == nullptr) return;
		// 关闭与重置走同一条作废路：先放掉参考窗那一次持有，再作废整池（计数清零、像素缓冲全部回收）
		d->sched.Reset();
		d->ready.clear();
		d->VoidAllFrames();
		delete d;
	}
	// 拆卸口没有回报通道：这里的 catch 是防御性保留（本路径不分配，也没有回报口可写）
	catch (const std::bad_alloc&)
	{
		return;
	}
}

/* ---- 编码器句柄：语法层真写 ---- */

struct CLV_VideoBitstream
{
	std::vector<uint8_t> bytes;
	std::vector<std::pair<size_t, size_t>> slices;	  // [偏移, 长度)；整帧一片时只有一条，覆盖全部字节
};

struct CLV_VideoEncoder
{
	CLV_VideoEncParamsC params {};
	uint32_t coded_width = 0u;
	uint32_t coded_height = 0u;
	uint32_t mb_width = 0u;
	uint32_t mb_height = 0u;
	int64_t last_dts = INT64_MIN;
	uint32_t next_frame_id = 0u;	// 片头 frame_id：只用于把同帧的片归组，不代表顺序
	/* 已进入本 GOP 的**连续 B 帧请求数**：与声明的 max_b_frames 对账，见 CLV_VideoEncoderEncode 入口。 */
	uint32_t b_run = 0u;
	/* 两块帧缓冲，分工不能混：
	 *   ref = **上一帧的重建**，只作预测源，本帧编码期间**一个字节都不许改**；
	 *   cur = 本帧正在写的重建，帧末与 ref 交换，本帧就此成为下一帧的参考。
	 * 预测源整帧只读、本帧重建另写一块：矢量可指到本宏块之前已重建的位置，
	 * 原地写回会让编码端读到本帧重建而解码端读到上一帧重建，两侧不再逐字节全等。
	 * 编码期间 cur 只由重建（ReconstructBlockToPlane）写入，任何预测都不读它。
	 * 懒分配：首帧编码时才建，故建编码器本身不占帧缓冲。B 帧不动这两块 —— B 不作参考。 */
	CLV_VideoFrame* ref = nullptr;
	CLV_VideoFrame* cur = nullptr;
	bool has_ref = false;

	~CLV_VideoEncoder()
	{
		delete ref;
		delete cur;
	}
};

CLV_VideoEncoder* CLV_VideoEncoderOpen(const CLV_VideoEncParamsC* params, CLV_VideoError* err)
{
	try
	{
		if (err != nullptr) *err = CLV_VIDEO_OK;
		if (params == nullptr)
		{
			if (err != nullptr) *err = CLV_VIDEO_INVALID_ARGUMENT;
			return nullptr;
		}
		/* 布局自报在先：非 0 又不等于本版本 sizeof 的对象，库不去猜它的字段在哪，直接拒。 */
		if (params->struct_size != 0u && params->struct_size != sizeof(CLV_VideoEncParamsC))
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}
		if (params->display_width == 0u || params->display_height == 0u)
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}
		if (params->bit_depth != 8u)
		{
			if (err != nullptr) *err = CLV_VIDEO_BIT_DEPTH_UNSUPPORTED;
			return nullptr;
		}
		if (params->quant_precision < kQuantPrecisionMin || params->quant_precision > kQuantPrecisionMax)
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}
		// 模式只有两个取值，第三个值既不是有损也不是无损，判值域不成立
		if (params->mode != CLV_VIDEO_MODE_LOSSY && params->mode != CLV_VIDEO_MODE_LOSSLESS)
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}
		/* `default_quant` 只作声明值：编码算术用的是每次调用传入的 q，本字段既不参与量化也不参与模式判定。
		 * 因此无损模式不校验它；有损模式照旧按本流声明的 quant_precision 取界，不写死档位。 */
		if (params->mode != CLV_VIDEO_MODE_LOSSLESS &&
			(params->default_quant < kQuantMin || params->default_quant > QuantCeiling(params->quant_precision)))
		{
			if (err != nullptr) *err = CLV_VIDEO_QUANT_OUT_OF_RANGE;
			return nullptr;
		}
		/* 补后尺寸积先比像素上界再建句柄：ref / cur 两块重建缓冲是首帧编码时懒分配的，
		 * 界判落在懒分配的上游，越界的声明交不出句柄，也不给编码途中留一条走到分配点才失败的岔路。 */
		const uint32_t coded_w = clv::video::CodedSize(params->display_width);
		const uint32_t coded_h = clv::video::CodedSize(params->display_height);
		if (! clv::video::CodedPixelsWithinBound(coded_w, coded_h))
		{
			if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
			return nullptr;
		}
		auto* e = new (std::nothrow) CLV_VideoEncoder();
		if (e == nullptr)
		{
			if (err != nullptr) *err = CLV_VIDEO_OUT_OF_MEMORY;
			return nullptr;
		}
		e->params = *params;
		e->coded_width = coded_w;
		e->coded_height = coded_h;
		e->mb_width = e->coded_width / kMbSize;
		e->mb_height = e->coded_height / kMbSize;
		// 每片宏块数是编码器配置，不进码流；这里只校验它切出的片数不越分片数上限 65 535
		if (params->slice_mb_count != 0u)
		{
			const uint64_t total = static_cast<uint64_t>(e->mb_width) * e->mb_height;
			const uint64_t n = (total + params->slice_mb_count - 1u) / params->slice_mb_count;
			if (n > kMaxFragmentCount)
			{
				if (err != nullptr) *err = CLV_VIDEO_VALUE_RANGE;
				delete e;
				return nullptr;
			}
		}
		return e;
	}
	catch (const std::bad_alloc&)
	{
		if (err != nullptr) *err = CLV_VIDEO_OUT_OF_MEMORY;
		return nullptr;
	}
}

CLV_VideoError CLV_VideoEncoderAllocFrame(CLV_VideoEncoder* e, CLV_VideoFrame** out)
{
	/* 帧指针提到 try 之外，只为了让 catch 能回收它：*out 只在最后一行交出，
	 * 所以进了 catch 就等于帧还没交出去，delete 一次不会与调用方重复持有。 */
	CLV_VideoFrame* f = nullptr;
	try
	{
		if (out == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
		*out = nullptr;
		if (e == nullptr) return CLV_VIDEO_INVALID_HANDLE;

		f = new (std::nothrow) CLV_VideoFrame();
		if (f == nullptr) return CLV_VIDEO_OUT_OF_MEMORY;
		f->coded_width = e->coded_width;
		f->coded_height = e->coded_height;
		f->display_width = e->params.display_width;
		f->display_height = e->params.display_height;
		f->bit_depth = e->params.bit_depth;
		// 界外尺寸不分配任何平面：交回值域码，*out 留在空指针（不交出半成品帧）
		if (! AllocPlanes(*f))
		{
			delete f;
			return CLV_VIDEO_VALUE_RANGE;
		}
		FillPlanesBlack(ViewOf(*f));
		f->handed = true;	 // 交出去的那一次：归还同样只受理一次，计数归零即释放（本帧不落池）
		*out = f;
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// AllocPlanes 里的 vector::assign 就是本口的抛出源：帧建了却没交出去，回收干净
		delete f;
		if (out != nullptr) *out = nullptr;
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

CLV_VideoError CLV_VideoEncoderEncode(CLV_VideoEncoder* e, CLV_VideoFrame* frame, CLV_VideoFrameType type, uint8_t q,
									  int64_t dts_ticks, int64_t pts_ticks, CLV_VideoBitstream** out)
{
	/* 载荷指针提到 try 之外只为让 catch 能回收它：*out 在最后一行才交出，
	 * 进了 catch 就说明载荷没交出去，delete 一次不会与调用方重复持有。 */
	CLV_VideoBitstream* bs = nullptr;
	try
	{
		if (out != nullptr) *out = nullptr;
		// 时间戳不落 VOP 载荷，只落容器包头（载荷内不重复带），故 pts 在本函数无消费点
		(void) pts_ticks;
		if (e == nullptr || frame == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		if (out == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
		/* 量化步长的域分三段：0 = 无损、1..上界 = 有损、其余域外非法。编码侧还要再看模式：
		 * 无损模式只接受 q = 0，有损模式只接受 [1, 上界]。带非零 q 的无损请求判参数矛盾——
		 * 既不把无损请求悄悄编成最细的有损档，也不把「模式没设对」的请求静默降级。 */
		if (e->params.mode == CLV_VIDEO_MODE_LOSSLESS)
		{
			if (q != kQuantLossless) return CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL;
		}
		else if (q < kQuantMin || q > QuantCeiling(e->params.quant_precision))
		{
			return CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL;
		}
		if (dts_ticks < e->last_dts) return CLV_VIDEO_STATE_ERROR;
		/* max_b_frames 是流的声明（容器 ext_type = 3 块），编码器不得超出：连续 B 的游程到达声明值
		 * 即判值域不成立，不产载荷。声明为 0 时任何 B 请求都不成立。游程按请求序推进 —— 请求之后
		 * 那一步失败也不回退，本判据管的是调用方要出的帧序列，不是成功次数。 */
		if (type == CLV_VIDEO_FRAME_B)
		{
			if (e->b_run >= static_cast<uint32_t>(e->params.max_b_frames)) return CLV_VIDEO_VALUE_RANGE;
			++e->b_run;
		}
		else
		{
			e->b_run = 0u;	  // I / P 断开 B 游程
		}

		clv::video::VopHeader head;
		/* 无参考可依的位置只接受 I 请求。分段容器的段界就是编码器实例界（一个实例编一段，
		 * 段首因此没有上一段的参考可依），这条一并保证「不跨段回引」。
		 * 这里不把帧间请求静默编成帧内：VOP 头仍写 P/B 的话，本库解码侧按参考帧缺失丢帧，
		 * 编出来的段自己读不回来。 */
		if (! e->has_ref && type != CLV_VIDEO_FRAME_I) return CLV_VIDEO_STATE_ERROR;
		head.type = (type == CLV_VIDEO_FRAME_I) ? clv::video::kVopTypeI :
					(type == CLV_VIDEO_FRAME_P) ? clv::video::kVopTypeP :
												  clv::video::kVopTypeB;
		head.quant = q;
		head.fcode_forward = 1u;
		head.fcode_backward = 1u;
		head.intra_dc_vlc_thr = 0u;
		/* 本流用哪一法是编译期事实（公开面不新增入参），但它随每一帧的帧头写进码流：
		 * 解码侧按帧头里那 3 bit 取法，不猜、也不拿建解码器时的假设顶替码流事实。 */
		head.coef_coding = kEncoderCoefCoding;
		head.dquant = 0;
		head.mb_count = static_cast<uint64_t>(e->mb_width) * e->mb_height;

		// 帧级生效量化步长：无损流（q = 0）原样给 0，有损流按 q' = clamp(q + d_vop + d_mb, kQuantMin, 上界)；宏块级再叠各自 dquant
		const uint8_t q_vop = FrameQuant(q, head.dquant, 0, e->params.quant_precision);

		/* 两块缓冲懒分配（建编码器不占帧缓冲），首帧编码时才建。
		 * 两块先建在局部、都成了才挂进句柄：只建成一块会让下一次跳过本分支，另一块永远是空指针。 */
		if (e->ref == nullptr)
		{
			CLV_VideoFrame* made[2] = { nullptr, nullptr };
			VideoErr alloc = VideoErr::kOk;
			for (uint32_t i = 0u; i < 2u; ++i)
			{
				auto* rf = new (std::nothrow) CLV_VideoFrame();
				if (rf == nullptr)
				{
					alloc = VideoErr::kOutOfMemory;
					break;
				}
				rf->coded_width = e->coded_width;
				rf->coded_height = e->coded_height;
				rf->display_width = e->params.display_width;
				rf->display_height = e->params.display_height;
				rf->bit_depth = e->params.bit_depth;
				if (! AllocPlanes(*rf))
				{
					delete rf;
					alloc = VideoErr::kValueRange;
					break;
				}
				FillPlanesBlack(ViewOf(*rf));
				made[i] = rf;
			}
			if (alloc != VideoErr::kOk)
			{
				delete made[0];
				delete made[1];
				return ToPublic(alloc);
			}
			e->ref = made[0];
			e->cur = made[1];
		}
		const PlaneView srcview = ViewOf(*frame);
		const PlaneView refview = ViewOf(*e->ref);	  // 预测源（本帧只读）
		const PlaneView curview = ViewOf(*e->cur);	  // 本帧重建的写入目标

		VopPayload payload;
		payload.head = head;
		if (head.type == clv::video::kVopTypeB)
		{
			/* 编码侧的 B 帧一律 direct + not_coded（不带残差、不带块数据），且不更新参考帧：
			 * B 帧的预测块生成（双向 / direct 缩放）只在解码侧走。
			 * 每格都是同一个形状，故按宏块总数计数填一遍，不必再展开成行列两层。 */
			for (uint64_t i = 0u; i < head.mb_count; ++i)
			{
				BvopMacroblock m;
				m.head.mb_type = clv::video::kBvopDirect;
				m.head.not_coded = true;
				m.head.cbpy = 0u;
				m.head.block_size = 0u;
				payload.bvops.push_back(std::move(m));
			}
		}
		else
		{
			// 无参考位置的帧间请求已在入口拒掉，走到这里 intra 只看帧类型
			const bool intra = (head.type == clv::video::kVopTypeI);
			for (uint32_t my = 0u; my < e->mb_height; ++my)
			{
				for (uint32_t mx = 0u; mx < e->mb_width; ++mx)
				{
					Macroblock m;
					m.head.not_coded = false;
					m.head.mb_type = intra ? clv::video::kMbTypeIntra : clv::video::kMbTypeInter;
					m.head.dquant = 0;
					// 运动估计只在帧间做，且**只读参考帧**（本帧的重建还没开始写）
					const MotionVector mv = intra ? MotionVector {} : EstimateMotion(srcview, refview, mx, my);
					m.head.mv = mv;
					const uint8_t qq = FrameQuant(q_vop, 0, m.head.dquant, e->params.quant_precision);
					uint8_t pattern = 0u;
					const VideoErr be =
						EncodeMacroblockBlocks(srcview, refview, curview, intra, mx, my, qq, mv, &pattern, &m.block);
					if (be != VideoErr::kOk) return static_cast<CLV_VideoError>(be);
					m.head.cbpy = static_cast<uint8_t>(pattern >> 2);		// bit5..2 亮度
					m.head.cbpc = static_cast<uint8_t>(pattern & 0x03u);	// bit1 Cb / bit0 Cr
					m.head.block_size = static_cast<uint32_t>(m.block.size());
					payload.macroblocks.push_back(std::move(m));
				}
			}
			// 本帧重建已就位：交换后 ref 就是本帧的重建，下一帧以它为参考
			std::swap(e->ref, e->cur);
			e->has_ref = true;
		}

		std::vector<std::vector<uint8_t>> pieces;
		const clv::video::VideoErr r = clv::video::EncodeVopSlices(
			e->params.quant_precision, e->mb_width, e->next_frame_id, e->params.slice_mb_count, payload, &pieces);
		if (r != clv::video::VideoErr::kOk) return static_cast<CLV_VideoError>(r);
		if (pieces.size() > 1u) ++e->next_frame_id;	   // 只有真分片时 frame_id 才有意义，单片帧不占号

		bs = new (std::nothrow) CLV_VideoBitstream();
		if (bs == nullptr) return CLV_VIDEO_OUT_OF_MEMORY;
		size_t off = 0u;
		for (std::vector<uint8_t>& piece: pieces)
		{
			bs->slices.emplace_back(off, piece.size());
			off += piece.size();
			bs->bytes.insert(bs->bytes.end(), piece.begin(), piece.end());
		}
		// 整帧一片时该片就是整帧载荷（不含片头）：上面的 emplace 已按同一件数记好，这里不再补一遍
		e->last_dts = dts_ticks;
		*out = bs;	  // 所有权交给调用方，由 CLV_VideoBitstreamRelease 回收
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// 组载荷的 emplace_back / insert 是抛出源：载荷没交出去就回收，*out 回到入口的清空态
		delete bs;
		if (out != nullptr) *out = nullptr;
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

void CLV_VideoEncoderClose(CLV_VideoEncoder* e)
{
	try
	{
		delete e;
	}
	// 拆卸口没有回报通道：释放路径上的分配失败只能就地吞掉，绝不许穿出 C 边界
	catch (const std::bad_alloc&)
	{
		return;
	}
}

/* 实现层测试钩子（`encoder_hook.h`）：暴露编码器自己的参考帧 = 最近一次 I / P 的内部重建。
 * 不带 CLV_EXPORT、不进 ABI；单测用它逐字节比对编码器内部重建与解码输出。 */
CLV_VideoFrame* clv_internal_encoder_reconstruction(const CLV_VideoEncoder* e)
{
	if (e == nullptr || ! e->has_ref) return nullptr;
	return e->ref;
}

CLV_VideoError CLV_VideoBitstreamGetBytes(const CLV_VideoBitstream* bs, const uint8_t** data, size_t* size)
{
	try
	{
		if (bs == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		if (data == nullptr || size == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
		*data = bs->bytes.data();
		*size = bs->bytes.size();
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		// 出参只在成功路径写，失败一律不写：不留半份指针 + 长度
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

CLV_VideoError CLV_VideoBitstreamSliceCount(const CLV_VideoBitstream* bs, uint32_t* out)
{
	try
	{
		if (bs == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		if (out == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
		*out = static_cast<uint32_t>(bs->slices.size());
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

CLV_VideoError CLV_VideoBitstreamGetSlice(const CLV_VideoBitstream* bs, uint32_t index, const uint8_t** data,
										  size_t* size)
{
	try
	{
		if (bs == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		if (data == nullptr || size == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
		if (index >= bs->slices.size()) return CLV_VIDEO_VALUE_RANGE;
		*data = bs->bytes.data() + bs->slices[index].first;
		*size = bs->slices[index].second;
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

void CLV_VideoBitstreamRelease(CLV_VideoBitstream* bs)
{
	try
	{
		delete bs;
	}
	// 拆卸口没有回报通道：释放路径上的分配失败只能就地吞掉，绝不许穿出 C 边界
	catch (const std::bad_alloc&)
	{
		return;
	}
}

uint32_t CLV_VideoFrameAddRef(CLV_VideoFrame* f)
{
	try
	{
		// 已作废的帧不复活：与空句柄同档返 0，不加计数
		if (f == nullptr || ! f->live) return 0u;
		return ++f->refs;
	}
	catch (const std::bad_alloc&)
	{
		return 0u;	  // 计数口没有 OOM 码可给，按空句柄同档返 0
	}
}

void CLV_VideoFrameRelease(CLV_VideoFrame* f)
{
	try
	{
		(void) GiveBackFrame(f, false);	   // 不回报的那扇门：判不外露，回收动作与 Free 同一份
	}
	// 拆卸口没有回报通道：这里的 catch 是防御性保留（回收路径本身不分配）
	catch (const std::bad_alloc&)
	{
		return;
	}
}

CLV_VideoError CLV_VideoFrameFree(CLV_VideoFrame* f)
{
	try
	{
		// 空指针 / 已被重置或关闭作废 / 交出那一次已经还过（重复归还）三档都判参数不成立，不改计数
		return GiveBackFrame(f, true) ? CLV_VIDEO_OK : CLV_VIDEO_INVALID_ARGUMENT;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

CLV_VideoError CLV_VideoFrameGetDesc(CLV_VideoFrame* f, CLV_VideoFrameDescC* desc, CLV_VideoFrameInfoC* info)
{
	try
	{
		if (f == nullptr) return CLV_VIDEO_INVALID_HANDLE;
		if (! f->live) return CLV_VIDEO_INVALID_ARGUMENT;	 // 作废帧不写出参：缓冲已回收
		if (desc != nullptr)
		{
			desc->pixel_format = CLV_VIDEO_PIX_YUV420P;
			desc->bit_depth = f->bit_depth;
			desc->coded_width = f->coded_width;
			desc->coded_height = f->coded_height;
			desc->display_width = f->display_width;
			desc->display_height = f->display_height;
			for (int p = 0; p < 3; ++p)
			{
				desc->stride[p] = f->stride[p];
				desc->plane[p] = f->buf[p].data();
			}
		}
		if (info != nullptr)
		{
			*info = f->info;
			info->disposition = CLV_VIDEO_DISP_OUTPUT;
		}
		return CLV_VIDEO_OK;
	}
	catch (const std::bad_alloc&)
	{
		return CLV_VIDEO_OUT_OF_MEMORY;
	}
}

/* StrError 一族不在收口口径内：它只做码 → 字符串字面量的映射，全程不分配，没有抛出源。
 * 给它套 catch 反而要为「OOM 时返回哪个串」编一个公开头里没承诺的值，故整族保持原样。 */
const char* CLV_VideoStrError(CLV_VideoError err)
{
	switch (err)
	{
	case CLV_VIDEO_OK: return "ok";
	case CLV_VIDEO_INVALID_HANDLE: return "invalid handle";
	case CLV_VIDEO_INVALID_ARGUMENT: return "invalid argument";
	case CLV_VIDEO_VALUE_RANGE: return "value range";
	case CLV_VIDEO_PAYLOAD_TOO_SMALL: return "payload too small";
	case CLV_VIDEO_HEADER_BAD: return "vop header";
	case CLV_VIDEO_QUANT_OUT_OF_RANGE: return "quant range";
	case CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL: return "quant range per call";
	case CLV_VIDEO_BIT_DEPTH_UNSUPPORTED: return "bit depth";
	case CLV_VIDEO_TOOL_EXCLUDED: return "tool excluded";
	case CLV_VIDEO_TOOL_LIST_ABSENT: return "tool list absent";
	/* 留白号位 73 / 78 / 81 / 82 这四格落到 default 分支返回 "unknown"，
	 * 「空位没人占」这件事由用例的段内遍历守住。 */
	case CLV_VIDEO_SLICE_HEADER_BAD: return "slice header";
	case CLV_VIDEO_SLICE_SEQUENCE: return "slice sequence";
	case CLV_VIDEO_REF_FRAME_MISSING: return "reference missing";
	case CLV_VIDEO_OUTPUT_TOO_SMALL: return "output too small";
	case CLV_VIDEO_STATE_ERROR: return "state error";
	case CLV_VIDEO_OUT_OF_MEMORY: return "out of memory";
	case CLV_VIDEO_STREAM_REJECTED: return "stream rejected";
	case CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH: return "payload length mismatch";
	}
	return "unknown";
}

