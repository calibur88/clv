/* 把「视频 + 音频」双流件与本库的字幕流合成一份三轨件（视频 / 音频 / 字幕）。
 *
 * 用法：mux_avs <av.clv> <sub.clv> <out.clv>
 * 只经公开 C ABI（CLV_Container.h）：读方逐包取出，写方逐包落回；字幕流改成 stream_id = 2，
 * ext_type = 4 的样式块跟着改挂到 2 号流。视频 / 音频的 dts / pts 原样保留（读方增量的累加在这里做）。
 *
 * 本件是素材生成器的一部分（不进 CMake 构建）：产物是 assets/fixtures/demo_avs_360p_q32.clv，
 * 配方与冻结值登记在 assets/README.md 与 assets/fixtures/provenance.txt。 */
#include "CLV_Container.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	struct Pkt
	{
		uint8_t id = 0;
		uint8_t key = 0;
		int64_t dts = 0;
		int64_t pts = 0;
		std::vector<uint8_t> data;
	};

	struct Ext
	{
		uint8_t id = 0;
		uint8_t type = 0;
		uint8_t version = 1;
		std::vector<uint8_t> data;
	};

	struct Media
	{
		std::vector<CLV_StreamDescC> streams;
		std::vector<Ext> exts;
		std::vector<Pkt> pkts;
	};

	bool ReadMedia(const char* path, Media* out)
	{
		CLV_ContainerError err = CLV_CONTAINER_OK;
		CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path, &err);
		if (r == nullptr)
		{
			std::printf("OPEN_FAIL %s: %s\n", path, CLV_ContainerStrError(err));
			return false;
		}
		uint32_t ns = 0;
		CLV_ContainerReaderStreamCount(r, &ns);
		for (uint32_t i = 0; i < ns; ++i)
		{
			CLV_StreamDescC d;
			std::memset(&d, 0, sizeof(d));
			uint8_t usable = 0;
			if (CLV_ContainerReaderGetStream(r, i, &d, &usable) != CLV_CONTAINER_OK || ! usable) continue;
			out->streams.push_back(d);
		}
		uint32_t nx = 0;
		CLV_ContainerReaderExtBlockCount(r, &nx);
		for (uint32_t i = 0; i < nx; ++i)
		{
			CLV_ExtBlockC b;
			std::memset(&b, 0, sizeof(b));
			if (CLV_ContainerReaderGetExtBlock(r, i, &b) != CLV_CONTAINER_OK || b.data == nullptr) continue;
			Ext e;
			e.id = b.stream_id;
			e.type = b.ext_type;
			e.version = b.ext_version;
			e.data.assign(b.data, b.data + b.size);
			out->exts.push_back(e);
		}
		int64_t dts_acc[256] = { 0 };
		CLV_FrameC f;
		std::memset(&f, 0, sizeof(f));
		uint8_t got = 0;
		while (CLV_ContainerReaderNextFrame(r, &f, &got) == CLV_CONTAINER_OK && got)
		{
			dts_acc[f.stream_id] += (int64_t) f.dts_delta;
			Pkt p;
			p.id = f.stream_id;
			p.key = f.is_keyframe;
			p.dts = dts_acc[f.stream_id];
			p.pts = p.dts + f.pts_delta;
			p.data.assign(f.payload, f.payload + f.payload_size);
			out->pkts.push_back(std::move(p));
		}
		CLV_ContainerReaderClose(r);
		return true;
	}
}	 // namespace

int main(int argc, char** argv)
{
	if (argc != 4)
	{
		std::printf("usage: mux_avs <av.clv> <sub.clv> <out.clv>\n");
		return 64;
	}
	Media av;
	Media sub;
	if (! ReadMedia(argv[1], &av) || ! ReadMedia(argv[2], &sub)) return 2;

	const CLV_StreamDescC* sub_desc = nullptr;
	for (const auto& d: sub.streams)
		if (d.stream_type == 2) sub_desc = &d;
	if (sub_desc == nullptr)
	{
		std::printf("NO_SUBTITLE_STREAM\n");
		return 3;
	}
	const uint8_t old_sub_id = sub_desc->stream_id;
	const uint8_t new_sub_id = 2;

	CLV_ContainerError werr = CLV_CONTAINER_OK;
	CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(argv[3], nullptr, &werr);
	if (w == nullptr)
	{
		std::printf("WRITE_OPEN_FAIL %s\n", CLV_ContainerStrError(werr));
		return 4;
	}
	for (const auto& d: av.streams)
		if (CLV_ContainerWriterAddStream(w, &d) != CLV_CONTAINER_OK) return 5;
	CLV_StreamDescC sd = *sub_desc;
	sd.stream_id = new_sub_id;
	if (CLV_ContainerWriterAddStream(w, &sd) != CLV_CONTAINER_OK) return 5;
	for (const auto& e: av.exts)
	{
		if (CLV_ContainerWriterAddExtBlock(w, e.id, e.type, e.version, e.data.data(), e.data.size()) !=
			CLV_CONTAINER_OK)
		{
			return 6;
		}
	}
	for (const auto& e: sub.exts)
	{
		if (e.id != old_sub_id || e.type != 4) continue;
		if (CLV_ContainerWriterAddExtBlock(w, new_sub_id, e.type, e.version, e.data.data(), e.data.size()) !=
			CLV_CONTAINER_OK)
		{
			return 6;
		}
	}
	for (const auto& p: av.pkts)
	{
		if (CLV_ContainerWriterWriteFrame(w, p.id, p.key, p.dts, p.pts, p.data.data(), p.data.size()) !=
			CLV_CONTAINER_OK)
		{
			return 7;
		}
	}
	for (const auto& p: sub.pkts)
	{
		if (p.id != old_sub_id) continue;
		if (CLV_ContainerWriterWriteFrame(w, new_sub_id, 0, p.dts, p.pts, p.data.data(), p.data.size()) !=
			CLV_CONTAINER_OK)
		{
			return 7;
		}
	}
	uint64_t packets = 0;
	if (CLV_ContainerWriterFinish(w, &packets) != CLV_CONTAINER_OK) return 8;
	CLV_ContainerWriterClose(w);
	std::printf("MUX_AVS_OK packets=%llu\n", (unsigned long long) packets);
	return 0;
}
