// Captive-portal setup: the board opens its own Wi-Fi network; a phone joins it,
// the setup page pops up, and the person picks Wi-Fi + password + their name.
#pragma once

// Blocks until settings are saved (then restarts the board) or 10 minutes pass
// (then restarts too, so a router outage can't strand the device in setup mode).
// wifi_ok: the board is online right now, so the page offers "keep my current Wi-Fi" (ticked) and changing
// just the name or quick-send icon doesn't need the Wi-Fi password typed again.
void setup_portal_run(const char *ap_name, bool wifi_ok = false);
