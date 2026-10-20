/* mtav.cpp —— 视音频同容器随机混合编码闭环探针
 *
 * 三模式串成一条链，中间态落盘，任一段可单独复跑：
 *   enc  按 plan 的单元序列编视频载荷 + 音频载荷，落**三份**中间件：
 *        store.bin（载荷仓）/ store.idx（索引）/ streaminfo.bin（音频流信息，34 字节）
 *   mux  从载荷仓按**随机交错序**写双流分段 .clv（三份中间件都要读，缺 streaminfo.bin 就停手）
 *   dec  OpenSegments 连续读，按 stream_id 分派，解回裸 YUV 与裸 PCM 并出对账数
 *
 * 只经公开 C ABI（CLV_Video.h / CLV_Audio.h / CLV_Container.h）：本件不 include 任何实现层头，
 * 参数块与流信息的解读一律走公开头给出的解读口（CLV_VideoParseEncMeta、CLV_AudioInspectFrame）。
 *
 * 本件不进构建面 —— 仓库里没有任何 CMake 目标或指令编译它，复现需手工编译。
 * 手工编译命令形（MSVC；两条按手里那份库挑一条，`<构建目录>` 换成自己的那一份）：
 *   cl /utf-8 /std:c++17 /EHsc /DCLV_USE_DLL /Iinclude scripts/mtav.cpp /link <构建目录>/clv.lib
 *   cl /utf-8 /std:c++17 /EHsc /Iinclude scripts/mtav.cpp <构建目录>/clv_static.lib
 * 第一条配动态件（配置期带 -DCLV_BUILD_SHARED=ON 才有 clv.lib 那份导入库），必须带 /DCLV_USE_DLL
 * 才拿得到 dllimport 声明；第二条配静态件，libFLAC 是私密链进 clv_static 的（见根 CMakeLists.txt
 * 的 clv_configure_library_target），手工链接侧要自己把那份 FLAC 库一并带上。
 * `/utf-8` 不能省：公开头与本件的中文都是无 BOM 的 UTF-8，仓库 target 也恒带这个开关（见根
 * CMakeLists.txt 的 clv_apply_target_flags）；缺它时编译器按本地代码页解字节，报出来的是一串
 * 看着像头文件坏了的连锁错，真错因是源字符集没指定。
 * 跑法：mtav enc <plan> <dir> → mtav mux <plan> <dir> → mtav dec <plan> <dir>。
 * 随机性只由 plan 的 seed 与段号决定，单元编排也落在 plan 里。本仓不带产出 plan 的脚本，
 * plan 由调用方自备（手写即可，语法见下一段）。
 *
 * plan 语法（一行一条指令；列之间至少一个空格；下表列数必须一字不差，缺列与多余列都判错；
 * 首个非空列以 '#' 起始的整行是注释；空行忽略；下表之外的一切关键字判 PLAN_FAIL 停手）：
 *   W <宽>                    原始显示宽，[1, 65535]，缺省 1280
 *   H <高>                    原始显示高，[1, 65535]，缺省 720
 *   QP <量化精度>             本流的量化精度声明，合法域 [3, 9]，v1 = 7
 *   LOSSLESS <0|1>            1 = 视频走无损档 ⇒ 本 plan 每个单元的 q 必须为 0
 *   BUDGET_V <字节>           一段内视频载荷的字节预算，非 0；到线即在下一帧切段
 *   BUDGET_A <字节>           一段内音频载荷的字节预算，非 0；到线即由下一帧视频切段
 *   SEED <数>                 包交错随机种子的基数，可缺省 0
 *   MAT <名> <yuv> <pcm> <帧数> <样本数>   一份素材：裸 YUV420P 路径、裸 S16LE PCM 路径、两条长度
 *   UNIT <素材名> <起始帧> <帧数> <q> <I帧周期>   一个编码单元；q 取 [1, 2^QP−1]（LOSSLESS=1 时恒 0），
 *                             周期 0 = 只在单元首帧与段首帧强制 I
 * 段文件名按三位编号排版（av_000.clv、av_001.clv…；段号越过 999 自然变四位）。
 * 路径列里不能带空格：列按空白切，带空格会被切成两列，落到「列数与表不符」那一档判错。
 * 单元的音频样本数不抄 MAT 那一列，按帧数在 30 fps 轴上占的秒数 × 44100 现算，缘由见 LoadPlan 内那条。
 *
 * 产物确定性：写盘的每一样（store.bin / store.idx / streaminfo.bin / av_*.clv / mux_report.txt /
 * dec_all.yuv / dec_all.pcm / dec_timeline.txt）里都没有时钟量 ⇒ 同一 plan 同一输入重跑逐字节可复现，
 * 不要给它们补时间戳。标准输出是跑况读数、不作对账件：唯一的墙钟量（编码步耗时）单独打一行
 * ENC_TIME，机器可读的 ENC_DONE / MUX / DEC_DONE 三族行不含时钟字段，要逐字 diff 就只 diff 这些行。
 *
 * 段界口径：段界只落在「被强制成 I 帧的那一帧」之前 —— 视频 P 帧回引上一帧的重建，
 * 一段是一个独立解码单元，不强制就跨段回引了（容器规范「分段容器」摘要：段首帧必须是 I 帧，编码侧义务）。
 * 音频侧 FLAC 帧本来自立，切在哪都行，但两条流共处一个文件，**段游标只有一个**。
 */
#include "CLV_Audio.h"
#include "CLV_Container.h"
#include "CLV_Video.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

static const uint8_t kVidStream = 0;	 // stream_id：视频
static const uint8_t kAudStream = 1;	 // stream_id：音频
static const uint64_t kVidTbDen = 30;	 // 视频时间基 = 1/30，dts 即全局帧号
static const uint64_t kGlobalTickPerSec =
	1081080000u;	// 全局 tick 刻度（文件头 duration_ticks 用的就是这一格），一帧 = 36 036 000 tick
/* 一帧在全局 tick 轴上的跨度 = 1 081 080 000 ÷ 30 = 36 036 000。两个操作数都取有符号：段刻度这一步
 * 是有符号乘法，喂进负 dts 的形状（仓内就有 neg_dts.clv 这种件）算出来的是负的 tick 数，不会因其中
 * 一侧是无符号而把整个乘法折成巨值；只有交给写入参数那一格时才显式转成该字段的无符号型。 */
static const int64_t kVidTickPerFrame = (int64_t) (kGlobalTickPerSec / kVidTbDen);
static const uint32_t kAudRate = 44100u;
static const uint16_t kAudCh = 2u;
static const uint16_t kAudBits = 16u;
static const uint16_t kAudBytesPerSample = kAudBits / 8u;	 // = 2

/* 裸 PCM 的字节数 = 样本数 × 声道 × 每样本字节。素材偏移与喂编码器的缓冲长度两处共用这一处算式：
 * 改声道数或位深只动上面两个常量，不会出现「偏移按旧宽度算、长度按新宽度算」的两本账。 */
static uint64_t PcmBytes(uint64_t samples) { return samples * kAudCh * kAudBytesPerSample; }

/* packed YUV420P 一帧的字节数（8 bit、三平面连续、不带行距）：编码侧取整帧与解码侧记字节走同一个式子。 */
static uint64_t Yuv420Bytes(uint64_t w, uint64_t h) { return w * h * 3ull / 2ull; }

/* 音频流信息载荷的字节数。出处 = CLV_Audio.h 对流信息载荷的声明（34 字节），那里没给符号常量，
 * 本件把它收成一处：写侧封装前判一次、读侧解回后再判一次，两处指向同一个名字。 */
static const size_t kStreamInfoBytes = 34;

/* 视频参数块（ext_type = 3）的载荷就是这 8 字节：写侧把 CLV_VideoEncMetaC 整块交出去、
 * 读侧走公开口 CLV_VideoParseEncMeta 解回来，两侧同一份布局 ⇒ 把「布局等于 8 字节」钉成编译期判点，
 * 布局一变在编译期就响，不留到封装环节静默错位。 */
static_assert(sizeof(CLV_VideoEncMetaC) == 8u, "ext_type = 3 的载荷长度按本头的 8 字节布局声明");

struct Mat
{
	std::string name, yuv, pcm;
	uint64_t frames = 0, samples = 0;
};

struct Unit
{
	std::string mat;
	uint64_t start = 0, frames = 0, samples = 0;
	uint8_t q = 0;
	uint32_t ird = 0;
};

struct Plan
{
	uint32_t W = 1280, H = 720;
	uint8_t qp = 7;
	bool lossless = false;	  // plan 里 `LOSSLESS 1` ⇒ 视频走无损档，此时每个单元的 q 必须为 0（LoadPlan 判住）
	uint64_t budget_v = 0, budget_a = 0;
	uint64_t seed = 0;
	std::vector<Mat> mats;
	std::vector<Unit> units;
};

/* 一帧载荷的账目：段号、仓内偏移、字节数、该流绝对 dts、关键帧标志。
 * 索引件的第 6 列（量化档 / 块大小回显）不进这张结构 —— 封装侧只消费上面五项；
 * 读索引时那一列照样算在列数里，列数不对的索引件当场暴露，而不是被读成 0 继续用。 */
struct Packet
{
	uint32_t seg = 0;
	uint64_t off = 0, size = 0, dts = 0;
	uint8_t key = 0;
};

struct Idx
{
	std::vector<Packet> V, A;
};

static const Mat* FindMat(const Plan& p, const std::string& n)
{
	for (auto& m: p.mats)
		if (m.name == n) return &m;
	return nullptr;
}

static uint64_t FrameBytes(const Plan& p) { return Yuv420Bytes(p.W, p.H); }

/* 整份读进内存：plan 与索引件都是小文本，固定行缓冲会把超长行（带绝对路径的那条素材行）截成两行
 * 各自解析 —— 那比报错更难查，所以这里不设行长上限。长度取不到就判不成立，不拿猜出来的长度往下走。 */
static bool ReadAll(FILE* f, std::string* out)
{
	if (_fseeki64(f, 0, SEEK_END) != 0) return false;
	const long long len = _ftelli64(f);
	if (len < 0) return false;
	if (_fseeki64(f, 0, SEEK_SET) != 0) return false;
	out->assign((size_t) len, '\0');
	if (len > 0 && std::fread(&(*out)[0], 1, (size_t) len, f) != (size_t) len)
	{
		out->clear();
		return false;
	}
	return true;
}

