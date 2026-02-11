#include "crypto.hpp"
#include "esp_log.h"

static const char *TAG = "crypto";

namespace flp {

void Crypto::init()
{
    ESP_LOGI(TAG, "Crypto engine initialized (HW AES)");
    // TODO: Initialize mbedtls AES-GCM context
}

int Crypto::encrypt(uint8_t *data, size_t len, size_t buf_capacity,
                    const uint8_t *key, const uint8_t *iv, size_t iv_len)
{
    // TODO: mbedtls_gcm_crypt_and_tag
    ESP_LOGD(TAG, "encrypt %zu bytes", len);
    return 0;
}

int Crypto::decrypt(uint8_t *data, size_t len,
                    const uint8_t *key, const uint8_t *iv, size_t iv_len)
{
    // TODO: mbedtls_gcm_auth_decrypt
    ESP_LOGD(TAG, "decrypt %zu bytes", len);
    return 0;
}

} // namespace flp
