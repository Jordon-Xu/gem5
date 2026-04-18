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
                           uint64_t total_taken_branches)
        : contextSnapshot(snapshot),
          totalBranches(total_branches),
          totalTakenBranches(total_taken_branches)
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

  private:
    BranchContextSnapshot contextSnapshot;
    uint64_t totalBranches = 0;
    uint64_t totalTakenBranches = 0;
};

} // namespace gem5

#endif // __CPU_BRANCH_CONTEXT_HH__
