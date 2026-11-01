#ifndef _M5NSCARELINK_H
#define _M5NSCARELINK_H

#include <Arduino.h>
#include "M5NSconfig.h"

// Fetches the latest sensor glucose from a Medtronic CareLink care partner account
// (MiniMed 780G / Simplera, the same data the CareLink Connect app shows).
// Mirrors readLibre()/readDexcom(): returns 0 on success, else an error code that is
// also logged via addErrorLog():
//   1301 CareLink communication / API error
//   1302 CareLink sign-in needed (no login stored, or the stored login has expired)
//   1303 no patient linked to this care partner account
//   1304 country not supported by CareLink, or CareLink configuration not readable
//   1004 no current sensor glucose value (sensor warming up, out of range, ...)
int readCareLink(tConfig *cfg, struct NSinfo *ns);

// Forgets the looked-up CareLink configuration and patient (not the login).
// Call when the CareLink country or patient setting changes.
void careLinkResetSession();

// True when a CareLink login (refresh token) is stored on the device.
bool careLinkHasLogin();

// True when the stored login was rejected and the user has to sign in again.
bool careLinkNeedsSignIn();

// Address of the CareLink sign-in page for this device's country. Empty on error,
// with the reason in errOut. Needs internet access.
String careLinkLoginUrl(tConfig *cfg, String &errOut);

// Completes sign-in from what the user pasted into the setup page: either the address
// the browser ended on after signing in (or just its code), or the JSON login file
// produced by carelink-python-client's login script. Returns true on success;
// msgOut is a short human-readable result either way.
bool careLinkSignIn(tConfig *cfg, const char *pasted, String &msgOut);

// Deletes the stored CareLink login from the device.
void careLinkSignOut();

// One-line status for the setup page, e.g. "Signed in, following Jane" or the last error.
String careLinkStatus();

#endif
