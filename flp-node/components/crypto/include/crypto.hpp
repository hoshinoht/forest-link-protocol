#pragma once

#include <cstdint>
#include <cstddef>

namespace flp {

class Crypto {
public:
    Crypto() = default;

    void init();

    // TODO: AES-128-GCM encrypt in-place, appends 16-byte auth tag
    int encrypt(uint8_t *data, size_t len, size_t buf_capacity,
                const uint8_t *key, const uint8_t *iv, size_t iv_len);

    // TODO: AES-128-GCM decrypt in-place, verifies auth tag
    int decrypt(uint8_t *data, size_t len,
                const uint8_t *key, const uint8_t *iv, size_t iv_len);

private:
    // TODO: mbedtls context, key storage
};

} // namespace flp
