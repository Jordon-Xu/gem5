#ifndef __MEM_CACHE_PREFETCH_CMC_HH__
#define __MEM_CACHE_PREFETCH_CMC_HH__
//#include <boost/circular_buffer.hpp>
//#include <boost/compute/detail/lru_cache.hpp>

//Adapted from https://github.com/OpenXiangShan/GEM5/blob/xs-dev/src/mem/cache/prefetch

#include "base/types.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "mem/packet.hh"
#include "params/CMCPrefetcher.hh"

namespace gem5
{
struct CMCPrefetcherParams;
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch
{


class CMCPrefetcher : public Queued
{
    BaseTags* cachetags;
  public:
  
    class StorageEntry;
    class RecordEntry
    {
        public:
            Addr pc;
            Addr addr;
            bool is_secure;
            bool has_prev_load_branch;
            bool last_branch_taken;
            RecordEntry(Addr p, Addr a, bool s, bool has_prev, bool taken)
                : pc(p), addr(a), is_secure(s),
                  has_prev_load_branch(has_prev), last_branch_taken(taken) {}
            RecordEntry()
                : addr(0), is_secure(true), has_prev_load_branch(false),
                  last_branch_taken(false) {}
    };
    class Recorder
    {
        public:
            std::vector<Addr> entries;
            int index;
            const int nr_entry;

            explicit Recorder(int degree)
                : entries(), index(0), nr_entry(degree)
            {
                assert(degree > 0);
            }
            bool entry_empty() { return entries.empty(); }
            Addr get_base_addr() { return entries[0]; }

            bool train_entry(Addr, bool, bool*);
            void reset();
            // const int nr_entry = 16;
        private:
    };

    // class StorageEntry : public TaggedEntry
    // {
    //     public:
    //         std::vector<Addr> addresses;
    //         int refcnt;
    //         uint64_t id;
    //         void invalidate() override;
    // };
    class StorageEntry : public TaggedEntry
    {
      public:
        using TaggedEntry::insert; // 解除 “hidden virtual” warning

        void insert(Addr tag, bool is_secure);
        bool match(Addr tag, bool is_secure) const;

        void invalidate() override;

        std::vector<Addr> addresses;
        int refcnt = 0;
        uint64_t id = 0;
    };





  private:
    Recorder *recorder;
    AssociativeSet<StorageEntry> storage;
    uint64_t acc_id = 1;
    const bool useLastBranchTaken;

  public:
    CMCPrefetcher(const CMCPrefetcherParams &p);
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache_accessor) override;
  private:
    uint64_t hash(Addr addr, Addr pc, bool has_prev_load_branch,
                  bool last_branch_taken) const {
        uint64_t branch_component = 0;
        if (useLastBranchTaken && has_prev_load_branch) {
            branch_component = last_branch_taken ? 2ULL : 1ULL;
        }
        return addr ^ (static_cast<uint64_t>(pc) << 8) ^
            branch_component;
    }

    static const int STACK_SIZE = 4;
    std::deque<RecordEntry> trigger;
    // RecordEntry trigger_stack[STACK_SIZE];
};



}  // namespace prefetch
}  // namespace gem5

#endif  // GEM5_SMS_HH
