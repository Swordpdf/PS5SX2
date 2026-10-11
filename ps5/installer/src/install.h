/* PS5SX2 Installer: install the latest release safely. */
#pragma once

enum {
  INSTALL_DONE = 0,     /* a new build was installed */
  INSTALL_NOTHING = 1,  /* up to date, switched off, or another install is running */
  INSTALL_FAILED = -1,  /* nothing changed, or everything was put back */
};

int install_run(void);

/* An install that stopped half-way (power cut, crash) is undone from its journal. Call with the lock held. */
int install_recover(void);
