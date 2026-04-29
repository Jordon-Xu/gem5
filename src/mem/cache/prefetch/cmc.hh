#ifndef __MEM_CACHE_PREFETCH_CMC_HH__
#define __MEM_CACHE_PREFETCH_CMC_HH__
//#include <boost/circular_buffer.hpp>
//#include <boost/compute/detail/lru_cache.hpp>

//Adapted from https://github.com/OpenXiangShan/GEM5/blob/xs-dev/src/mem/cache/prefetch

#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>

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
            bool use_prev_branch_for_chooser;
            Addr prev_branch_pc;
            bool last_branch_taken;
            RecordEntry(Addr p, Addr a, bool s, bool has_prev,
                        bool use_for_chooser, Addr branch_pc, bool taken)
                : pc(p), addr(a), is_secure(s),
                  has_prev_load_branch(has_prev),
                  use_prev_branch_for_chooser(use_for_chooser),
                  prev_branch_pc(branch_pc),
                  last_branch_taken(taken) {}
            RecordEntry()
                : addr(0), is_secure(true), has_prev_load_branch(false),
                  use_prev_branch_for_chooser(false),
                  prev_branch_pc(0),
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
    struct ChooserKey
    {
        Addr loadPc = 0;
        Addr prevBranchPc = 0;
        bool prevBranchTaken = false;

        bool
        operator==(const ChooserKey &other) const
        {
            return loadPc == other.loadPc &&
                   prevBranchPc == other.prevBranchPc &&
                   prevBranchTaken == other.prevBranchTaken;
        }
    };

    struct ChooserKeyHash
    {
        std::size_t
        operator()(const ChooserKey &key) const
        {
            return std::hash<Addr>{}(key.loadPc) ^
                (std::hash<Addr>{}(key.prevBranchPc) << 1) ^
                (std::hash<bool>{}(key.prevBranchTaken) << 2);
        }
    };

    struct HeadDeltaChooserEntry
    {
        static constexpr unsigned MaxCandidates = 2;

        int64_t deltaBlocks[MaxCandidates] = {0, 0};
        unsigned counts[MaxCandidates] = {0, 0};
        int utilityScores[MaxCandidates] = {0, 0};
        bool valid[MaxCandidates] = {false, false};
        unsigned samples = 0;
    };

    struct HeadDeltaHintResult
    {
        bool changed = false;
        bool hasPrediction = false;
        int64_t predictedDeltaBlocks = 0;
        ChooserKey chooserKey;
    };

    struct PendingHeadPrediction
    {
        bool hasBaselineHead = false;
        int64_t baselineHeadDeltaBlocks = 0;
        bool hasChooserPrediction = false;
        bool fromAccessPredictor = false;
        int64_t chooserHeadDeltaBlocks = 0;
        ChooserKey chooserKey;
    };

    struct PrevBranchPcStats
    {
        uint64_t taken = 0;
        uint64_t notTaken = 0;
        bool countedBiased = false;
    };

    struct Stats : public statistics::Group
    {
        Stats(statistics::Group *parent);

        statistics::Scalar totalLookups;
        statistics::Scalar lookupsWithValidPrevBranch;
        statistics::Scalar prevBranchTakenLookups;
        statistics::Scalar prevBranchNotTakenLookups;
        statistics::Scalar lookupAugmentedKeyDiffersFromBaseline;
        statistics::Scalar prevBranchDistinctPCs;
        statistics::Scalar prevBranchBiasedPCs;
        statistics::Scalar prevBranchBiasedLookups;
        statistics::Scalar prevBranchFilteredLookups;
        statistics::Scalar prevBranchChooserUsableLookups;
        statistics::Scalar baselineLookupHits;
        statistics::Scalar augmentedLookupHits;
        statistics::Scalar prefetchCandidatesFromAugmentedKeys;

        statistics::Scalar trainCompletions;
        statistics::Scalar trainsWithValidPrevBranch;
        statistics::Scalar prevBranchTakenTrains;
        statistics::Scalar prevBranchNotTakenTrains;
        statistics::Scalar trainAugmentedKeyDiffersFromBaseline;
        statistics::Scalar baselineTrainHits;
        statistics::Scalar augmentedTrainHits;
        statistics::Scalar chooserLookups;
        statistics::Scalar chooserHits;
        statistics::Scalar chooserEligible;
        statistics::Scalar chooserLowPuritySkips;
        statistics::Scalar chooserPredictedAlreadyHead;
        statistics::Scalar chooserPredictedFoundInStream;
        statistics::Scalar chooserPredictedNotInStream;
        statistics::Scalar chooserNoAction;
        statistics::Scalar chooserHeadPromotions;
        statistics::Scalar chooserSecondChoicePromotions;
        statistics::Scalar chooserConstructedHeads;
        statistics::Scalar chooserSelectiveConstructedHeads;
        statistics::Scalar chooserConstructThresholdSkips;
        statistics::Scalar chooserConstructCacheSkips;
        statistics::Scalar chooserUtilityConstructSkips;
        statistics::Scalar chooserUtilityLimitSkips;
        statistics::Scalar headPredictionFeedbacks;
        statistics::Scalar baselineHeadCorrect;
        statistics::Scalar chooserHeadPredictions;
        statistics::Scalar chooserHeadCorrect;
        statistics::Scalar chooserOnlyCorrect;
        statistics::Scalar baselineOnlyCorrect;
        statistics::Scalar chooserSameAsBaseline;
        statistics::Scalar chooserDiffersFromBaseline;
        statistics::Scalar chooserUtilityPositiveUpdates;
        statistics::Scalar chooserUtilityNegativeUpdates;
        statistics::Scalar accessHeadLookups;
        statistics::Scalar accessHeadEligible;
        statistics::Scalar accessHeadIssued;
        statistics::Scalar accessHeadLowScoreSkips;
        statistics::Scalar accessHeadCacheSkips;
        statistics::Scalar accessHeadFeedbacks;
        statistics::Scalar chooserAdaptiveLimitIssues;
        statistics::Scalar chooserLimitedIssues;
        statistics::Scalar chooserDroppedCandidates;
        statistics::Scalar chooserTrainUpdates;

        statistics::Formula prevBranchFeatureValidRate;
        statistics::Formula baselineLookupHitRate;
        statistics::Formula augmentedLookupHitRate;
        statistics::Formula prevBranchTakenRate;
        statistics::Formula prevBranchBiasedLookupRate;
        statistics::Formula prevBranchFilteredRate;
        statistics::Formula prevBranchChooserUsableRate;
        statistics::Formula trainPrevBranchFeatureValidRate;
        statistics::Formula baselineTrainHitRate;
        statistics::Formula augmentedTrainHitRate;
        statistics::Formula prevBranchTakenTrainRate;
        statistics::Formula chooserHitRate;
        statistics::Formula chooserEligibleRate;
    } cmcStats;

    Recorder *recorder;
    AssociativeSet<StorageEntry> storage;
    uint64_t acc_id = 1;
    const bool useLastBranchTaken;
    const unsigned chooserTopK;
    const bool chooserUseTaken;
    const unsigned chooserMinSamples;
    const unsigned chooserMinConfidence;
    const unsigned chooserMinTopPct;
    const bool chooserLimitOnHit;
    const bool chooserAdaptiveLimit;
    const bool chooserOnlyPredicted;
    const unsigned chooserBaselineFallbackDegree;
    const bool chooserConstructPredicted;
    const bool chooserSelectiveConstruct;
    const unsigned chooserConstructMinSamples;
    const unsigned chooserConstructMinConfidence;
    const unsigned chooserConstructMinTopPct;
    const bool chooserConstructCacheFilter;
    const bool chooserUseUtilityScore;
    const int chooserConstructMinScore;
    const int chooserThrottleMinScore;
    const int chooserUtilityMaxScore;
    const bool prevBranchAccessPredictor;
    const int prevBranchAccessMinScore;
    const bool prevBranchAccessCacheFilter;
    const bool filterBiasedPrevBranches;
    const unsigned branchBiasMinSamples;
    const unsigned branchBiasMaxPct;
    const std::string prevBranchDumpFile;
    const uint64_t prevBranchDumpLimit;
    std::ofstream prevBranchDumpStream;
    uint64_t prevBranchDumpedSamples = 0;
    std::unordered_map<Addr, Addr> lastObservedBlockByPc;
    std::unordered_map<Addr, PendingHeadPrediction> pendingHeadPredictions;
    std::unordered_map<Addr, PrevBranchPcStats> prevBranchPcStats;
    std::unordered_map<ChooserKey, HeadDeltaChooserEntry, ChooserKeyHash>
        headDeltaChooser;

  public:
    CMCPrefetcher(const CMCPrefetcherParams &p);
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache_accessor) override;
  private:
    uint64_t
    hash(Addr addr, Addr pc) const
    {
        return addr ^ (static_cast<uint64_t>(pc) << 8);
    }

    static const int STACK_SIZE = 4;
    std::deque<RecordEntry> trigger;
    ChooserKey makeChooserKey(Addr load_pc, Addr prev_branch_pc,
                              bool prev_branch_taken) const;
    bool isPrevBranchPcBiased(const PrevBranchPcStats &stats) const;
    bool updatePrevBranchPcStats(Addr prev_branch_pc, bool taken);
    void dumpPrevBranchSample(Addr pc, Addr block_addr, bool has_prev_pc,
                              Addr delta_blocks, bool prev_branch_valid,
                              Addr prev_branch_pc, bool prev_branch_taken,
                              bool prev_branch_biased,
                              bool prev_branch_usable,
                              bool cache_miss, bool match_entry);
    HeadDeltaHintResult applyHeadDeltaHint(Addr block_addr, Addr pc,
                                           bool prev_branch_valid,
                                           Addr prev_branch_pc,
                                           bool prev_branch_taken,
                                           std::vector<AddrPriority> &addresses,
                                           const CacheAccessor &cache,
                                           bool is_secure);
    bool issueAccessHeadPrediction(Addr block_addr, Addr pc,
                                   bool prev_branch_valid,
                                   Addr prev_branch_pc,
                                   bool prev_branch_taken,
                                   std::vector<AddrPriority> &addresses,
                                   const CacheAccessor &cache,
                                   bool is_secure);
    void evaluatePendingHeadPrediction(Addr pc, int64_t actual_delta_blocks);
    void updateHeadDeltaChooser(Addr pc, Addr prev_branch_pc,
                                bool prev_branch_taken,
                                int64_t head_delta_blocks);
    // RecordEntry trigger_stack[STACK_SIZE];
};



}  // namespace prefetch
}  // namespace gem5

#endif  // GEM5_SMS_HH