/* 按空白串切列：连续两个空格不再产出空字段（空字段会被下一档读成 0）。 */
static std::vector<std::string> SplitCols(const std::string& s)
{
	static const char* kWs = " \t\r\n\v\f";
	std::vector<std::string> out;
	size_t pos = 0;
	for (;;)
	{
		const size_t b = s.find_first_not_of(kWs, pos);
		if (b == std::string::npos) break;
		const size_t e = s.find_first_of(kWs, b);
		out.push_back(e == std::string::npos ? s.substr(b) : s.substr(b, e - b));
		pos = (e == std::string::npos) ? s.size() : e;
	}
	return out;
}

/* 计划与索引件里的每个数字都过这一道：整列必须是十进制非负整数，且落在 [lo, hi]，否则把原文报回去。
 * out 与 err 都可以给空指针（索引件那一侧只判成不成立，不带话术）。
 * 不带域的转换（atoi / strtoul 直接把结果截给窄类型）是本件最容易骗人的一档 —— 量化档写 256 会被
 * (uint8_t) 折成 0，而 0 在码流里是无损信号，一份坏 plan 就此变成一次看着正常的无损跑。
 * 合法域取自公开头：量化精度域 [3,9] 与原始显示尺寸域 [1,65535]（include/CLV_Video.h），
 * 量化档上界 = 2^量化精度 − 1（同文件的量化域那条：0 = 无损、1..2^p−1 = 有损、其余域外非法）。 */
static bool ParseCol(const std::string& col, uint64_t lo, uint64_t hi, const char* what, uint64_t* out,
					 std::string* err)
{
	auto fail = [&](const std::string& msg)
	{
		if (err != nullptr) *err = std::string(what) + ": " + msg;
		return false;
	};
	if (col.empty()) return fail("empty column");
	uint64_t v = 0;
	for (char ch: col)
	{
		if (ch < '0' || ch > '9') return fail("not a non-negative decimal integer: '" + col + "'");
		const uint64_t d = (uint64_t) (ch - '0');
		if (v > (UINT64_MAX - d) / 10ull) return fail("beyond uint64: '" + col + "'");
		v = v * 10ull + d;
	}
	if (v < lo || v > hi) return fail("out of range [" + std::to_string(lo) + ", " + std::to_string(hi) + "]: " + col);
	if (out != nullptr) *out = v;
	return true;
}

static bool LoadPlan(const char* path, Plan* p, std::string* err)
{
	FILE* f = nullptr;
	if (fopen_s(&f, path, "rb") != 0 || ! f)
	{
		*err = "plan open";
		return false;
	}
	std::string text;
	const bool whole = ReadAll(f, &text);
	fclose(f);
	if (! whole)
	{
		*err = "plan read";
		return false;
	}
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos) nl = text.size();
		const std::vector<std::string> tk = SplitCols(text.substr(pos, nl - pos));
		pos = nl + 1;
		if (tk.empty()) continue;
		if (tk[0][0] == '#') continue;
		const size_t n = tk.size();
		uint64_t v = 0;
		if (tk[0] == "W" && n == 2)
		{
			if (! ParseCol(tk[1], 1ull, 65535ull, "W", &v, err)) return false;
			p->W = (uint32_t) v;	// 上面那一道的域判把值收在 [1,65535] ⇒ 窄化到公开头的 16 位显示尺寸不丢值
		}
		else if (tk[0] == "H" && n == 2)
		{
			if (! ParseCol(tk[1], 1ull, 65535ull, "H", &v, err)) return false;
			p->H = (uint32_t) v;	// 同上
		}
		else if (tk[0] == "QP" && n == 2)
		{
			if (! ParseCol(tk[1], 3ull, 9ull, "QP", &v, err)) return false;
			p->qp = (uint8_t) v;
		}
		else if (tk[0] == "LOSSLESS" && n == 2)
		{
			if (! ParseCol(tk[1], 0ull, 1ull, "LOSSLESS", &v, err)) return false;
			p->lossless = (v != 0);
		}
		else if (tk[0] == "BUDGET_V" && n == 2)
		{
			if (! ParseCol(tk[1], 1ull, UINT64_MAX, "BUDGET_V", &v, err)) return false;
			p->budget_v = v;
		}
		else if (tk[0] == "BUDGET_A" && n == 2)
		{
			if (! ParseCol(tk[1], 1ull, UINT64_MAX, "BUDGET_A", &v, err)) return false;
			p->budget_a = v;
		}
		else if (tk[0] == "SEED" && n == 2)
		{
			if (! ParseCol(tk[1], 0ull, UINT64_MAX, "SEED", &v, err)) return false;
			p->seed = v;
		}
		else if (tk[0] == "MAT" && n == 6)
		{
			Mat m;
			m.name = tk[1];
			m.yuv = tk[2];
			m.pcm = tk[3];
			if (! ParseCol(tk[4], 1ull, UINT64_MAX, "MAT frames", &v, err)) return false;
			m.frames = v;
			if (! ParseCol(tk[5], 1ull, UINT64_MAX, "MAT samples", &v, err)) return false;
			m.samples = v;
			p->mats.push_back(m);
		}
		else if (tk[0] == "UNIT" && n == 6)
		{
			Unit u;
			u.mat = tk[1];
			if (! ParseCol(tk[2], 0ull, UINT64_MAX, "UNIT start", &v, err)) return false;
			u.start = v;
			if (! ParseCol(tk[3], 1ull, UINT64_MAX, "UNIT frames", &v, err)) return false;
			u.frames = v;
			if (! ParseCol(tk[4], 0ull, 255ull, "UNIT q", &v, err)) return false;
			u.q = (uint8_t) v;
			if (! ParseCol(tk[5], 0ull, UINT32_MAX, "UNIT ird", &v, err)) return false;
			u.ird = (uint32_t) v;
			/* 音频样本数按「该单元帧数在 30 fps 轴上占的秒数 × 44100」定，不抄素材里的 samples 字段 ——
			 * 编子区间时两者不等，抄了就把音视频时长拉岔。 */
			const Mat* m = FindMat(*p, u.mat);
			if (! m)
			{
				*err = "UNIT references unknown MAT " + u.mat;
				return false;
			}
			u.samples = (uint64_t) ((double) u.frames / (double) kVidTbDen * (double) kAudRate + 0.5);
			if (u.start > UINT64_MAX - u.frames || u.start > UINT64_MAX - u.samples || u.start + u.frames > m->frames ||
				u.start + u.samples > m->samples)
			{
				*err = "UNIT slice beyond MAT";
				return false;
			}
			p->units.push_back(u);
		}
		else
		{
			*err =
				"bad plan token: " + tk[0] + " (关键字不在表内，或列数与表不符，本行共 " + std::to_string(n) + " 列)";
			return false;
		}
	}
	if (p->units.empty() || p->mats.empty() || p->budget_v == 0 || p->budget_a == 0)
	{
		*err = "plan incomplete";
		return false;
	}
	/* 无损档与量化域这两道判点放在读完整份 plan 之后走：QP 与 UNIT 的先后不该改变判定，
	 * 而把矛盾的计划留到第一帧才炸（库侧那一道「模式与参数矛盾」）等于让一次跑白烧到中段。
	 * 判式与库侧同式：无损 ⇒ 每个单元的 q 恰为 0；有损 ⇒ q 落在 [1, 2^quant_precision − 1]。 */
	const uint64_t qcap = (1ull << p->qp) - 1ull;
	for (const Unit& u: p->units)
	{
		if (p->lossless && u.q != 0)
		{
			*err = "LOSSLESS=1 要求单元 " + u.mat + " 的 q 为 0，实得 " + std::to_string(u.q);
			return false;
		}
		if (! p->lossless && (u.q < 1 || (uint64_t) u.q > qcap))
		{
			*err = "单元 " + u.mat + " 的 q 越出有损档合法域 [1, " + std::to_string(qcap) + "]，实得 " +
				   std::to_string(u.q);
			return false;
		}
	}
	return true;
}

static void SegPath(std::string& out, const std::string& dir, uint32_t i)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "av_%03u.clv", i);
	out = dir + "/" + buf;
}

/* ================================ enc ================================ */

struct EncRes
{
	int rc = 0;
	std::string err;
	uint64_t vpkt = 0, apkt = 0, segs = 0, vbytes = 0, abytes = 0;
};

/* 把一帧 packed YUV 铺进编码器给的帧缓冲（带补边）。补边一律铺黑电平 16 / 中性色度 128，且只补右与下：
 * 补后尺寸 = 原始尺寸向上取整到 16 的倍数（本头的 CodedSize 口），只有非 16 倍数的那一侧才产生补边宽度
 * （例如高 1080 补到 1088；1920、1280、720 都是 16 的倍数，补边宽度为 0）。
 * 反过来按 coded_* 整行去 memcpy 源数据，在非 16 倍数的尺寸上就是读越界 —— 源里没有补边那几列。
 * 描述符取不到就判失败回报：铺了半帧去编码，编出来的载荷与源帧对不上账。 */
static bool FillFrame(CLV_VideoFrame* fr, const uint8_t* src, uint32_t W, uint32_t H)
{
	CLV_VideoFrameDescC d;
	std::memset(&d, 0, sizeof(d));
	if (CLV_VideoFrameGetDesc(fr, &d, nullptr) != CLV_VIDEO_OK) return false;
	const uint32_t cw = d.coded_width, ch = d.coded_height;
	for (uint32_t y = 0; y < ch; ++y)
	{
		uint8_t* dst = d.plane[0] + (size_t) y * d.stride[0];
		if (y < H)
		{
			memcpy(dst, src + (size_t) y * W, W);
			if (cw > W) memset(dst + W, 16, cw - W);
		}
		else
		{
			memset(dst, 16, cw);
		}
	}
	const uint8_t* cu = src + (size_t) W * H;
	const uint8_t* cv = cu + (size_t) (W / 2) * (H / 2);
	for (uint32_t y = 0; y < ch / 2; ++y)
	{
		uint8_t* du = d.plane[1] + (size_t) y * d.stride[1];
		uint8_t* dv = d.plane[2] + (size_t) y * d.stride[2];
		if (y < H / 2)
		{
			memcpy(du, cu + (size_t) y * (W / 2), W / 2);
			memcpy(dv, cv + (size_t) y * (W / 2), W / 2);
			if (cw / 2 > W / 2)
			{
				memset(du + W / 2, 128, cw / 2 - W / 2);
				memset(dv + W / 2, 128, cw / 2 - W / 2);
			}
		}
		else
		{
			memset(du, 128, cw / 2);
			memset(dv, 128, cw / 2);
		}
	}
	return true;
}

