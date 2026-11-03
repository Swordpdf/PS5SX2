/* PS5SX2 (test build 186, AI-assisted): the driver's flips from the CPU (sceVideoOutSubmitFlip) instead of a GPU submission of their own.
 *
 * On firmware 8.40 and older a GPU submission that reaches an idle GPU only starts at the next vblank, and ps5vk 964919b914 submits every
 * flip as a submission of its own (ps5vk_queue_flip: the previous flip confirmed shown, sceAgcDcbSetFlip into the buffer's flip stream,
 * sceAgcDriverSubmitDcb, sceAgcSuspendPoint). swordpdf's original PS5 at 4.03, Ratchet & Clank with frame generation at 6x and 119.88 Hz
 * (2026-11-03), with the barrier draw (GSDeviceVK.cpp) and half-size generation: 120 presents a second for 172 of 177 s with these
 * flips, 69 of 76 s with the driver's, and about half the delay (present to shown ~21 ms against ~40).
 *
 * link-vk.sh links this file only after it made the copy of the driver archive take it: ps5vk_queue_flip weak in ps5vk_queue.o, so this
 * definition is the one the queue's worker (ps5vk_queue_run_job) and ps5vk_wsi.c call, and a second copy of ps5vk_queue.o with only the
 * driver's own flip global, renamed ps5vk_queue_flip_dcb, which the flag file vk_dcbflip brings back.
 *
 * What the driver's flip does first is kept: the last flip submitted confirmed shown (VideoOut's flip status word 3 holds the last shown
 * flip's argument, the driver's marker), so at most one flip is pending and a buffer is never flipped while it may still be shown; then
 * the flip with the marker as its argument (the driver's own waits and the late display wait read word 3), at the next vblank (mode 1);
 * with `wait`, until it is shown. The GPU has finished the buffer by then: the worker runs a flip after the submission before it, and a
 * submission returns once its completion marker arrived. Not done here: the driver's one-time barrier probe and its queue profile lines.
 *
 * Review of 2026-11-03: the wait is for the last flip that was submitted, not marker - 1: ps5vk_wsi.c has used up a marker before it calls
 * here, and the worker drops the result, so after one failed flip every later one would wait 200 vblanks for a flip that never came. A run
 * of failures marks the queue lost with the reason (as the driver's flip does), so the app ends at once and says why. With the flag
 * vk_flipsuspend (the driver then skips its steps' suspend points) the flip passes one itself.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

int sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t flip_mode, int64_t flip_arg);
int sceVideoOutGetFlipStatus(int32_t handle, uint64_t status[16]);
int sceVideoOutWaitVblank(int32_t handle);
int sceAgcSuspendPoint(void);
uint32_t ps5vk_swapchain_images(void);
/* Mesa's vk_queue_set_lost; the queue the driver passes here starts with its struct vk_queue (ps5vk_queue_flip calls it with the same) */
int _vk_queue_set_lost(void *queue, const char *file, int line, const char *msg, ...);
/* the driver's own flip, renamed in the second copy of ps5vk_queue.o (see above) */
int ps5vk_queue_flip_dcb(void *queue, int video, uint32_t buffer_index, int64_t marker, bool wait);

#define FLIP_VK_SUCCESS 0
#define FLIP_VK_DEVICE_LOST (-4)
#define FLIP_MODE_VSYNC 1u /* the driver's PS5VK_FLIP_MODE (sceAgcDcbSetFlip's fourth argument) */
#define FLIP_WAIT_VBLANKS 200 /* the driver's PS5VK_FLIP_WAITS */
#define FLIP_LOST_AFTER 8 /* failed flips in a row that end the run */

static int s_mode = -1; /* 1: CPU flips, 0: the driver's */
static bool s_suspend = false; /* vk_flipsuspend: a suspend point after each flip */
static int s_video = -1; /* the VideoOut handle the flips below went to */
static int64_t s_submitted = 0; /* the last marker submitted on it (0: none yet) */
static unsigned s_failures = 0, s_failed_in_a_row = 0;

static bool flip_wait_shown(int video, int64_t marker)
{
   uint64_t st[16] = {0};
   for (int i = 0; i < FLIP_WAIT_VBLANKS; i++) {
      if (sceVideoOutGetFlipStatus(video, st) == 0 && (int64_t)st[3] >= marker)
         return true;
      sceVideoOutWaitVblank(video);
   }
   return false;
}

static int flip_failed(void *queue, const char *why, int64_t marker, int code)
{
   if (s_failures++ < 5) {
      printf("[flip] flip %lld not made: %s (0x%08x)\n", (long long)marker, why, (unsigned)code);
      fflush(stdout);
   }
   if (++s_failed_in_a_row >= FLIP_LOST_AFTER)
      _vk_queue_set_lost(queue, __FILE__, __LINE__, "%u flips from the CPU failed in a row, the last: %s (0x%08x)", s_failed_in_a_row, why,
                         (unsigned)code);
   return FLIP_VK_DEVICE_LOST;
}

int ps5vk_queue_flip(void *queue, int video, uint32_t buffer_index, int64_t marker, bool wait)
{
   if (s_mode < 0) {
      struct stat st;
      s_mode = stat("/data/PCSX2/flags/vk_dcbflip", &st) == 0 ? 0 : 1;
      s_suspend = stat("/data/PCSX2/flags/vk_flipsuspend", &st) == 0;
      printf("[flip] %s%s\n", s_mode ? "flips from the CPU (sceVideoOutSubmitFlip), not a GPU submission each (flag vk_dcbflip: the driver's)"
                                     : "the driver's flips (a GPU submission each; flag vk_dcbflip)",
             s_mode && s_suspend ? "; a suspend point after each (vk_flipsuspend)" : "");
      fflush(stdout);
   }
   if (s_mode == 0)
      return ps5vk_queue_flip_dcb(queue, video, buffer_index, marker, wait);
   if (buffer_index >= ps5vk_swapchain_images())
      return flip_failed(queue, "no such buffer", marker, (int)buffer_index);
   if (video != s_video || marker <= s_submitted) {
      /* another VideoOut handle, or the same number handed out again to a new swapchain: its markers start over */
      s_video = video;
      s_submitted = 0;
   }
   if (s_submitted > 0 && !flip_wait_shown(video, s_submitted))
      return flip_failed(queue, "the previous flip did not reach the display within 200 vblanks", s_submitted, 0);
   const int r = sceVideoOutSubmitFlip(video, (int32_t)buffer_index, FLIP_MODE_VSYNC, marker);
   if (r < 0)
      return flip_failed(queue, "sceVideoOutSubmitFlip refused it", marker, r);
   s_submitted = marker;
   s_failed_in_a_row = 0;
   if (s_suspend)
      sceAgcSuspendPoint();
   if (wait && !flip_wait_shown(video, marker))
      return flip_failed(queue, "it did not reach the display within 200 vblanks", marker, 0);
   return FLIP_VK_SUCCESS;
}
