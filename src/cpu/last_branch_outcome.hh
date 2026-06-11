#ifndef __CPU_LAST_BRANCH_OUTCOME_HH__
#define __CPU_LAST_BRANCH_OUTCOME_HH__

#include "base/extensible.hh"
#include "mem/request.hh"

namespace gem5
{

class LastBranchOutcomeExtension
    : public Extension<Request, LastBranchOutcomeExtension>
{
  public:
    LastBranchOutcomeExtension(bool valid, Addr pc, bool taken,
                               bool bp_high_confidence)
        : valid(valid), pc(pc), taken(taken),
          bpHighConfidence(bp_high_confidence)
    {}

    std::unique_ptr<ExtensionBase>
    clone() const override
    {
        return std::unique_ptr<LastBranchOutcomeExtension>(
            new LastBranchOutcomeExtension(valid, pc, taken,
                                           bpHighConfidence));
    }

    bool
    hasPreviousLoadOutcome() const
    {
        return valid;
    }

    bool
    previousLoadWasTaken() const
    {
        return taken;
    }

    bool
    previousLoadBranchBpHighConfidence() const
    {
        return bpHighConfidence;
    }

    Addr
    previousLoadBranchPC() const
    {
        return pc;
    }

  private:
    bool valid;
    Addr pc;
    bool taken;
    bool bpHighConfidence;
};

} // namespace gem5

#endif // __CPU_LAST_BRANCH_OUTCOME_HH__