/* 帧归还走带回报的那扇门（CLV_VideoFrameFree）：本件是公开 ABI 的外部消费者，归还的返回码与其余公开口
 * 同规格判掉，不成立的三档（传入为空 / 该帧已被重置或关闭作废 / 同一帧重复归还）都在跑况里留一行。
 * 不带回报的那扇门只在库内计数上等价，判不出「这次归还到底受理没受理」，而本件的占用账
 * （同时在场的帧数 = 参考缓冲 maxlen + 未归还数）恰恰要靠这一格。
 * 归还后该帧指针失效，置空由调用点自己做。 */
static bool FreeVideoFrame(CLV_VideoFrame* fr, const char* stage)
{
	const CLV_VideoError rr = CLV_VideoFrameFree(fr);
	if (rr != CLV_VIDEO_OK)
	{
		std::printf("VID_FRAME_FREE_ERR stage=%s rc=%d %s\n", stage, (int) rr, CLV_VideoStrError(rr));
		return false;
	}
	return true;
}

/* 落仓与落盘的每次写都要求写满：写不进就是这一份中间件废了，继续跑只会产出一堆对不上账的件，
 * 所以每一处写返回值（fread 之外的 fwrite / fprintf / fclose）都判，并把失败变成非 0 的退出码。 */
static bool PutAll(FILE* f, const void* data, size_t n) { return n == 0 || fwrite(data, 1, n, f) == n; }

/* 抽干音频编码器当前能产出的帧，逐帧落仓。
 * dts 由**逐帧实测块大小**累加导出 —— CLV_Audio.h「时间轴」那条的口径：容器带的 dts_delta 只作校验、
 * 不作时间来源；变长块流上拿「首帧块大小恒定」推后续会把整条音轨推歪。
 * 出参 cut：本批里音频包数越了预算，需要由**下一帧视频**落段（段游标只有一个，而 I 帧只能落在视频帧上）。
 * 返回值 0 = 本批干净；非 0 由调用方转成失败退出，具体哪一档看这一路打出的 AUD_* / STORE_* 行。 */
static int DrainAudio(CLV_AudioEncoder* ea, FILE* sb, FILE* si, uint32_t seg, uint64_t* sample_base, uint64_t* aseg,
					  uint64_t budget_a, uint64_t* apkt, uint64_t* abytes, uint8_t* cut)
{
	for (;;)
	{
		const uint8_t* data = nullptr;
		size_t size = 0;
		uint8_t got = 0;
		const CLV_AudioError er = CLV_AudioEncoderNextFrame(ea, &data, &size, &got);
		if (er != CLV_AUDIO_OK)
		{
			std::printf("AUD_NEXT_ERR=%d %s\n", (int) er, CLV_AudioStrError(er));
			return (int) er;
		}
		if (! got) break;
		if (data == nullptr || size == 0 || size > 0xFFFFFFFFu)
		{
			std::printf("AUD_BAD_SIZE %p %zu\n", (const void*) data, size);
			return -1;
		}
		CLV_AudioFrameHeadC hh;
		std::memset(&hh, 0, sizeof(hh));
		const CLV_AudioError ir = CLV_AudioInspectFrame(data, size, &hh);
		if (ir != CLV_AUDIO_OK)
		{
			std::printf("AUD_INSPECT_ERR=%d %s size=%zu\n", (int) ir, CLV_AudioStrError(ir), size);
			return (int) ir;
		}
		const long long off = _ftelli64(sb);
		if (off < 0)
		{
			std::printf("STORE_TELL_FAIL size=%zu\n", size);
			return -2;
		}
		if (! PutAll(sb, data, size))
		{
			std::printf("AUD_STORE_WRITE_FAIL seg=%u size=%zu\n", seg, size);
			return -2;
		}
		/* 索引件的每一行都得整行落下：落半截读回来就是列数不对，或偏移与长度错位，两头都对不上账。 */
		if (std::fprintf(si, "A %u %lld %zu %llu 0 %u\n", seg, off, size, (unsigned long long) *sample_base,
						 hh.blocksize) < 0)
		{
			std::printf("AUD_IDX_WRITE_FAIL seg=%u\n", seg);
			return -2;
		}
		*sample_base += hh.blocksize;
		*aseg += size;
		(*apkt)++;
		*abytes += size;
		if (*aseg >= budget_a)
		{
			*cut = 1;
			*aseg = 0;
		}
	}
	return 0;
}

