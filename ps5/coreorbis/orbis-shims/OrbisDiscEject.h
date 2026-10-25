// PS5SX2 2.16 (AI-assisted): the drive's eject button while PS5SX2 holds a disc drive open.
//
// FreeBSD's cd(4) driver sends PREVENT MEDIUM REMOVAL when /dev/cdN is opened with a disc in (cdcheckmedia) and ALLOW only at the last close. The
// app keeps /dev/cd0 open for as long as a game plays from it (IOCtlSrc) and for the whole of a disc copy (ProsperoDiscDump), so the drive refused
// its eject button for all of that time (tester, 2026-10-26, FW 13.60 phat: the disc did not come out while playing nor while copying; the helper's
// log shows it out only after the app's run had ended). The disc helper's opens are short (open, size, close) and do not hold it. CDIOCALLOW takes the lock off again
// (the helper's USB copy does the same since 171h); the driver can set it again when it looks at the media afresh (after a UNIT ATTENTION, for
// one), so the callers repeat it every few seconds. With the lock off, an eject makes the reads fail: the game sees an open tray (IOCtlSrc::DiscReady),
// the copy stops and keeps what it has for the next time the disc goes in (ProsperoDiscDump).
//
// Switch file disc_lockeject: the old behaviour (the tray stays locked while the app holds the drive).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <sys/cdio.h>
#include <sys/ioctl.h>

#include <cerrno>

namespace orbis_disc
{
	// Every this many milliseconds the lock is taken off again (it costs nothing when it is already off: the driver sends no command then).
	constexpr long long kAllowEveryMs = 3000;

	// 0, or the errno of CDIOCALLOW.
	inline int AllowEject(int cd_fd)
	{
		if (cd_fd < 0)
			return EBADF;
		return ioctl(cd_fd, CDIOCALLOW) == 0 ? 0 : errno;
	}
} // namespace orbis_disc
