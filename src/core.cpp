#include "core.hpp"
#include <limits>
namespace evening {
const char *kindName(Kind k) {
    switch (k) {
    case Kind::Once:
        return "once";
    case Kind::Weekly:
        return "weekly";
    default:
        return "countdown";
    }
}
const char *statusName(Status s) {
    switch (s) {
    case Status::Cancelled:
        return "cancelled";
    case Status::Snoozed:
        return "snoozed";
    case Status::Missed:
        return "missed";
    case Status::Requesting:
        return "requesting";
    case Status::Requested:
        return "requested";
    default:
        return "failed";
    }
}
Task *Engine::task(const std::string &id) {
    for (auto &t : state.tasks)
        if (t.id == id)
            return &t;
    return nullptr;
}
const Task *Engine::task(const std::string &id) const {
    for (auto &t : state.tasks)
        if (t.id == id)
            return &t;
    return nullptr;
}
void Engine::append(const Occurrence &p, Status s, Snapshot now, const std::string &detail) {
    auto *t = task(p.taskId);
    state.logs.insert(state.logs.begin(), Log{p.key, t ? t->name : "", detail, now.wall, p.due, s});
    if (state.logs.size() > 100)
        state.logs.resize(100);
}
bool Engine::finish(const std::string &key, Status s, Snapshot now, const std::string &detail) {
    auto it =
        std::find_if(state.pending.begin(), state.pending.end(), [&](const auto &p) { return p.key == key; });
    if (it == state.pending.end())
        return false;
    append(*it, s, now, detail);
    if (auto *t = task(it->taskId)) {
        if (t->kind == Kind::Weekly)
            t->lastHandledDay = std::max(t->lastHandledDay, it->day);
        else
            t->completed = true;
    }
    state.pending.erase(it);
    return true;
}
bool Engine::fill(Snapshot now) {
    bool changed = false;
    for (auto &t : state.tasks) {
        if (!t.enabled || t.completed)
            continue;
        bool exists = std::any_of(state.pending.begin(), state.pending.end(), [&](const auto &p) {
            return p.taskId == t.id && (t.kind != Kind::Weekly || !p.detached);
        });
        if (exists)
            continue;
        Time due = 0;
        int day = t.day;
        if (t.kind == Kind::Weekly) {
            int start = std::max(calendar_->local(now.wall).day, t.lastHandledDay + 1);
            for (int d = start; d < start + 15; ++d) {
                if (!(t.weekdays & (1 << weekday(d))))
                    continue;
                auto resolved = calendar_->resolve(d, t.minute);
                if (!resolved || *resolved <= now.wall)
                    continue;
                due = *resolved;
                day = d;
                break;
            }
            if (!due)
                continue;
        } else
            due = t.onceDue;
        Occurrence p{t.id + "/" + std::to_string(t.revision) + "/" + std::to_string(day),
                     t.id,
                     t.revision,
                     day,
                     due,
                     due};
        if (t.kind == Kind::Countdown)
            p.monotonicDue = now.steady + (due - now.wall);
        state.pending.push_back(p);
        changed = true;
        if (due <= now.wall)
            finish(p.key, Status::Missed, now, "任务时间已过");
    }
    return changed;
}
void Engine::promoteWarning() {
    if (active || otherWarnings.empty())
        return;
    auto it = std::min_element(otherWarnings.begin(), otherWarnings.end(),
                               [](const auto &a, const auto &b) { return a.monotonicDue < b.monotonicDue; });
    active = std::move(*it);
    otherWarnings.erase(it);
}
void Engine::clearWarnings() {
    active.reset();
    otherWarnings.clear();
    for (auto &p : state.pending)
        p.warned = false;
}
bool Engine::cycleWarning() {
    if (!active || otherWarnings.empty())
        return false;
    otherWarnings.push_back(std::move(*active));
    active = std::move(otherWarnings.front());
    otherWarnings.erase(otherWarnings.begin());
    return true;
}
TickResult Engine::tick(Snapshot now, Reconcile reason) {
    TickResult out;
    if (reason != Reconcile::Normal) {
        // Even disabled one-off tasks keep their local date/time when the Windows zone changes.
        if (reason == Reconcile::Startup || reason == Reconcile::TimeChange) {
            for (auto &t : state.tasks)
                if (t.kind == Kind::Once && !t.completed) {
                    auto localDue = calendar_->resolve(t.day, t.minute);
                    Time value = localDue ? *localDue + (t.onceDue % Minute + Minute) % Minute : now.wall;
                    if (value != t.onceDue) {
                        t.onceDue = value;
                        out.changed = true;
                    }
                }
        }
        clearWarnings();
        for (auto &p : state.pending) {
            p.warned = false;
            const auto *t = task(p.taskId);
            if (!t)
                continue;
            if (reason == Reconcile::TimeChange && (t->kind == Kind::Countdown || p.detached) &&
                p.monotonicDue) {
                p.due = now.wall + (p.monotonicDue - now.steady);
                out.changed = true;
            } else if ((reason == Reconcile::TimeChange || reason == Reconcile::Startup) && !p.detached &&
                       t->kind != Kind::Countdown) {
                auto resolved = t->kind == Kind::Once ? std::optional<Time>{t->onceDue}
                                                      : calendar_->resolve(p.day, t->minute);
                p.due = resolved.value_or(now.wall);
                p.originalDue = p.due;
                out.changed = true;
            }
            if (t->kind == Kind::Countdown || p.detached) {
                if (!p.monotonicDue || reason == Reconcile::Startup)
                    p.monotonicDue = now.steady + (p.due - now.wall);
                else if (reason == Reconcile::Resume)
                    p.due = now.wall + (p.monotonicDue - now.steady);
            }
        }
    }
    if (state.settings.paused && (active || !otherWarnings.empty())) {
        clearWarnings();
        out.changed = true;
    }
    auto dispatchGroup = [&](const Group &group) {
        for (const auto &key : group.keys)
            if (finish(key, Status::Requesting, now, ""))
                out.dispatch.push_back(key);
        out.changed = true;
    };
    if (!state.settings.paused) {
        if (active && active->monotonicDue <= now.steady) {
            dispatchGroup(*active);
            active.reset();
        }
        for (auto it = otherWarnings.begin(); it != otherWarnings.end();) {
            if (it->monotonicDue <= now.steady) {
                dispatchGroup(*it);
                it = otherWarnings.erase(it);
            } else
                ++it;
        }
        promoteWarning();
    }
    std::vector<std::string> overdue, stale;
    for (auto &p : state.pending) {
        const auto *t = task(p.taskId);
        if (!t || !t->enabled || t->revision != p.revision) {
            stale.push_back(p.key);
            continue;
        }
        bool member =
            active && std::find(active->keys.begin(), active->keys.end(), p.key) != active->keys.end();
        member = member || std::any_of(otherWarnings.begin(), otherWarnings.end(), [&](const auto &g) {
                     return std::find(g.keys.begin(), g.keys.end(), p.key) != g.keys.end();
                 });
        if (member)
            continue;
        if (p.monotonicDue && (t->kind == Kind::Countdown || p.detached)) {
            Time adjusted = now.wall + (p.monotonicDue - now.steady);
            if (adjusted != p.due) {
                p.due = adjusted;
            }
        }
        if (p.due <= now.wall)
            overdue.push_back(p.key);
    }
    for (const auto &key : overdue)
        out.changed = finish(key, Status::Missed, now, "错过执行时间，已跳过") || out.changed;
    for (const auto &key : stale) {
        std::erase_if(state.pending, [&](const auto &p) { return p.key == key; });
        out.changed = true;
    }
    out.changed = fill(now) || out.changed;
    if (!state.settings.paused) {
        while (true) {
            auto it = state.pending.end();
            for (auto candidate = state.pending.begin(); candidate != state.pending.end(); ++candidate)
                if (!candidate->warned && (it == state.pending.end() || candidate->due < it->due))
                    it = candidate;
            if (it == state.pending.end() || it->due <= now.wall || it->due > now.wall + Minute)
                break;
            Time original = it->due;
            Time deadline = std::max(original, now.wall + Minute);
            Group group{{}, deadline, now.steady + (deadline - now.wall)};
            for (auto &p : state.pending)
                if (!p.warned && p.due == original) {
                    p.warned = true;
                    p.due = deadline;
                    p.monotonicDue = group.monotonicDue;
                    group.keys.push_back(p.key);
                }
            if (!active)
                active = std::move(group);
            else
                otherWarnings.push_back(std::move(group));
            out.changed = true;
            out.warningOpened = true;
        }
    }
    return out;
}
bool Engine::cancel(Snapshot now, const std::string &detail) {
    if (!active)
        return false;
    auto keys = active->keys;
    active.reset();
    for (const auto &key : keys)
        finish(key, Status::Cancelled, now, detail);
    promoteWarning();
    fill(now);
    return true;
}
bool Engine::snooze(Snapshot now, int minutes) {
    if (!active || minutes <= 0)
        return false;
    for (const auto &key : active->keys) {
        auto it = std::find_if(state.pending.begin(), state.pending.end(),
                               [&](const auto &p) { return p.key == key; });
        if (it == state.pending.end())
            continue;
        append(*it, Status::Snoozed, now, "推迟 " + std::to_string(minutes) + " 分钟");
        if (auto *t = task(it->taskId); t && t->kind == Kind::Weekly)
            t->lastHandledDay = std::max(t->lastHandledDay, it->day);
        it->due = now.wall + minutes * Minute;
        it->monotonicDue = now.steady + minutes * Minute;
        it->warned = false;
        it->detached = true;
    }
    active.reset();
    promoteWarning();
    fill(now);
    return true;
}
bool Engine::pause(Snapshot now, bool value) {
    if (state.settings.paused == value)
        return false;
    if (value)
        clearWarnings();
    state.settings.paused = value;
    tick(now, Reconcile::Resume);
    return true;
}
void Engine::cancelTaskWarning(const std::string &id, Snapshot now, const std::string &detail) {
    auto remove = [&](Group &group) {
        std::vector<std::string> affected;
        for (const auto &p : state.pending)
            if (p.taskId == id && std::find(group.keys.begin(), group.keys.end(), p.key) != group.keys.end())
                affected.push_back(p.key);
        for (const auto &key : affected) {
            finish(key, Status::Cancelled, now, detail);
            std::erase(group.keys, key);
        }
    };
    if (active) {
        remove(*active);
        if (active->keys.empty())
            active.reset();
    }
    for (auto &group : otherWarnings)
        remove(group);
    std::erase_if(otherWarnings, [](const auto &group) { return group.keys.empty(); });
    promoteWarning();
}
void Engine::upsert(Task value, Snapshot now) {
    if (auto *existing = task(value.id)) {
        bool changed = existing->kind != value.kind;
        if (!changed) {
            if (value.kind == Kind::Weekly)
                changed = existing->minute != value.minute || existing->weekdays != value.weekdays;
            else
                changed = existing->onceDue != value.onceDue;
        }
        if (changed) {
            cancelTaskWarning(value.id, now, "编辑任务");
            value.revision = existing->revision + 1;
            value.lastHandledDay = -2000000;
            value.completed = false;
            std::erase_if(state.pending, [&](const auto &p) { return p.taskId == value.id; });
        } else {
            value.revision = existing->revision;
            value.lastHandledDay = existing->lastHandledDay;
            value.completed = existing->completed;
        }
        *existing = std::move(value);
        if (!existing->enabled) {
            std::string id = existing->id;
            cancelTaskWarning(id, now, "停用任务");
            std::erase_if(state.pending, [&](const auto &p) { return p.taskId == id; });
        }
    } else
        state.tasks.push_back(std::move(value));
    fill(now);
}
void Engine::erase(const std::string &id, Snapshot now) {
    cancelTaskWarning(id, now, "删除任务");
    std::erase_if(state.pending, [&](const auto &p) { return p.taskId == id; });
    std::erase_if(state.tasks, [&](const auto &t) { return t.id == id; });
}
void Engine::enable(const std::string &id, bool value, Snapshot now) {
    auto *t = task(id);
    if (!t)
        return;
    t->enabled = value;
    if (!value) {
        cancelTaskWarning(id, now, "停用任务");
        std::erase_if(state.pending, [&](const auto &p) { return p.taskId == id; });
    } else
        fill(now);
}
void Engine::result(const std::vector<std::string> &keys, bool success, const std::string &detail) {
    for (auto &l : state.logs)
        if (l.status == Status::Requesting && std::find(keys.begin(), keys.end(), l.key) != keys.end()) {
            l.status = success ? Status::Requested : Status::Failed;
            l.detail = detail;
        }
}
Time Engine::nextWait(Snapshot now) const {
    Time wait = Day;
    if (!state.settings.paused) {
        if (active)
            wait = std::clamp(active->monotonicDue - now.steady, Time{20}, Time{1000});
        for (const auto &g : otherWarnings)
            wait = std::min(wait, std::clamp(g.monotonicDue - now.steady, Time{20}, Time{1000}));
    }
    for (const auto &p : state.pending) {
        if (p.warned && !state.settings.paused)
            continue;
        Time until = p.due - now.wall - (state.settings.paused ? 0 : Minute);
        wait = std::min(wait, std::max(Time{20}, until));
    }
    return wait;
}
const Occurrence *Engine::next() const {
    if (state.pending.empty())
        return nullptr;
    return &*std::min_element(state.pending.begin(), state.pending.end(),
                              [](const auto &a, const auto &b) { return a.due < b.due; });
}
Time Engine::remaining(Snapshot now) const {
    return active ? std::max(Time{0}, active->monotonicDue - now.steady) : 0;
}
} // namespace evening