static EncRes mode_enc(const Plan& p, const std::string& dir)
{
	EncRes r;
	const std::string sp = dir + "/store.bin", ip = dir + "/store.idx";
	FILE* sb = nullptr;
	FILE* si = nullptr;
	if (fopen_s(&sb, sp.c_str(), "wb") != 0 || ! sb)
	{
		r.rc = 1;
		r.err = "store.bin";
		return r;
	}
	if (fopen_s(&si, ip.c_str(), "wb") != 0 || ! si)
	{
		r.rc = 2;
		r.err = "store.idx";
		fclose(sb);
		return r;
	}

	const uint64_t FB = FrameBytes(p);
	std::vector<uint8_t> fb(FB), pcm;

	CLV_VideoError verr = CLV_VIDEO_OK;
	CLV_VideoEncParamsC ep;
	std::memset(&ep, 0, sizeof(ep));
	ep.struct_size = sizeof(ep);
	ep.display_width = (uint16_t) p.W;	  // LoadPlan 的域判把 W / H 收在 [1,65535] ⇒ 这一档窄化不丢值
	ep.display_height = (uint16_t) p.H;
	ep.bit_depth = 8;
	ep.quant_precision = p.qp;
	ep.default_quant = p.units[0].q;
	ep.max_b_frames = 0;
	ep.slice_mb_count = 0;
	ep.intra_refresh_period = 0;	// I 的节奏由本探针定（单元首帧 / 周期 / 段首帧三处）
	ep.timebase_den = kVidTbDen;
	ep.mode = p.lossless ? CLV_VIDEO_MODE_LOSSLESS : CLV_VIDEO_MODE_LOSSY;
	CLV_VideoEncoder* ev = CLV_VideoEncoderOpen(&ep, &verr);
	if (! ev)
	{
		r.rc = 3;
		r.err = "VidEncOpen";
		std::printf("VID_ENC_OPEN_ERR=%d %s\n", (int) verr, CLV_VideoStrError(verr));
		fclose(sb);
		fclose(si);
		return r;
	}
	CLV_VideoFrame* fr = nullptr;
	/* 这一份持有跨整个编码循环：一块入帧缓冲逐帧铺数据、逐帧编码，最后一次编码用完才在收尾处归还。
	 * 循环里不逐帧归还的判点是「这块缓冲下一帧还要不要铺」——本件按单元顺序复用同一块，收走了下一帧
	 * 就没地方铺。交不出帧的档里帧出参停在空指针（本口每条失败路都先把出参写空再返回），手里没帧可还。 */
	verr = CLV_VideoEncoderAllocFrame(ev, &fr);
	if (verr != CLV_VIDEO_OK || ! fr)
	{
		r.rc = 4;
		r.err = "AllocFrame";
		std::printf("VID_ALLOC_ERR rc=%d %s\n", (int) verr, CLV_VideoStrError(verr));
		if (fr != nullptr && ! FreeVideoFrame(fr, "alloc_fail")) r.err = "AllocFrame + frame not returned";
		fr = nullptr;
		CLV_VideoEncoderClose(ev);
		fclose(sb);
		fclose(si);
		return r;
	}

	CLV_AudioError ae = CLV_AUDIO_OK;
	CLV_AudioEncParamsC ap;
	std::memset(&ap, 0, sizeof(ap));
	ap.struct_size = sizeof(ap);
	ap.sample_rate = kAudRate;
	ap.channels = kAudCh;
	ap.bit_depth = kAudBits;
	ap.blocksize = 4096;	// 采样率 ≤ 48000 时流式子集的块上界是 4608，取 4096
	ap.level = 5;
	CLV_AudioEncoder* ea = CLV_AudioEncoderOpen(&ap, &ae);
	if (! ea)
	{
		r.rc = 5;
		r.err = "AudEncOpen";
		std::printf("AUD_ENC_OPEN_ERR=%d %s\n", (int) ae, CLV_AudioStrError(ae));
		if (! FreeVideoFrame(fr, "aud_enc_open")) r.err = "AudEncOpen + frame not returned";
		fr = nullptr;
		CLV_VideoEncoderClose(ev);
		fclose(sb);
		fclose(si);
		return r;
	}

	uint32_t cur_seg = 0;
	uint64_t vseg_bytes = 0, aseg_bytes = 0;
	uint64_t vno = 0, sample_base = 0;
	uint8_t pend_cut = 0;
	auto t0 = std::chrono::steady_clock::now();
	int bad = 0;

	for (size_t ui = 0; ui < p.units.size() && ! bad; ++ui)
	{
		const Unit& u = p.units[ui];
		const Mat* m = FindMat(p, u.mat);
		FILE* vf = nullptr;
		FILE* af = nullptr;
		if (fopen_s(&vf, m->yuv.c_str(), "rb") != 0 || ! vf)
		{
			r.rc = 6;
			r.err = "yuv open " + m->yuv;
			bad = 1;
			break;
		}
		if (fopen_s(&af, m->pcm.c_str(), "rb") != 0 || ! af)
		{
			r.rc = 7;
			r.err = "pcm open " + m->pcm;
			fclose(vf);
			bad = 1;
			break;
		}
		/* 素材偏移两处都从「样本数 → 字节数」那一个式子导出（PcmBytes）， seek 的返回码判住：
		 * 定位没到位就停手，往下读的是错位的素材，编出来的每一帧都跟账对不上。 */
		if (_fseeki64(vf, (long long) (u.start * FB), SEEK_SET) != 0 ||
			_fseeki64(af, (long long) PcmBytes((u.start * kAudRate) / kVidTbDen), SEEK_SET) != 0)
		{
			r.rc = 19;
			r.err = "mat seek " + m->name;
			std::printf("MAT_SEEK_FAIL mat=%s start=%llu\n", m->name.c_str(), (unsigned long long) u.start);
			fclose(vf);
			fclose(af);
			bad = 1;
			break;
		}

		std::printf("UNIT %zu mat=%s start=%llu frames=%llu samples=%llu q=%u ird=%u\n", ui, u.mat.c_str(),
					(unsigned long long) u.start, (unsigned long long) u.frames, (unsigned long long) u.samples, u.q,
					u.ird);
		uint64_t fed = 0;
		for (uint64_t j = 0; j < u.frames; ++j)
		{
			/* 音频按帧切片喂：每帧样本数 = 该帧在 30 fps 轴上占的那段。
			 * 用 floor 差分而非 round 累加，各帧之和恰等于 u.samples，末帧再吃掉舍入余量。 */
			const uint64_t cum0 = (j * kAudRate) / kVidTbDen;
			uint64_t cum1 = ((j + 1) * kAudRate) / kVidTbDen;
			if (j + 1 == u.frames) cum1 = u.samples;
			const uint32_t nsamp = (uint32_t) (cum1 - cum0);

			if (fread(fb.data(), 1, FB, vf) != FB)
			{
				r.rc = 8;
				r.err = "yuv short read";
				bad = 1;
				break;
			}

			const bool by_v = (vseg_bytes >= p.budget_v);
			const bool by_a = (pend_cut != 0);
			if (by_v || by_a)
			{
				cur_seg++;
				vseg_bytes = 0;
				aseg_bytes = 0;
				pend_cut = 0;
				std::printf("SEG_CUT at=%u by=%s\n", cur_seg, by_v ? "video" : "audio");
			}
			const bool is_i = by_v || by_a || j == 0 || (u.ird != 0 && (j % u.ird) == 0);

			if (! FillFrame(fr, fb.data(), p.W, p.H))
			{
				r.rc = 20;
				r.err = "FrameGetDesc f" + std::to_string(vno);
				std::printf("VID_DESC_FAIL f=%llu\n", (unsigned long long) vno);
				bad = 1;
				break;
			}
			CLV_VideoBitstream* bs = nullptr;
			verr = CLV_VideoEncoderEncode(ev, fr, is_i ? CLV_VIDEO_FRAME_I : CLV_VIDEO_FRAME_P, u.q, (int64_t) vno,
										  (int64_t) vno, &bs);
			if (verr != CLV_VIDEO_OK || ! bs)
			{
				r.rc = 9;
				r.err = "VidEncode f" + std::to_string(vno);
				std::printf("VID_ENC_ERR f=%llu rc=%d %s\n", (unsigned long long) vno, (int) verr,
							CLV_VideoStrError(verr));
				bad = 1;
				break;
			}
			const uint8_t* pb = nullptr;
			size_t psz = 0;
			const CLV_VideoError brs = CLV_VideoBitstreamGetBytes(bs, &pb, &psz);
			const long long off = _ftelli64(sb);
			if (brs != CLV_VIDEO_OK || pb == nullptr || off < 0 || ! PutAll(sb, pb, psz) ||
				std::fprintf(si, "V %u %lld %zu %llu %d %u\n", cur_seg, off, psz, (unsigned long long) vno,
							 is_i ? 1 : 0, u.q) < 0)
			{
				r.rc = 21;
				r.err = "store write f" + std::to_string(vno);
				std::printf("VID_STORE_FAIL f=%llu rc=%d size=%zu off=%lld\n", (unsigned long long) vno, (int) brs, psz,
							off);
				CLV_VideoBitstreamRelease(bs);
				bad = 1;
				break;
			}
			CLV_VideoBitstreamRelease(bs);
			vseg_bytes += psz;
			r.vpkt++;
			r.vbytes += psz;
			vno++;

			pcm.resize((size_t) PcmBytes(nsamp));
			if (fread(pcm.data(), 1, pcm.size(), af) != pcm.size())
			{
				r.rc = 10;
				r.err = "pcm short read";
				bad = 1;
				break;
			}
			fed += nsamp;
			ae = CLV_AudioEncoderWrite(ea, pcm.data(), pcm.size(), nsamp);
			if (ae != CLV_AUDIO_OK)
			{
				r.rc = 11;
				r.err = "AudWrite";
				std::printf("AUD_WRITE_ERR=%d %s nsamp=%u\n", (int) ae, CLV_AudioStrError(ae), nsamp);
				bad = 1;
				break;
			}
			if (DrainAudio(ea, sb, si, cur_seg, &sample_base, &aseg_bytes, p.budget_a, &r.apkt, &r.abytes, &pend_cut) !=
				0)
			{
				r.rc = 12;
				r.err = "DrainAudio (see AUD_*/STORE_* line above)";
				bad = 1;
				break;
			}
		}
		fclose(vf);
		fclose(af);
		if (! bad && fed != u.samples)
		{
			r.rc = 13;
			r.err = "audio slice count mismatch";
			bad = 1;
		}
	}

	if (! bad)
	{
		/* 冲刷尾帧：冲完才算本流样本喂尽，此后 TakeStreamInfo 才有意义。 */
		ae = CLV_AudioEncoderFlush(ea);
		if (ae != CLV_AUDIO_OK)
		{
			r.rc = 14;
			r.err = "AudFlush";
			std::printf("AUD_FLUSH_ERR=%d\n", (int) ae);
			bad = 1;
		}
	}
	if (! bad)
	{
		uint8_t dummy = 0;
		if (DrainAudio(ea, sb, si, cur_seg, &sample_base, &aseg_bytes, (uint64_t) -1, &r.apkt, &r.abytes, &dummy) != 0)
		{
			r.rc = 15;
			r.err = "DrainAudio tail (see AUD_*/STORE_* line above)";
			bad = 1;
		}
	}
	if (! bad)
	{
		CLV_AudioBlob* blob = nullptr;
		ae = CLV_AudioEncoderTakeStreamInfo(ea, &blob);
		if (ae != CLV_AUDIO_OK || ! blob)
		{
			r.rc = 16;
			r.err = "TakeStreamInfo";
			std::printf("AUD_SI_ERR=%d\n", (int) ae);
			bad = 1;
		}
		else
		{
			const uint8_t* sd = nullptr;
			size_t ss = 0;
			const CLV_AudioError br = CLV_AudioBlobGetBytes(blob, &sd, &ss);
			if (br != CLV_AUDIO_OK || sd == nullptr)
			{
				r.rc = 17;
				r.err = "blob bytes";
				std::printf("BLOB_ERR=%d\n", (int) br);
			}
			else
			{
				FILE* wf = nullptr;
				if (fopen_s(&wf, (dir + "/streaminfo.bin").c_str(), "wb") != 0 || ! wf)
				{
					r.rc = 18;
					r.err = "streaminfo open";
				}
				else
				{
					/* 先写完再关：关闭本身也可能把缓冲里的最后一笔失败带出来（写成短路就把句柄漏了）。
					 * 这一份是封装步的必需件（缺它 mux 直接停手），落一半等于落了一份坏的。 */
					const bool written = PutAll(wf, sd, ss);
					const bool closed = (fclose(wf) == 0);
					if (! written || ! closed)
					{
						r.rc = 23;
						r.err = "streaminfo write";
						std::printf("STREAMINFO_WRITE_FAIL size=%zu written=%d closed=%d\n", ss, written ? 1 : 0,
									closed ? 1 : 0);
					}
					else
					{
						std::printf("STREAMINFO size=%zu audio_total_samples=%llu\n", ss,
									(unsigned long long) sample_base);
					}
				}
			}
			CLV_AudioBlobRelease(blob);
		}
	}
	r.segs = cur_seg + 1;
	const auto t1 = std::chrono::steady_clock::now();
	/* 载荷仓与索引件要等 fclose 才把缓冲倒干净，写失败可能只在这一道露头 ⇒ 关闭的返回码也判。 */
	const int close_mask = (fclose(sb) != 0 ? 1 : 0) | (fclose(si) != 0 ? 2 : 0);
	if (close_mask != 0 && r.rc == 0)
	{
		r.rc = 22;
		r.err = "store close fail";
		std::printf("STORE_CLOSE_FAIL mask=%d\n", close_mask);
	}
	std::printf("ENC_DONE rc=%d vpkt=%llu apkt=%llu segs=%u vbytes=%llu abytes=%llu\n", r.rc,
				(unsigned long long) r.vpkt, (unsigned long long) r.apkt, (unsigned) r.segs,
				(unsigned long long) r.vbytes, (unsigned long long) r.abytes);
	/* 唯一的墙钟读数单独一行：机器可读的那几族行（ENC_DONE / MUX / DEC_DONE）不含时钟字段，
	 * 要逐字 diff 跑日志就跳过这一行。 */
	std::printf("ENC_TIME ms=%.0f\n", std::chrono::duration<double, std::milli>(t1 - t0).count());

	CLV_AudioEncoderClose(ea);
	/* 编码循环的最后一格：这块入帧缓冲到这里再用完，立刻归还。归还的返回码判掉，
	 * 本跑一路干净时就把它记成失败，不留「帧没还但退出码 0」的形态。 */
	if (! FreeVideoFrame(fr, "enc_tail") && r.rc == 0)
	{
		r.rc = 24;
		r.err = "FrameFree (enc)";
	}
	fr = nullptr;
	CLV_VideoEncoderClose(ev);
	return r;
}

/* ================================ mux ================================ */

/* 索引件按列读：第 1 列是 V / A，其后依次是段号、仓内偏移、字节数、该流绝对 dts、关键帧标志、
 * 量化档 / 块大小回显。整份读进内存再按列切（固定行缓冲会把长行截断成两行各自解析），列数与列值都判。
 * 这份件由本件自己产出，读回来对不上就是它坏了 ⇒ 直接判不成立，不带着错账往下封。
 * 第 6 列照样数进列数，但只占位读掉：封装侧不消费它（见 Packet 结构那条）。 */
