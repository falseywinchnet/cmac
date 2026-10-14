#include "refresh/refresh.hpp"

#include <cstdio>

namespace cmac {

RefreshPlan plan_refresh(const LoadedSnapshot* const saved, const std::string& root, const RefreshPolicy& policy) {
    RefreshPlan plan{};
    // Taken before anything is read: a change during the refresh is either
    // seen now or replayed next time, never lost.
    plan.position = current_journal_position(root);
    if (saved == nullptr) {
        plan.reason = "no saved scan";
        return plan;
    }
    const SnapshotInfo& info = (*saved).info;
    if (info.root != root) {
        plan.reason = "the saved scan is of another folder";
        return plan;
    }
    if (info.system_build != current_system_build()) {
        plan.reason = "the system was updated since the saved scan";
        return plan;
    }
    if (plan.position.store.empty() || info.journal_store != plan.position.store) {
        plan.reason = "the change journal was reset since the saved scan";
        return plan;
    }
    const double estimate = estimated_replay_seconds(info.journal_event_id, plan.position.event_id);
    if (estimate > policy.replay_budget_seconds) {
        char text[128] = {};
        std::snprintf(text, sizeof text, "replaying the changes would take about %.0f s", estimate);
        plan.reason = text;
        return plan;
    }
    plan.replay = replay_journal(root, info.journal_event_id, plan.position.event_id, policy.replay_timeout);
    if (!plan.replay.complete) {
        plan.reason = "the change journal did not answer in time";
        return plan;
    }
    if (plan.replay.rescan_everything) {
        plan.reason = "the change journal lost events";
        return plan;
    }
    plan.reuse = plan_reuse((*saved).snapshot, plan.replay.folders);
    plan.incremental = true;
    char text[160] = {};
    std::snprintf(text, sizeof text, "%llu changes replayed in %.2f s; %llu folders to reread, %llu to rescan",
                  static_cast<unsigned long long>(plan.replay.events), plan.replay.seconds,
                  static_cast<unsigned long long>(plan.reuse.relisted),
                  static_cast<unsigned long long>(plan.reuse.rescanned));
    plan.reason = text;
    return plan;
}

} // namespace cmac
