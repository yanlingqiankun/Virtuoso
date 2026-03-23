
#pragma once
#include "pagetable.h"
#include "site_clock.h"
#include <shared_mutex>
#include <unordered_map>

namespace ParametricDramDirectoryMSI
{

	class PageTableRadix : public PageTable
	{

	private:
		struct PTFrame;
		std::vector<std::shared_mutex> m_page_locks;
		static const int NUM_PAGE_LOCKS = 256;
		enum page_permission {
			READ_WRITE,
			MOVING
		};

		struct PTE
		{
			bool valid; // Valid bit
			IntPtr ppn; // Physical page number
		};

		struct PTEntry
		{
			bool is_pte; // Whether the entry is a PTE or a pointer to the next level of the page table
			page_permission permission;
			subsecond_time_t DMA_finish;
			union
			{
				PTE translation;
				PTFrame *next_level;
			} data; // Stores whether the entry is a PTE or a pointer to the next level of the page table
			PTEntry() {
				permission = READ_WRITE;
				DMA_finish = SubsecondTime::Zero();
			}
		};

		struct PTFrame
		{
			PTEntry *entries;	 // 512 entries, 4KB page table frames
			IntPtr emulated_ppn; // Address of the frame in the physical memory (this is emulated)
		};

		typedef struct PTFrame PT;

		PT *root;		  // Root of the radix tree
		int m_frame_size; // Size of the frame in bytes
		int levels;		  // Number of levels in the radix tree

		// We use 3 page walk caches, one for the first three levels of the radix tree

		std::ofstream log_file; // Log file for the page table
		std::string log_file_name;

		struct Stats
		{
			UInt64 page_table_walks;
			UInt64 ptw_num_cache_accesses; // Number of cache accesses for page table walks
			UInt64 pf_num_cache_accesses;  // Number of cache accesses for page faults
			UInt64 page_faults;
			UInt64 *page_size_discovery; // Number of times each page size is discovered
			UInt64 allocated_frames;	 // Number of frames allocated for the page table
			UInt64 page_faults_of_migration;
		} stats;

	public:
		PageTableRadix(int core_id, String name, String type, int page_sizes, int *page_size_list, int levels, int frame_size, bool is_guest = false);
		PTWResult initializeWalk(IntPtr address, bool count, bool is_prefetch = false, bool restart_walk = false);
		int updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames);
		void deletePage(IntPtr address);
		void page_moving(IntPtr address) override;
		void DMA_move_page(IntPtr address, IntPtr new_ppn, subsecond_time_t finish_time) override;
		IntPtr getPhysicalSpace(int size);
		String getType() { return "radix"; };
		int getMaxLevel() { return levels; };
		std::shared_mutex& get_lock_for_page(IntPtr address) override;
		bool check_page_exist(IntPtr address) override;
		void incrementPageFaultsOfMigration() override { stats.page_faults_of_migration++; }

		// ===== SITE (Self-Invalidating TLB Entries) =====
		/**
		 * @brief ETT entry: stores per-page expiration metadata for SITE.
		 * The lease is now global (in SiteLogicalClock), so each ETT entry
		 * only stores the expiration_time computed at page allocation.
		 */
		struct SiteETTEntry
		{
			UInt32 expiration_time;  // Expiration time = allocate_time + global_lease

			SiteETTEntry()
				: expiration_time(0) {}
		};

		/**
		 * @brief Get (or create with defaults) the SITE ETT entry for a given VPN.
		 */
		SiteETTEntry& getSiteETTEntry(IntPtr vpn)
		{
			auto it = site_ett.find(vpn);
			if (it == site_ett.end()) {
				site_ett[vpn] = SiteETTEntry();
			}
			return site_ett[vpn];
		}

		/**
		 * @brief Set the SITE expiration time for a given VPN.
		 * Called at page allocation time.
		 */
		void setSiteExpiration(IntPtr vpn, UInt32 expiration)
		{
			site_ett[vpn].expiration_time = expiration;
		}

		/**
		 * @brief Clear the SITE ETT entry for a given VPN.
		 * Called when the page is freed/invalidated.
		 */
		void clearSiteETTEntry(IntPtr vpn)
		{
			site_ett.erase(vpn);
		}

	private:
		std::unordered_map<IntPtr, SiteETTEntry> site_ett; // SITE ETT mapping: VPN -> ETT entry
	};
}