static bool LoadIdx(const std::string& ip, Idx* out)
{
	FILE* f = nullptr;
	if (fopen_s(&f, ip.c_str(), "rb") != 0 || ! f) return false;
	std::string text;
	const bool whole = ReadAll(f, &text);
	fclose(f);
	if (! whole) return false;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos) nl = text.size();
		const std::vector<std::string> tk = SplitCols(text.substr(pos, nl - pos));
		pos = nl + 1;
		if (tk.empty()) continue;
		if (tk[0] != "V" && tk[0] != "A") continue;
		if (tk.size() != 7) return false;
		uint64_t seg = 0, off = 0, sz = 0, dts = 0, key = 0;
		if (! ParseCol(tk[1], 0ull, UINT32_MAX, "idx seg", &seg, nullptr) ||
			! ParseCol(tk[2], 0ull, INT64_MAX, "idx off", &off, nullptr) ||
			! ParseCol(tk[3], 1ull, UINT32_MAX, "idx size", &sz, nullptr) ||
			! ParseCol(tk[4], 0ull, UINT64_MAX, "idx dts", &dts, nullptr) ||
			! ParseCol(tk[5], 0ull, 1ull, "idx key", &key, nullptr) ||
			! ParseCol(tk[6], 0ull, UINT32_MAX, "idx echo", nullptr, nullptr))
		{
			return false;
		}
		Packet pk;
		pk.seg = (uint32_t) seg;
		pk.off = off;
		pk.size = (size_t) sz;
		pk.dts = dts;
		pk.key = (uint8_t) key;
		if (tk[0] == "V") out->V.push_back(pk);
		else out->A.push_back(pk);
	}
	return ! out->V.empty() && ! out->A.empty();
}

struct MuxRes
{
	int rc = 0;
	std::string err;
};

/* 索引件只解析一遍：段数由主流程按这一份解析结果算出，本步直接用传下来的那一份驱动写入。
 * 改前这里还解析第二遍，两遍之间不核对 ⇒ 同一份索引件两本账，段号越界那一类判也保不住。 */
