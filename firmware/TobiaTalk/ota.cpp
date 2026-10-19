#include "ota.h"
#include <HTTPClient.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "ota_public.h"     // PUBLIC_KEY / PUBLIC_KEY_LEN, generated from signing/ota_private.pem

// The Arduino core marks a new image valid at startup unless this returns true.
// We mark it valid ourselves after the first successful server contact.
extern "C" bool verifyRollbackLater() { return true; }

bool ota_pending_verify()
{
  esp_ota_img_states_t state;
  const esp_partition_t *running = esp_ota_get_running_partition();
  return esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_mark_valid()
{
  if (ota_pending_verify()) esp_ota_mark_app_valid_cancel_rollback();
}

void ota_rollback()
{
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

bool ota_install(WiFiClientSecure &tls, const String &base_url, const char *family_key, int version, size_t size,
                 String &err)
{
  HTTPClient http;
  http.setTimeout(30000);
  if (!http.begin(tls, base_url + "/firmware?version=" + String(version))) { err = "connect"; return false; }
  http.addHeader("x-family-key", family_key);
  int code = http.GET();
  if (code != 200) { err = "http " + String(code); http.end(); return false; }

  int len = http.getSize();
  if (len <= 512 || (size_t)len != size) { err = "bad size"; http.end(); return false; }

  UpdaterECDSAVerifier verifier(PUBLIC_KEY, PUBLIC_KEY_LEN, HASH_SHA256);
  if (!Update.installSignature(&verifier)) { err = "sig setup"; http.end(); return false; }
  if (!Update.begin(len)) { err = "no space"; http.end(); return false; }

  size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();
  if (written != (size_t)len) {
    Update.abort();
    err = "short download";
    return false;
  }
  if (!Update.end(true)) {            // true = also verify the signature
    err = Update.errorString();
    return false;
  }
  ESP.restart();
  return true;
}
