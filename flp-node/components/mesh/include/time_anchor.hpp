#pragma once

/*
 * time_anchor.hpp — FTSP-style logical clock helpers for FLP.
 *
 * Devices maintain a small TimeAnchor (declared in packet.hpp) that tracks
 * the most authoritative cloud epoch they have seen, with a max-register
 * CRDT merge rule. The hop schedule is then a pure function of
 * (seed, anchor, now), so all converged nodes compute the same WiFi
 * channel and LoRa frequency independently.
 *
 * This header is the single source of truth for:
 *   - the merge rule (should_adopt)
 *   - the slot extrapolator (current_slot)
 *   - the HMAC-SHA256 channel/frequency derivation
 *   - the device-side hop set + slot duration constants
 *
 * Constants here MUST match cloud-admin/internal/epoch/publisher.go.
 */

#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "mbedtls/md.h"
#include "packet.hpp"

namespace flp::time_anchor
{

/* Slot duration. Must match cloud-admin epoch.SlotMs (30000 ms).
 * 30 s gives ~60 hours of clock-drift margin on a 50 ppm XTAL — way
 * more than enough for an opportunistic-uplink mesh. */
inline constexpr uint32_t kSlotMs = 30000;

/* WiFi hop set. Must match cloud-admin epoch.WifiChannels.
 * {1, 6, 11} are the three non-overlapping 2.4 GHz channels. */
inline constexpr uint8_t kWifiChannels[] = {1, 6, 11};
inline constexpr size_t kWifiChannelCount =
    sizeof(kWifiChannels) / sizeof(kWifiChannels[0]);

/* LoRa hop set. Must match cloud-admin epoch.LoraFreqsHz.
 * Three spaced 2.4 GHz LoRa frequencies — gives some interference
 * diversity without forcing the SX1280 to retune across the full band. */
inline constexpr uint32_t kLoraFreqsHz[] = {
    2403000000U,
    2425000000U,
    2479000000U,
};
inline constexpr size_t kLoraFreqCount =
    sizeof(kLoraFreqsHz) / sizeof(kLoraFreqsHz[0]);

/* LoRa hops less often than WiFi to keep retune cost low — one LoRa
 * slot covers this many WiFi slots. */
inline constexpr uint32_t kLoraSlotsPerWifiSlot = 4; /* 120 s LoRa slot */

/* Every Nth send_discovery() also piggy-backs the 32-byte hop seed so
 * un-seeded neighbors can pick it up without a separate request packet.
 * At the 10 s discovery cadence this is once every 60 s. */
inline constexpr uint32_t kSeedBroadcastPeriod = 6;

/*
 * Max-register CRDT merge rule.
 *
 * Returns true iff the received anchor is strictly newer than the local
 * one and should replace it. The total order is:
 *   1. higher cloud_incarnation wins  (cloud-admin reboot fence)
 *   2. higher cloud_epoch wins        (newer slot reading)
 *   3. lower origin_node wins         (deterministic tiebreak so the
 *                                      same anchor is chosen mesh-wide
 *                                      regardless of arrival order)
 *
 * Note that origin_local_ms is NOT part of the comparison — it is purely
 * a local extrapolation reference and gets re-stamped on every adopt.
 */
inline bool should_adopt(const TimeAnchor &recv, const TimeAnchor &local)
{
    if (recv.cloud_incarnation != local.cloud_incarnation)
    {
        return recv.cloud_incarnation > local.cloud_incarnation;
    }
    if (recv.cloud_epoch != local.cloud_epoch)
    {
        return recv.cloud_epoch > local.cloud_epoch;
    }
    return recv.origin_node < local.origin_node;
}

/*
 * Extrapolate the current slot from a held anchor.
 *
 * The anchor records "at MY local time origin_local_ms, the cloud epoch
 * was cloud_epoch". To compute the current slot we just advance from
 * that reference using our local monotonic clock. This is correct
 * regardless of whether cloud is currently reachable.
 */
inline uint64_t current_slot(const TimeAnchor &anchor, uint32_t now_ms)
{
    if (now_ms < anchor.origin_local_ms)
    {
        /* Clock went backward (e.g. wrap of 32-bit ms after ~49 days).
         * Treat as zero elapsed; the next adopt will re-anchor cleanly. */
        return anchor.cloud_epoch;
    }
    uint32_t elapsed = now_ms - anchor.origin_local_ms;
    return anchor.cloud_epoch + (uint64_t) (elapsed / kSlotMs);
}

/*
 * HMAC-SHA256 wrapper around mbedtls. Writes 32 bytes to out[].
 * Returns true on success.
 */
inline bool hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len,
                        uint8_t out[32])
{
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info)
    {
        return false;
    }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    bool ok = false;
    if (mbedtls_md_setup(&ctx, info, 1 /* HMAC */) == 0 &&
        mbedtls_md_hmac_starts(&ctx, key, key_len) == 0 &&
        mbedtls_md_hmac_update(&ctx, data, data_len) == 0 &&
        mbedtls_md_hmac_finish(&ctx, out) == 0)
    {
        ok = true;
    }
    mbedtls_md_free(&ctx);
    return ok;
}

/*
 * Derive the WiFi channel for a given slot. Pure function of (seed, slot).
 * All converged nodes that share the same seed and the same slot compute
 * the same channel — that is the whole point of the design.
 */
inline uint8_t wifi_channel_for_slot(const uint8_t seed[32], uint64_t slot)
{
    uint8_t h[32];
    if (!hmac_sha256(seed, 32, (const uint8_t *) &slot, sizeof(slot), h))
    {
        return kWifiChannels[0]; /* defensive fallback */
    }
    return kWifiChannels[h[0] % kWifiChannelCount];
}

/*
 * Derive the LoRa frequency for the current slot. LoRa hops on a
 * coarser cadence (kLoraSlotsPerWifiSlot WiFi slots per LoRa slot) so
 * the SX1280 retune cost is amortised.
 */
inline uint32_t lora_freq_for_slot(const uint8_t seed[32], uint64_t slot)
{
    uint64_t lora_slot = slot / kLoraSlotsPerWifiSlot;
    uint8_t h[32];
    if (!hmac_sha256(seed, 32, (const uint8_t *) &lora_slot,
                     sizeof(lora_slot), h))
    {
        return kLoraFreqsHz[0]; /* defensive fallback */
    }
    /* Use byte 1 (not byte 0) so wifi and lora picks are independent
     * even though they share the same input slot space. */
    return kLoraFreqsHz[h[1] % kLoraFreqCount];
}

} /* namespace flp::time_anchor */
