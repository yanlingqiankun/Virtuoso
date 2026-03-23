#pragma once

#include "fixed_types.h"
#include <atomic>
#include <vector>
#include <algorithm>

/**
 * @brief Global logical clock for SITE (Self-Invalidating TLB Entries).
 *
 * The clock is based on the number of main memory (DRAM) accesses,
 * NOT on physical time or clock cycles.
 *
 * Key design points:
 *   - A single global lease value (not per-ETT-entry).
 *   - All cores record misses via atomic operations against a global counter.
 *   - When the miss counter exceeds the threshold, the lease is doubled.
 *   - The global lease is read at page allocation time to compute expiration.
 *
 * All SITE parameters are configurable via the [site] section in .cfg files.
 */
class SiteLogicalClock
{
public:
    static SiteLogicalClock* getInstance()
    {
        static SiteLogicalClock instance;
        return &instance;
    }

    /**
     * @brief Increment the global access counter. Called by DRAM controller on each access.
     */
    void incrementAccessCounter()
    {
        UInt32 old_val = m_global_access_counter.fetch_add(1, std::memory_order_relaxed);
        // Broadcast periodically
        if ((old_val + 1) % m_broadcast_interval == 0) {
            broadcastTime();
        }
    }

    /**
     * @brief Get the current global logical time.
     */
    UInt32 getGlobalTime() const
    {
        return m_global_access_counter.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get the local logical time for a specific core.
     * @param core_id The core ID.
     */
    UInt32 getCoreLocalTime(core_id_t core_id) const
    {
        if (core_id < 0 || (size_t)core_id >= m_per_core_time.size())
            return m_global_access_counter.load(std::memory_order_relaxed);
        return m_per_core_time[core_id];
    }

    /**
     * @brief Initialize per-core time registers and load config. Call once during setup.
     * @param num_cores Number of cores in the system.
     * @param broadcast_interval Broadcast interval (from cfg, default 100).
     * @param initial_lease Initial lease length (from cfg, default 200).
     * @param miss_threshold Number of misses before doubling lease (from cfg, default 3).
     * @param max_lease Maximum lease length (from cfg, default 100000).
     * @param min_lease Minimum lease length (from cfg, default 50).
     */
    void init(int num_cores,
              UInt32 broadcast_interval = 100,
              UInt32 initial_lease = 200,
              int miss_threshold = 3,
              UInt32 max_lease = 100000,
              UInt32 min_lease = 50)
    {
        m_per_core_time.resize(num_cores, 0);
        m_global_access_counter.store(0, std::memory_order_relaxed);
        m_broadcast_interval = broadcast_interval;
        m_initial_lease = initial_lease;
        m_current_lease.store(initial_lease, std::memory_order_relaxed);
        m_miss_threshold = miss_threshold;
        m_max_lease = max_lease;
        m_min_lease = min_lease;
        m_global_miss_counter.store(0, std::memory_order_relaxed);
    }

    // ===== Global Lease Operations =====

    /**
     * @brief Get the current global lease value.
     * All pages use this same lease when computing their expiration time.
     */
    UInt32 getCurrentLease() const
    {
        return m_current_lease.load(std::memory_order_relaxed);
    }

    /**
     * @brief Record a TLB miss due to expired entry (called from any core).
     * Atomically increments a global miss counter. When the threshold is
     * exceeded, the global lease is doubled (up to max_lease) and the
     * counter is reset.
     * @return true if lease was extended, false otherwise.
     */
    bool recordMiss()
    {
        UInt32 old_count = m_global_miss_counter.fetch_add(1, std::memory_order_relaxed);
        if ((int)(old_count + 1) >= m_miss_threshold)
        {
            // Try to reset the counter (only one thread should succeed)
            UInt32 expected = old_count + 1;
            if (m_global_miss_counter.compare_exchange_strong(expected, 0, std::memory_order_relaxed))
            {
                // Double the lease, capped at max_lease
                UInt32 cur = m_current_lease.load(std::memory_order_relaxed);
                UInt32 new_lease = std::min(cur * 2, m_max_lease);
                m_current_lease.store(new_lease, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Shrink the global lease (e.g., when TLB shootdown must be performed).
     * Halves the lease, capped at min_lease.
     */
    void shrinkLease()
    {
        UInt32 cur = m_current_lease.load(std::memory_order_relaxed);
        UInt32 new_lease = std::max(cur / 2, m_min_lease);
        m_current_lease.store(new_lease, std::memory_order_relaxed);
    }

    // ===== Configurable SITE parameters (read from [site] in cfg) =====
    UInt32 getBroadcastInterval() const { return m_broadcast_interval; }
    UInt32 getInitialLease()      const { return m_initial_lease; }
    int    getMissThreshold()     const { return m_miss_threshold; }
    UInt32 getMaxLease()          const { return m_max_lease; }
    UInt32 getMinLease()          const { return m_min_lease; }

private:
    SiteLogicalClock()
        : m_global_access_counter(0),
          m_broadcast_interval(100),
          m_initial_lease(200),
          m_current_lease(200),
          m_miss_threshold(3),
          m_max_lease(100000),
          m_min_lease(50),
          m_global_miss_counter(0)
    {}

    /**
     * @brief Broadcast the current global time to all per-core registers.
     */
    void broadcastTime()
    {
        UInt32 current = m_global_access_counter.load(std::memory_order_relaxed);
        for (size_t i = 0; i < m_per_core_time.size(); i++) {
            m_per_core_time[i] = current;
        }
    }

    std::atomic<UInt32> m_global_access_counter;
    std::vector<UInt32> m_per_core_time; // Per-core local time registers

    // Configurable SITE parameters
    UInt32 m_broadcast_interval;  // Broadcast every N DRAM accesses
    UInt32 m_initial_lease;       // Initial lease length (for reset)

    // Global lease state (NEW: single shared lease, not per-ETT-entry)
    std::atomic<UInt32> m_current_lease;      // Current global lease length
    int    m_miss_threshold;                   // Miss count before doubling lease
    UInt32 m_max_lease;                        // Maximum lease length
    UInt32 m_min_lease;                        // Minimum lease length
    std::atomic<UInt32> m_global_miss_counter; // Global miss counter (across all cores)
};
