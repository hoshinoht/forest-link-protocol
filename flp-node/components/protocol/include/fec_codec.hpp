#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "esp_heap_caps.h"
#include "packet.hpp"

namespace flp
{

class FecEncoder
{
  public:
    FecEncoder() = default;
    ~FecEncoder()
    {
        if (parity_)
        {
            heap_caps_free(parity_);
            parity_ = nullptr;
        }
    }

    /* Non-copyable (owns heap memory) */
    FecEncoder(const FecEncoder &) = delete;
    FecEncoder &operator=(const FecEncoder &) = delete;

    void reset()
    {
        count_ = 0;
        parity_len_ = 0;
        ensure_allocated();
        if (parity_)
        {
            memset(parity_, 0, MAX_MTU);
        }
    }

    void ingest(const uint8_t *frag, size_t len)
    {
        ensure_allocated();
        if (!parity_) return;
        if (len > parity_len_)
            parity_len_ = len;
        for (size_t i = 0; i < len; i++)
        {
            parity_[i] ^= frag[i];
        }
        count_++;
    }

    bool group_complete() const { return count_ >= FEC_GROUP_SIZE; }
    const uint8_t *parity_data() const { return parity_; }
    size_t parity_len() const { return parity_len_; }

  private:
    void ensure_allocated()
    {
        if (parity_) return;
        parity_ = static_cast<uint8_t *>(
            heap_caps_calloc(1, MAX_MTU, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }

    uint8_t *parity_ = nullptr;
    size_t parity_len_ = 0;
    uint8_t count_ = 0;
};

class FecDecoder
{
  public:
    FecDecoder() = default;
    ~FecDecoder()
    {
        if (heap_mem_)
        {
            heap_caps_free(heap_mem_);
            heap_mem_ = nullptr;
        }
    }

    /* Non-copyable (owns heap memory) */
    FecDecoder(const FecDecoder &) = delete;
    FecDecoder &operator=(const FecDecoder &) = delete;

    void reset()
    {
        ensure_allocated();
        if (!heap_mem_) return;
        for (auto &g : heap_mem_->groups)
        {
            g.reset();
        }
        missing_idx_ = -1;
        recovered_ = false;
        recovered_len_ = 0;
        last_used_ = 0;
    }

    /* Returns true if a fragment was recovered */
    bool ingest(uint16_t seq, const uint8_t *data, size_t len, bool is_parity)
    {
        if (!heap_mem_ || len > MAX_MTU)
            return false;

        uint16_t group = seq / (FEC_GROUP_SIZE + 1);
        uint8_t idx = seq % (FEC_GROUP_SIZE + 1);
        if (idx > FEC_GROUP_SIZE)
            return false;

        int gi = -1;
        for (int i = 0; i < 2; i++)
        {
            if (heap_mem_->groups[i].active && heap_mem_->groups[i].current_group == group)
            {
                gi = i;
                break;
            }
        }
        if (gi < 0)
        {
            gi = (last_used_ == 0) ? 1 : 0;
            heap_mem_->groups[gi].reset();
            heap_mem_->groups[gi].current_group = group;
            heap_mem_->groups[gi].active = true;
        }
        last_used_ = static_cast<uint8_t>(gi);

        auto &g = heap_mem_->groups[gi];
        if (!g.slots[idx].received)
        {
            memcpy(g.slots[idx].data, data, len);
            g.slots[idx].len = len;
            g.slots[idx].received = true;
            g.slots[idx].is_parity = is_parity;
            g.slot_count++;
        }

        if (g.slot_count == FEC_GROUP_SIZE)
        {
            return try_recover(g);
        }
        return false;
    }

    bool was_recovered() const { return recovered_; }
    uint16_t recovered_seq() const
    {
        return recovered_group_ * (FEC_GROUP_SIZE + 1) +
               static_cast<uint16_t>(missing_idx_);
    }
    const uint8_t *recovered_data() const { return heap_mem_ ? heap_mem_->recovered_buf : nullptr; }
    size_t recovered_len() const { return recovered_len_; }

  private:
    struct Slot
    {
        uint8_t data[MAX_MTU] = {};
        size_t len = 0;
        bool received = false;
        bool is_parity = false;
    };

    struct GroupState
    {
        Slot slots[FEC_GROUP_SIZE + 1] = {};
        uint8_t slot_count = 0;
        uint16_t current_group = 0;
        bool active = false;

        void reset()
        {
            memset(slots, 0, sizeof(slots));
            slot_count = 0;
            current_group = 0;
            active = false;
        }
    };

    /* All large buffers live in a single PSRAM allocation */
    struct HeapState
    {
        GroupState groups[2] = {};
        uint8_t recovered_buf[MAX_MTU] = {};
    };

    HeapState *heap_mem_ = nullptr;

    void ensure_allocated()
    {
        if (heap_mem_) return;
        heap_mem_ = static_cast<HeapState *>(
            heap_caps_calloc(1, sizeof(HeapState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        /* No internal fallback — PSRAM is required for FEC state */
    }

    bool has_parity_check(const GroupState &g) const
    {
        for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++)
        {
            if (g.slots[i].received && g.slots[i].is_parity)
                return true;
        }
        return false;
    }

    bool try_recover(GroupState &g)
    {
        if (!heap_mem_) return false;

        int missing = -1;
        for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++)
        {
            if (!g.slots[i].received)
            {
                if (missing >= 0)
                    return false;
                missing = i;
            }
        }
        if (missing < 0)
            return false;
        if (static_cast<uint8_t>(missing) == FEC_GROUP_SIZE &&
            !has_parity_check(g))
        {
            return false;
        }

        missing_idx_ = missing;
        recovered_group_ = g.current_group;

        memset(heap_mem_->recovered_buf, 0, sizeof(heap_mem_->recovered_buf));
        recovered_len_ = 0;
        for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++)
        {
            if (static_cast<int>(i) == missing)
                continue;
            if (g.slots[i].len > recovered_len_)
                recovered_len_ = g.slots[i].len;
            for (size_t j = 0; j < g.slots[i].len; j++)
            {
                heap_mem_->recovered_buf[j] ^= g.slots[i].data[j];
            }
        }
        recovered_ = true;
        return true;
    }

    uint8_t last_used_ = 0;
    int missing_idx_ = -1;
    bool recovered_ = false;
    size_t recovered_len_ = 0;
    uint16_t recovered_group_ = 0;
};

} /* namespace flp */
