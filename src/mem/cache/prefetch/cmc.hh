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
#include "sim/probe/probe.hh"

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
            uint64_t branch_ctx; 
            RecordEntry(Addr p, Addr a, bool s, uint64_t ctx)
                : pc(p), addr(a), is_secure(s), branch_ctx(ctx) {}
            RecordEntry() : addr(0), is_secure(true), branch_ctx(0) {}
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

  /* branch context state */
  uint64_t currentBranchCtx = 0;
  bool ctxEnable;
  unsigned ctxShift;

  public:
    CMCPrefetcher(const CMCPrefetcherParams &p);
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache_accessor) override;

    /* first-step branch context hook */
    void notifyRetiredBranch(Addr branch_pc);
    void addEventProbeRetiredInsts(SimObject *obj, const char *name);
    void addEventProbeRetiredBranches(SimObject *obj, const char *name);

  private:
    uint64_t hash(Addr addr, Addr pc, uint64_t ctx) {
      uint64_t h = addr;
      h ^= (static_cast<uint64_t>(pc) << 8);
      h ^= (ctx + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
      return h;
    }

    uint64_t getCurrentBranchCtx() const
    {
        return ctxEnable ? currentBranchCtx : 0;
    }

    void updateBranchCtx(Addr branch_pc);

    class PrefetchListenerPC : public ProbeListenerArgBase<Addr>
    {
      public:
        PrefetchListenerPC(CMCPrefetcher &_parent, const std::string &name)
            : ProbeListenerArgBase<Addr>(name), parent(_parent)
        {}

        void notify(const Addr &pc) override;

      private:
        CMCPrefetcher &parent;
    };

std::vector<ProbeListenerPtr<PrefetchListenerPC>> listenersPC;

    static const int STACK_SIZE = 4;
    std::deque<RecordEntry> trigger;
    // RecordEntry trigger_stack[STACK_SIZE];
};



}  // namespace prefetch
}  // namespace gem5

#endif  // GEM5_SMS_HH
