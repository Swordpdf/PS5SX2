/* PS5SX2 Installer: stays running, and sends each PS5SX2 session's logs when the app closes. */
#pragma once

/* Runs until the no-log-upload switch appears (or forever). Call only while holding logger.lock. */
int logger_run(void);

/* One pass over the outbox; returns the number of reports sent, -1 if sending isn't set up. */
int logger_send_outbox(void);

/* The relay's address (upload-url.txt, else the built-in one); "" when there is none. */
const char *logger_relay_url(void);

/* Makes console-id.txt (random, not the console's own IDs) if it's missing. */
void logger_ensure_console_id(void);
