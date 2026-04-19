#ifndef __MEM_CACHE_PREFETCH_CMC_HH__
#define __MEM_CACHE_PREFETCH_CMC_HH__

#include <algorithm>
#include <deque>
#include <limits>

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
        struct ContextStream
        {
            bool valid = false;
            uint16_t ctxTag = 0;
            uint8_t confidence = 0;
            uint64_t lastTouch = 0;
            std::vector<Addr> addresses;

            void
            invalidate()
            {
                valid = false;
                ctxTag = 0;
                confidence = 0;
                lastTouch = 0;
                addresses.clear();
            }
        };

        using TaggedEntry::insert; // 解除 “hidden virtual” warning

        explicit StorageEntry(unsigned variantCount = 1)
            : variants(std::max(1u, variantCount))
        {}

        void insert(Addr tag, bool is_secure);
        bool match(Addr tag, bool is_secure) const;

        void invalidate() override;
        void clearStreams();

        std::vector<ContextStream> variants;
    };





  private:
    Recorder *recorder;
    AssociativeSet<StorageEntry> storage;
    const unsigned baseDegree;
    uint64_t ctxVariantClock = 0;

  /* branch context state */
  uint64_t currentBranchCtx = 0;
  bool ctxEnable;
  bool ctxTakenOnly;
  bool ctxUseExecuteBranches;
  bool ctxUseRequestSnapshot;
  bool ctxUseLoadPcSnapshot;
  const unsigned ctxLoadPcMinBranches;
  bool ctxLoadPcMultiVariantOnly;
  unsigned ctxShift;
  unsigned ctxBits;
  unsigned ctxWindowSize;
  uint64_t retiredBranchCount = 0;
  unsigned ctxUpdatePeriod;
  const unsigned ctxMismatchDegree;

    struct CMCStats : public statistics::Group
    {
        CMCStats(statistics::Group *parent);

        statistics::Scalar primaryHits;
        statistics::Scalar ctxMatches;
        statistics::Scalar ctxMismatches;
        statistics::Scalar pfCandidatesFromMatch;
        statistics::Scalar pfCandidatesFromMismatch;
        statistics::Scalar ctxVariantAllocations;
        statistics::Scalar ctxVariantReplacements;
        statistics::Scalar ctxSingleVariantMismatches;
        statistics::Scalar ctxMultiVariantMismatches;
        statistics::Scalar ctxRequestSnapshots;
        statistics::Scalar ctxLoadPcSnapshots;
        statistics::Scalar ctxLoadPcFallbacks;
        statistics::Scalar ctxLoadPcInsufficientBranches;
        statistics::Scalar ctxLoadPcSingleVariantSkips;
        statistics::Scalar ctxLoadPcNoBetterMatchSkips;
        statistics::Scalar ctxLoadPcDisambiguations;
        statistics::Scalar ctxGlobalFallbacks;
    } statsCMC;

  public:
    CMCPrefetcher(const CMCPrefetcherParams &p);
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache_accessor) override;

    /* Branch-context hook driven by retired-branch probes. */
    void notifyRetiredBranch(Addr branch_pc);
    void notifyRetiredTakenBranch(Addr branch_pc);
    void notifyExecutedBranch(Addr branch_pc);
    void notifyExecutedTakenBranch(Addr branch_pc);
    void addEventProbeRetiredInsts(SimObject *obj, const char *name);
    void addEventProbeRetiredBranches(SimObject *obj, const char *name);
    void addEventProbeRetiredTakenBranches(SimObject *obj, const char *name);
    void addEventProbeExecutedBranches(SimObject *obj, const char *name);
    void addEventProbeExecutedTakenBranches(SimObject *obj, const char *name);

  private:
    uint64_t hash(Addr addr, Addr pc, uint64_t ctx) {
        // Context is intentionally excluded from the primary key and used
        // only as a secondary filter on top of baseline CMC.
        (void)ctx;
        return addr ^ (static_cast<uint64_t>(pc) << 8);
    }

    uint16_t compressCtx(uint64_t ctx) const
    {
        if (!ctxEnable || ctxBits == 0) {
            return 0;
        }

        const unsigned bits = std::min(ctxBits, 16u);
        const uint64_t mask = (1ULL << bits) - 1;
        return static_cast<uint16_t>(ctx & mask);
    }

    unsigned allowedDegree(bool ctx_match, size_t available) const
    {
        const unsigned cappedAvailable = static_cast<unsigned>(
            std::min<size_t>(available, std::numeric_limits<unsigned>::max()));
        const unsigned fullDegree = std::min(baseDegree, cappedAvailable);

        if (!ctxEnable || ctxBits == 0 || ctx_match) {
            return fullDegree;
        }

        return std::min(ctxMismatchDegree, fullDegree);
    }

    uint64_t getCurrentBranchCtx() const
    {
        return ctxEnable ? currentBranchCtx : 0;
    }

    struct SnapshotCtxResult
    {
        uint64_t ctx = 0;
        unsigned selectedBranches = 0;
        unsigned contributingBranches = 0;
    };

    struct AccessCtxSelection
    {
        uint64_t ctx = 0;
        bool usedRequestSnapshot = false;
        bool usedLoadPcSnapshot = false;
        bool loadPcFallback = false;
        bool loadPcInsufficient = false;
        bool loadPcSingleVariantSkip = false;
        bool loadPcNoBetterMatchSkip = false;
    };

    SnapshotCtxResult buildCtxFromSnapshot(
        const BranchContextSnapshot &snapshot, uint64_t total_branches,
        uint64_t total_taken_branches) const;
    AccessCtxSelection selectAccessBranchCtx(
        const PrefetchInfo &pfi, const StorageEntry *match_entry) const;

    uint64_t mixBranchCtx(uint64_t ctx, Addr branch_pc) const
    {
        const unsigned s = ctxShift & 63;

        if (s == 0) {
            return ctx ^ static_cast<uint64_t>(branch_pc);
        }

        return (ctx << s) ^ (ctx >> (64 - s)) ^
               static_cast<uint64_t>(branch_pc);
    }

    void updateBranchCtx(Addr branch_pc);
    using ContextStream = StorageEntry::ContextStream;

    unsigned validVariantCount(const StorageEntry *entry) const
    {
        return std::count_if(entry->variants.begin(), entry->variants.end(),
            [](const ContextStream &variant) { return variant.valid; });
    }

    bool hasContextStream(const StorageEntry *entry, uint16_t ctx_tag) const
    {
        return std::any_of(entry->variants.begin(), entry->variants.end(),
            [ctx_tag](const ContextStream &variant) {
                return variant.valid && variant.ctxTag == ctx_tag;
            });
    }

    ContextStream *findContextStream(StorageEntry *entry, uint16_t ctx_tag) const;
    ContextStream *selectIssueStream(StorageEntry *entry, uint16_t ctx_tag,
                                     bool &ctx_match) const;
    ContextStream *selectTrainingStream(StorageEntry *entry, uint16_t ctx_tag,
                                        bool &allocated, bool &replaced);
    void touchContextStream(ContextStream &stream, bool reinforce);

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

    class PrefetchTakenListenerPC : public ProbeListenerArgBase<Addr>
    {
      public:
        PrefetchTakenListenerPC(
            CMCPrefetcher &_parent, const std::string &name)
            : ProbeListenerArgBase<Addr>(name), parent(_parent)
        {}

        void notify(const Addr &pc) override;

      private:
        CMCPrefetcher &parent;
    };

    class PrefetchExecutedListenerPC : public ProbeListenerArgBase<Addr>
    {
      public:
        PrefetchExecutedListenerPC(
            CMCPrefetcher &_parent, const std::string &name)
            : ProbeListenerArgBase<Addr>(name), parent(_parent)
        {}

        void notify(const Addr &pc) override;

      private:
        CMCPrefetcher &parent;
    };

    class PrefetchExecutedTakenListenerPC : public ProbeListenerArgBase<Addr>
    {
      public:
        PrefetchExecutedTakenListenerPC(
            CMCPrefetcher &_parent, const std::string &name)
            : ProbeListenerArgBase<Addr>(name), parent(_parent)
        {}

        void notify(const Addr &pc) override;

      private:
        CMCPrefetcher &parent;
    };

    std::vector<ProbeListenerPtr<PrefetchListenerPC>> listenersPC;
    std::vector<ProbeListenerPtr<PrefetchTakenListenerPC>> listenersTakenPC;
    std::vector<ProbeListenerPtr<PrefetchExecutedListenerPC>>
        listenersExecutedPC;
    std::vector<ProbeListenerPtr<PrefetchExecutedTakenListenerPC>>
        listenersExecutedTakenPC;

    std::deque<Addr> recentBranchPCs;
    static const int STACK_SIZE = 4;
    std::deque<RecordEntry> trigger;
    // RecordEntry trigger_stack[STACK_SIZE];
};



}  // namespace prefetch
}  // namespace gem5

#endif  // GEM5_SMS_HH
