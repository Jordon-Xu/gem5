
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
    trigger()
{
                    trigger.clear();
}

void
CMCPrefetcher::updateBranchCtx(Addr branch_pc)
{
    currentBranchCtx =
        (currentBranchCtx << branchShift) ^
        (currentBranchCtx >> (64 - branchShift)) ^
        static_cast<uint64_t>(branch_pc);
}

void
CMCPrefetcher::notifyRetiredBranch(Addr branch_pc)
{
    updateBranchCtx(branch_pc);
    DPRINTF(HWPrefetch, "CMC retired pc=%lx new_ctx=%lx\n",
        branch_pc, currentBranchCtx);
}

void
CMCPrefetcher::PrefetchListenerPC::notify(const Addr &pc)
{
    parent.notifyRetiredBranch(pc);
}

void
CMCPrefetcher::addEventProbeRetiredInsts(SimObject *obj, const char *name)
{
    listenersPC.push_back(
        obj->getProbeManager()->connect<PrefetchListenerPC>(*this, name));
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


    DPRINTF(HWPrefetch, "CMC train: pc: %lx, addr: %lx, ctx: %lx\n", pc, block_addr, ctx);

    
    

    // Prefetch: check if there is a match
    StorageEntry *match_entry = storage.findEntry(hash(block_addr, pc, ctx), is_secure);
    // prefetchStats.metadataAccesses++;
    if (match_entry) {
        storage.accessEntry(match_entry);
        // prefetch on cache miss or on prefetch hit
        DPRINTF(HWPrefetch, "Storage hit, trigger pc: %lx, addr: %lx\n",
                pc, block_addr);
      //printf("=== Storage hit, trigger addr: %lx\n", block_addr);

        for (auto addr: match_entry->addresses) {
            addresses.push_back(AddrPriority(addr, 0));
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
			//finished gets set once a coalesced set of 16 targets get stored in the training entry.
            //printf("trigger train finished, pc: %lx, addr: %lx\n",
               //     trigger_head.pc, trigger_head.addr);

            StorageEntry *entry = storage.findEntry(hash(trigger_head.addr, trigger_head.pc, trigger_head.branch_ctx), trigger_head.is_secure);
            if (entry) {
                // storage.accessEntry(entry); do not update replacement
                DPRINTF(HWPrefetch, "CMC: enter the same trigger, pc: %lx, addr: %lx, ctx: %lx\n",
                                    trigger_head.pc, trigger_head.addr, trigger_head.branch_ctx);
                entry->addresses = recorder->entries;


            } else {
                entry = storage.findVictim(hash(trigger_head.addr, trigger_head.pc, trigger_head.branch_ctx));
                entry->addresses = recorder->entries;



                storage.insertEntry(
                    hash(trigger_head.addr, trigger_head.pc, trigger_head.branch_ctx),
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
            
			if (trigger.size()<1) {
				//This is fixed: if we don't have this here, we end up with triggers and trainings as distinct,
				//meaning we never reach 100% coverage.
				//This makes the trigger the last addr of the previous group, provided no other trigger already
				// is prepared, because it was a recent reused addr.
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
}



}  // prefetch
}  // gem5
