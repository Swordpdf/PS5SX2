/* PS5 port frontend, PC harness only: libchdr's calls fe_games.cpp makes, answering "no CHD here" (the harness's
 * games are made up, and libchdr's own build is not worth it for the preview).
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "libchdr/chd.h"

chd_error chd_open(const char *filename, int mode, chd_file *parent, chd_file **chd)
{
	(void)filename;
	(void)mode;
	(void)parent;
	*chd = 0;
	return CHDERR_FILE_NOT_FOUND;
}

void chd_close(chd_file *chd)
{
	(void)chd;
}

const char *chd_error_string(chd_error err)
{
	(void)err;
	return "no CHD support in the PC harness";
}

const chd_header *chd_get_header(chd_file *chd)
{
	(void)chd;
	return 0;
}

chd_error chd_read(chd_file *chd, uint32_t hunknum, void *buffer)
{
	(void)chd;
	(void)hunknum;
	(void)buffer;
	return CHDERR_INVALID_FILE;
}

chd_error chd_get_metadata(chd_file *chd, uint32_t searchtag, uint32_t searchindex, void *output, uint32_t outputlen,
	uint32_t *resultlen, uint32_t *resulttag, uint8_t *resultflags)
{
	(void)chd;
	(void)searchtag;
	(void)searchindex;
	(void)output;
	(void)outputlen;
	(void)resultlen;
	(void)resulttag;
	(void)resultflags;
	return CHDERR_METADATA_NOT_FOUND;
}
