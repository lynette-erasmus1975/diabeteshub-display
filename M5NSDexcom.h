#ifndef _M5NSDEXCOM_H
#define _M5NSDEXCOM_H

#include "M5NSconfig.h"

// Fetches the latest glucose readings from a Dexcom Share ("Follow") account.
// Mirrors readNightscout(): returns 0 on success, else an error code (also logged via addErrorLog).
int readDexcom(tConfig *cfg, struct NSinfo *ns);

// Clears the cached Dexcom account/session IDs and the bad-credential backoff flag.
// Call whenever the Dexcom username, password or server region changes.
void dexcomResetSession();

// Shared direction-string -> arrow angle mapping (also used by readNightscout).
int directionToArrowAngle(const char *sensDir);

#endif
