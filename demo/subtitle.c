/* 字幕管线演示（M5）：SDL3 窗口自动播放一趟视频 + 音频，字幕层由解析器与渲染器产出后叠在视频上。
 * 播放器逻辑在 subtitle_player.c（demo 局部件）。
 *
 * 用法：clv_demo_subtitle.exe [av.clv] [sub.clv]
 * 默认 av = demo_av_1080p_q32.clv（视频 + 音频），sub = subtitle_pipeline.clv（字幕流）。
 * 末行固定为 DEMO_SUBTITLE_DONE shown=… dropped=… frames=… blocks=… glyphs=…，
 * 退出码 0 = 有过字幕帧且 dropped 为 0。 */
#include "subtitle_player.h"

#include <stdio.h>

int main(int argc, char** argv)
{
	const char* av = argc > 1 ? argv[1] : NULL;
	const char* sub = argc > 2 ? argv[2] : NULL;
	if (argc > 3)
	{
		printf("usage: %s [av.clv] [sub.clv]\n", argv[0]);
		return 64;
	}
	return clv_demo_subtitle_run(av, sub);
}
