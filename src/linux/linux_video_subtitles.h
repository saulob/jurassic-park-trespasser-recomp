/* Subtitles over the game's Smacker videos on Linux: the same timelines, wrapping, placement and outline as the Windows host
 * (jpt_video_subtitles), the text rasterised by linux_text, laid over the frame the presenter shows. The guest surface and the
 * pixels of the video are never touched. */
#ifndef JPT_LINUX_VIDEO_SUBTITLES_H
#define JPT_LINUX_VIDEO_SUBTITLES_H

#include <stdint.h>

#include "jpt_video_subtitles.h"
#include "linux_window.h"
#include "smacker_backend.h"

/* Reads the settings file (the `video_subtitles` switch, the language the version plays in) and loads that language's interface
 * text. A missing file means the defaults. Safe to call again. */
void lx_vsub_init(const char* settings_file, const char* version_id);
void lx_vsub_set_enabled(int enabled);

/* One video at a time, identified by the Smack object the guest holds. */
void lx_vsub_opened(uint32_t video, const char* guest_name, const smk_source_t* source);
/* The playback of a timeline already chosen (opened does this once the file is known). */
void lx_vsub_begin_timeline(uint32_t video, const jvs_timeline_t* timeline);
void lx_vsub_frame(uint32_t video, uint32_t frame, int x, int y, int w, int h);
void lx_vsub_closed(uint32_t video);
void lx_vsub_reset(void);

/* The cue on screen now, -1 when none. */
int lx_vsub_cue(void);
/* The frame with the subtitle blended in (XRGB8888, in an internal buffer valid until the next call); 0 when nothing shows. */
int lx_vsub_compose(const lx_frame_t* in, lx_frame_t* out);

/* The text the cue shows in the language in use, UTF-8; NULL without a timeline or cue. */
const char* lx_vsub_text(int cue);

#endif
