/* disc_compat_db.h (PS5SX2 helper 171k, 2026-10-29, AI-assisted): the app.db side of disc_compat.h (needs sqlite3.h before it).
 *
 * purge_title_rows (disctile.c, 171d) deletes the rows a removed tile of ours leaves in the home screen's database, its concept rows
 * included: every concept row whose localConceptId is one of the tile's concepts. A concept the shell shares with a title that is NOT one
 * of PS5SX2's own IDs (a PS5SX3 tile, a real game) must not go with it: the PS5SX3 helper skips such a purge for its own tiles
 * (dts_purge_checked: -2), and this does the same per concept here. A concept is kept out of the purge when any user's title row with that
 * concept, or any concept row with that id, names a title outside PS5SX2's IDs (dc_ps5sx2_tid). Read only; the caller's transaction.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DISC_COMPAT_DB_H
#define DISC_COMPAT_DB_H

#include "disc_compat.h"

/* 1 when `sql` (one ? = the concept id) gives a row whose first column is a title ID that is set and not one of PS5SX2's */
static inline int dc_db_foreign(sqlite3 *db, const char *sql, const char *cid) {
    sqlite3_stmt *st = NULL;
    int foreign = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, cid, -1, SQLITE_TRANSIENT);
    while (!foreign && sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        if (t && t[0] && !dc_ps5sx2_tid(t)) foreign = 1;
    }
    sqlite3_finalize(st);
    return foreign;
}

/* `cids` is an SQL list ('a','b',...) of the removed tile's concepts (as purge_title_rows builds it). The concepts another title uses are
 * taken out of it (rewritten in place) and named in `kept` ("a b"); returns how many were taken out. icon_tabs: the users' tbl_iconinfo_*
 * tables; concept_tabs: their tbl_concepticoninfo_* (an empty name: none). */
static inline int dc_db_keep_shared_concepts(sqlite3 *db, char icon_tabs[][64], char concept_tabs[][80], int nt, char *cids, size_t max,
                                             char *kept, size_t kmax) {
    char out[256] = "";
    size_t ol = 0, kl = 0;
    int dropped = 0;
    if (kept && kmax) kept[0] = '\0';
    for (const char *p = cids; (p = strchr(p, '\'')) != NULL;) {
        const char *e = strchr(p + 1, '\'');
        if (!e) break;
        char cid[96];
        const size_t n = (size_t)(e - p - 1) < sizeof(cid) - 1 ? (size_t)(e - p - 1) : sizeof(cid) - 1;
        memcpy(cid, p + 1, n);
        cid[n] = '\0';
        p = e + 1;
        int foreign = 0;
        for (int i = 0; i < nt && !foreign; i++) {
            char sql[200];
            snprintf(sql, sizeof(sql), "SELECT titleId FROM %s WHERE localConceptId=?", icon_tabs[i]);
            foreign = dc_db_foreign(db, sql, cid);
            if (!foreign && concept_tabs[i][0]) {
                snprintf(sql, sizeof(sql), "SELECT primaryTitleId FROM %s WHERE localConceptId=?", concept_tabs[i]);
                foreign = dc_db_foreign(db, sql, cid);
            }
        }
        if (foreign) {
            dropped++;
            if (kept && kl + n + 2 < kmax) kl += (size_t)snprintf(kept + kl, kmax - kl, "%s%s", kl ? " " : "", cid);
        } else if (ol + n + 4 < sizeof(out)) {
            ol += (size_t)snprintf(out + ol, sizeof(out) - ol, "%s'%s'", ol ? "," : "", cid);
        }
    }
    snprintf(cids, max, "%s", out);
    return dropped;
}

#endif
