/*
    Wee Glo (DiabetesHub Ireland): Improv Wi-Fi over USB serial.

    Lets the install page (ESP Web Tools) set the home Wi-Fi straight after installing,
    while Wee Glo is still plugged into the computer, so families don't need to join
    Wee Glo's own setup Wi-Fi. Protocol: https://www.improv-wifi.com/serial/

    This file is part of the DiabetesHub display, released under the GNU GPL v3.
*/
#ifndef WEEGLO_IMPROV_H
#define WEEGLO_IMPROV_H

// Call often from loop(), in both normal and setup (bootstrap) mode.
void improvLoop();
void improvBeacon();   // only announces Wee Glo, safe to call while starting up

#endif
