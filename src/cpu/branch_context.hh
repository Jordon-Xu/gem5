#ifndef __CPU_BRANCH_CONTEXT_HH__
#define __CPU_BRANCH_CONTEXT_HH__

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "base/extensible.hh"
#include "base/types.hh"

namespace gem5
{

class Request;

class BranchContextSnapshot
{
  public:
    static constexpr size_t MaxEntries = 32;

    struct Entry
    {
        Addr pc = 0;
        bool taken = false;
    };

    void
    clear()
    {
        entryCount = 0;
    }

    void
    push(Addr pc, bool taken)
    {
        const Entry entry{pc, taken};
        if (entryCount < MaxEntries) {
            entries[entryCount++] = entry;
            return;
        }

        for (size_t i = 1; i < MaxEntries; ++i) {
            entries[i - 1] = entries[i];
        }
        entries.back() = entry;
    }

    bool
    empty() const
    {
        return entryCount == 0;
    }

    size_t
    size() const
    {
        return entryCount;
    }

    const Entry &
    operator[](size_t index) const
    {
        assert(index < entryCount);
        return entries[index];
    }

  private:
    std::array<Entry, MaxEntries> entries = {};
    size_t entryCount = 0;
};

class BranchContextExtension : public Extension<Request, BranchContextExtension>
{
  public:
    BranchContextExtension() = default;

    BranchContextExtension(const BranchContextSnapshot &snapshot,
                           uint64_t total_branches,
                           uint64_t total_taken_branches,
                           const BranchContextSnapshot &load_pc_snapshot = {},
                           uint64_t load_pc_total_branches = 0,
                           uint64_t load_pc_total_taken_branches = 0,
                           bool valid_load_pc_snapshot = false)
        : contextSnapshot(snapshot),
          totalBranches(total_branches),
          totalTakenBranches(total_taken_branches),
          loadPcContextSnapshot(load_pc_snapshot),
          loadPcTotalBranches(load_pc_total_branches),
          loadPcTotalTakenBranches(load_pc_total_taken_branches),
          validLoadPcContext(valid_load_pc_snapshot)
    {}

    std::unique_ptr<ExtensionBase>
    clone() const override
    {
        return std::make_unique<BranchContextExtension>(*this);
    }

    const BranchContextSnapshot &
    snapshot() const
    {
        return contextSnapshot;
    }

    uint64_t
    branchCount() const
    {
        return totalBranches;
    }

    uint64_t
    takenBranchCount() const
    {
        return totalTakenBranches;
    }

    bool
    hasLoadPcSnapshot() const
    {
        return validLoadPcContext;
    }

    const BranchContextSnapshot &
    loadPcSnapshot() const
    {
        assert(hasLoadPcSnapshot());
        return loadPcContextSnapshot;
    }

    uint64_t
    loadPcBranchCount() const
    {
        assert(hasLoadPcSnapshot());
        return loadPcTotalBranches;
    }

    uint64_t
    loadPcTakenBranchCount() const
    {
        assert(hasLoadPcSnapshot());
        return loadPcTotalTakenBranches;
    }

  private:
    BranchContextSnapshot contextSnapshot;
    uint64_t totalBranches = 0;
    uint64_t totalTakenBranches = 0;
    BranchContextSnapshot loadPcContextSnapshot;
    uint64_t loadPcTotalBranches = 0;
    uint64_t loadPcTotalTakenBranches = 0;
    bool validLoadPcContext = false;
};

} // namespace gem5

#endif // __CPU_BRANCH_CONTEXT_HH__
