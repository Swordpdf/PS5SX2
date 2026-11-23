# PS5SX2's Vulkan driver: RADV

Since 2.60, PS5SX2 draws with RADV, Mesa's Vulkan driver for AMD GPUs, in the PS5 port of [Mihawk-99](https://github.com/mihawk-99)
([PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)), with the changes in
`ps5sx2-radv.patch` on top of PS5_Mesa's `main` at `75238ae4db3`. 2.50 and earlier linked ps5vk, a smaller driver of its own;
`ps5/coreorbis/link-vk.sh` still links it, and the eboot finds out at run time which one it has (`include-orbis/OrbisDriver.h`).

## What the patch changes

- **Timed waits that slept.** C11's condition variables (Mesa's `cnd_t`) time out on the wall clock, and two waits handed them deadlines
  on the monotonic clock, decades in the past: the wait for a submission to be made (`radv_ps5_sync.c`) and the swapchain's wait for a
  free framebuffer (`wsi_common_videoout.c`) returned at once and spun with their lock. The deadlines are converted now.
- **Push descriptors on the recording thread.** With `RADV_THREADED_RECORDING=1` (Mihawk's threaded layer, which PS5SX2 turns on) every
  `vkCmdPushDescriptorSet` waited for the recording thread to catch up; PCSX2 pushes the descriptors of nearly every draw. The layer now
  queues them with a copy of each write (`radv_threaded_layer.c`).
- **New memory cleared in one pass.** A new buffer's memory was cleared with `memset` and then a cache flush line by line: tens of
  milliseconds for an upscaled render target. Streaming stores do it in one pass and leave no line in the caches
  (`radv_ps5_zero_new`; `RADV_PS5_ZERO_FLUSH=1`, or PS5SX2's flag file `vk_zeroflush`, is the old way).
- **The output follows each swapchain's mode.** The 119.88 Hz output was taken once for the whole process when the title's `param.json`
  declared it. Now a swapchain made in a 119.88 Hz mode switches the output (frame generation), one made in a 59.94 Hz mode puts it back
  (the shelf, games without frame generation), and the output goes back to 59.94 Hz as the title leaves (`wsi_videoout_restore_default`,
  and at exit). On a display with a variable refresh rate, which idles at the bottom of its range with nothing flipping, flips decide whether
  the display follows. `wsi_videoout_refresh_millihertz` and `wsi_videoout_output_status` tell the title what the output did.

## Building it

```sh
PS5_MESA=<clone of PS5_Mesa> PS5_VULKAN=<clone of PS5_Vulkan at 7f492b4> OUT=<folder> ps5/radv/build-radv.sh
```

PS5_Vulkan provides the payload SDK the archive is compiled against (run its `tools/setup-native-dependencies.sh` first) and the cross
file. The eboot is then linked with `ps5/coreorbis/link-radv.sh` (after `make -f Makefile.vk`), with `PS5_RADV_ARCHIVE=$OUT/lib/libvulkan_radeon.ps5.a`,
`PS5_RADV_PLATFORM` the `libps5platform.a` of Mihawk-99's [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) fork at
`3345cde`, and `PS5_RADV_BUILTINS` clang 18's `libclang_rt.builtins-x86_64.a`.

## Licences

Mesa and its RADV are under the MIT licence (with the parts of Mesa they use under their own permissive licences); Mihawk-99's PS5 port of
them is MIT too. The patch is under the same terms as the files it changes.