static MuxRes mode_mux(const Plan& p, const std::string& dir, uint32_t nseg, const Idx& idx)
{
	MuxRes r;
	FILE* sf = nullptr;
	if (fopen_s(&sf, (dir + "/store.bin").c_str(), "rb") != 0 || ! sf)
	{
		r.rc = 2;
		r.err = "store open";
		return r;
	}
	std::vector<uint8_t> streaminfo;
	{
		FILE* f = nullptr;
		if (fopen_s(&f, (dir + "/streaminfo.bin").c_str(), "rb") != 0 || ! f)
		{
			r.rc = 3;
			r.err = "streaminfo open";
			fclose(sf);
			return r;
		}
		/* 长度一律走 64 位的定位口并判返回码：32 位的 ftell 取回 -1 时按 size_t 收就是巨大长度，
		 * 拿它去 resize 只会把错误推迟成一次看不懂的分配失败。 */
		std::string blob;
		const bool got = ReadAll(f, &blob);
		fclose(f);
		if (! got)
		{
			r.rc = 4;
			r.err = "streaminfo read";
			fclose(sf);
			return r;
		}
		streaminfo.assign(blob.begin(), blob.end());
	}
	if (streaminfo.size() != kStreamInfoBytes)
	{
		r.rc = 5;
		r.err = "streaminfo size";
		fclose(sf);
		return r;
	}

	std::vector<std::vector<const Packet*>> bv(nseg), ba(nseg);
	for (const Packet& k: idx.V) bv[k.seg].push_back(&k);
	for (const Packet& k: idx.A) ba[k.seg].push_back(&k);

	std::vector<uint8_t> pay;
	FILE* rep = nullptr;
	/* 包序指纹件也要开得成才算这一步走通：打不开就报非 0，不静默跳过 ⇒
	 * 少了这份件，跑完看不出交错序是哪一种，复现无从对起。 */
	if (fopen_s(&rep, (dir + "/mux_report.txt").c_str(), "wb") != 0 || ! rep)
	{
		r.rc = 15;
		r.err = "mux report open";
		std::printf("MUX_REPORT_OPEN_FAIL dir=%s\n", dir.c_str());
		fclose(sf);
		return r;
	}

	for (uint32_t s = 0; s < nseg && r.rc == 0; ++s)
	{
		/* 一条流在这一段里一帧都没有 ⇒ 该段凑不出 first_dts，写进去就是坏段。
		 * 预算切得过碎时会现这种段，判错停手而不是硬写。 */
		if (bv[s].empty() || ba[s].empty())
		{
			r.rc = 6;
			r.err = "segment " + std::to_string(s) + " has an empty stream";
			std::printf("SEG_EMPTY seg=%u nv=%zu na=%zu\n", s, bv[s].size(), ba[s].size());
			break;
		}
		CLV_ContainerError cerr = CLV_CONTAINER_OK;
		CLV_WriteParams wp;
		std::memset(&wp, 0, sizeof(wp));
		wp.struct_size = sizeof(wp);
		wp.index_present = 1;
		wp.globally_sorted = 0;	   // 包区按写入序 = 随机交错序；跨流呈现顺序由使用方按各流时间基重排
		wp.segmented = 1;
		wp.seg_flags = 0x03;	// bit0 seg_total 已知 / bit1 段首帧在视频流上是关键帧（切段即强制 I）
		wp.seg_index = s;
		wp.seg_total = nseg;
		/* 段起止刻度要的是**全局 tick**（1 / 1 081 080 000 秒），不是本流帧号：本流时间基 1/30 ⇒ 一帧 36 036 000 tick。
		 * 直接把帧 dts 写进来就与同一文件里头部的 duration_ticks 差一个换算因子，而库侧只搬不解释
		 * ⇒ 段刻度静默错位、照常封得进文件，故必须换算后再写。
		 * 乘法两个操作数都取 int64_t（kVidTickPerFrame）⇒ 喂进负 dts 时算的是负的 tick 数，
		 * 不会因为操作数里有一侧无符号就把整个乘法折成巨值；最后一档才显式转成该字段的无符号型。 */
		const int64_t v_start_ticks = (int64_t) bv[s].front()->dts * kVidTickPerFrame;
		const int64_t v_end_ticks = (int64_t) bv[s].back()->dts * kVidTickPerFrame;
		wp.seg_start_ticks = (uint64_t) v_start_ticks;
		wp.seg_end_ticks = (uint64_t) v_end_ticks;
		std::string path;
		SegPath(path, dir, s);
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), &wp, &cerr);
		if (! w)
		{
			r.rc = 7;
			r.err = "WrOpen seg" + std::to_string(s);
			std::printf("WR_OPEN_ERR seg=%u rc=%d %s\n", s, (int) cerr, CLV_ContainerStrError(cerr));
			break;
		}
		/* 首段 first_dts 恒 0（零起点），后续段 = 本段该流首帧的绝对 dts。两条流各自独立取。 */
		CLV_StreamDescC vd, ad;
		std::memset(&vd, 0, sizeof(vd));
		vd.stream_id = kVidStream;
		vd.stream_type = 0;
		vd.codec_version = 1;
		vd.timebase_num = 1;
		vd.timebase_den = kVidTbDen;
		vd.max_packet_size = 8388567u;
		vd.bit_depth = 8;
		vd.first_dts = s == 0 ? 0 : bv[s].front()->dts;
		std::memset(&ad, 0, sizeof(ad));
		ad.stream_id = kAudStream;
		ad.stream_type = 1;
		ad.codec_id = 1;
		ad.codec_version = 1;
		ad.timebase_num = 1;
		ad.timebase_den = kAudRate;
		ad.max_packet_size = 262144u;
		ad.sample_rate = kAudRate;
		ad.channels = kAudCh;
		ad.bit_depth = kAudBits;
		ad.first_dts = s == 0 ? 0 : ba[s].front()->dts;
		if ((cerr = CLV_ContainerWriterAddStream(w, &vd)) != CLV_CONTAINER_OK)
		{
			r.rc = 8;
			r.err = "AddStream V";
			std::printf("ADDV_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
		}
		else if ((cerr = CLV_ContainerWriterAddStream(w, &ad)) != CLV_CONTAINER_OK)
		{
			r.rc = 9;
			r.err = "AddStream A";
			std::printf("ADDA_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
		}
		else
		{
			CLV_VideoEncMetaC em;
			std::memset(&em, 0, sizeof(em));
			em.quant_precision = p.qp;
			em.display_width = (uint16_t) p.W;	  // LoadPlan 的域判把 W 收在 [1,65535] ⇒ 窄化不丢值
			em.display_height = (uint16_t) p.H;
			/* 参数块的载荷就是这 8 字节（CLV_Video.h 的 ext_type = 3 声明），读回侧走 CLV_VideoParseEncMeta 解，
			 * 两侧同一份布局；max_b_frames 与 ext_tools 本件恒 0。 */
			if ((cerr = CLV_ContainerWriterAddExtBlock(w, kVidStream, 3, 1, &em, sizeof(em))) != CLV_CONTAINER_OK)
			{
				r.rc = 10;
				r.err = "ExtBlock V";
				std::printf("EXTV_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
			}
			else if ((cerr = CLV_ContainerWriterAddExtBlock(w, kAudStream, 5, 1, streaminfo.data(),
															streaminfo.size())) != CLV_CONTAINER_OK)
			{
				r.rc = 11;
				r.err = "ExtBlock A";
				std::printf("EXTA_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
			}
			else
			{
				/* 随机交错：取哪条流的下一包按各自剩余数加权 ⇒ 两桶同时见底。
				 * 每条流自己的 dts 非降由桶内次序保证，与跨流顺序无关（writer.cpp AdvanceTimeline 逐流判）。 */
				std::mt19937_64 rng(p.seed + 0x9E3779B97F4A7C15ull * (s + 1));
				size_t iv = 0, ia = 0;
				uint64_t nv = 0, na = 0, vbytes = 0, abytes = 0;
				std::string layout;	   // 包序指纹：'v'/'a' 串，随机性可直接目视
				while (iv < bv[s].size() || ia < ba[s].size())
				{
					const size_t rv = bv[s].size() - iv, ra = ba[s].size() - ia;
					const bool take_v =
						(rv == 0) ? false :
						(ra == 0) ? true :
									(std::generate_canonical<double, 20>(rng) < (double) rv / (double) (rv + ra));
					const Packet* pk = take_v ? bv[s][iv++] : ba[s][ia++];
					if (pay.size() < pk->size) pay.resize(pk->size);
					if (_fseeki64(sf, (long long) pk->off, SEEK_SET) != 0 ||
						fread(pay.data(), 1, pk->size, sf) != pk->size)
					{
						r.rc = 12;
						r.err = "store read";
						std::printf("STORE_READ_FAIL seg=%u off=%llu size=%zu\n", s, (unsigned long long) pk->off,
									pk->size);
						break;
					}
					cerr = CLV_ContainerWriterWriteFrame(w, take_v ? kVidStream : kAudStream, pk->key,
														 (int64_t) pk->dts, (int64_t) pk->dts, pay.data(), pk->size);
					if (cerr != CLV_CONTAINER_OK)
					{
						r.rc = 13;
						r.err = "WriteFrame seg" + std::to_string(s);
						std::printf("WRF_ERR seg=%u kind=%c dts=%llu rc=%d %s\n", s, take_v ? 'V' : 'A',
									(unsigned long long) pk->dts, (int) cerr, CLV_ContainerStrError(cerr));
						break;
					}
					layout.push_back(take_v ? 'v' : 'a');
					if (take_v)
					{
						nv++;
						vbytes += pk->size;
					}
					else
					{
						na++;
						abytes += pk->size;
					}
				}
				if (r.rc == 0)
				{
					uint64_t pk_out = 0;
					if (CLV_ContainerWriterFinish(w, &pk_out) != CLV_CONTAINER_OK)
					{
						r.rc = 14;
						r.err = "Finish seg" + std::to_string(s);
						std::printf("FINISH_ERR seg=%u\n", s);
					}
					else
					{
						struct _stati64 st;
						uint64_t fsz = 0;
						if (_stati64(path.c_str(), &st) == 0) fsz = (uint64_t) st.st_size;
						std::printf("MUX seg=%u nv=%llu na=%llu pkts=%llu vbytes=%llu abytes=%llu file=%llu "
									"vfirst=%llu vlast=%llu afirst=%llu alast=%llu\n",
									s, (unsigned long long) nv, (unsigned long long) na, (unsigned long long) pk_out,
									(unsigned long long) vbytes, (unsigned long long) abytes, (unsigned long long) fsz,
									(unsigned long long) bv[s].front()->dts, (unsigned long long) bv[s].back()->dts,
									(unsigned long long) ba[s].front()->dts, (unsigned long long) ba[s].back()->dts);
						if (std::fprintf(rep, "SEG %u LAYOUT %s\n", s, layout.c_str()) < 0)
						{
							r.rc = 16;
							r.err = "mux report write seg" + std::to_string(s);
							std::printf("MUX_REPORT_WRITE_FAIL seg=%u\n", s);
						}
					}
				}
			}
		}
		CLV_ContainerWriterClose(w);
	}
	/* 报告件与载荷仓的关闭都判：写缓冲里的失败只在这一道才露头，漏掉就是「跑绿了但件是半份」。
	 * 两份件在这条路径上都必定已开成（开不成已在前面带非 0 退出），所以这里不再判空。 */
	const int close_mask = (fclose(rep) != 0 ? 1 : 0) | (fclose(sf) != 0 ? 2 : 0);
	if (close_mask != 0 && r.rc == 0)
	{
		r.rc = 17;
		r.err = "mux close fail";
		std::printf("MUX_CLOSE_FAIL mask=%d\n", close_mask);
	}
	if (r.rc == 0) std::printf("MUX_DONE segs=%u\n", nseg);
	else std::printf("MUX_FAIL rc=%d %s\n", r.rc, r.err.c_str());
	return r;
}

/* ================================ dec ================================ */

struct DecRes
{
	int rc = 0;
	std::string err;
	uint64_t vframes = 0, asamples = 0, vbytes = 0, abytes = 0;
};

/* 解回的这一腿不需要 plan 里的任何声明：尺寸、量化精度、B 帧数都从流里的参数块取（下面那一圈
 * 建解码器的入参全部来自 CLV_VideoParseEncMeta 的产出），plan 只在主流程那侧供帧数与样本数对账。 */
static DecRes mode_dec(const std::string& dir, uint32_t nseg)
{
	DecRes r;
	std::vector<std::string> paths;
	for (uint32_t s = 0; s < nseg; ++s)
	{
		std::string q;
		SegPath(q, dir, s);
		paths.push_back(q);
	}
	std::vector<const char*> raw;
	for (auto& q: paths) raw.push_back(q.c_str());

	CLV_ContainerError cerr = CLV_CONTAINER_OK;
	CLV_ContainerReader* rd = CLV_ContainerReaderOpenSegments(raw.data(), (uint32_t) paths.size(), 1, &cerr);
	if (! rd)
	{
		r.rc = 1;
		r.err = "OpenSegments";
		std::printf("OPENSEG_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
		return r;
	}
	uint32_t nsc = 0;
	cerr = CLV_ContainerReaderStreamCount(rd, &nsc);
	if (cerr != CLV_CONTAINER_OK)
	{
		r.rc = 30;
		r.err = "StreamCount";
		std::printf("STREAM_COUNT_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
		CLV_ContainerReaderClose(rd);
		return r;
	}
	int vid = -1, aud = -1;
	CLV_StreamDescC vd, ad;
	std::memset(&vd, 0, sizeof(vd));
	std::memset(&ad, 0, sizeof(ad));
	for (uint32_t i = 0; i < nsc; ++i)
	{
		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		uint8_t usable = 0;
		if (CLV_ContainerReaderGetStream(rd, i, &d, &usable) != CLV_CONTAINER_OK) continue;
		if (d.stream_type == 0)
		{
			vid = d.stream_id;
			vd = d;
		}
		else if (d.stream_type == 1)
		{
			aud = d.stream_id;
			ad = d;
		}
		std::printf("STREAM i=%u id=%u type=%u codec=%u tb=%llu/%llu sr=%u ch=%u bits=%u maxpk=%u first_dts=%llu "
					"usable=%u\n",
					i, d.stream_id, d.stream_type, d.codec_id, (unsigned long long) d.timebase_num,
					(unsigned long long) d.timebase_den, d.sample_rate, d.channels, d.bit_depth, d.max_packet_size,
					(unsigned long long) d.first_dts, usable);
	}
	if (vid < 0 || aud < 0)
	{
		r.rc = 2;
		r.err = "stream missing";
		CLV_ContainerReaderClose(rd);
		return r;
	}

	uint32_t nec = 0;
	cerr = CLV_ContainerReaderExtBlockCount(rd, &nec);
	if (cerr != CLV_CONTAINER_OK)
	{
		r.rc = 31;
		r.err = "ExtBlockCount";
		std::printf("EXT_COUNT_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
		CLV_ContainerReaderClose(rd);
		return r;
	}
	std::vector<uint8_t> streaminfo;
	CLV_VideoEncMetaC em;
	std::memset(&em, 0, sizeof(em));
	bool have_em = false;
	for (uint32_t i = 0; i < nec; ++i)
	{
		CLV_ExtBlockC eb;
		std::memset(&eb, 0, sizeof(eb));
		if (CLV_ContainerReaderGetExtBlock(rd, i, &eb) != CLV_CONTAINER_OK) continue;
		if (eb.data == nullptr && eb.size != 0)
		{
			std::printf("EXT_NO_DATA i=%u type=%u size=%zu\n", i, eb.ext_type, eb.size);
			continue;
		}
		std::printf("EXT i=%u sid=%u type=%u ver=%u size=%zu\n", i, eb.stream_id, eb.ext_type, eb.ext_version, eb.size);
		if (eb.stream_id == (uint8_t) aud && eb.ext_type == 5) streaminfo.assign(eb.data, eb.data + eb.size);
		if (eb.stream_id == (uint8_t) vid && eb.ext_type == 3)
		{
			/* 参数块走公开解读口 CLV_VideoParseEncMeta，不在本件里按结构体布局手拆字节：
			 * 那一道口专判载荷长度、保留位与 ext_tools、量化精度域 [3,9]、宽高非 0，
			 * 本件要的正是同一份判定（与 demo/video.c 的走法一致）。解不成就当参数块缺失处理，
			 * 让下面那道「缺参数块 ⇒ 建不成解码器」的判点接住，不拿一份没验过的声明往下解。 */
			CLV_VideoEncMetaC parsed;
			std::memset(&parsed, 0, sizeof(parsed));
			const CLV_VideoError pr = CLV_VideoParseEncMeta(eb.data, eb.size, &parsed);
			if (pr != CLV_VIDEO_OK)
			{
				std::printf("PARAM_PARSE_ERR rc=%d %s size=%zu\n", (int) pr, CLV_VideoStrError(pr), eb.size);
				continue;
			}
			em = parsed;
			have_em = true;
		}
	}
	if (streaminfo.size() != kStreamInfoBytes || ! have_em)
	{
		r.rc = 3;
		r.err = "ext block absent";
		std::printf("EXT_MISS si=%zu have_em=%d\n", streaminfo.size(), have_em ? 1 : 0);
		CLV_ContainerReaderClose(rd);
		return r;
	}

	CLV_VideoStreamParamsC vp;
	std::memset(&vp, 0, sizeof(vp));
	vp.struct_size = sizeof(vp);
	vp.bit_depth = 8;
	/* 建解码器的流侧参数一律取自参数块（公开头写明宽高与量化精度必须由它声明，缺块或声明为 0 打不开解码器）。
	 * 走了解读口之后 quant_precision 必在 [3,9] 内、宽高必非 0 ⇒ 原来那句「解不出就退回 plan 里的 QP」
	 * 成一档死分支，删掉：拿 plan 的声明盖过流里的声明正是这类件最容易掩盖的错位。 */
	vp.quant_precision = em.quant_precision;
	vp.max_b_frames = em.max_b_frames;	  // 公开头写明它是本流 B 帧数的唯一权威声明 ⇒ 从参数块取，不靠清零留 0
	vp.display_width = em.display_width;
	vp.display_height = em.display_height;
	vp.obmc_disable = 1;
	CLV_VideoError verr = CLV_VIDEO_OK;
	CLV_VideoDecoder* dv = CLV_VideoDecoderOpen(&vp, &verr);
	if (! dv)
	{
		r.rc = 4;
		r.err = "VidDecOpen";
		std::printf("VID_DEC_OPEN_ERR=%d %s\n", (int) verr, CLV_VideoStrError(verr));
		CLV_ContainerReaderClose(rd);
		return r;
	}
	CLV_AudioStreamParamsC asp;
	std::memset(&asp, 0, sizeof(asp));
	asp.struct_size = sizeof(asp);
	asp.sample_rate = ad.sample_rate;
	asp.channels = ad.channels;
	asp.bit_depth = ad.bit_depth;
	asp.max_packet_size = ad.max_packet_size;
	CLV_AudioError aer = CLV_AUDIO_OK;
	CLV_AudioDecoder* da = CLV_AudioDecoderOpen(&asp, streaminfo.data(), streaminfo.size(), &aer);
	if (! da)
	{
		r.rc = 5;
		r.err = "AudDecOpen";
		std::printf("AUD_DEC_OPEN_ERR=%d %s\n", (int) aer, CLV_AudioStrError(aer));
		CLV_VideoDecoderClose(dv);
		CLV_ContainerReaderClose(rd);
		return r;
	}

	FILE* yf = nullptr;
	FILE* pf = nullptr;
	FILE* tf = nullptr;
	if (fopen_s(&yf, (dir + "/dec_all.yuv").c_str(), "wb") != 0 || ! yf)
	{
		r.rc = 6;
		r.err = "yuv out";
	}
	if (r.rc == 0 && (fopen_s(&pf, (dir + "/dec_all.pcm").c_str(), "wb") != 0 || ! pf))
	{
		r.rc = 7;
		r.err = "pcm out";
	}
	if (r.rc == 0 && (fopen_s(&tf, (dir + "/dec_timeline.txt").c_str(), "wb") != 0 || ! tf))
	{
		r.rc = 8;
		r.err = "timeline out";
	}

	const uint32_t W = em.display_width, H = em.display_height;
	/* 累加器逐段归零：后续段不平移 ⇒ 该段首包的 dts_delta 本身就是绝对 dts，
	 * 「跨段一个累加器加到底」不成立（CLV_Container.h 的 OpenSegments 注释写死了这条）。 */
	uint64_t v_dts = 0, a_dts = 0, a_expect = 0;
	uint64_t cur_seg = 0;
	bool first_seg = true;
	uint64_t vdrop = 0, adrop = 0;
	uint64_t vout_flushed = 0;

	/* 排空视频输出队列。flushing=1 走 Flush 口（段尾/流尾把固有延迟那几帧放出来），
	 * 否则走 NextOutput 口。漏掉这一步的实测形态就是「帧缺口 = 段数」。 */
	auto drain = [&](int flushing) -> int
	{
		CLV_VideoFrame* of = nullptr;
		uint8_t og = 0;
		for (;;)
		{
			verr = flushing ? CLV_VideoDecoderFlush(dv, &of, &og) : CLV_VideoDecoderNextOutput(dv, &of, &og);
			if (verr != CLV_VIDEO_OK)
			{
				/* 两条取帧口都在入口先把帧出参写成空指针，不成立的档一个字节都不交出 ⇒ 这一支手里没有帧可还；
				 * 对空指针调归还只会落参数不成立那一档，那条报行会被读成失败原因，盖掉真正的返回码。 */
				std::printf("VOUT_ERR flush=%d rc=%d %s\n", flushing, (int) verr, CLV_VideoStrError(verr));
				return 11;
			}
			if (! og) break;
			/* 描述符先清零再取、判过返回码才用：拿没写成的描述符去写文件，读到的是空指针或上一帧的行距。
			 * 归还点留在每一轮末尾：平面字节指向库拥有的缓冲，只到该帧最后一个引用归还为止有效，
			 * 写盘必须排在归还之前；取描述符失败与写盘失败两支都同样走到这里，不在那里另开一处。 */
			CLV_VideoFrameDescC d;
			std::memset(&d, 0, sizeof(d));
			const CLV_VideoError dr = CLV_VideoFrameGetDesc(of, &d, nullptr);
			int bad = 0;
			if (dr != CLV_VIDEO_OK || d.plane[0] == nullptr || d.plane[1] == nullptr || d.plane[2] == nullptr)
			{
				std::printf("VDESC_ERR flush=%d rc=%d %s\n", flushing, (int) dr, CLV_VideoStrError(dr));
				bad = 36;
			}
			else
			{
				int werr = 0;
				for (uint32_t y = 0; y < H; ++y)
					if (! PutAll(yf, d.plane[0] + (size_t) y * d.stride[0], W)) werr = 1;
				for (uint32_t y = 0; y < H / 2; ++y)
					if (! PutAll(yf, d.plane[1] + (size_t) y * d.stride[1], W / 2)) werr = 1;
				for (uint32_t y = 0; y < H / 2; ++y)
					if (! PutAll(yf, d.plane[2] + (size_t) y * d.stride[2], W / 2)) werr = 1;
				if (werr)
				{
					std::printf("YUV_WRITE_FAIL frame=%llu\n", (unsigned long long) r.vframes);
					bad = 36;
				}
				else
				{
					r.vframes++;
					r.vbytes += Yuv420Bytes(W, H);
					if (flushing) vout_flushed++;
				}
			}
			if (! FreeVideoFrame(of, "dec_drain") && ! bad) bad = 43;
			of = nullptr;
			if (bad) return bad;
		}
		return 0;
	};

	if (r.rc == 0)
	{
		CLV_FrameC f;
		uint8_t got = 0;
		for (;;)
		{
			cerr = CLV_ContainerReaderNextFrame(rd, &f, &got);
			if (cerr != CLV_CONTAINER_OK)
			{
				r.rc = 9;
				r.err = "NextFrame";
				std::printf("NEXTFRAME_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
				break;
			}
			if (! got) break;
			/* 段号在 NextFrame 之后查：本帧来自哪一段，句柄的「当前段」就已经走到哪一段。
			 * 查不动就判失败退出，不拿手里那个旧值当读数 —— 那不是「没换段」，那是没查到。 */
			uint64_t si = cur_seg;
			cerr = CLV_ContainerReaderSegmentIndex(rd, &si);
			if (cerr != CLV_CONTAINER_OK)
			{
				r.rc = 32;
				r.err = "SegmentIndex";
				std::printf("SEG_INDEX_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
				break;
			}
			if (! first_seg && si != cur_seg)
			{
				if (std::fprintf(tf, "SEG_END seg=%llu v_dts=%llu a_dts=%llu vframes=%llu asamples=%llu\n",
								 (unsigned long long) cur_seg, (unsigned long long) v_dts, (unsigned long long) a_dts,
								 (unsigned long long) r.vframes, (unsigned long long) r.asamples) < 0)
				{
					r.rc = 34;
					r.err = "timeline write";
					std::printf("TIMELINE_WRITE_FAIL seg=%llu\n", (unsigned long long) cur_seg);
					break;
				}
				const int db = drain(1);
				if (db)
				{
					r.rc = db;
					break;
				}
				/* 段界只复位**视频**解码器：本段首帧被强制成 I，参考链自断，复位不丢信息。
				 * 音频解码器不复位 —— CLV_AudioDecoderReset 会清掉本流的样本累加基准
				 * （src/audio/clv_audio.cpp 的 next_sample），而头里写明音频 DTS 由逐帧实测块大小
				 * 累加导出、容器带的 dts_delta 只作校验。一条连续 FLAC 流跨段复位等于把绝对时间轴打回 0，
				 * 出参 first_sample 就不再与容器的绝对 dts 可比。 */
				verr = CLV_VideoDecoderReset(dv);
				if (verr != CLV_VIDEO_OK)
				{
					r.rc = 35;
					r.err = "VidReset";
					std::printf("VRESET_ERR seg=%llu rc=%d %s\n", (unsigned long long) si, (int) verr,
								CLV_VideoStrError(verr));
					break;
				}
				v_dts = 0;
				a_dts = 0;
				cur_seg = si;
				uint64_t tot = 0;
				cerr = CLV_ContainerReaderSegmentTotal(rd, &tot);
				if (cerr != CLV_CONTAINER_OK)
				{
					r.rc = 33;
					r.err = "SegmentTotal";
					std::printf("SEG_TOTAL_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
					break;
				}
				std::printf("SEG_SWITCH -> %llu/%llu\n", (unsigned long long) si, (unsigned long long) tot);
			}
			else if (first_seg)
			{
				cur_seg = si;
				first_seg = false;
			}
			if (! f.complete)
			{
				std::printf("INCOMPLETE_FRAME sid=%u\n", f.stream_id);
				continue;
			}
			if (f.stream_id == (uint8_t) vid)
			{
				v_dts += f.dts_delta;
				const int64_t pts = (int64_t) v_dts + f.pts_delta;
				CLV_VideoFrameInfoC fi;
				verr =
					CLV_VideoDecoderDecode(dv, f.payload, f.payload_size,
										   f.fragments_expected ? f.fragments_expected : 1, (int64_t) v_dts, pts, &fi);
				if (verr != CLV_VIDEO_OK)
				{
					r.rc = 10;
					r.err = "VidDecode";
					std::printf("VDEC_ERR dts=%llu rc=%d %s\n", (unsigned long long) v_dts, (int) verr,
								CLV_VideoStrError(verr));
					break;
				}
				if (fi.disposition != CLV_VIDEO_DISP_OUTPUT && fi.disposition != CLV_VIDEO_DISP_DELAYED)
				{
					vdrop++;
					std::printf("VDROP dts=%llu disp=%d\n", (unsigned long long) v_dts, (int) fi.disposition);
				}
				const int db0 = drain(0);
				if (db0)
				{
					r.rc = db0;
					break;
				}
			}
			else if (f.stream_id == (uint8_t) aud)
			{
				a_dts += f.dts_delta;
				CLV_AudioBuffer* buf = nullptr;
				uint8_t ag = 0;
				CLV_AudioFrameInfoC ai;
				aer = CLV_AudioDecoderDecodeFrame(da, f.payload, f.payload_size, &buf, &ag, &ai);
				if (aer != CLV_AUDIO_OK)
				{
					r.rc = 12;
					r.err = "AudDecode";
					std::printf("ADEC_ERR dts=%llu rc=%d %s\n", (unsigned long long) a_dts, (int) aer,
								CLV_AudioStrError(aer));
					break;
				}
				if (! ag)
				{
					adrop++;
					continue;
				}
				CLV_AudioBufferDescC bd;
				std::memset(&bd, 0, sizeof(bd));
				aer = CLV_AudioBufferGetDesc(buf, &bd);
				if (aer != CLV_AUDIO_OK || bd.data == nullptr)
				{
					r.rc = 37;
					r.err = "BufferGetDesc";
					std::printf("ABUF_DESC_ERR rc=%d %s\n", (int) aer, CLV_AudioStrError(aer));
					CLV_AudioBufferRelease(buf);
					break;
				}
				/* 三件同时对上才算音频时间轴自足：库实测首样本号 == 容器 dts 累加值 == 本探针连续计数，
				 * 且字节数与 samples×ch×bytes_per_sample 相符。 */
				if (ai.first_sample != a_dts || ai.first_sample != a_expect ||
					bd.size != (size_t) bd.samples_per_channel * bd.channels * bd.bytes_per_sample)
				{
					r.rc = 13;
					r.err = "audio timeline mismatch";
					std::printf("A_SYNC_BAD first=%llu container_dts=%llu expect=%llu size=%zu spc=%u ch=%u bps=%u\n",
								(unsigned long long) ai.first_sample, (unsigned long long) a_dts,
								(unsigned long long) a_expect, bd.size, bd.samples_per_channel, bd.channels,
								bd.bytes_per_sample);
					CLV_AudioBufferRelease(buf);
					break;
				}
				if (! PutAll(pf, bd.data, bd.size))
				{
					r.rc = 38;
					r.err = "pcm write";
					std::printf("PCM_WRITE_FAIL size=%zu\n", bd.size);
					CLV_AudioBufferRelease(buf);
					break;
				}
				r.asamples += bd.samples_per_channel;
				r.abytes += bd.size;
				a_expect += bd.samples_per_channel;
				CLV_AudioBufferRelease(buf);
			}
		}
		if (r.rc == 0)
		{
			const int db1 = drain(1);
			if (db1) r.rc = db1;
		}
	}

	{
		CLV_ReaderStatsC st;
		std::memset(&st, 0, sizeof(st));
		cerr = CLV_ContainerReaderStats(rd, &st);
		if (cerr != CLV_CONTAINER_OK)
		{
			r.rc = 39;
			r.err = "ReaderStats";
			std::printf("CONT_STAT_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
		}
		else
		{
			std::printf("CONT_STAT pk_ok=%llu crc=%llu field=%llu too_large=%llu unknown_stream=%llu resync=%llu "
						"frames_ok=%llu incomplete=%llu gaps=%llu ext_bad=%llu trunc=%u aband=%u\n",
						(unsigned long long) st.packets_ok, (unsigned long long) st.dropped_crc,
						(unsigned long long) st.dropped_field, (unsigned long long) st.dropped_too_large,
						(unsigned long long) st.dropped_unknown_stream, (unsigned long long) st.resync_count,
						(unsigned long long) st.frames_ok, (unsigned long long) st.frames_incomplete,
						(unsigned long long) st.fragment_gaps, (unsigned long long) st.ext_blocks_bad, st.truncated,
						st.abandoned);
		}
	}
	{
		CLV_ReaderSegStatsC sg;
		std::memset(&sg, 0, sizeof(sg));
		cerr = CLV_ContainerReaderSegStats(rd, &sg);
		if (cerr != CLV_CONTAINER_OK)
		{
			r.rc = 40;
			r.err = "SegStats";
			std::printf("SEG_STAT_ERR rc=%d %s\n", (int) cerr, CLV_ContainerStrError(cerr));
		}
		else
		{
			std::printf("SEG_STAT block_missing=%llu block_bad=%llu disc=%llu no_param=%llu read=%llu missing=%llu "
						"mismatch=%llu reordered=%llu last=%llu ext_scan=%llu ext_inc=%llu\n",
						(unsigned long long) sg.block_missing, (unsigned long long) sg.block_bad,
						(unsigned long long) sg.discontinuities, (unsigned long long) sg.video_stream_no_param_block,
						(unsigned long long) sg.segments_read, (unsigned long long) sg.segments_missing,
						(unsigned long long) sg.segments_mismatched, (unsigned long long) sg.segments_reordered,
						(unsigned long long) sg.last_seg_index, (unsigned long long) sg.segments_ext_scanned,
						(unsigned long long) sg.segments_ext_inconsistent);
		}
	}
	{
		CLV_AudioStatsC as;
		std::memset(&as, 0, sizeof(as));
		aer = CLV_AudioDecoderStats(da, &as);
		if (aer != CLV_AUDIO_OK)
		{
			r.rc = 41;
			r.err = "AudioStats";
			std::printf("AUD_STAT_ERR rc=%d %s\n", (int) aer, CLV_AudioStrError(aer));
		}
		else
		{
			std::printf(
				"AUD_STAT frames_ok=%llu drop_head=%llu drop_mismatch=%llu blocks_stored=%llu drop_padding=%llu "
				"rej_streaminfo=%llu skip_unknown=%llu mask_removed=%llu rejected=%u\n",
				(unsigned long long) as.frames_ok, (unsigned long long) as.frames_dropped_header,
				(unsigned long long) as.frames_dropped_mismatch, (unsigned long long) as.blocks_stored,
				(unsigned long long) as.blocks_dropped_padding, (unsigned long long) as.blocks_rejected_streaminfo,
				(unsigned long long) as.blocks_skipped_unknown, (unsigned long long) as.mask_keys_removed,
				as.stream_rejected);
		}
	}
	/* 三份输出件的关闭也判：写缓冲里的失败只在这一道露头，漏掉就是「解回绿了但件是半份」。
	 * 三个句柄在这里统一置空，后续不再有任何路径碰它们。 */
	int close_mask = 0;
	if (tf) close_mask |= (fclose(tf) != 0 ? 1 : 0);
	if (pf) close_mask |= (fclose(pf) != 0 ? 2 : 0);
	if (yf) close_mask |= (fclose(yf) != 0 ? 4 : 0);
	tf = nullptr;
	pf = nullptr;
	yf = nullptr;
	if (close_mask != 0 && r.rc == 0)
	{
		r.rc = 42;
		r.err = "dec out close fail";
		std::printf("DEC_CLOSE_FAIL mask=%d\n", close_mask);
	}
	CLV_AudioDecoderClose(da);
	CLV_VideoDecoderClose(dv);
	CLV_ContainerReaderClose(rd);
	std::printf("DEC_DONE rc=%d vframes=%llu vbytes=%llu asamples=%llu abytes=%llu vdrop=%llu adrop=%llu "
				"flushed=%llu\n",
				r.rc, (unsigned long long) r.vframes, (unsigned long long) r.vbytes, (unsigned long long) r.asamples,
				(unsigned long long) r.abytes, (unsigned long long) vdrop, (unsigned long long) adrop,
				(unsigned long long) vout_flushed);
	if (r.rc == 0)
	{
		std::printf("DECODED_TOTAL=%llu (%.3f GiB)\n", (unsigned long long) (r.vbytes + r.abytes),
					(double) (r.vbytes + r.abytes) / 1073741824.0);
	}
	else
	{
		std::printf("DEC_FAIL %s\n", r.err.c_str());
	}
	return r;
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::printf("usage: mtav <enc|mux|dec> <plan> <dir>   (plan 语法与手工编译命令见本件头注释)\n");
		return 64;
	}
	const std::string mode = argv[1], planp = argv[2], dir = argv[3];
	Plan p;
	std::string perr;
	if (! LoadPlan(planp.c_str(), &p, &perr))
	{
		std::printf("PLAN_FAIL %s\n", perr.c_str());
		return 65;
	}
	uint64_t want_frames = 0, want_samples = 0;
	for (auto& u: p.units)
	{
		want_frames += u.frames;
		want_samples += u.samples;
	}
	std::printf("PLAN W=%u H=%u qp=%u lossless=%d seed=%llu budget_v=%llu budget_a=%llu mats=%zu units=%zu "
				"want_frames=%llu want_samples=%llu\n",
				p.W, p.H, p.qp, p.lossless ? 1 : 0, (unsigned long long) p.seed, (unsigned long long) p.budget_v,
				(unsigned long long) p.budget_a, p.mats.size(), p.units.size(), (unsigned long long) want_frames,
				(unsigned long long) want_samples);

	if (mode == "enc")
	{
		EncRes r = mode_enc(p, dir);
		if (r.rc)
		{
			std::printf("MTAV_ENC_FAIL rc=%d %s\n", r.rc, r.err.c_str());
			return r.rc;
		}
		std::printf("MTAV_ENC_OK\n");
		return 0;
	}
	uint32_t nseg = 0;
	Idx idx;
	if (mode == "mux" || mode == "dec")
	{
		/* 索引件在这一次跑里只解析一遍：段数由这一遍算出，封装步直接用同一份驱动写入。
		 * 改前主流程与封装步各解析一遍、两遍之间不核对 ⇒ 同一份件两本账，段号对不上也没人判。 */
		if (! LoadIdx(dir + "/store.idx", &idx))
		{
			std::printf("IDX_FAIL\n");
			return 66;
		}
		/* 段数 = 出现过的最大段号 + 1，用 uint64 累加再把结果收进 uint32：
		 * 段号顶到 UINT32_MAX 时「+1」在 32 位里回绕成 0，那一档会拿 0 个桶去接住这条包，越界就是这么来的。 */
		uint64_t seg_max = 0;
		for (const Packet& k: idx.V)
			if ((uint64_t) k.seg + 1ull > seg_max) seg_max = (uint64_t) k.seg + 1ull;
		for (const Packet& k: idx.A)
			if ((uint64_t) k.seg + 1ull > seg_max) seg_max = (uint64_t) k.seg + 1ull;
		if (seg_max == 0 || seg_max > (uint64_t) UINT32_MAX)
		{
			std::printf("IDX_SEG_BAD max=%llu\n", (unsigned long long) seg_max);
			return 66;
		}
		nseg = (uint32_t) seg_max;
	}
	if (mode == "mux")
	{
		MuxRes r = mode_mux(p, dir, nseg, idx);
		if (r.rc) return r.rc;
		std::printf("MTAV_MUX_OK nseg=%u\n", nseg);
		return 0;
	}
	if (mode == "dec")
	{
		DecRes r = mode_dec(dir, nseg);
		if (r.rc) return r.rc;
		if (r.vframes != want_frames)
		{
			std::printf("FRAME_DEFICIT got=%llu want=%llu\n", (unsigned long long) r.vframes,
						(unsigned long long) want_frames);
			return 20;
		}
		if (r.asamples != want_samples)
		{
			std::printf("SAMPLE_DEFICIT got=%llu want=%llu\n", (unsigned long long) r.asamples,
						(unsigned long long) want_samples);
			return 21;
		}
		std::printf("MTAV_DEC_OK nseg=%u\n", nseg);
		return 0;
	}
	std::printf("MODE_UNKNOWN %s\n", mode.c_str());
	return 67;
}
