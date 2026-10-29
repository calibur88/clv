/* 字幕管线演示（M5）：入口在 subtitle.c，播放逻辑在本目录的 subtitle_player.c（SDL3 可视化）。 */
#ifndef CLV_DEMO_SUBTITLE_PLAYER_H
#define CLV_DEMO_SUBTITLE_PLAYER_H

#ifdef __cplusplus
extern "C"
{
#endif

	int clv_demo_subtitle_run(const char* av_path, const char* sub_path);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_DEMO_SUBTITLE_PLAYER_H
