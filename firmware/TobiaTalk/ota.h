// Over-the-air updates: signed firmware downloaded from the relay server, with
// automatic rollback if the new version cannot reach the server after booting.
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>

// Downloads /firmware?version=N, checks the ECDSA signature against the public key
// compiled into this firmware, installs it and restarts. Returns only on failure
// (reason in `err`). `tls` must already be configured by the caller.
bool ota_install(WiFiClientSecure &tls, const String &base_url, const char *family_key, int version, size_t size,
                 String &err);

// A freshly installed image boots "pending verify". Mark it good once it has talked
// to the server; if that never happens, roll back to the previous image.
bool ota_pending_verify();
void ota_mark_valid();
void ota_rollback();   // reboots into the previous image
