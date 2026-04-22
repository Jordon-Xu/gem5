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
    LastBranchOutcomeExtension(bool valid, bool taken)
        : valid(valid), taken(taken)
    {}

    std::unique_ptr<ExtensionBase>
    clone() const override
    {
        return std::unique_ptr<LastBranchOutcomeExtension>(
            new LastBranchOutcomeExtension(valid, taken));
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

  private:
    bool valid;
    bool taken;
};

} // namespace gem5

#endif // __CPU_LAST_BRANCH_OUTCOME_HH__
