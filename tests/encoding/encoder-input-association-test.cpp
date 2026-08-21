#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>

namespace {

using SourceFrameId = std::uint64_t;
using LogicalSlotId = std::uint64_t;

struct Submission {
    SourceFrameId source_frame_id;
    std::uint64_t composition_cts;
};

class SourceSlotResolver final {
public:
    void SetOrigin(const Submission& submission, LogicalSlotId slot_id) {
        source_origin_ = submission.source_frame_id;
        slot_origin_ = slot_id;
    }

    std::optional<LogicalSlotId> Resolve(const Submission& submission) const {
        if (!source_origin_ || submission.source_frame_id < *source_origin_) {
            return std::nullopt;
        }
        const LogicalSlotId slot = slot_origin_ + submission.source_frame_id - *source_origin_;
        return slots_.find(slot) != slots_.end() ? std::optional<LogicalSlotId>(slot) : std::nullopt;
    }

    void AddSlot(LogicalSlotId slot_id, std::uint64_t cts) { slots_.emplace(slot_id, cts); }

private:
    std::optional<SourceFrameId> source_origin_;
    LogicalSlotId slot_origin_ = 0;
    std::map<LogicalSlotId, std::uint64_t> slots_;
};

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void TestTextureAliasRetainsDistinctSourceSlots() {
    SourceSlotResolver resolver;
    resolver.AddSlot(100, 1'000);
    resolver.AddSlot(101, 1'016);
    resolver.AddSlot(102, 1'033);

    const Submission rendered{50, 1'000};
    const Submission repeated{51, 1'000}; // texture CTS alias, advanced source identity
    resolver.SetOrigin(rendered, 100);

    const auto rendered_slot = resolver.Resolve(rendered);
    const auto repeated_slot = resolver.Resolve(repeated);
    Require(rendered_slot && *rendered_slot == 100, "rendered source slot must resolve");
    Require(repeated_slot && *repeated_slot == 101,
            "aliased texture submission must resolve to the advanced logical slot");
    Require(*rendered_slot != *repeated_slot, "source identity must not collapse CTS aliases");
}

void TestSourceRegressionIsRejected() {
    SourceSlotResolver resolver;
    resolver.AddSlot(100, 1'000);
    resolver.SetOrigin({50, 1'000}, 100);
    Require(!resolver.Resolve({49, 984}), "source identity regression must be rejected");
}

} // namespace

int main() {
    TestTextureAliasRetainsDistinctSourceSlots();
    TestSourceRegressionIsRejected();
    return EXIT_SUCCESS;
}
