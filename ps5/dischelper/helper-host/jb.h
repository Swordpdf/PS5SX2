/* onionHEN-style app jailbreak (by title ID), restored.
 *
 * Watches every PPSA sandbox slot's download0/ for the classic
 * etahen_jailbreak / onionhen_jailbreak marker (JSON {"PID":n}), checks
 * the slot's title ID against the built-ins (PPSA99203, PPSA99303) and
 * /data/whitelist.txt, then applies the full onionHEN privilege set
 * (uid 0, authid 0x4801000000000013, full caps, attrs[3]|0x80) and moves
 * the process's root/jail to the system root vnode. Ack = marker unlink.
 */
#pragma once

void *payload_jb_thread(void *arg);
