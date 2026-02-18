#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace flp
{

class ITransport
{
  public:
    virtual ~ITransport() = default;

    virtual void init() = 0;
    virtual void deinit() = 0;

    virtual int send(uint16_t peer_addr, const uint8_t *data, size_t len) = 0;
};

} // namespace flp
