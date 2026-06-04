
//Adapted from https://github.com/OpenXiangShan/GEM5/blob/xs-dev/src/mem/cache/prefetch
#include "mem/cache/prefetch/cmc.hh"

#include <algorithm>

#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/CMCPrefetcher.hh"

namespace gem5
{
namespace prefetch
{
CMCPrefetcher::Stats::Stats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(totalLookups, statistics::units::Count::get(),
        "Total CMC metadata lookups"),
    ADD_STAT(lookupsWithValidPrevBranch, statistics::units::Count::get(),
        "Lookups with a valid previous same-load-PC branch feature"),
    ADD_STAT(prevBranchTakenLookups, statistics::units::Count::get(),
        "Lookups whose valid previous same-load-PC branch outcome was taken"),
    ADD_STAT(prevBranchNotTakenLookups, statistics::units::Count::get(),
        "Lookups whose valid previous same-load-PC branch outcome was not "
        "taken"),
    ADD_STAT(lookupAugmentedKeyDiffersFromBaseline,
        statistics::units::Count::get(),
        "Lookups where the augmented key differs from the baseline key"),
    ADD_STAT(prevBranchDistinctPCs, statistics::units::Count::get(),
        "Distinct previous branch-context PCs observed by CMC"),
    ADD_STAT(prevBranchBiasedPCs, statistics::units::Count::get(),
        "Previous branch-context PCs classified as outcome-biased"),
    ADD_STAT(prevBranchBiasedLookups, statistics::units::Count::get(),
        "Lookups whose previous branch-context PC is outcome-biased"),
    ADD_STAT(prevBranchFilteredLookups, statistics::units::Count::get(),
        "Lookups where the previous branch feature is filtered out"),
    ADD_STAT(prevBranchChooserUsableLookups, statistics::units::Count::get(),
        "Lookups with a previous branch feature usable by the chooser"),
    ADD_STAT(baselineLookupHits, statistics::units::Count::get(),
        "Lookup hits using the baseline (block_addr, load_pc) key"),
    ADD_STAT(augmentedLookupHits, statistics::units::Count::get(),
        "Lookup hits using the augmented key"),
    ADD_STAT(prefetchCandidatesFromAugmentedKeys,
        statistics::units::Count::get(),
        "Prefetch candidates emitted from lookups whose augmented key differs "
        "from the baseline key"),
    ADD_STAT(trainCompletions, statistics::units::Count::get(),
        "Completed CMC stream training events"),
    ADD_STAT(trainsWithValidPrevBranch, statistics::units::Count::get(),
        "Training completions with a valid previous same-load-PC branch "
        "feature"),
    ADD_STAT(prevBranchTakenTrains, statistics::units::Count::get(),
        "Training completions whose valid previous same-load-PC branch "
        "outcome was taken"),
    ADD_STAT(prevBranchNotTakenTrains, statistics::units::Count::get(),
        "Training completions whose valid previous same-load-PC branch "
        "outcome was not taken"),
    ADD_STAT(trainAugmentedKeyDiffersFromBaseline,
        statistics::units::Count::get(),
        "Training completions where the augmented key differs from the "
        "baseline key"),
    ADD_STAT(baselineTrainHits, statistics::units::Count::get(),
        "Training completions that find an existing baseline-key entry"),
    ADD_STAT(augmentedTrainHits, statistics::units::Count::get(),
        "Training completions that find an existing branch-augmented-key "
        "entry"),
    ADD_STAT(chooserLookups, statistics::units::Count::get(),
        "Lookups that query the prev-branch head-delta chooser"),
    ADD_STAT(chooserHits, statistics::units::Count::get(),
        "Lookups that find a prev-branch head-delta chooser entry"),
    ADD_STAT(chooserEligible, statistics::units::Count::get(),
        "Chooser hits that pass the sample and confidence thresholds"),
    ADD_STAT(chooserLowPuritySkips, statistics::units::Count::get(),
        "Chooser hits skipped because the top head-delta share is too low"),
    ADD_STAT(chooserPredictedAlreadyHead, statistics::units::Count::get(),
        "Eligible chooser predictions that already match the baseline CMC "
        "head candidate"),
    ADD_STAT(chooserPredictedFoundInStream, statistics::units::Count::get(),
        "Eligible chooser predictions found somewhere in the baseline CMC "
        "candidate stream"),
    ADD_STAT(chooserPredictedNotInStream, statistics::units::Count::get(),
        "Eligible chooser predictions not found in the baseline CMC "
        "candidate stream"),
    ADD_STAT(chooserNoAction, statistics::units::Count::get(),
        "Eligible chooser hits that did not change the issued candidates"),
    ADD_STAT(chooserHeadPromotions, statistics::units::Count::get(),
        "Lookups where the chooser promotes a matching head delta"),
    ADD_STAT(chooserSecondChoicePromotions, statistics::units::Count::get(),
        "Lookups where the chooser promotes its second head-delta candidate"),
    ADD_STAT(chooserConstructedHeads, statistics::units::Count::get(),
        "Chooser hits where the predicted head address was constructed "
        "instead of found in the baseline stream"),
    ADD_STAT(chooserSelectiveConstructedHeads, statistics::units::Count::get(),
        "Chooser hits where a missing predicted head was constructed without "
        "throttling the baseline stream"),
    ADD_STAT(chooserConstructedActiveCmcDelta, statistics::units::Count::get(),
        "Constructed chooser heads whose delta exists in another active CMC "
        "stream for the same load PC"),
    ADD_STAT(chooserConstructedEvictedCmcDelta, statistics::units::Count::get(),
        "Constructed chooser heads whose delta was previously present in a "
        "CMC stream for the same load PC but later evicted or overwritten"),
    ADD_STAT(chooserConstructedNeverCmcDelta, statistics::units::Count::get(),
        "Constructed chooser heads whose delta has not been seen in any CMC "
        "stream for the same load PC"),
    ADD_STAT(chooserConstructThresholdSkips, statistics::units::Count::get(),
        "Chooser predictions not constructed because construct-only "
        "confidence thresholds rejected them"),
    ADD_STAT(chooserConstructCacheSkips, statistics::units::Count::get(),
        "Chooser predictions not constructed because the predicted address "
        "was already in cache or MSHR"),
    ADD_STAT(chooserUtilityConstructSkips, statistics::units::Count::get(),
        "Chooser predictions not constructed because their utility score was "
        "below the construct threshold"),
    ADD_STAT(chooserUtilityLimitSkips, statistics::units::Count::get(),
        "Chooser hits not used for stream limiting because their utility "
        "score was below the throttle threshold"),
    ADD_STAT(headPredictionFeedbacks, statistics::units::Count::get(),
        "Same-load-PC next-delta feedback samples for issued baseline heads"),
    ADD_STAT(baselineHeadCorrect, statistics::units::Count::get(),
        "Feedback samples where the baseline CMC head delta matched the next "
        "same-load-PC delta"),
    ADD_STAT(chooserHeadPredictions, statistics::units::Count::get(),
        "Feedback samples with a chooser predicted head delta"),
    ADD_STAT(chooserHeadCorrect, statistics::units::Count::get(),
        "Feedback samples where the chooser predicted head delta matched the "
        "next same-load-PC delta"),
    ADD_STAT(chooserOnlyCorrect, statistics::units::Count::get(),
        "Feedback samples where the chooser head was correct and the "
        "baseline head was not"),
    ADD_STAT(baselineOnlyCorrect, statistics::units::Count::get(),
        "Feedback samples where the baseline head was correct and the "
        "chooser head was not"),
    ADD_STAT(chooserSameAsBaseline, statistics::units::Count::get(),
        "Feedback samples where the chooser predicted the same head delta as "
        "baseline CMC"),
    ADD_STAT(chooserDiffersFromBaseline, statistics::units::Count::get(),
        "Feedback samples where the chooser predicted a different head delta "
        "from baseline CMC"),
    ADD_STAT(chooserUtilityPositiveUpdates, statistics::units::Count::get(),
        "Chooser utility score increments from next-delta feedback"),
    ADD_STAT(chooserUtilityNegativeUpdates, statistics::units::Count::get(),
        "Chooser utility score decrements from next-delta feedback"),
    ADD_STAT(chooserLimitPositiveUpdates, statistics::units::Count::get(),
        "Chooser stream-limit score increments when limiting would keep the "
        "next same-load-PC delta or the delta was absent from the CMC stream"),
    ADD_STAT(chooserLimitNegativeUpdates, statistics::units::Count::get(),
        "Chooser stream-limit score decrements when the next same-load-PC "
        "delta was only present in the dropped CMC tail"),
    ADD_STAT(chooserTailLimitSkips, statistics::units::Count::get(),
        "Adaptive limit candidates skipped because their stream-tail safety "
        "score was below the throttle threshold"),
    ADD_STAT(accessHeadLookups, statistics::units::Count::get(),
        "Ordinary access-time branch-head predictor lookups"),
    ADD_STAT(accessHeadEligible, statistics::units::Count::get(),
        "Access-time branch-head predictor lookups that pass frequency "
        "thresholds"),
    ADD_STAT(accessHeadIssued, statistics::units::Count::get(),
        "Access-time branch-head predictions issued"),
    ADD_STAT(accessHeadLookaheadIssued, statistics::units::Count::get(),
        "Access-time branch-head predictions issued at a farther-ahead "
        "multiple of the predicted delta after nearer addresses were blocked"),
    ADD_STAT(accessHeadActiveCmcDelta, statistics::units::Count::get(),
        "Access-time heads whose delta exists in another active CMC stream "
        "for the same load PC"),
    ADD_STAT(accessHeadEvictedCmcDelta, statistics::units::Count::get(),
        "Access-time heads whose delta was previously present in a CMC stream "
        "for the same load PC but later evicted or overwritten"),
    ADD_STAT(accessHeadNeverCmcDelta, statistics::units::Count::get(),
        "Access-time heads whose delta has not been seen in any CMC stream "
        "for the same load PC"),
    ADD_STAT(accessHeadLowScoreSkips, statistics::units::Count::get(),
        "Access-time branch-head predictions skipped by utility score"),
    ADD_STAT(accessHeadCacheSkips, statistics::units::Count::get(),
        "Access-time branch-head predictions skipped because the address was "
        "already in cache or MSHR"),
    ADD_STAT(accessHeadFeedbacks, statistics::units::Count::get(),
        "Next same-load-PC delta feedback samples for access-time branch-head "
        "predictions"),
    ADD_STAT(chooserAdaptiveLimitIssues, statistics::units::Count::get(),
        "Chooser hits that limited the stream due to adaptive utility score"),
    ADD_STAT(chooserLimitedIssues, statistics::units::Count::get(),
        "Chooser hits that limited the issued prefetch candidate list"),
    ADD_STAT(chooserDroppedCandidates, statistics::units::Count::get(),
        "Prefetch candidates dropped because the chooser limited a stream"),
    ADD_STAT(sourceTrackedBaselineHead, statistics::units::Count::get(),
        "Tracked issued prefetch candidates from the baseline CMC head"),
    ADD_STAT(sourceTrackedBaselineFallback, statistics::units::Count::get(),
        "Tracked issued prefetch candidates from the baseline CMC fallback "
        "prefix"),
    ADD_STAT(sourceTrackedBaselineTail, statistics::units::Count::get(),
        "Tracked issued prefetch candidates from the baseline CMC tail"),
    ADD_STAT(sourceTrackedChooserHead, statistics::units::Count::get(),
        "Tracked issued prefetch candidates introduced or promoted by the "
        "branch chooser"),
    ADD_STAT(sourceTrackedAccessHead, statistics::units::Count::get(),
        "Tracked issued prefetch candidates from the access-time branch-head "
        "predictor"),
    ADD_STAT(sourceUsefulFeedbacks, statistics::units::Count::get(),
        "Useful prefetched blocks matched back to a tracked CMC candidate "
        "source"),
    ADD_STAT(sourceUsefulBaselineHead, statistics::units::Count::get(),
        "Useful tracked prefetches from the baseline CMC head"),
    ADD_STAT(sourceUsefulBaselineFallback, statistics::units::Count::get(),
        "Useful tracked prefetches from the baseline CMC fallback prefix"),
    ADD_STAT(sourceUsefulBaselineTail, statistics::units::Count::get(),
        "Useful tracked prefetches from the baseline CMC tail"),
    ADD_STAT(sourceUsefulChooserHead, statistics::units::Count::get(),
        "Useful tracked prefetches introduced or promoted by the branch "
        "chooser"),
    ADD_STAT(sourceUsefulAccessHead, statistics::units::Count::get(),
        "Useful tracked prefetches from the access-time branch-head "
        "predictor"),
    ADD_STAT(cmcStorageUpdates, statistics::units::Count::get(),
        "Completed trainings that update an existing CMC storage entry"),
    ADD_STAT(cmcStorageInsertions, statistics::units::Count::get(),
        "Completed trainings that insert into a CMC storage entry"),
    ADD_STAT(cmcStorageVictimReplacements, statistics::units::Count::get(),
        "CMC insertions that replace a valid victim entry"),
    ADD_STAT(cmcStorageVictimInvalid, statistics::units::Count::get(),
        "CMC insertions that use an invalid victim entry"),
    ADD_STAT(chooserTrainUpdates, statistics::units::Count::get(),
        "Training completions that update the prev-branch head-delta chooser"),
    ADD_STAT(remapSegmentsTrained, statistics::units::Count::get(),
        "CMC remap stream segments written during training"),
    ADD_STAT(remapContinuationSegments, statistics::units::Count::get(),
        "CMC remap segments whose lookup key is a previously recorded stream "
        "address"),
    ADD_STAT(remapBranchKeyedSegments, statistics::units::Count::get(),
        "CMC remap segments written with previous-branch context in the "
        "storage key"),
    ADD_STAT(remapBranchSplitStops, statistics::units::Count::get(),
        "CMC remap segments ended early because the recorded previous-branch "
        "context changed"),
    ADD_STAT(prevBranchFeatureValidRate, statistics::units::Ratio::get(),
        "Fraction of lookups with a valid previous same-load-PC branch "
        "feature"),
    ADD_STAT(baselineLookupHitRate, statistics::units::Ratio::get(),
        "Baseline-key lookup hit rate"),
    ADD_STAT(augmentedLookupHitRate, statistics::units::Ratio::get(),
        "Augmented-key lookup hit rate"),
    ADD_STAT(prevBranchTakenRate, statistics::units::Ratio::get(),
        "Taken fraction among valid previous-branch lookups"),
    ADD_STAT(prevBranchBiasedLookupRate, statistics::units::Ratio::get(),
        "Fraction of valid previous-branch lookups from biased branch PCs"),
    ADD_STAT(prevBranchFilteredRate, statistics::units::Ratio::get(),
        "Fraction of valid previous-branch lookups filtered before chooser"),
    ADD_STAT(prevBranchChooserUsableRate, statistics::units::Ratio::get(),
        "Fraction of valid previous-branch lookups usable by chooser"),
    ADD_STAT(trainPrevBranchFeatureValidRate, statistics::units::Ratio::get(),
        "Fraction of completed trainings with a valid previous same-load-PC "
        "branch feature"),
    ADD_STAT(baselineTrainHitRate, statistics::units::Ratio::get(),
        "Baseline-key hit rate at training completion"),
    ADD_STAT(augmentedTrainHitRate, statistics::units::Ratio::get(),
        "Augmented-key hit rate at training completion"),
    ADD_STAT(prevBranchTakenTrainRate, statistics::units::Ratio::get(),
        "Taken fraction among valid previous-branch training completions"),
    ADD_STAT(chooserHitRate, statistics::units::Ratio::get(),
        "Head-delta chooser hit rate among chooser lookups"),
    ADD_STAT(chooserEligibleRate, statistics::units::Ratio::get(),
        "Fraction of chooser hits that are eligible to issue hints")
{
    using namespace statistics;

    prevBranchFeatureValidRate.flags(total);
    prevBranchFeatureValidRate = lookupsWithValidPrevBranch / totalLookups;

    baselineLookupHitRate.flags(total);
    baselineLookupHitRate = baselineLookupHits / totalLookups;

    augmentedLookupHitRate.flags(total);
    augmentedLookupHitRate = augmentedLookupHits / totalLookups;

    prevBranchTakenRate.flags(total);
    prevBranchTakenRate = prevBranchTakenLookups / lookupsWithValidPrevBranch;

    prevBranchBiasedLookupRate.flags(total);
    prevBranchBiasedLookupRate =
        prevBranchBiasedLookups / lookupsWithValidPrevBranch;

    prevBranchFilteredRate.flags(total);
    prevBranchFilteredRate =
        prevBranchFilteredLookups / lookupsWithValidPrevBranch;

    prevBranchChooserUsableRate.flags(total);
    prevBranchChooserUsableRate =
        prevBranchChooserUsableLookups / lookupsWithValidPrevBranch;

    trainPrevBranchFeatureValidRate.flags(total);
    trainPrevBranchFeatureValidRate =
        trainsWithValidPrevBranch / trainCompletions;

    baselineTrainHitRate.flags(total);
    baselineTrainHitRate = baselineTrainHits / trainCompletions;

    augmentedTrainHitRate.flags(total);
    augmentedTrainHitRate = augmentedTrainHits / trainCompletions;

    prevBranchTakenTrainRate.flags(total);
    prevBranchTakenTrainRate =
        prevBranchTakenTrains / trainsWithValidPrevBranch;

    chooserHitRate.flags(total);
    chooserHitRate = chooserHits / chooserLookups;

    chooserEligibleRate.flags(total);
    chooserEligibleRate = chooserEligible / chooserHits;
}

//int64_t CMCPrefetcher::global_timestamp=0;
//int CMCPrefetcher::target_ways=0;
//int CMCPrefetcher::current_ways=0;

//CMCPrefetcher::SizeDuel* CMCPrefetcher::sizeDuelPtr=nullptr;

//std::vector<uint32_t> CMCPrefetcher::setPrefetch(17,0);

CMCPrefetcher::CMCPrefetcher(const CMCPrefetcherParams &p)
: Queued(p),
    cachetags(p.cachetags),
    cmcStats(this),
    recorder(new Recorder(p.degree)),
    storage(p.storage_assoc, p.storage_entries, p.storage_indexing_policy,
            p.storage_replacement_policy, StorageEntry()),
    issueCmcStream(p.issue_cmc_stream),
    useLastBranchTaken(p.use_last_branch_taken),
    chooserTopK(std::min<unsigned>(
        p.prev_branch_chooser_topk, HeadDeltaChooserEntry::MaxCandidates)),
    chooserUseTaken(p.prev_branch_chooser_use_taken),
    chooserUseBranchPc(p.prev_branch_chooser_use_branch_pc),
    chooserMinSamples(p.prev_branch_chooser_min_samples),
    chooserMinConfidence(p.prev_branch_chooser_min_confidence),
    chooserMinTopPct(p.prev_branch_chooser_min_top_pct),
    chooserLimitOnHit(p.prev_branch_chooser_limit_on_hit),
    chooserModifyBaseline(p.prev_branch_chooser_modify_baseline),
    chooserAdaptiveLimit(p.prev_branch_chooser_adaptive_limit),
    chooserAdaptiveObservationOnly(
        p.prev_branch_chooser_adaptive_observation_only),
    chooserAdaptiveBlacklist(p.prev_branch_chooser_adaptive_blacklist),
    chooserOnlyPredicted(p.prev_branch_chooser_only_predicted),
    chooserBaselineFallbackDegree(p.prev_branch_chooser_baseline_fallback_degree),
    chooserConstructPredicted(p.prev_branch_chooser_construct_predicted),
    chooserSelectiveConstruct(p.prev_branch_chooser_selective_construct),
    chooserConstructMinSamples(p.prev_branch_chooser_construct_min_samples),
    chooserConstructMinConfidence(p.prev_branch_chooser_construct_min_confidence),
    chooserConstructMinTopPct(
        std::min<unsigned>(p.prev_branch_chooser_construct_min_top_pct, 100)),
    chooserConstructCacheFilter(p.prev_branch_chooser_construct_cache_filter),
    chooserUseUtilityScore(p.prev_branch_chooser_use_utility_score),
    chooserConstructMinScore(p.prev_branch_chooser_construct_min_score),
    chooserThrottleMinScore(p.prev_branch_chooser_throttle_min_score),
    chooserTailThrottleMinScore(p.prev_branch_chooser_tail_throttle_min_score),
    chooserUtilityMaxScore(std::max(1, p.prev_branch_chooser_utility_max_score)),
    prevBranchAccessPredictor(p.prev_branch_access_predictor),
    prevBranchAccessMinScore(p.prev_branch_access_min_score),
    prevBranchAccessLookahead(std::max(1U, p.prev_branch_access_lookahead)),
    prevBranchAccessCacheFilter(p.prev_branch_access_cache_filter),
    filterBiasedPrevBranches(p.prev_branch_filter_biased),
    branchBiasMinSamples(p.prev_branch_bias_min_samples),
    branchBiasMaxPct(std::min<unsigned>(p.prev_branch_bias_max_pct, 100)),
    remapStreams(p.remap_streams),
    remapSegmentDegree(std::max(1U, p.remap_segment_degree)),
    remapUseBranchContext(p.remap_use_branch_context),
    remapUseTaken(p.remap_use_taken),
    remapSplitOnBranchChange(p.remap_split_on_branch_change),
    prevBranchDumpFile(p.prev_branch_dump_file),
    prevBranchDumpLimit(p.prev_branch_dump_limit),
    trigger()
{
    trigger.clear();
    if (!prevBranchDumpFile.empty() && prevBranchDumpLimit > 0) {
        prevBranchDumpStream.open(prevBranchDumpFile, std::ios::out |
            std::ios::trunc);
        if (!prevBranchDumpStream) {
            fatal("Failed to open prev-branch dump file %s",
                  prevBranchDumpFile.c_str());
        }
        prevBranchDumpStream
            << "sample_idx,pc,block_addr,has_prev_pc,delta_blocks,"
            << "prev_branch_valid,prev_branch_pc,prev_branch_taken,"
            << "prev_branch_biased,prev_branch_usable,cache_miss,"
            << "match_entry\n";
    }
}

CMCPrefetcher::ChooserKey
CMCPrefetcher::makeChooserKey(Addr load_pc, Addr prev_branch_pc,
                              bool prev_branch_taken) const
{
    return ChooserKey{
        load_pc,
        chooserUseBranchPc ? prev_branch_pc : 0,
        chooserUseTaken ? prev_branch_taken : false
    };
}

uint64_t
CMCPrefetcher::makeStorageKey(Addr addr, Addr pc, bool prev_branch_valid,
                              Addr prev_branch_pc,
                              bool prev_branch_taken) const
{
    uint64_t key = hash(addr, pc);
    if (remapStreams && remapUseBranchContext && prev_branch_valid) {
        key ^= static_cast<uint64_t>(prev_branch_pc) << 17;
        key ^= static_cast<uint64_t>(prev_branch_pc) >> 3;
        if (remapUseTaken && prev_branch_taken) {
            key ^= 0x9e3779b97f4a7c15ULL;
        }
    }
    return key;
}

bool
CMCPrefetcher::remapBranchContextDiffers(
    const Recorder::Access &previous, const Recorder::Access &current) const
{
    if (!remapSplitOnBranchChange || !remapUseBranchContext) {
        return false;
    }

    if (previous.use_prev_branch_for_chooser !=
        current.use_prev_branch_for_chooser) {
        return true;
    }

    if (!previous.use_prev_branch_for_chooser) {
        return false;
    }

    if (previous.prev_branch_pc != current.prev_branch_pc) {
        return true;
    }

    return remapUseTaken &&
        previous.last_branch_taken != current.last_branch_taken;
}

CMCPrefetcher::StorageEntry *
CMCPrefetcher::writeStorageEntry(uint64_t key, Addr pc, Addr trigger_addr,
                                 bool is_secure,
                                 const std::vector<Addr> &addresses,
                                 bool count_train_hit)
{
    StorageEntry *entry = storage.findEntry(key, is_secure);
    if (entry) {
        cmcStats.cmcStorageUpdates++;
        if (count_train_hit) {
            cmcStats.augmentedTrainHits++;
        }
        removeActiveCmcDeltas(*entry);
        entry->addresses = addresses;
        installActiveCmcDeltas(*entry, pc, trigger_addr, addresses);
        return entry;
    }

    entry = storage.findVictim(key);
    cmcStats.cmcStorageInsertions++;
    if (entry->isValid()) {
        cmcStats.cmcStorageVictimReplacements++;
        removeActiveCmcDeltas(*entry);
    } else {
        cmcStats.cmcStorageVictimInvalid++;
    }
    entry->addresses = addresses;
    installActiveCmcDeltas(*entry, pc, trigger_addr, addresses);
    storage.insertEntry(key, is_secure, entry);
    return entry;
}

void
CMCPrefetcher::trainRemapEntries(const RecordEntry &trigger_head)
{
    const auto &entries = recorder->entries;
    if (entries.empty()) {
        return;
    }

    const bool has_access_context = recorder->accesses.size() == entries.size();
    bool first_segment = true;
    std::size_t lookup_idx = 0;

    for (std::size_t offset = 0; offset < entries.size();) {
        std::size_t end = offset + 1;
        bool split_on_branch = false;
        while (end < entries.size() &&
               end - offset < remapSegmentDegree) {
            if (has_access_context &&
                remapBranchContextDiffers(recorder->accesses[end - 1],
                                          recorder->accesses[end])) {
                split_on_branch = true;
                break;
            }
            end++;
        }

        std::vector<Addr> segment(entries.begin() + offset,
                                  entries.begin() + end);

        Addr lookup_pc = trigger_head.pc;
        Addr lookup_block = trigger_head.addr;
        bool lookup_secure = trigger_head.is_secure;
        bool lookup_branch_valid = trigger_head.use_prev_branch_for_chooser;
        Addr lookup_branch_pc = trigger_head.prev_branch_pc;
        bool lookup_branch_taken = trigger_head.last_branch_taken;

        if (!first_segment && has_access_context) {
            const auto &lookup = recorder->accesses[lookup_idx];
            lookup_pc = lookup.pc;
            lookup_block = blockIndex(lookup.addr);
            lookup_secure = lookup.is_secure;
            lookup_branch_valid = lookup.use_prev_branch_for_chooser;
            lookup_branch_pc = lookup.prev_branch_pc;
            lookup_branch_taken = lookup.last_branch_taken;
            cmcStats.remapContinuationSegments++;
        }

        const uint64_t key = makeStorageKey(
            lookup_block, lookup_pc, lookup_branch_valid, lookup_branch_pc,
            lookup_branch_taken);
        writeStorageEntry(key, lookup_pc, lookup_block, lookup_secure,
                          segment, offset == 0);
        cmcStats.remapSegmentsTrained++;
        if (remapUseBranchContext && lookup_branch_valid) {
            cmcStats.remapBranchKeyedSegments++;
        }
        if (split_on_branch) {
            cmcStats.remapBranchSplitStops++;
        }

        lookup_idx = offset;
        offset = end;
        first_segment = false;
    }
}

void
CMCPrefetcher::removeActiveCmcDeltas(const StorageEntry &entry)
{
    if (!entry.isValid() || entry.addresses.empty()) {
        return;
    }

    auto active_it = activeCmcDeltasByPc.find(entry.triggerPc);
    if (active_it == activeCmcDeltasByPc.end()) {
        return;
    }

    for (Addr addr : entry.addresses) {
        const int64_t delta_blocks =
            static_cast<int64_t>(blockIndex(addr)) -
            static_cast<int64_t>(entry.triggerAddr);
        evictedCmcDeltasByPc[entry.triggerPc].insert(delta_blocks);

        auto delta_it = active_it->second.find(delta_blocks);
        if (delta_it == active_it->second.end()) {
            continue;
        }
        if (delta_it->second <= 1) {
            active_it->second.erase(delta_it);
        } else {
            delta_it->second--;
        }
    }

    if (active_it->second.empty()) {
        activeCmcDeltasByPc.erase(active_it);
    }
}

void
CMCPrefetcher::installActiveCmcDeltas(StorageEntry &entry, Addr pc,
                                      Addr trigger_addr,
                                      const std::vector<Addr> &addresses)
{
    entry.triggerPc = pc;
    entry.triggerAddr = trigger_addr;

    auto &active = activeCmcDeltasByPc[pc];
    for (Addr addr : addresses) {
        const int64_t delta_blocks =
            static_cast<int64_t>(blockIndex(addr)) -
            static_cast<int64_t>(trigger_addr);
        active[delta_blocks]++;
    }
}

CMCPrefetcher::CmcDeltaStatus
CMCPrefetcher::classifyCmcDelta(Addr pc, int64_t delta_blocks) const
{
    const auto active_it = activeCmcDeltasByPc.find(pc);
    if (active_it != activeCmcDeltasByPc.end() &&
        active_it->second.find(delta_blocks) != active_it->second.end()) {
        return CmcDeltaStatus::Active;
    }

    const auto evicted_it = evictedCmcDeltasByPc.find(pc);
    if (evicted_it != evictedCmcDeltasByPc.end() &&
        evicted_it->second.find(delta_blocks) != evicted_it->second.end()) {
        return CmcDeltaStatus::Evicted;
    }

    return CmcDeltaStatus::NeverSeen;
}

bool
CMCPrefetcher::isPrevBranchPcBiased(const PrevBranchPcStats &stats) const
{
    const uint64_t total = stats.taken + stats.notTaken;
    if (total < branchBiasMinSamples || total == 0) {
        return false;
    }

    const uint64_t dominant = std::max(stats.taken, stats.notTaken);
    return dominant * 100 >= total * branchBiasMaxPct;
}

bool
CMCPrefetcher::updatePrevBranchPcStats(Addr prev_branch_pc, bool taken)
{
    auto inserted = prevBranchPcStats.emplace(
        prev_branch_pc, PrevBranchPcStats());
    auto &stats = inserted.first->second;
    if (inserted.second) {
        cmcStats.prevBranchDistinctPCs++;
    }

    if (taken) {
        stats.taken++;
    } else {
        stats.notTaken++;
    }

    const bool biased = isPrevBranchPcBiased(stats);
    if (biased) {
        cmcStats.prevBranchBiasedLookups++;
        if (!stats.countedBiased) {
            stats.countedBiased = true;
            cmcStats.prevBranchBiasedPCs++;
        }
    }

    return biased;
}

void
CMCPrefetcher::dumpPrevBranchSample(Addr pc, Addr block_addr, bool has_prev_pc,
                                    Addr delta_blocks,
                                    bool prev_branch_valid,
                                    Addr prev_branch_pc,
                                    bool prev_branch_taken,
                                    bool prev_branch_biased,
                                    bool prev_branch_usable,
                                    bool cache_miss, bool match_entry)
{
    if (!prevBranchDumpStream || prevBranchDumpedSamples >= prevBranchDumpLimit) {
        return;
    }

    prevBranchDumpStream
        << prevBranchDumpedSamples << ','
        << pc << ','
        << block_addr << ','
        << (has_prev_pc ? 1 : 0) << ','
        << delta_blocks << ','
        << (prev_branch_valid ? 1 : 0) << ','
        << prev_branch_pc << ','
        << (prev_branch_taken ? 1 : 0) << ','
        << (prev_branch_biased ? 1 : 0) << ','
        << (prev_branch_usable ? 1 : 0) << ','
        << (cache_miss ? 1 : 0) << ','
        << (match_entry ? 1 : 0) << '\n';
    prevBranchDumpedSamples++;
}

CMCPrefetcher::HeadDeltaHintResult
CMCPrefetcher::applyHeadDeltaHint(Addr block_addr, Addr pc,
                                  bool prev_branch_valid,
                                  Addr prev_branch_pc,
                                  bool prev_branch_taken,
                                  std::vector<AddrPriority> &addresses,
                                  const CacheAccessor &cache, bool is_secure)
{
    HeadDeltaHintResult result;
    if (!useLastBranchTaken || !prev_branch_valid || addresses.empty()) {
        return result;
    }

    cmcStats.chooserLookups++;
    const ChooserKey chooser_key =
        makeChooserKey(pc, prev_branch_pc, prev_branch_taken);
    const auto chooser_it = headDeltaChooser.find(chooser_key);
    if (chooser_it == headDeltaChooser.end()) {
        return result;
    }

    cmcStats.chooserHits++;
    const auto &entry = chooser_it->second;
    if (entry.samples < chooserMinSamples ||
        !entry.valid[0] || entry.counts[0] < chooserMinConfidence) {
        return result;
    }
    if (entry.samples > 0 &&
        static_cast<uint64_t>(entry.counts[0]) * 100 <
            static_cast<uint64_t>(entry.samples) * chooserMinTopPct) {
        cmcStats.chooserLowPuritySkips++;
        return result;
    }

    cmcStats.chooserEligible++;
    const unsigned limit = std::max(1U, chooserTopK);
    for (unsigned candidate = 0;
         candidate < limit && candidate < HeadDeltaChooserEntry::MaxCandidates;
         ++candidate) {
        if (!entry.valid[candidate]) {
            continue;
        }

        const int64_t predicted_delta = entry.deltaBlocks[candidate];
        if (!result.hasPrediction) {
            result.hasPrediction = true;
            result.predictedDeltaBlocks = predicted_delta;
            result.chooserKey = chooser_key;
        }
        AddrPriority predicted_addr(0, 0);
        bool predicted_found = false;
        bool predicted_constructed = false;
        std::size_t predicted_idx = 0;
        for (std::size_t idx = 0; idx < addresses.size(); ++idx) {
            const int64_t candidate_delta =
                static_cast<int64_t>(blockIndex(addresses[idx].first)) -
                static_cast<int64_t>(block_addr);
            if (candidate_delta == predicted_delta) {
                predicted_addr = addresses[idx];
                predicted_found = true;
                predicted_idx = idx;
                break;
            }
        }

        if (predicted_found) {
            cmcStats.chooserPredictedFoundInStream++;
            if (predicted_idx == 0) {
                cmcStats.chooserPredictedAlreadyHead++;
            }
        } else {
            cmcStats.chooserPredictedNotInStream++;
        }

        if (!chooserModifyBaseline) {
            cmcStats.chooserNoAction++;
            return result;
        }

        const bool utility_construct_allowed =
            !chooserUseUtilityScore ||
            entry.utilityScores[candidate] >= chooserConstructMinScore;
        const bool utility_limit_allowed =
            !chooserUseUtilityScore ||
            entry.utilityScores[candidate] >= chooserThrottleMinScore;
        const bool tail_limit_allowed =
            !chooserAdaptiveLimit ||
            entry.limitScores[candidate] >= chooserTailThrottleMinScore;
        const bool use_adaptive_decision =
            chooserAdaptiveLimit && !chooserAdaptiveObservationOnly;
        const bool blacklist_allows_limit =
            !chooserAdaptiveBlacklist || tail_limit_allowed;
        const bool adaptive_limit_active =
            use_adaptive_decision && !chooserAdaptiveBlacklist &&
            utility_limit_allowed &&
            tail_limit_allowed;
        const bool hard_limit_active =
            chooserLimitOnHit &&
            ((!chooserAdaptiveLimit || chooserAdaptiveObservationOnly) ||
             (use_adaptive_decision && chooserAdaptiveBlacklist)) &&
            blacklist_allows_limit &&
            utility_limit_allowed;
        const bool limit_this_hint =
            hard_limit_active || adaptive_limit_active;

        const bool can_construct =
            chooserConstructPredicted &&
            (limit_this_hint || chooserSelectiveConstruct ||
             (chooserAdaptiveLimit && chooserUseUtilityScore &&
              utility_construct_allowed));
        if (!predicted_found && chooserConstructPredicted &&
            (chooserLimitOnHit || chooserAdaptiveLimit ||
             chooserSelectiveConstruct) &&
            !can_construct) {
            cmcStats.chooserUtilityConstructSkips++;
        }
        if (!predicted_found && can_construct) {
            const unsigned min_samples =
                chooserConstructMinSamples == 0 ?
                chooserMinSamples : chooserConstructMinSamples;
            const unsigned min_confidence =
                chooserConstructMinConfidence == 0 ?
                chooserMinConfidence : chooserConstructMinConfidence;
            if (entry.samples < min_samples ||
                entry.counts[candidate] < min_confidence ||
                (chooserConstructMinTopPct > 0 && entry.samples > 0 &&
                 static_cast<uint64_t>(entry.counts[candidate]) * 100 <
                    static_cast<uint64_t>(entry.samples) *
                    chooserConstructMinTopPct)) {
                cmcStats.chooserConstructThresholdSkips++;
                continue;
            }

            const int64_t predicted_block =
                static_cast<int64_t>(block_addr) + predicted_delta;
            if (predicted_block >= 0) {
                predicted_addr = AddrPriority(
                    static_cast<Addr>(predicted_block) << lBlkSize, 0);
                if (chooserConstructCacheFilter &&
                    (cache.inCache(predicted_addr.first, is_secure) ||
                     cache.inMissQueue(predicted_addr.first, is_secure))) {
                    cmcStats.chooserConstructCacheSkips++;
                    continue;
                }
                predicted_constructed = true;
            }
        }

        if (!predicted_found && !predicted_constructed) {
            continue;
        }

        if ((chooserLimitOnHit || chooserAdaptiveLimit) &&
            !utility_limit_allowed) {
            cmcStats.chooserUtilityLimitSkips++;
        }
        if (chooserAdaptiveLimit && utility_limit_allowed &&
            !tail_limit_allowed) {
            cmcStats.chooserTailLimitSkips++;
        }

        if (limit_this_hint) {
            std::vector<AddrPriority> limited;
            limited.reserve(1 + chooserBaselineFallbackDegree);
            limited.push_back(predicted_addr);

            if (!chooserOnlyPredicted) {
                unsigned fallback_count = 0;
                const Addr predicted_block =
                    blockIndex(predicted_addr.first);
                for (const auto &candidate_addr : addresses) {
                    if (fallback_count >= chooserBaselineFallbackDegree) {
                        break;
                    }
                    if (blockIndex(candidate_addr.first) == predicted_block) {
                        continue;
                    }
                    limited.push_back(candidate_addr);
                    fallback_count++;
                }
            }

            if (addresses.size() > limited.size()) {
                cmcStats.chooserDroppedCandidates +=
                    addresses.size() - limited.size();
            }
            addresses.swap(limited);
            cmcStats.chooserLimitedIssues++;
            if (adaptive_limit_active && !chooserLimitOnHit) {
                cmcStats.chooserAdaptiveLimitIssues++;
            }
            if (predicted_constructed) {
                switch (classifyCmcDelta(pc, predicted_delta)) {
                  case CmcDeltaStatus::Active:
                    cmcStats.chooserConstructedActiveCmcDelta++;
                    break;
                  case CmcDeltaStatus::Evicted:
                    cmcStats.chooserConstructedEvictedCmcDelta++;
                    break;
                  case CmcDeltaStatus::NeverSeen:
                    cmcStats.chooserConstructedNeverCmcDelta++;
                    break;
                }
                cmcStats.chooserConstructedHeads++;
            }
            if (predicted_found && predicted_idx != 0) {
                cmcStats.chooserHeadPromotions++;
                if (candidate == 1) {
                    cmcStats.chooserSecondChoicePromotions++;
                }
            }
            result.changed = true;
            return result;
        } else if (predicted_constructed) {
            addresses.insert(addresses.begin(), predicted_addr);
            switch (classifyCmcDelta(pc, predicted_delta)) {
              case CmcDeltaStatus::Active:
                cmcStats.chooserConstructedActiveCmcDelta++;
                break;
              case CmcDeltaStatus::Evicted:
                cmcStats.chooserConstructedEvictedCmcDelta++;
                break;
              case CmcDeltaStatus::NeverSeen:
                cmcStats.chooserConstructedNeverCmcDelta++;
                break;
            }
            cmcStats.chooserConstructedHeads++;
            cmcStats.chooserSelectiveConstructedHeads++;
            result.changed = true;
            return result;
        } else if (predicted_found && predicted_idx != 0) {
            std::swap(addresses[0], addresses[predicted_idx]);
            cmcStats.chooserHeadPromotions++;
            if (candidate == 1) {
                cmcStats.chooserSecondChoicePromotions++;
            }
            result.changed = true;
            return result;
        } else if (predicted_found && predicted_idx == 0) {
            cmcStats.chooserNoAction++;
            return result;
        }
    }

    cmcStats.chooserNoAction++;
    return result;
}

bool
CMCPrefetcher::issueAccessHeadPrediction(Addr block_addr, Addr pc,
                                         bool prev_branch_valid,
                                         Addr prev_branch_pc,
                                         bool prev_branch_taken,
                                         std::vector<AddrPriority> &addresses,
                                         const CacheAccessor &cache,
                                         bool is_secure)
{
    if (!prevBranchAccessPredictor || !useLastBranchTaken ||
        !prev_branch_valid) {
        return false;
    }

    cmcStats.accessHeadLookups++;
    const ChooserKey chooser_key =
        makeChooserKey(pc, prev_branch_pc, prev_branch_taken);
    auto chooser_it = headDeltaChooser.find(chooser_key);
    if (chooser_it == headDeltaChooser.end()) {
        return false;
    }

    const auto &entry = chooser_it->second;
    if (entry.samples < chooserMinSamples ||
        !entry.valid[0] || entry.counts[0] < chooserMinConfidence) {
        return false;
    }
    if (entry.samples > 0 &&
        static_cast<uint64_t>(entry.counts[0]) * 100 <
            static_cast<uint64_t>(entry.samples) * chooserMinTopPct) {
        return false;
    }

    cmcStats.accessHeadEligible++;
    const unsigned limit = std::max(1U, chooserTopK);
    for (unsigned candidate = 0;
         candidate < limit && candidate < HeadDeltaChooserEntry::MaxCandidates;
         ++candidate) {
        if (!entry.valid[candidate]) {
            continue;
        }

        if (entry.utilityScores[candidate] < prevBranchAccessMinScore) {
            cmcStats.accessHeadLowScoreSkips++;
            continue;
        }

        for (unsigned distance = 1; distance <= prevBranchAccessLookahead;
             ++distance) {
            const int64_t predicted_block =
                static_cast<int64_t>(block_addr) +
                entry.deltaBlocks[candidate] * static_cast<int64_t>(distance);
            if (predicted_block < 0) {
                break;
            }

            const Addr predicted_addr =
                static_cast<Addr>(predicted_block) << lBlkSize;
            if (prevBranchAccessCacheFilter &&
                (cache.inCache(predicted_addr, is_secure) ||
                 cache.inMissQueue(predicted_addr, is_secure))) {
                cmcStats.accessHeadCacheSkips++;
                continue;
            }

            const int64_t issued_delta_blocks =
                entry.deltaBlocks[candidate] * static_cast<int64_t>(distance);
            switch (classifyCmcDelta(pc, issued_delta_blocks)) {
              case CmcDeltaStatus::Active:
                cmcStats.accessHeadActiveCmcDelta++;
                break;
              case CmcDeltaStatus::Evicted:
                cmcStats.accessHeadEvictedCmcDelta++;
                break;
              case CmcDeltaStatus::NeverSeen:
                cmcStats.accessHeadNeverCmcDelta++;
                break;
            }
            addresses.push_back(AddrPriority(predicted_addr, 0));
            TrackedPrefetchSource source;
            source.source = CandidateSource::AccessHead;
            source.hasChooser = true;
            source.chooserKey = chooser_key;
            source.chooserDeltaBlocks = entry.deltaBlocks[candidate];
            notePrefetchSource(predicted_addr, source);
            cmcStats.accessHeadIssued++;
            if (distance > 1) {
                cmcStats.accessHeadLookaheadIssued++;
            }

            if (distance == 1) {
                PendingHeadPrediction pending;
                pending.hasChooserPrediction = true;
                pending.fromAccessPredictor = true;
                pending.chooserHeadDeltaBlocks = entry.deltaBlocks[candidate];
                pending.chooserKey = chooser_key;
                pendingHeadPredictions[pc] = pending;
            }
            return true;
        }
    }

    return false;
}

void
CMCPrefetcher::evaluatePendingHeadPrediction(Addr pc,
                                             int64_t actual_delta_blocks)
{
    const auto pending_it = pendingHeadPredictions.find(pc);
    if (pending_it == pendingHeadPredictions.end()) {
        return;
    }

    const PendingHeadPrediction pending = pending_it->second;
    pendingHeadPredictions.erase(pending_it);

    bool baseline_correct = false;
    if (pending.hasBaselineHead) {
        cmcStats.headPredictionFeedbacks++;
        baseline_correct =
            pending.baselineHeadDeltaBlocks == actual_delta_blocks;
        if (baseline_correct) {
            cmcStats.baselineHeadCorrect++;
        }
    }

    if (!pending.hasChooserPrediction) {
        return;
    }

    if (pending.fromAccessPredictor) {
        cmcStats.accessHeadFeedbacks++;
    }
    cmcStats.chooserHeadPredictions++;
    const bool chooser_correct =
        pending.chooserHeadDeltaBlocks == actual_delta_blocks;
    if (chooser_correct) {
        cmcStats.chooserHeadCorrect++;
    }

    if (pending.hasBaselineHead) {
        if (pending.chooserHeadDeltaBlocks == pending.baselineHeadDeltaBlocks) {
            cmcStats.chooserSameAsBaseline++;
        } else {
            cmcStats.chooserDiffersFromBaseline++;
        }

        if (chooser_correct && !baseline_correct) {
            cmcStats.chooserOnlyCorrect++;
        } else if (baseline_correct && !chooser_correct) {
            cmcStats.baselineOnlyCorrect++;
        }
    }

    auto chooser_it = headDeltaChooser.find(pending.chooserKey);
    if (chooser_it == headDeltaChooser.end()) {
        return;
    }

    auto &entry = chooser_it->second;
    for (unsigned i = 0; i < HeadDeltaChooserEntry::MaxCandidates; ++i) {
        if (!entry.valid[i] ||
            entry.deltaBlocks[i] != pending.chooserHeadDeltaBlocks) {
            continue;
        }

        if (chooser_correct && (!pending.hasBaselineHead || !baseline_correct)) {
            entry.utilityScores[i] = std::min(
                chooserUtilityMaxScore, entry.utilityScores[i] + 1);
            cmcStats.chooserUtilityPositiveUpdates++;
        } else if (!chooser_correct &&
                   (!pending.hasBaselineHead || baseline_correct)) {
            entry.utilityScores[i] = std::max(
                -chooserUtilityMaxScore, entry.utilityScores[i] - 1);
            cmcStats.chooserUtilityNegativeUpdates++;
        }

        if (!pending.baselineStreamDeltaBlocks.empty()) {
            bool kept_by_limit =
                actual_delta_blocks == pending.chooserHeadDeltaBlocks;
            bool only_in_dropped_tail = false;
            unsigned fallback_count = 0;

            for (const auto delta : pending.baselineStreamDeltaBlocks) {
                if (delta == pending.chooserHeadDeltaBlocks) {
                    continue;
                }

                if (fallback_count < pending.limitFallbackDegree) {
                    if (delta == actual_delta_blocks) {
                        kept_by_limit = true;
                    }
                    fallback_count++;
                } else if (delta == actual_delta_blocks) {
                    only_in_dropped_tail = true;
                }
            }

            if (only_in_dropped_tail && !kept_by_limit) {
                entry.limitScores[i] = std::max(
                    -chooserUtilityMaxScore, entry.limitScores[i] - 8);
                cmcStats.chooserLimitNegativeUpdates++;
            } else if (kept_by_limit) {
                entry.limitScores[i] = std::min(
                    chooserUtilityMaxScore, entry.limitScores[i] + 1);
                cmcStats.chooserLimitPositiveUpdates++;
            }
        }
        return;
    }
}

void
CMCPrefetcher::notePrefetchSource(Addr addr,
                                  const TrackedPrefetchSource &source)
{
    const Addr block_addr = blockIndex(addr);
    if (trackedPrefetchSources.size() >= MaxTrackedPrefetchSources) {
        trackedPrefetchSources.erase(trackedPrefetchSources.begin());
    }
    trackedPrefetchSources[block_addr] = source;

    switch (source.source) {
      case CandidateSource::BaselineHead:
        cmcStats.sourceTrackedBaselineHead++;
        break;
      case CandidateSource::BaselineFallback:
        cmcStats.sourceTrackedBaselineFallback++;
        break;
      case CandidateSource::BaselineTail:
        cmcStats.sourceTrackedBaselineTail++;
        break;
      case CandidateSource::ChooserHead:
        cmcStats.sourceTrackedChooserHead++;
        break;
      case CandidateSource::AccessHead:
        cmcStats.sourceTrackedAccessHead++;
        break;
    }
}

void
CMCPrefetcher::observePrefetchSourceFeedback(Addr block_addr)
{
    const auto source_it = trackedPrefetchSources.find(block_addr);
    if (source_it == trackedPrefetchSources.end()) {
        return;
    }

    const TrackedPrefetchSource source = source_it->second;
    trackedPrefetchSources.erase(source_it);
    cmcStats.sourceUsefulFeedbacks++;

    switch (source.source) {
      case CandidateSource::BaselineHead:
        cmcStats.sourceUsefulBaselineHead++;
        break;
      case CandidateSource::BaselineFallback:
        cmcStats.sourceUsefulBaselineFallback++;
        break;
      case CandidateSource::BaselineTail:
        cmcStats.sourceUsefulBaselineTail++;
        break;
      case CandidateSource::ChooserHead:
        cmcStats.sourceUsefulChooserHead++;
        break;
      case CandidateSource::AccessHead:
        cmcStats.sourceUsefulAccessHead++;
        break;
    }

    if (!source.hasChooser) {
        return;
    }

    auto chooser_it = headDeltaChooser.find(source.chooserKey);
    if (chooser_it == headDeltaChooser.end()) {
        return;
    }

    auto &entry = chooser_it->second;
    for (unsigned i = 0; i < HeadDeltaChooserEntry::MaxCandidates; ++i) {
        if (!entry.valid[i] ||
            entry.deltaBlocks[i] != source.chooserDeltaBlocks) {
            continue;
        }

        switch (source.source) {
          case CandidateSource::BaselineTail:
            // A useful tail candidate is direct evidence against limiting this
            // chooser stream too aggressively.
            entry.limitScores[i] = std::max(
                -chooserUtilityMaxScore, entry.limitScores[i] - 8);
            break;
          case CandidateSource::ChooserHead:
          case CandidateSource::AccessHead:
            // Useful branch-issued heads should become easier to construct or
            // issue on access-path opportunities.
            entry.utilityScores[i] = std::min(
                chooserUtilityMaxScore, entry.utilityScores[i] + 2);
            entry.limitScores[i] = std::min(
                chooserUtilityMaxScore, entry.limitScores[i] + 1);
            break;
          case CandidateSource::BaselineHead:
          case CandidateSource::BaselineFallback:
            // These candidates would survive conservative limiting.
            entry.limitScores[i] = std::min(
                chooserUtilityMaxScore, entry.limitScores[i] + 1);
            break;
        }
        return;
    }
}

void
CMCPrefetcher::recordIssuedPrefetchSources(
    Addr trigger_block_addr,
    const std::vector<AddrPriority> &issued,
    const std::vector<AddrPriority> &baseline_stream,
    const HeadDeltaHintResult &hint)
{
    if (issued.empty()) {
        return;
    }

    auto find_baseline_pos = [this, &baseline_stream](Addr block_addr) {
        for (std::size_t i = 0; i < baseline_stream.size(); ++i) {
            if (blockIndex(baseline_stream[i].first) == block_addr) {
                return i;
            }
        }
        return baseline_stream.size();
    };

    std::size_t predicted_pos = baseline_stream.size();
    if (hint.hasPrediction) {
        const int64_t predicted_block =
            static_cast<int64_t>(trigger_block_addr) +
            hint.predictedDeltaBlocks;
        if (predicted_block >= 0) {
            predicted_pos = find_baseline_pos(static_cast<Addr>(predicted_block));
        }
    }

    for (const auto &candidate : issued) {
        const Addr candidate_block = blockIndex(candidate.first);
        const std::size_t baseline_pos = find_baseline_pos(candidate_block);
        const int64_t candidate_delta =
            static_cast<int64_t>(candidate_block) -
            static_cast<int64_t>(trigger_block_addr);

        TrackedPrefetchSource source;
        source.hasChooser = hint.hasPrediction;
        source.chooserKey = hint.chooserKey;
        source.chooserDeltaBlocks = hint.predictedDeltaBlocks;

        if (hint.hasPrediction &&
            candidate_delta == hint.predictedDeltaBlocks &&
            (baseline_pos != 0 || predicted_pos == baseline_stream.size())) {
            source.source = CandidateSource::ChooserHead;
        } else if (baseline_pos == 0) {
            source.source = CandidateSource::BaselineHead;
        } else if (baseline_pos < baseline_stream.size() &&
                   baseline_pos <= chooserBaselineFallbackDegree) {
            source.source = CandidateSource::BaselineFallback;
        } else {
            source.source = CandidateSource::BaselineTail;
        }

        notePrefetchSource(candidate.first, source);
    }
}

void
CMCPrefetcher::updateHeadDeltaChooser(Addr pc, Addr prev_branch_pc,
                                      bool prev_branch_taken,
                                      int64_t head_delta_blocks)
{
    auto &entry = headDeltaChooser[
        makeChooserKey(pc, prev_branch_pc, prev_branch_taken)];
    entry.samples++;

    for (unsigned i = 0; i < HeadDeltaChooserEntry::MaxCandidates; ++i) {
        if (entry.valid[i] && entry.deltaBlocks[i] == head_delta_blocks) {
            entry.counts[i]++;
            if (i == 1 && entry.counts[1] > entry.counts[0]) {
                std::swap(entry.deltaBlocks[0], entry.deltaBlocks[1]);
                std::swap(entry.counts[0], entry.counts[1]);
                std::swap(entry.utilityScores[0], entry.utilityScores[1]);
                std::swap(entry.limitScores[0], entry.limitScores[1]);
                std::swap(entry.valid[0], entry.valid[1]);
            }
            return;
        }
    }

    for (unsigned i = 0; i < HeadDeltaChooserEntry::MaxCandidates; ++i) {
        if (!entry.valid[i]) {
            entry.valid[i] = true;
            entry.deltaBlocks[i] = head_delta_blocks;
            entry.counts[i] = 1;
            entry.utilityScores[i] = 0;
            entry.limitScores[i] = 0;
            return;
        }
    }

    if (entry.counts[1] > 0) {
        entry.counts[1]--;
    }
    if (entry.counts[1] == 0) {
        entry.deltaBlocks[1] = head_delta_blocks;
        entry.counts[1] = 1;
        entry.utilityScores[1] = 0;
        entry.limitScores[1] = 0;
        entry.valid[1] = true;
    }
}



void
CMCPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                 std::vector<AddrPriority> &addresses,
                                 const CacheAccessor &cache_accessor)
{
    
    
   //PC isolation here lowers coverage...
    Addr pc = pfi.hasPC() ? pfi.getPC() : 0;

    Addr addr = pfi.getAddr();
    Addr block_addr = blockIndex(addr); // takes off 6 Least Significant Bits for cache line
    bool is_secure = pfi.isSecure();
    if (pfi.wasPrefetched()) {
        observePrefetchSourceFeedback(block_addr);
    }
    const bool prev_load_branch_valid = pfi.hasPrevLoadBranchOutcome();
    const Addr prev_load_branch_pc =
        prev_load_branch_valid ? pfi.getPrevLoadBranchPC() : 0;
    const bool prev_load_branch_taken =
        prev_load_branch_valid ? pfi.getPrevLoadBranchTaken() : false;
    const bool baseline_event = pfi.isCacheMiss() || pfi.wasPrefetched();
    bool prev_load_branch_biased = false;
    bool prev_load_branch_usable = false;
    uint64_t baseline_key = hash(block_addr, pc);
    bool has_prev_pc = false;
    Addr delta_blocks = 0;
    int64_t actual_delta_blocks = 0;
    const auto prev_block_it = lastObservedBlockByPc.find(pc);
    if (prev_block_it != lastObservedBlockByPc.end()) {
        has_prev_pc = true;
        actual_delta_blocks =
            static_cast<int64_t>(block_addr) -
            static_cast<int64_t>(prev_block_it->second);
        delta_blocks = static_cast<Addr>(actual_delta_blocks);
        evaluatePendingHeadPrediction(pc, actual_delta_blocks);
    } else {
        pendingHeadPredictions.erase(pc);
    }
    lastObservedBlockByPc[pc] = block_addr;

    if (!baseline_event) {
        issueAccessHeadPrediction(block_addr, pc, prev_load_branch_valid,
                                  prev_load_branch_pc, prev_load_branch_taken,
                                  addresses, cache_accessor, is_secure);
        return;
    }

    cmcStats.totalLookups++;
    if (prev_load_branch_valid) {
        cmcStats.lookupsWithValidPrevBranch++;
        if (prev_load_branch_taken) {
            cmcStats.prevBranchTakenLookups++;
        } else {
            cmcStats.prevBranchNotTakenLookups++;
        }
        cmcStats.lookupAugmentedKeyDiffersFromBaseline++;
        prev_load_branch_biased = updatePrevBranchPcStats(
            prev_load_branch_pc, prev_load_branch_taken);
        prev_load_branch_usable =
            !(filterBiasedPrevBranches && prev_load_branch_biased);
        if (prev_load_branch_usable) {
            cmcStats.prevBranchChooserUsableLookups++;
        } else {
            cmcStats.prevBranchFilteredLookups++;
        }
    }
    baseline_key = makeStorageKey(block_addr, pc, prev_load_branch_usable,
                                  prev_load_branch_pc,
                                  prev_load_branch_taken);

    StorageEntry *baseline_entry = storage.findEntry(baseline_key, is_secure);
    if (baseline_entry) {
        cmcStats.baselineLookupHits++;
    }

    DPRINTF(HWPrefetch,
            "CMC train: pc: %lx, addr: %lx, prev_valid: %d, prev_branch_pc: "
            "%lx, prev_taken: %d\n",
            pc, block_addr, prev_load_branch_valid, prev_load_branch_pc,
            prev_load_branch_taken);

    // Prefetch: check if there is a match
    StorageEntry *match_entry = baseline_entry;
    dumpPrevBranchSample(pc, block_addr, has_prev_pc, delta_blocks,
                         prev_load_branch_valid, prev_load_branch_pc,
                         prev_load_branch_taken, prev_load_branch_biased,
                         prev_load_branch_usable, pfi.isCacheMiss(),
                         match_entry != nullptr);
    // prefetchStats.metadataAccesses++;
    if (match_entry) {
        cmcStats.augmentedLookupHits++;
        storage.accessEntry(match_entry);
        // prefetch on cache miss or on prefetch hit
        DPRINTF(HWPrefetch, "Storage hit, trigger pc: %lx, addr: %lx\n",
                pc, block_addr);
      //printf("=== Storage hit, trigger addr: %lx\n", block_addr);

        if (issueCmcStream) {
            for (auto addr: match_entry->addresses) {
                addresses.push_back(AddrPriority(addr, 0));
            }
        }
        const std::vector<AddrPriority> baseline_stream = addresses;
        if (prev_load_branch_valid && issueCmcStream) {
            cmcStats.prefetchCandidatesFromAugmentedKeys +=
                match_entry->addresses.size();
        }
        PendingHeadPrediction pending;
        if (!addresses.empty()) {
            pending.hasBaselineHead = true;
            pending.baselineHeadDeltaBlocks =
                static_cast<int64_t>(blockIndex(addresses.front().first)) -
                static_cast<int64_t>(block_addr);
            pending.limitFallbackDegree = chooserBaselineFallbackDegree;
            pending.baselineStreamDeltaBlocks.reserve(addresses.size());
            for (const auto &candidate_addr : addresses) {
                pending.baselineStreamDeltaBlocks.push_back(
                    static_cast<int64_t>(blockIndex(candidate_addr.first)) -
                    static_cast<int64_t>(block_addr));
            }
        }
        HeadDeltaHintResult hint;
        if (issueCmcStream || chooserModifyBaseline) {
            hint = applyHeadDeltaHint(
                block_addr, pc, prev_load_branch_usable,
                prev_load_branch_pc, prev_load_branch_taken,
                addresses, cache_accessor, is_secure);
        }
        if (hint.hasPrediction) {
            pending.hasChooserPrediction = true;
            pending.chooserHeadDeltaBlocks = hint.predictedDeltaBlocks;
            pending.chooserKey = hint.chooserKey;
        }
        if (pending.hasBaselineHead) {
            pendingHeadPredictions[pc] = pending;
        } else {
            pendingHeadPredictions.erase(pc);
        }
        recordIssuedPrefetchSources(block_addr, addresses, baseline_stream, hint);
    } else {
        pendingHeadPredictions.erase(pc);
    }

    // Train: update temporal access chain
    bool finished = false;

	
    /* 1. Train trigger */
    //This will try to reuse an existing trigger that has been seen recently, should one exist.
    //Otherwise, it will find a new trigger.
    //Intuition is that if you didn't aggressively reuse triggers, you'd pollute your cache with an entry per address, which adds massive redundancy,
    //evicts other useful metadata, and will take ages to replace old, potentially wrong entries.
    bool train_trigger =
        (trigger.size()<1 || match_entry) && trigger.size()<STACK_SIZE;
    bool do_training =
        !trigger.empty(); //This is fixed: we should still train the Markov table on a newly seen trigger, as long as there is another trigger in the list already
        //Really, we should remove trigger[1] and add a new trigger[3] when we have match_entry, to prioritise newer match_entries rather than attaching to really old ones.
    if (train_trigger) {
        //printf("train_trigger index: %d, addr: %lx\n",
        //        trigger.size(), block_addr);
        assert(trigger.size()<STACK_SIZE);

        trigger.push_back(RecordEntry(
            pc, block_addr, is_secure,
            prev_load_branch_valid, prev_load_branch_usable,
            prev_load_branch_pc,
            prev_load_branch_taken));
    }

    /* 2. Train entry */
    if (do_training) {
        bool trained = recorder->train_entry(
            addr, is_secure, &finished, pc, prev_load_branch_valid,
            prev_load_branch_usable, prev_load_branch_pc,
            prev_load_branch_taken);
        auto &trigger_head = trigger.front();
        if (trained) {
            //printf("trained %x\n", block_addr);
        }
        if (finished) {
			//finished gets set once a coalesced set of 16 targets get stored in the training entry.
            //printf("trigger train finished, pc: %lx, addr: %lx\n",
               //     trigger_head.pc, trigger_head.addr);
            const uint64_t baseline_train_key = makeStorageKey(
                trigger_head.addr, trigger_head.pc,
                trigger_head.use_prev_branch_for_chooser,
                trigger_head.prev_branch_pc,
                trigger_head.last_branch_taken);

            cmcStats.trainCompletions++;
            if (trigger_head.has_prev_load_branch) {
                cmcStats.trainsWithValidPrevBranch++;
                if (trigger_head.last_branch_taken) {
                    cmcStats.prevBranchTakenTrains++;
                } else {
                    cmcStats.prevBranchNotTakenTrains++;
                }
                cmcStats.trainAugmentedKeyDiffersFromBaseline++;
            }

            if (storage.findEntry(baseline_train_key,
                                  trigger_head.is_secure)) {
                cmcStats.baselineTrainHits++;
            }

            if (remapStreams) {
                trainRemapEntries(trigger_head);
            } else {
                // storage.accessEntry(entry); do not update replacement
                DPRINTF(HWPrefetch,
                        "CMC: enter the same trigger, pc: %lx, addr: %lx\n",
                        trigger_head.pc, trigger_head.addr);
                writeStorageEntry(
                    baseline_train_key, trigger_head.pc, trigger_head.addr,
                    trigger_head.is_secure, recorder->entries, true);
            }

            for (auto addr: recorder->entries) {
                DPRINTF(HWPrefetch, "entry addr: 0x%lx\n",
                        addr);
            }
            if (useLastBranchTaken &&
                trigger_head.use_prev_branch_for_chooser &&
                !recorder->entries.empty()) {
                const int64_t head_delta_blocks =
                    static_cast<int64_t>(blockIndex(recorder->entries.front())) -
                    static_cast<int64_t>(trigger_head.addr);
                updateHeadDeltaChooser(
                    trigger_head.pc, trigger_head.prev_branch_pc,
                    trigger_head.last_branch_taken,
                    head_delta_blocks);
                cmcStats.chooserTrainUpdates++;
            }
            // prefetchStats.metadataAccesses++;
            trigger.pop_front();
            
			if (trigger.size()<1) {
				//This is fixed: if we don't have this here, we end up with triggers and trainings as distinct,
				//meaning we never reach 100% coverage.
				//This makes the trigger the last addr of the previous group, provided no other trigger already
				// is prepared, because it was a recent reused addr.
					trigger.push_back(RecordEntry(
	                    pc, block_addr, is_secure,
	                    prev_load_branch_valid, prev_load_branch_usable,
	                    prev_load_branch_pc,
	                    prev_load_branch_taken));
				}
            recorder->reset();

        }
    }

}

Addr cut_offset(Addr addr, int offset)
{
    return (addr >> offset) << offset;
}

bool
CMCPrefetcher::Recorder::train_entry(
    Addr addr,
    bool is_secure,
    bool *finished,
    Addr pc,
    bool has_prev_load_branch,
    bool use_prev_branch_for_chooser,
    Addr prev_branch_pc,
    bool last_branch_taken
) {

        entries.push_back(addr);
        accesses.push_back({
            pc,
            addr,
            is_secure,
            has_prev_load_branch,
            use_prev_branch_for_chooser,
            prev_branch_pc,
            last_branch_taken
        });
        index++;
        //There was an off-by-one error here: it stored 17 entries

    if (index >= nr_entry) {
        // entry full
        *finished = true;
    }
    
    return true;
}



void
CMCPrefetcher::Recorder::reset() {
    index = 0;
    entries.clear();
    accesses.clear();
}

// void
// CMCPrefetcher::StorageEntry::invalidate() {
//     if (false) {
//         if (this->isValid()) {
//             printf("entry victim: refcnt = %d\n", this->refcnt);
//         }
//     }
//     TaggedEntry::invalidate();
// }

void
CMCPrefetcher::StorageEntry::insert(Addr tag, bool is_secure)
{
    setValid();
    setTag(tag);

    if (is_secure) {
        setSecure();
    } else {
        clearSecure();
    }
}

bool
CMCPrefetcher::StorageEntry::match(Addr tag, bool is_secure) const
{
    return isValid() && (getTag() == tag) && (isSecure() == is_secure);
}

void
CMCPrefetcher::StorageEntry::invalidate()
{
    TaggedEntry::invalidate();
    addresses.clear();
    triggerPc = 0;
    triggerAddr = 0;
}



}  // prefetch
}  // gem5
