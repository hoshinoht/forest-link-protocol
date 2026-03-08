#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "packet.hpp"

namespace flp
{

struct BufferSlab
{
    uint8_t data[MAX_MTU];
    size_t len = 0;
    int8_t rssi = 0;
    RxTransport source = RxTransport::ESPNOW;
    std::atomic<uint8_t> refcount{0};
};

class BufferPool
{
  public:
    static constexpr uint8_t POOL_SIZE = 24;

    void init();
    BufferSlab *acquire();
    void release(BufferSlab *slab);
    void add_ref(BufferSlab *slab);

  private:
    BufferSlab slabs_[POOL_SIZE];
    int8_t freelist_[POOL_SIZE];
    std::atomic<int8_t> top_{-1};
};

} /* namespace flp */
