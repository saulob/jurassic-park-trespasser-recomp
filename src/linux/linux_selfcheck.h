/* `trespasser_recomp --build-info` and `--self-check`: what this binary is and what it holds, as JSON on stdout. Headless: no log
 * file, no window, no audio device, no guest code runs. Each capability is a real call into the module that provides it, so a
 * binary built without a feature, or from older sources, says so. */
#ifndef JPT_LINUX_SELFCHECK_H
#define JPT_LINUX_SELFCHECK_H

int lx_build_info_print(void);
/* 0 when every capability answered. */
int lx_selfcheck_run(void);

#endif
