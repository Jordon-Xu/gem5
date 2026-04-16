
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
            p.storage_replacement_policy, StorageEntry()),
    baseDegree(p.degree),
    currentBranchCtx(0),
    ctxEnable(p.ctx_enable),
    ctxTakenOnly(p.ctx_taken_only),
    ctxShift(p.ctx_shift),
    ctxBits(p.ctx_bits),
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
             "prefetch candidates issued from context mismatches")
{
}

void
CMCPrefetcher::updateBranchCtx(Addr branch_pc)
{
    if (!ctxEnable) {
        return;
    }

    const unsigned s = ctxShift & 63;

    if (s == 0) {
        currentBranchCtx ^= static_cast<uint64_t>(branch_pc);
    } else {
        currentBranchCtx =
            (currentBranchCtx << s) ^
            (currentBranchCtx >> (64 - s)) ^
            static_cast<uint64_t>(branch_pc);
    }
}

void
CMCPrefetcher::notifyRetiredBranch(Addr branch_pc)
{
    if (!ctxEnable || ctxTakenOnly) {
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
    if (!ctxEnable || !ctxTakenOnly) {
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
CMCPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                 std::vector<AddrPriority> &addresses,
                                 const CacheAccessor &cache_accessor)
{


   //PC isolation here lowers coverage...
    Addr pc = pfi.hasPC() ? pfi.getPC() : 0;

    Addr addr = pfi.getAddr();
    Addr block_addr = blockIndex(addr); // takes off 6 Least Significant Bits for cache line
    bool is_secure = pfi.isSecure();
    uint64_t ctx = getCurrentBranchCtx();


    // DPRINTF(HWPrefetch, "CMC train: pc: %lx, addr: %lx, ctx: %lx\n", pc, block_addr, ctx);

    auto lookup_key = hash(block_addr, pc, 0);
    const uint16_t cur_ctx_tag = compressCtx(ctx);
    DPRINTF(HWPrefetch,
        "CMC lookup pc=%lx addr=%lx ctx=%lx ctx_tag=%x key=%lx\n",
        pc, block_addr, ctx, cur_ctx_tag, lookup_key);


    // Prefetch: check if there is a match
    StorageEntry *match_entry = storage.findEntry(lookup_key, is_secure);
    // prefetchStats.metadataAccesses++;
    bool ctx_match = false;
    if (match_entry) {
        statsCMC.primaryHits++;
        storage.accessEntry(match_entry);
        ctx_match = (match_entry->ctx_tag == cur_ctx_tag);
        if (ctx_match) {
            statsCMC.ctxMatches++;
        } else {
            statsCMC.ctxMismatches++;
        }

        DPRINTF(HWPrefetch,
                "CMC primary hit pc=%lx addr=%lx entry_ctx=%x "
                "cur_ctx=%x ctx_match=%d\n",
                pc, block_addr, match_entry->ctx_tag, cur_ctx_tag,
                ctx_match);

        const unsigned issue_deg =
            allowedDegree(ctx_match, match_entry->addresses.size());
        DPRINTF(HWPrefetch,
                "Storage hit, trigger pc=%lx addr=%lx issue_deg=%u "
                "ctx_match=%d\n",
                pc, block_addr, issue_deg, ctx_match);

        unsigned issued = 0;
        for (auto pf_addr: match_entry->addresses) {
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
        bool trained = recorder->train_entry(addr, is_secure, &finished);
        auto &trigger_head = trigger.front();
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
            if (entry) {
                // storage.accessEntry(entry); do not update replacement
                DPRINTF(HWPrefetch,
                        "CMC: enter the same trigger, pc: %lx, addr: %lx, "
                        "ctx: %lx\n",
                        trigger_head.pc, trigger_head.addr,
                        trigger_head.branch_ctx);
                entry->addresses = recorder->entries;
                entry->ctx_tag = train_ctx_tag;


            } else {
                entry = storage.findVictim(train_key);
                entry->addresses = recorder->entries;
                entry->ctx_tag = train_ctx_tag;


                storage.insertEntry(
                    train_key,
                    trigger_head.is_secure,
                    entry
                );
            }

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
CMCPrefetcher::StorageEntry::invalidate()
{
    TaggedEntry::invalidate();
    addresses.clear();
    ctx_tag = 0;
}



}  // prefetch
}  // gem5
