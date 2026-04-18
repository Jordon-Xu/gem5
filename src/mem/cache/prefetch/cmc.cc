
//Adapted from https://github.com/OpenXiangShan/GEM5/blob/xs-dev/src/mem/cache/prefetch
#include "mem/cache/prefetch/cmc.hh"

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
    baseDegree(p.degree),
    currentBranchCtx(0),
    ctxEnable(p.ctx_enable),
    ctxTakenOnly(p.ctx_taken_only),
    ctxUseExecuteBranches(p.ctx_use_execute_branches),
    ctxUseRequestSnapshot(p.ctx_use_request_snapshot),
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
    ADD_STAT(ctxGlobalFallbacks, statistics::units::Count::get(),
             "number of accesses that fell back to global branch context")
{
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

uint64_t
CMCPrefetcher::getAccessBranchCtx(const PrefetchInfo &pfi) const
{
    if (!ctxEnable) {
        return 0;
    }

    if (!ctxUseRequestSnapshot || !pfi.hasBranchContextSnapshot()) {
        return getCurrentBranchCtx();
    }

    const auto &snapshot = pfi.getBranchContextSnapshot();
    const uint64_t total_selected = ctxTakenOnly ?
        pfi.getBranchContextTotalTakenBranches() :
        pfi.getBranchContextTotalBranches();
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

    if (selected_branches.empty()) {
        return 0;
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

    uint64_t ctx = 0;
    size_t start = 0;
    if (ctxWindowSize != 0 && contributing_branches.size() > ctxWindowSize) {
        start = contributing_branches.size() - ctxWindowSize;
    }

    for (size_t i = start; i < contributing_branches.size(); ++i) {
        ctx = mixBranchCtx(ctx, contributing_branches[i]);
    }

    return ctx;
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


   //PC isolation here lowers coverage...
    Addr pc = pfi.hasPC() ? pfi.getPC() : 0;

    Addr addr = pfi.getAddr();
    Addr block_addr = blockIndex(addr); // takes off 6 Least Significant Bits for cache line
    bool is_secure = pfi.isSecure();
    const bool using_request_snapshot =
        ctxEnable && ctxUseRequestSnapshot && pfi.hasBranchContextSnapshot();
    uint64_t ctx = getAccessBranchCtx(pfi);
    if (using_request_snapshot) {
        statsCMC.ctxRequestSnapshots++;
    } else {
        statsCMC.ctxGlobalFallbacks++;
    }


    // DPRINTF(HWPrefetch, "CMC train: pc: %lx, addr: %lx, ctx: %lx\n", pc, block_addr, ctx);

    auto lookup_key = hash(block_addr, pc, 0);
    const uint16_t cur_ctx_tag = compressCtx(ctx);
    DPRINTF(HWPrefetch,
        "CMC lookup pc=%lx addr=%lx ctx=%lx ctx_tag=%x key=%lx "
        "request_snapshot=%d\n",
        pc, block_addr, ctx, cur_ctx_tag, lookup_key,
        using_request_snapshot);


    // Prefetch: check if there is a match
    StorageEntry *match_entry = storage.findEntry(lookup_key, is_secure);
    // prefetchStats.metadataAccesses++;
    bool ctx_match = false;
    if (match_entry) {
        statsCMC.primaryHits++;
        storage.accessEntry(match_entry);
        ContextStream *match_stream =
            selectIssueStream(match_entry, cur_ctx_tag, ctx_match);
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
            for (auto pf_addr: match_stream->addresses) {
                if (issued >= issue_deg) {
                    break;
                }
                addresses.push_back(AddrPriority(pf_addr, 0));
                issued++;
            }
            if (ctx_match) {
                statsCMC.pfCandidatesFromMatch += issued;
            } else {
                statsCMC.pfCandidatesFromMismatch += issued;
            }
        }
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

        trigger.push_back(RecordEntry(pc, block_addr, is_secure, ctx));
    }

    /* 2. Train entry */
    if (do_training) {
        auto &trigger_head = trigger.front();
        bool trained = recorder->train_entry(addr, is_secure, &finished);
        if (trained) {
            //printf("trained %x\n", block_addr);
        }
        if (finished) {
            // finished gets set once a coalesced set of 16 targets get
            // stored in the training entry.
            //printf("trigger train finished, pc: %lx, addr: %lx\n",
               //     trigger_head.pc, trigger_head.addr);

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
