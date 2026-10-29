/* Init and mount lines that survive a kernel panic: one fsync'ed append per
 * step to /data/PCSX2/logs/payload-init.log. The klog ring does not survive a
 * KP; this file does. A fatal load that adds NO new line here died before the
 * payload's main() started, i.e. on the loader side. */
#pragma once

void payload_progress(const char *text);
