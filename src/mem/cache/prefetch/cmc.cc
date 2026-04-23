
//Adapted from https://github.com/OpenXiangShan/GEM5/blob/xs-dev/src/mem/cache/prefetch
#include "mem/cache/prefetch/cmc.hh"

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
    ADD_STAT(chooserHeadPromotions, statistics::units::Count::get(),
        "Lookups where the chooser promotes a matching head delta"),
    ADD_STAT(chooserTrainUpdates, statistics::units::Count::get(),
        "Training completions that update the prev-branch head-delta chooser"),
    ADD_STAT(prevBranchFeatureValidRate, statistics::units::Ratio::get(),
        "Fraction of lookups with a valid previous same-load-PC branch "
        "feature"),
    ADD_STAT(baselineLookupHitRate, statistics::units::Ratio::get(),
        "Baseline-key lookup hit rate"),
    ADD_STAT(augmentedLookupHitRate, statistics::units::Ratio::get(),
        "Augmented-key lookup hit rate"),
    ADD_STAT(prevBranchTakenRate, statistics::units::Ratio::get(),
        "Taken fraction among valid previous-branch lookups"),
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
        "Head-delta chooser hit rate among chooser lookups")
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
    useLastBranchTaken(p.use_last_branch_taken),
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
            << "cache_miss,match_entry\n";
    }
}

void
CMCPrefetcher::dumpPrevBranchSample(Addr pc, Addr block_addr, bool has_prev_pc,
                                    Addr delta_blocks,
                                    bool prev_branch_valid,
                                    Addr prev_branch_pc,
                                    bool prev_branch_taken,
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
        << (cache_miss ? 1 : 0) << ','
        << (match_entry ? 1 : 0) << '\n';
    prevBranchDumpedSamples++;
}

