#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "packet.hpp"

namespace flp
{

class FecEncoder
{
  public:
    void reset()
    {
        count_ = 0;
        parity_len_ = 0;
        memset(parity_, 0, sizeof(parity_));
    }

    void ingest(const uint8_t *frag, size_t len)
    {
        /* XOR into accumulator */
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
    uint8_t parity_[MAX_MTU] = {};
    size_t parity_len_ = 0;
    uint8_t count_ = 0;
};

class FecDecoder
{
  public:
    void reset()
    {
        memset(slots_, 0, sizeof(slots_));
        slot_count_ = 0;
        has_parity_ = false;
        missing_idx_ = -1;
        recovered_ = false;
        recovered_len_ = 0;
    }

    /* Returns true if a fragment was recovered */
    bool ingest(uint16_t seq, const uint8_t *data, size_t len, bool is_parity)
    {
        /* Guard: reject oversized payloads before any buffer access */
        if (len > MAX_MTU)
            return false;

        uint16_t group = seq / (FEC_GROUP_SIZE + 1);
        uint8_t idx = seq % (FEC_GROUP_SIZE + 1);

        /* New group? Reset. */
        if (group != current_group_ || !active_)
        {
            reset();
            current_group_ = group;
            active_ = true;
        }

        if (idx > FEC_GROUP_SIZE)
            return false;

        if (!slots_[idx].received)
        {
            memcpy(slots_[idx].data, data, len);
            slots_[idx].len = len;
            slots_[idx].received = true;
            slots_[idx].is_parity = is_parity;
            slot_count_++;
        }

        /* Try recovery: need exactly K of K+1 */
        if (slot_count_ == FEC_GROUP_SIZE)
        {
            return try_recover();
        }
        return false;
    }

    bool was_recovered() const { return recovered_; }
    uint16_t recovered_seq() const
    {
        return current_group_ * (FEC_GROUP_SIZE + 1) +
               static_cast<uint16_t>(missing_idx_);
    }
    const uint8_t *recovered_data() const { return recovered_buf_; }
    size_t recovered_len() const { return recovered_len_; }

  private:
    bool try_recover()
    {
        /* Find the one missing slot */
        int missing = -1;
        for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++)
        {
            if (!slots_[i].received)
            {
                if (missing >= 0)
                    return false; /* more than one missing */
                missing = i;
            }
        }
        if (missing < 0)
            return false; /* none missing (all received) */
        if (static_cast<uint8_t>(missing) == FEC_GROUP_SIZE &&
            !has_parity_check())
        {
            /* Missing the parity — nothing to recover, all data is present */
            return false;
        }

        missing_idx_ = missing;

        /* XOR all received slots to recover */
        memset(recovered_buf_, 0, sizeof(recovered_buf_));
        recovered_len_ = 0;
        for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++)
        {
            if (static_cast<int>(i) == missing)
                continue;
            if (slots_[i].len > recovered_len_)
                recovered_len_ = slots_[i].len;
            for (size_t j = 0; j < slots_[i].len; j++)
            {
                recovered_buf_[j] ^= slots_[i].data[j];
            }
        }
        recovered_ = true;
        return true;
    }

    bool has_parity_check() const
    {
        /* Check if any slot is parity */
        for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++)
        {
            if (slots_[i].received && slots_[i].is_parity)
                return true;
        }
        return false;
    }

    struct Slot
    {
        uint8_t data[MAX_MTU] = {};
        size_t len = 0;
        bool received = false;
        bool is_parity = false;
    };

    Slot slots_[FEC_GROUP_SIZE + 1] = {};
    uint8_t slot_count_ = 0;
    bool has_parity_ = false;
    int missing_idx_ = -1;
    bool recovered_ = false;
    uint8_t recovered_buf_[MAX_MTU] = {};
    size_t recovered_len_ = 0;
    uint16_t current_group_ = 0;
    bool active_ = false;
};

} /* namespace flp */
