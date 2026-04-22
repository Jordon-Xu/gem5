
//Adapted from https://github.com/OpenXiangShan/GEM5/blob/xs-dev/src/mem/cache/prefetch
#include "mem/cache/prefetch/cmc.hh"

#include <cstdint>

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/CMCPrefetcher.hh"

namespace gem5
{
namespace prefetch
{
//int64_t CMCPrefetcher::global_timestamp=0;
//int CMCPrefetcher::target_ways=0;
//int CMCPrefetcher::current_ways=0;

//CMCPrefetcher::SizeDuel* CMCPrefetcher::sizeDuelPtr=nullptr;

//std::vector<uint32_t> CMCPrefetcher::setPrefetch(17,0);

CMCPrefetcher::CMCPrefetcher(const CMCPrefetcherParams &p)
: Queued(p),
    cachetags(p.cachetags),
    recorder(new Recorder(p.degree)),
    storage(p.storage_assoc, p.storage_entries, p.storage_indexing_policy,
            p.storage_replacement_policy,
            StorageEntry(std::max(1u, p.ctx_variants))),
    deltaPredictor(p.delta_ctx_pred_assoc, p.delta_ctx_pred_entries,
            p.delta_ctx_pred_indexing_policy,
            p.delta_ctx_pred_replacement_policy, DeltaPredictionEntry()),
    baseDegree(p.degree),
    analysisDumpFileName(p.analysis_dump_file),
    analysisDumpLimit(p.analysis_dump_limit),
    currentBranchCtx(0),
    ctxEnable(p.ctx_enable),
    ctxTakenOnly(p.ctx_taken_only),
    ctxUseExecuteBranches(p.ctx_use_execute_branches),
    ctxUseRequestSnapshot(p.ctx_use_request_snapshot),
    ctxUseLoadPcSnapshot(p.ctx_use_load_pc_snapshot),
    ctxLoadPcMinBranches(p.ctx_load_pc_min_branches),
    ctxLoadPcMultiVariantOnly(p.ctx_load_pc_multivariant_only),
    deltaCtxPredEnable(p.delta_ctx_pred_enable),
    deltaCtxPredTopK(std::min(
        CMCPrefetcher::DeltaPredictionEntry::MaxCandidates,
        std::max(1u, p.delta_ctx_pred_topk))),
    ctxShift(p.ctx_shift),
    ctxBits(p.ctx_bits),
    ctxWindowSize(p.ctx_window_size),
    retiredBranchCount(0),
    ctxUpdatePeriod(p.ctx_update_period),
    ctxMismatchDegree(p.ctx_mismatch_degree),
    statsCMC(this),
    trigger()
{
    trigger.clear();

    if (!analysisDumpFileName.empty()) {
        analysisDump = simout.create(analysisDumpFileName, false, true);
        fatal_if(!analysisDump,
            "unable to open CMC analysis dump file %s",
            analysisDumpFileName);
        *analysisDump->stream()
            << "sample_idx,pc,block_addr,has_prev_pc,delta_blocks,"
            << "selected_ctx,selected_ctx_tag,"
            << "global_ctx,global_ctx_tag,"
            << "global_selected_branches,global_contributing_branches,"
            << "has_request_snapshot,"
            << "load_pc_ctx,load_pc_ctx_tag,"
            << "load_pc_selected_branches,load_pc_contributing_branches,"
            << "has_load_pc_snapshot,"
            << "used_load_pc_snapshot,load_pc_fallback,"
            << "load_pc_insufficient,load_pc_single_variant_skip,"
            << "load_pc_no_better_match_skip,cache_miss,match_entry,"
            << "valid_variants\n";
    }
}

CMCPrefetcher::~CMCPrefetcher()
{
    if (analysisDump) {
        simout.close(analysisDump);
    }
}

CMCPrefetcher::CMCStats::CMCStats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(primaryHits, statistics::units::Count::get(),
             "number of baseline-key hits in CMC storage"),
    ADD_STAT(ctxMatches, statistics::units::Count::get(),
             "number of baseline-key hits whose context tags matched"),
    ADD_STAT(ctxMismatches, statistics::units::Count::get(),
             "number of baseline-key hits whose context tags mismatched"),
    ADD_STAT(pfCandidatesFromMatch, statistics::units::Count::get(),
             "prefetch candidates issued from context matches"),
    ADD_STAT(pfCandidatesFromMismatch, statistics::units::Count::get(),
             "prefetch candidates issued from context mismatches"),
    ADD_STAT(ctxVariantAllocations, statistics::units::Count::get(),
             "number of context-stream slots allocated within a primary key"),
    ADD_STAT(ctxVariantReplacements, statistics::units::Count::get(),
             "number of context-stream slots replaced within a primary key"),
    ADD_STAT(ctxSingleVariantMismatches, statistics::units::Count::get(),
             "number of context mismatches on primary keys with one stream"),
    ADD_STAT(ctxMultiVariantMismatches, statistics::units::Count::get(),
             "number of context mismatches on primary keys with multiple "
             "streams"),
    ADD_STAT(ctxRequestSnapshots, statistics::units::Count::get(),
             "number of accesses that used request-carried context"),
    ADD_STAT(ctxLoadPcSnapshots, statistics::units::Count::get(),
             "number of accesses that used same-load-PC branch windows"),
    ADD_STAT(ctxLoadPcFallbacks, statistics::units::Count::get(),
             "number of accesses whose same-load-PC windows fell back to the "
             "global request snapshot"),
    ADD_STAT(ctxLoadPcInsufficientBranches, statistics::units::Count::get(),
             "number of accesses whose same-load-PC windows had too little "
             "selected branch signal"),
    ADD_STAT(ctxLoadPcSingleVariantSkips, statistics::units::Count::get(),
             "number of accesses whose same-load-PC windows were skipped "
             "because the primary key did not yet show multiple variants"),
    ADD_STAT(ctxLoadPcNoBetterMatchSkips, statistics::units::Count::get(),
             "number of accesses whose same-load-PC windows did not improve "
             "variant selection over the global request snapshot"),
    ADD_STAT(ctxLoadPcDisambiguations, statistics::units::Count::get(),
             "number of accesses whose same-load-PC windows disambiguated a "
             "multi-variant primary key better than the global request "
             "snapshot"),
    ADD_STAT(ctxGlobalFallbacks, statistics::units::Count::get(),
             "number of accesses that fell back to global branch context"),
    ADD_STAT(deltaCtxPredLookups, statistics::units::Count::get(),
             "number of raw-context delta predictor lookups"),
    ADD_STAT(deltaCtxPredHits, statistics::units::Count::get(),
             "number of raw-context delta predictor hits"),
    ADD_STAT(deltaCtxPredTrainUpdates, statistics::units::Count::get(),
             "number of raw-context delta predictor training updates"),
    ADD_STAT(deltaCtxPredTrainOverrides, statistics::units::Count::get(),
             "number of raw-context delta predictor entry overrides"),
    ADD_STAT(deltaCtxPredVariantHints, statistics::units::Count::get(),
             "number of stream selections guided by the delta predictor"),
    ADD_STAT(deltaCtxPredHeadPromotions, statistics::units::Count::get(),
             "number of head candidates promoted by the delta predictor")
{
}

void
CMCPrefetcher::DeltaPredictionEntry::insert(Addr tag, bool is_secure)
{
    setValid();
    setTag(tag);
    for (auto &candidate : candidates) {
        candidate.invalidate();
    }

    if (is_secure) {
        setSecure();
    } else {
        clearSecure();
    }
}

bool
CMCPrefetcher::DeltaPredictionEntry::match(Addr tag, bool is_secure) const
{
    return isValid() && (getTag() == tag) && (isSecure() == is_secure);
}

void
CMCPrefetcher::DeltaPredictionEntry::invalidate()
{
    TaggedEntry::invalidate();
    for (auto &candidate : candidates) {
        candidate.invalidate();
    }
}

void
CMCPrefetcher::updateBranchCtx(Addr branch_pc)
{
    if (!ctxEnable) {
        return;
    }

    if (ctxWindowSize == 0) {
        currentBranchCtx = mixBranchCtx(currentBranchCtx, branch_pc);
        return;
    }

    recentBranchPCs.push_back(branch_pc);
    while (recentBranchPCs.size() > ctxWindowSize) {
        recentBranchPCs.pop_front();
    }

    uint64_t new_ctx = 0;
    for (auto recent_pc : recentBranchPCs) {
        new_ctx = mixBranchCtx(new_ctx, recent_pc);
    }
    currentBranchCtx = new_ctx;
}

CMCPrefetcher::SnapshotCtxResult
CMCPrefetcher::buildCtxFromSnapshot(const BranchContextSnapshot &snapshot,
                                    uint64_t total_branches,
                                    uint64_t total_taken_branches) const
{
    SnapshotCtxResult result;
    const uint64_t total_selected = ctxTakenOnly ?
        total_taken_branches :
        total_branches;
    const unsigned update_period = std::max(1u, ctxUpdatePeriod);

    std::vector<Addr> selected_branches;
    selected_branches.reserve(snapshot.size());

    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto &entry = snapshot[i];
        if (ctxTakenOnly && !entry.taken) {
            continue;
        }
        selected_branches.push_back(entry.pc);
    }
    result.selectedBranches = selected_branches.size();

