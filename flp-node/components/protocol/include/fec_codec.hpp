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
        for (auto &g : groups_)
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
        /* Guard: reject oversized payloads before any buffer access */
        if (len > MAX_MTU)
            return false;

        uint16_t group = seq / (FEC_GROUP_SIZE + 1);
        uint8_t idx = seq % (FEC_GROUP_SIZE + 1);
        if (idx > FEC_GROUP_SIZE)
            return false;

        int gi = -1;
        for (int i = 0; i < 2; i++)
        {
            if (groups_[i].active && groups_[i].current_group == group)
            {
                gi = i;
                break;
            }
        }
        if (gi < 0)
        {
            gi = (last_used_ == 0) ? 1 : 0;
            groups_[gi].reset();
            groups_[gi].current_group = group;
            groups_[gi].active = true;
        }
        last_used_ = static_cast<uint8_t>(gi);

        auto &g = groups_[gi];
        if (!g.slots[idx].received)
        {
            memcpy(g.slots[idx].data, data, len);
            g.slots[idx].len = len;
            g.slots[idx].received = true;
            g.slots[idx].is_parity = is_parity;
            g.slot_count++;
        }

        /* Try recovery: need exactly K of K+1 */
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
    const uint8_t *recovered_data() const { return recovered_buf_; }
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

        memset(recovered_buf_, 0, sizeof(recovered_buf_));
        recovered_len_ = 0;
        for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++)
        {
            if (static_cast<int>(i) == missing)
                continue;
            if (g.slots[i].len > recovered_len_)
                recovered_len_ = g.slots[i].len;
            for (size_t j = 0; j < g.slots[i].len; j++)
            {
                recovered_buf_[j] ^= g.slots[i].data[j];
            }
        }
        recovered_ = true;
        return true;
    }

    GroupState groups_[2] = {};
    uint8_t last_used_ = 0;
    int missing_idx_ = -1;
    bool recovered_ = false;
    uint8_t recovered_buf_[MAX_MTU] = {};
    size_t recovered_len_ = 0;
    uint16_t recovered_group_ = 0;
};

} /* namespace flp */