bool
CMCPrefetcher::applyHeadDeltaHint(Addr block_addr, Addr pc,
                                  bool prev_branch_valid,
                                  Addr prev_branch_pc,
                                  std::vector<AddrPriority> &addresses)
{
    if (!useLastBranchTaken || !prev_branch_valid || addresses.empty()) {
        return false;
    }

    cmcStats.chooserLookups++;
    const ChooserKey chooser_key{pc, prev_branch_pc};
    const auto chooser_it = headDeltaChooser.find(chooser_key);
    if (chooser_it == headDeltaChooser.end()) {
        return false;
    }

    cmcStats.chooserHits++;
    const int64_t predicted_delta = chooser_it->second.deltaBlocks;
    for (std::size_t idx = 0; idx < addresses.size(); ++idx) {
        const int64_t candidate_delta =
            static_cast<int64_t>(blockIndex(addresses[idx].first)) -
            static_cast<int64_t>(block_addr);
        if (candidate_delta == predicted_delta) {
            if (idx != 0) {
                std::swap(addresses[0], addresses[idx]);
            }
            cmcStats.chooserHeadPromotions++;
            return true;
        }
    }

    return false;
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
    const bool prev_load_branch_valid = pfi.hasPrevLoadBranchOutcome();
    const Addr prev_load_branch_pc =
        prev_load_branch_valid ? pfi.getPrevLoadBranchPC() : 0;
    const bool prev_load_branch_taken =
        prev_load_branch_valid ? pfi.getPrevLoadBranchTaken() : false;
    const uint64_t baseline_key = hash(block_addr, pc);
    bool has_prev_pc = false;
    Addr delta_blocks = 0;
    const auto prev_block_it = lastObservedBlockByPc.find(pc);
    if (prev_block_it != lastObservedBlockByPc.end()) {
        has_prev_pc = true;
        delta_blocks = block_addr - prev_block_it->second;
    }
    lastObservedBlockByPc[pc] = block_addr;

    cmcStats.totalLookups++;
    if (prev_load_branch_valid) {
        cmcStats.lookupsWithValidPrevBranch++;
        if (prev_load_branch_taken) {
            cmcStats.prevBranchTakenLookups++;
        } else {
            cmcStats.prevBranchNotTakenLookups++;
        }
        cmcStats.lookupAugmentedKeyDiffersFromBaseline++;
    }

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
                         prev_load_branch_taken, pfi.isCacheMiss(),
                         match_entry != nullptr);
    // prefetchStats.metadataAccesses++;
    if (match_entry) {
        cmcStats.augmentedLookupHits++;
        storage.accessEntry(match_entry);
        // prefetch on cache miss or on prefetch hit
        DPRINTF(HWPrefetch, "Storage hit, trigger pc: %lx, addr: %lx\n",
                pc, block_addr);
      //printf("=== Storage hit, trigger addr: %lx\n", block_addr);

        for (auto addr: match_entry->addresses) {
            addresses.push_back(AddrPriority(addr, 0));
        }
        if (prev_load_branch_valid) {
            cmcStats.prefetchCandidatesFromAugmentedKeys +=
                match_entry->addresses.size();
        }
        applyHeadDeltaHint(block_addr, pc, prev_load_branch_valid,
                           prev_load_branch_pc, addresses);
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
            prev_load_branch_valid, prev_load_branch_pc,
            prev_load_branch_taken));
    }

    /* 2. Train entry */
    if (do_training) {
        bool trained = recorder->train_entry(addr, is_secure, &finished);
        auto &trigger_head = trigger.front();
        if (trained) {
            //printf("trained %x\n", block_addr);
        }
        if (finished) {
			//finished gets set once a coalesced set of 16 targets get stored in the training entry.
            //printf("trigger train finished, pc: %lx, addr: %lx\n",
               //     trigger_head.pc, trigger_head.addr);
            const uint64_t baseline_train_key = hash(
                trigger_head.addr, trigger_head.pc);

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

            StorageEntry *baseline_train_entry = storage.findEntry(
                baseline_train_key, trigger_head.is_secure);
            if (baseline_train_entry) {
                cmcStats.baselineTrainHits++;
            }

            StorageEntry *entry = baseline_train_entry;
            if (entry) {
                cmcStats.augmentedTrainHits++;
                // storage.accessEntry(entry); do not update replacement
                DPRINTF(HWPrefetch, "CMC: enter the same trigger, pc: %lx, addr: %lx\n",
                                    trigger_head.pc, trigger_head.addr);
                entry->addresses = recorder->entries;

            } else {
                entry = storage.findVictim(baseline_train_key);
                entry->addresses = recorder->entries;

                storage.insertEntry(
                    baseline_train_key,
                    trigger_head.is_secure,
                    entry
                );
            }

            for (auto addr: recorder->entries) {
                DPRINTF(HWPrefetch, "entry addr: 0x%lx\n",
                        addr);
            }
            if (useLastBranchTaken && trigger_head.has_prev_load_branch &&
                !recorder->entries.empty()) {
                const int64_t head_delta_blocks =
                    static_cast<int64_t>(blockIndex(recorder->entries.front())) -
                    static_cast<int64_t>(trigger_head.addr);
                const ChooserKey chooser_key{
                    trigger_head.pc, trigger_head.prev_branch_pc};
                auto &chooser_entry = headDeltaChooser[chooser_key];
                if (chooser_entry.confidence == 0 ||
                    chooser_entry.deltaBlocks == head_delta_blocks) {
                    chooser_entry.deltaBlocks = head_delta_blocks;
                    if (chooser_entry.confidence < 3) {
                        chooser_entry.confidence++;
                    }
                } else {
                    chooser_entry.confidence--;
                    if (chooser_entry.confidence == 0) {
                        chooser_entry.deltaBlocks = head_delta_blocks;
                        chooser_entry.confidence = 1;
                    }
                }
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
	                    prev_load_branch_valid, prev_load_branch_pc,
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
}



}  // prefetch
}  // gem5