    if (selected_branches.empty()) {
        return result;
    }

    const uint64_t visible_selected = selected_branches.size();
    const uint64_t base_ordinal = total_selected > visible_selected ?
        total_selected - visible_selected : 0;
    std::vector<Addr> contributing_branches;
    contributing_branches.reserve(selected_branches.size());

    for (size_t i = 0; i < selected_branches.size(); ++i) {
        const uint64_t ordinal = base_ordinal + i + 1;
        if ((ordinal % update_period) == 0) {
            contributing_branches.push_back(selected_branches[i]);
        }
    }
    result.contributingBranches = contributing_branches.size();

    size_t start = 0;
    if (ctxWindowSize != 0 && contributing_branches.size() > ctxWindowSize) {
        start = contributing_branches.size() - ctxWindowSize;
    }

    for (size_t i = start; i < contributing_branches.size(); ++i) {
        result.ctx = mixBranchCtx(result.ctx, contributing_branches[i]);
    }

    return result;
}

CMCPrefetcher::AccessCtxSelection
CMCPrefetcher::selectAccessBranchCtx(const PrefetchInfo &pfi,
                                     const StorageEntry *match_entry) const
{
    AccessCtxSelection selection;

    if (!ctxEnable) {
        return selection;
    }

    if (!ctxUseRequestSnapshot || !pfi.hasBranchContextSnapshot()) {
        selection.ctx = getCurrentBranchCtx();
        return selection;
    }

    selection.usedRequestSnapshot = true;
    selection.ctx = buildCtxFromSnapshot(
        pfi.getBranchContextSnapshot(),
        pfi.getBranchContextTotalBranches(),
        pfi.getBranchContextTotalTakenBranches()).ctx;

    if (!ctxUseLoadPcSnapshot || !pfi.hasLoadPcBranchContextSnapshot()) {
        return selection;
    }

    const auto load_pc_result = buildCtxFromSnapshot(
            pfi.getLoadPcBranchContextSnapshot(),
            pfi.getLoadPcBranchContextTotalBranches(),
            pfi.getLoadPcBranchContextTotalTakenBranches());
    if (!match_entry) {
        selection.loadPcFallback = true;
        selection.loadPcSingleVariantSkip = true;
        return selection;
    }

    const bool multi_variant_match =
        match_entry && validVariantCount(match_entry) > 1;
    const bool variant_allowed =
        !ctxLoadPcMultiVariantOnly || multi_variant_match;

    if (load_pc_result.contributingBranches < ctxLoadPcMinBranches) {
        selection.loadPcFallback = true;
        selection.loadPcInsufficient = true;
        return selection;
    }

    if (!variant_allowed) {
        selection.loadPcFallback = true;
        selection.loadPcSingleVariantSkip = true;
        return selection;
    }

    const uint16_t global_ctx_tag = compressCtx(selection.ctx);
    const uint16_t load_pc_ctx_tag = compressCtx(load_pc_result.ctx);
    const bool global_exact = hasContextStream(match_entry, global_ctx_tag);
    const bool load_pc_exact = hasContextStream(match_entry, load_pc_ctx_tag);

    if (!(load_pc_exact && !global_exact)) {
        selection.loadPcFallback = true;
        selection.loadPcNoBetterMatchSkip = true;
        return selection;
    }

    selection.ctx = load_pc_result.ctx;
    selection.usedLoadPcSnapshot = true;
    return selection;
}

void
CMCPrefetcher::maybeDumpAnalysisSample(const PrefetchInfo &pfi, Addr pc,
                                       Addr block_addr, uint64_t selected_ctx,
                                       const StorageEntry *match_entry,
                                       const AccessCtxSelection &ctx_selection)
{
    if (!analysisDump) {
        return;
    }
    if (analysisDumpLimit != 0 && analysisSampleCount >= analysisDumpLimit) {
        return;
    }

    SnapshotCtxResult global_result;
    SnapshotCtxResult load_pc_result;
    const bool has_request_snapshot = pfi.hasBranchContextSnapshot();
    const bool has_load_pc_snapshot = pfi.hasLoadPcBranchContextSnapshot();

    if (has_request_snapshot) {
        global_result = buildCtxFromSnapshot(
            pfi.getBranchContextSnapshot(),
            pfi.getBranchContextTotalBranches(),
            pfi.getBranchContextTotalTakenBranches());
    }

    if (has_load_pc_snapshot) {
        load_pc_result = buildCtxFromSnapshot(
            pfi.getLoadPcBranchContextSnapshot(),
            pfi.getLoadPcBranchContextTotalBranches(),
            pfi.getLoadPcBranchContextTotalTakenBranches());
    }

    int has_prev_pc = 0;
    int64_t delta_blocks = 0;
    auto prev_it = lastBlockAddrByPc.find(pc);
    if (prev_it != lastBlockAddrByPc.end()) {
        has_prev_pc = 1;
        delta_blocks = static_cast<int64_t>(block_addr) -
            static_cast<int64_t>(prev_it->second);
    }
    lastBlockAddrByPc[pc] = block_addr;

    const unsigned valid_variants =
        match_entry ? validVariantCount(match_entry) : 0;

    *analysisDump->stream()
        << analysisSampleCount << ','
        << pc << ','
        << block_addr << ','
        << has_prev_pc << ','
        << delta_blocks << ','
        << selected_ctx << ','
        << compressCtx(selected_ctx) << ','
        << global_result.ctx << ','
        << compressCtx(global_result.ctx) << ','
        << global_result.selectedBranches << ','
        << global_result.contributingBranches << ','
        << (has_request_snapshot ? 1 : 0) << ','
        << load_pc_result.ctx << ','
        << compressCtx(load_pc_result.ctx) << ','
        << load_pc_result.selectedBranches << ','
        << load_pc_result.contributingBranches << ','
        << (has_load_pc_snapshot ? 1 : 0) << ','
        << (ctx_selection.usedLoadPcSnapshot ? 1 : 0) << ','
        << (ctx_selection.loadPcFallback ? 1 : 0) << ','
        << (ctx_selection.loadPcInsufficient ? 1 : 0) << ','
        << (ctx_selection.loadPcSingleVariantSkip ? 1 : 0) << ','
        << (ctx_selection.loadPcNoBetterMatchSkip ? 1 : 0) << ','
        << (pfi.isCacheMiss() ? 1 : 0) << ','
        << (match_entry ? 1 : 0) << ','
        << valid_variants << '\n';
    analysisSampleCount++;
}

void
CMCPrefetcher::notifyRetiredBranch(Addr branch_pc)
{
    if (!ctxEnable || ctxTakenOnly || ctxUseExecuteBranches) {
        return;
    }

    retiredBranchCount++;
    if ((retiredBranchCount % ctxUpdatePeriod) != 0) {
        return;
    }

    updateBranchCtx(branch_pc);
    DPRINTF(HWPrefetch, "CMC retired branch pc=%lx new_ctx=%lx\n",
            branch_pc, currentBranchCtx);
}

void
CMCPrefetcher::notifyRetiredTakenBranch(Addr branch_pc)
{
    if (!ctxEnable || !ctxTakenOnly || ctxUseExecuteBranches) {
        return;
    }

    retiredBranchCount++;
    if ((retiredBranchCount % ctxUpdatePeriod) != 0) {
        return;
    }

    updateBranchCtx(branch_pc);
    DPRINTF(HWPrefetch, "CMC retired taken branch pc=%lx new_ctx=%lx\n",
            branch_pc, currentBranchCtx);
}

void
CMCPrefetcher::notifyExecutedBranch(Addr branch_pc)
{
    if (!ctxEnable || ctxTakenOnly || !ctxUseExecuteBranches) {
        return;
    }

    retiredBranchCount++;
    if ((retiredBranchCount % ctxUpdatePeriod) != 0) {
        return;
    }

    updateBranchCtx(branch_pc);
    DPRINTF(HWPrefetch, "CMC executed branch pc=%lx new_ctx=%lx\n",
            branch_pc, currentBranchCtx);
}

void
CMCPrefetcher::notifyExecutedTakenBranch(Addr branch_pc)
{
    if (!ctxEnable || !ctxTakenOnly || !ctxUseExecuteBranches) {
        return;
    }

    retiredBranchCount++;
    if ((retiredBranchCount % ctxUpdatePeriod) != 0) {
        return;
    }

    updateBranchCtx(branch_pc);
    DPRINTF(HWPrefetch, "CMC executed taken branch pc=%lx new_ctx=%lx\n",
            branch_pc, currentBranchCtx);
}

CMCPrefetcher::ContextStream *
CMCPrefetcher::findContextStream(StorageEntry *entry, uint16_t ctx_tag) const
{
    for (auto &variant : entry->variants) {
        if (variant.valid && variant.ctxTag == ctx_tag) {
            return &variant;
        }
    }

    return nullptr;
}

CMCPrefetcher::ContextStream *
CMCPrefetcher::selectIssueStream(StorageEntry *entry, uint16_t ctx_tag,
                                 bool &ctx_match) const
{
    if (auto *exact = findContextStream(entry, ctx_tag)) {
        ctx_match = true;
        return exact;
    }

    ctx_match = false;
    ContextStream *best = nullptr;
    for (auto &variant : entry->variants) {
        if (!variant.valid) {
            continue;
        }

        if (!best || variant.confidence > best->confidence ||
            (variant.confidence == best->confidence &&
             variant.lastTouch > best->lastTouch)) {
            best = &variant;
        }
    }

    return best;
}

CMCPrefetcher::ContextStream *
CMCPrefetcher::selectTrainingStream(StorageEntry *entry, uint16_t ctx_tag,
                                    bool &allocated, bool &replaced)
{
    allocated = false;
    replaced = false;

    if (auto *exact = findContextStream(entry, ctx_tag)) {
        return exact;
    }

    for (auto &variant : entry->variants) {
        if (!variant.valid) {
            allocated = true;
            return &variant;
        }
    }

    allocated = true;
    replaced = true;
    ContextStream *victim = &entry->variants.front();
    for (auto &variant : entry->variants) {
        if (variant.confidence < victim->confidence ||
            (variant.confidence == victim->confidence &&
             variant.lastTouch < victim->lastTouch)) {
            victim = &variant;
        }
    }

    return victim;
}

void
CMCPrefetcher::touchContextStream(ContextStream &stream, bool reinforce)
{
    stream.lastTouch = ++ctxVariantClock;
    if (reinforce && stream.confidence < std::numeric_limits<uint8_t>::max()) {
        stream.confidence++;
    }
}

void
CMCPrefetcher::touchDeltaPrediction(
    DeltaPredictionEntry::DeltaCandidate &candidate, bool reinforce)
{
    candidate.lastTouch = ++deltaPredClock;
    if (reinforce &&
        candidate.confidence < std::numeric_limits<uint8_t>::max()) {
        candidate.confidence++;
    }
}

uint64_t
CMCPrefetcher::deltaPredKey(Addr pc, uint64_t ctx) const
{
    const uint64_t pc_part = static_cast<uint64_t>(pc) * 11400714819323198485ull;
    return ctx ^ pc_part ^ (pc_part >> 23);
}

CMCPrefetcher::DeltaPredictionEntry *
CMCPrefetcher::findDeltaPrediction(Addr pc, uint64_t ctx, bool is_secure)
{
    return deltaPredictor.findEntry(deltaPredKey(pc, ctx), is_secure);
}

CMCPrefetcher::DeltaPredictionEntry *
CMCPrefetcher::findOrAllocateDeltaPrediction(Addr pc, uint64_t ctx,
                                             bool is_secure)
{
    const auto key = deltaPredKey(pc, ctx);
    if (auto *entry = deltaPredictor.findEntry(key, is_secure)) {
        return entry;
    }

    auto *entry = deltaPredictor.findVictim(key);
    deltaPredictor.insertEntry(key, is_secure, entry);
    return entry;
}

std::vector<int64_t>
CMCPrefetcher::collectPredictedDeltas(const DeltaPredictionEntry &entry) const
{
    std::vector<const DeltaPredictionEntry::DeltaCandidate *> ranked;
    ranked.reserve(entry.candidates.size());

    for (const auto &candidate : entry.candidates) {
        if (candidate.valid && candidate.confidence > 0) {
            ranked.push_back(&candidate);
        }
    }

    std::sort(ranked.begin(), ranked.end(),
        [](const auto *lhs, const auto *rhs) {
            if (lhs->confidence != rhs->confidence) {
                return lhs->confidence > rhs->confidence;
            }
            return lhs->lastTouch > rhs->lastTouch;
        });

    std::vector<int64_t> deltas;
    const size_t keep = std::min<size_t>(deltaCtxPredTopK, ranked.size());
    deltas.reserve(keep);
    for (size_t i = 0; i < keep; ++i) {
        deltas.push_back(ranked[i]->deltaBlocks);
    }

    return deltas;
}

void
CMCPrefetcher::trainDeltaPredictor(Addr pc,
    const DeltaPredictionState &prev_state, Addr current_block_addr)
{
    if (!deltaCtxPredEnable || !prev_state.valid) {
        return;
    }

    const int64_t observed_delta =
        static_cast<int64_t>(current_block_addr) -
        static_cast<int64_t>(prev_state.blockAddr);
    auto *entry = findOrAllocateDeltaPrediction(
        pc, prev_state.ctx, prev_state.isSecure);
    statsCMC.deltaCtxPredTrainUpdates++;

    for (auto &candidate : entry->candidates) {
        if (candidate.valid && candidate.deltaBlocks == observed_delta) {
            touchDeltaPrediction(candidate, true);
            return;
        }
    }

    for (auto &candidate : entry->candidates) {
        if (!candidate.valid) {
            candidate.valid = true;
            candidate.deltaBlocks = observed_delta;
            candidate.confidence = 0;
            candidate.lastTouch = 0;
            touchDeltaPrediction(candidate, true);
            return;
        }
    }

    auto *victim = &entry->candidates.front();
    for (auto &candidate : entry->candidates) {
        if (candidate.confidence < victim->confidence ||
            (candidate.confidence == victim->confidence &&
             candidate.lastTouch < victim->lastTouch)) {
            victim = &candidate;
        }
    }

    if (victim->confidence > 1) {
        victim->confidence--;
        touchDeltaPrediction(*victim, false);
        return;
    }

    victim->valid = true;
    victim->deltaBlocks = observed_delta;
    victim->confidence = 0;
    victim->lastTouch = 0;
    touchDeltaPrediction(*victim, true);
    statsCMC.deltaCtxPredTrainOverrides++;
}

CMCPrefetcher::ContextStream *
CMCPrefetcher::selectStreamByPredictedDelta(StorageEntry *entry,
    Addr current_block_addr, const std::vector<int64_t> &predicted_deltas) const
{
    if (predicted_deltas.empty()) {
        return nullptr;
    }

    ContextStream *best = nullptr;
    size_t best_rank = std::numeric_limits<size_t>::max();
    size_t best_match_pos = std::numeric_limits<size_t>::max();

    for (auto &variant : entry->variants) {
        if (!variant.valid) {
            continue;
        }

        size_t variant_best_rank = std::numeric_limits<size_t>::max();
        size_t variant_best_pos = std::numeric_limits<size_t>::max();
        size_t match_pos = 0;
        bool matched = false;
        for (const auto pf_addr : variant.addresses) {
            const int64_t candidate_delta =
                static_cast<int64_t>(blockIndex(pf_addr)) -
                static_cast<int64_t>(current_block_addr);
            for (size_t rank = 0; rank < predicted_deltas.size(); ++rank) {
                if (candidate_delta == predicted_deltas[rank]) {
                    matched = true;
                    if (rank < variant_best_rank ||
                        (rank == variant_best_rank &&
                         match_pos < variant_best_pos)) {
                        variant_best_rank = rank;
                        variant_best_pos = match_pos;
                    }
                }
            }
            match_pos++;
        }

        if (!matched) {
            continue;
        }

        if (!best || variant_best_rank < best_rank ||
            (variant_best_rank == best_rank &&
             variant_best_pos < best_match_pos) ||
            (variant_best_rank == best_rank &&
             variant_best_pos == best_match_pos &&
             variant.confidence > best->confidence) ||
            (variant_best_rank == best_rank &&
             variant_best_pos == best_match_pos &&
             variant.confidence == best->confidence &&
             variant.lastTouch > best->lastTouch)) {
            best = &variant;
            best_rank = variant_best_rank;
            best_match_pos = variant_best_pos;
        }
    }

    return best;
}

unsigned
CMCPrefetcher::issueStreamWithPredictedDelta(ContextStream &stream,
    Addr current_block_addr, const std::vector<int64_t> &predicted_deltas,
    unsigned issue_deg,
    std::vector<AddrPriority> &addresses)
{
    if (issue_deg == 0) {
        return 0;
    }

    auto promote_it = stream.addresses.end();
    size_t best_rank = std::numeric_limits<size_t>::max();
    for (auto it = stream.addresses.begin(); it != stream.addresses.end();
         ++it) {
        const int64_t candidate_delta =
            static_cast<int64_t>(blockIndex(*it)) -
            static_cast<int64_t>(current_block_addr);
        for (size_t rank = 0; rank < predicted_deltas.size(); ++rank) {
            if (candidate_delta == predicted_deltas[rank] &&
                rank < best_rank) {
                promote_it = it;
                best_rank = rank;
            }
        }
    }

    unsigned issued = 0;
    if (promote_it != stream.addresses.end()) {
        addresses.push_back(AddrPriority(*promote_it, 0));
        issued++;
        if (promote_it != stream.addresses.begin()) {
            statsCMC.deltaCtxPredHeadPromotions++;
        }
    }

    for (auto it = stream.addresses.begin();
         it != stream.addresses.end() && issued < issue_deg; ++it) {
        if (it == promote_it) {
            continue;
        }
        addresses.push_back(AddrPriority(*it, 0));
        issued++;
    }

    return issued;
}

void
CMCPrefetcher::PrefetchListenerPC::notify(const Addr &pc)
{
    parent.notifyRetiredBranch(pc);
}

void
CMCPrefetcher::PrefetchTakenListenerPC::notify(const Addr &pc)
{
    parent.notifyRetiredTakenBranch(pc);
}

void
CMCPrefetcher::PrefetchExecutedListenerPC::notify(const Addr &pc)
{
    parent.notifyExecutedBranch(pc);
}

void
CMCPrefetcher::PrefetchExecutedTakenListenerPC::notify(const Addr &pc)
{
    parent.notifyExecutedTakenBranch(pc);
}

void
CMCPrefetcher::addEventProbeRetiredInsts(SimObject *obj, const char *name)
{
    listenersPC.push_back(
        obj->getProbeManager()->connect<PrefetchListenerPC>(*this, name));
}

void
CMCPrefetcher::addEventProbeRetiredBranches(SimObject *obj, const char *name)
{
    listenersPC.push_back(
        obj->getProbeManager()->connect<PrefetchListenerPC>(*this, name));
}

void
CMCPrefetcher::addEventProbeRetiredTakenBranches(
    SimObject *obj, const char *name)
{
    listenersTakenPC.push_back(
        obj->getProbeManager()->connect<PrefetchTakenListenerPC>(
            *this, name));
}

void
CMCPrefetcher::addEventProbeExecutedBranches(SimObject *obj, const char *name)
{
    listenersExecutedPC.push_back(
        obj->getProbeManager()->connect<PrefetchExecutedListenerPC>(
            *this, name));
}

void
CMCPrefetcher::addEventProbeExecutedTakenBranches(
    SimObject *obj, const char *name)
{
    listenersExecutedTakenPC.push_back(
        obj->getProbeManager()->connect<PrefetchExecutedTakenListenerPC>(
            *this, name));
}

void
CMCPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                 std::vector<AddrPriority> &addresses,
                                 const CacheAccessor &cache_accessor)
{
    Addr pc = pfi.hasPC() ? pfi.getPC() : 0;
    Addr addr = pfi.getAddr();
    Addr block_addr = blockIndex(addr);
    bool is_secure = pfi.isSecure();
    auto lookup_key = hash(block_addr, pc, 0);
    StorageEntry *match_entry = storage.findEntry(lookup_key, is_secure);
    const auto ctx_selection = selectAccessBranchCtx(pfi, match_entry);
    uint64_t ctx = ctx_selection.ctx;
    if (ctx_selection.usedRequestSnapshot) {
        statsCMC.ctxRequestSnapshots++;
        if (ctx_selection.usedLoadPcSnapshot) {
            statsCMC.ctxLoadPcSnapshots++;
        }
        if (ctx_selection.loadPcFallback) {
            statsCMC.ctxLoadPcFallbacks++;
        }
        if (ctx_selection.loadPcInsufficient) {
            statsCMC.ctxLoadPcInsufficientBranches++;
        }
        if (ctx_selection.loadPcSingleVariantSkip) {
            statsCMC.ctxLoadPcSingleVariantSkips++;
        }
        if (ctx_selection.loadPcNoBetterMatchSkip) {
            statsCMC.ctxLoadPcNoBetterMatchSkips++;
        }
        if (ctx_selection.usedLoadPcSnapshot) {
            statsCMC.ctxLoadPcDisambiguations++;
        }
    } else {
        statsCMC.ctxGlobalFallbacks++;
    }

    maybeDumpAnalysisSample(
        pfi, pc, block_addr, ctx, match_entry, ctx_selection);

    const uint16_t cur_ctx_tag = compressCtx(ctx);
    std::vector<int64_t> predicted_deltas;
    if (deltaCtxPredEnable && pfi.hasPC()) {
        statsCMC.deltaCtxPredLookups++;
        if (auto *delta_entry = findDeltaPrediction(pc, ctx, is_secure)) {
            predicted_deltas = collectPredictedDeltas(*delta_entry);
            if (!predicted_deltas.empty()) {
                statsCMC.deltaCtxPredHits++;
                for (auto &candidate : delta_entry->candidates) {
                    if (candidate.valid &&
                        std::find(predicted_deltas.begin(),
                                  predicted_deltas.end(),
                                  candidate.deltaBlocks) !=
                            predicted_deltas.end()) {
                        touchDeltaPrediction(candidate, false);
                    }
                }
            }
        }
    }

    DPRINTF(HWPrefetch,
        "CMC lookup pc=%lx addr=%lx ctx=%lx ctx_tag=%x key=%lx "
        "request_snapshot=%d load_pc_snapshot=%d load_pc_fallback=%d "
        "load_pc_single_variant_skip=%d load_pc_no_better_match_skip=%d "
        "pred_hit=%d pred_delta0=%ld pred_count=%zu\n",
        pc, block_addr, ctx, cur_ctx_tag, lookup_key,
        ctx_selection.usedRequestSnapshot, ctx_selection.usedLoadPcSnapshot,
        ctx_selection.loadPcFallback, ctx_selection.loadPcSingleVariantSkip,
        ctx_selection.loadPcNoBetterMatchSkip, !predicted_deltas.empty(),
        predicted_deltas.empty() ? 0 : predicted_deltas.front(),
        predicted_deltas.size());

    bool ctx_match = false;
    if (match_entry) {
        statsCMC.primaryHits++;
        storage.accessEntry(match_entry);
        ContextStream *match_stream =
            selectIssueStream(match_entry, cur_ctx_tag, ctx_match);
        if (!predicted_deltas.empty() && (!ctx_match || !match_stream)) {
            if (auto *hint_stream = selectStreamByPredictedDelta(
                    match_entry, block_addr, predicted_deltas)) {
                if (hint_stream != match_stream) {
                    match_stream = hint_stream;
                    statsCMC.deltaCtxPredVariantHints++;
                }
            }
        }
        const unsigned valid_variants = validVariantCount(match_entry);
        const bool context_sensitive = (valid_variants > 1);
        if (ctx_match) {
            statsCMC.ctxMatches++;
        } else {
            statsCMC.ctxMismatches++;
            if (context_sensitive) {
                statsCMC.ctxMultiVariantMismatches++;
            } else {
                statsCMC.ctxSingleVariantMismatches++;
            }
        }

        if (match_stream) {
            if (ctx_match) {
                touchContextStream(*match_stream, true);
            }
            DPRINTF(HWPrefetch,
                    "CMC primary hit pc=%lx addr=%lx stream_ctx=%x "
                    "cur_ctx=%x ctx_match=%d conf=%u valid_variants=%u\n",
                    pc, block_addr, match_stream->ctxTag, cur_ctx_tag,
                    ctx_match, match_stream->confidence, valid_variants);

            const unsigned issue_deg =
                allowedDegree(ctx_match, match_stream->addresses.size());
            DPRINTF(HWPrefetch,
                    "Storage hit, trigger pc=%lx addr=%lx issue_deg=%u "
                    "ctx_match=%d context_sensitive=%d\n",
                    pc, block_addr, issue_deg, ctx_match,
                    context_sensitive);

            unsigned issued = 0;
            if (!predicted_deltas.empty()) {
                issued = issueStreamWithPredictedDelta(
                    *match_stream, block_addr, predicted_deltas, issue_deg,
                    addresses);
            } else {
                for (auto pf_addr: match_stream->addresses) {
                    if (issued >= issue_deg) {
                        break;
                    }
                    addresses.push_back(AddrPriority(pf_addr, 0));
                    issued++;
                }
            }
            if (ctx_match) {
                statsCMC.pfCandidatesFromMatch += issued;
            } else {
                statsCMC.pfCandidatesFromMismatch += issued;
            }
        }
    }

    if (pfi.hasPC()) {
        auto &prev_state = lastDeltaTrainByPc[pc];
        trainDeltaPredictor(pc, prev_state, block_addr);
        prev_state.blockAddr = block_addr;
        prev_state.ctx = ctx;
        prev_state.isSecure = is_secure;
        prev_state.valid = true;
    }

    bool finished = false;

    /* 1. Train trigger */
    bool train_trigger =
        (trigger.size()<1 || match_entry) && trigger.size()<STACK_SIZE;
    bool do_training =
        !trigger.empty();
    if (train_trigger) {
        assert(trigger.size()<STACK_SIZE);

        trigger.push_back(RecordEntry(pc, block_addr, is_secure, ctx));
    }

    /* 2. Train entry */
    if (do_training) {
        auto &trigger_head = trigger.front();
        bool trained = recorder->train_entry(addr, is_secure, &finished);
        (void)trained;
        if (finished) {
            auto train_key = hash(trigger_head.addr, trigger_head.pc, 0);
            uint16_t train_ctx_tag = compressCtx(trigger_head.branch_ctx);
            DPRINTF(HWPrefetch,
                "CMC train-key pc=%lx addr=%lx ctx=%lx ctx_tag=%x key=%lx\n",
                trigger_head.pc, trigger_head.addr,
                trigger_head.branch_ctx, train_ctx_tag, train_key);
            StorageEntry *entry = storage.findEntry(
                train_key, trigger_head.is_secure);
            if (!entry) {
                entry = storage.findVictim(train_key);
                entry->clearStreams();
                storage.insertEntry(
                    train_key,
                    trigger_head.is_secure,
                    entry
                );
            }

            bool allocated = false;
            bool replaced = false;
            ContextStream *stream = selectTrainingStream(
                entry, train_ctx_tag, allocated, replaced);
            if (allocated) {
                statsCMC.ctxVariantAllocations++;
            }
            if (replaced) {
                statsCMC.ctxVariantReplacements++;
            }

            DPRINTF(HWPrefetch,
                    "CMC train stream pc=%lx addr=%lx train_ctx=%x "
                    "allocated=%d replaced=%d\n",
                    trigger_head.pc, trigger_head.addr, train_ctx_tag,
                    allocated, replaced);

            stream->valid = true;
            stream->ctxTag = train_ctx_tag;
            if (allocated) {
                stream->confidence = 0;
            }
            stream->addresses = recorder->entries;
            touchContextStream(*stream, true);

            for (auto addr: recorder->entries) {
                DPRINTF(HWPrefetch, "entry addr: 0x%lx\n",
                        addr);
            }
            // prefetchStats.metadataAccesses++;
            trigger.pop_front();

            if (trigger.size() < 1) {
                // This is fixed: if we don't have this here, we end up with
                // triggers and trainings as distinct, meaning we never reach
                // 100% coverage. This makes the trigger the last addr of the
                // previous group, provided no other trigger already is
                // prepared, because it was a recent reused addr.
                trigger.push_back(RecordEntry(pc, block_addr, is_secure, ctx));
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
    bool *finished
) {

        entries.push_back(addr);
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
CMCPrefetcher::StorageEntry::clearStreams()
{
    for (auto &variant : variants) {
        variant.invalidate();
    }
}

void
CMCPrefetcher::StorageEntry::invalidate()
{
    TaggedEntry::invalidate();
    clearStreams();
}



}  // prefetch
}  // gem5
