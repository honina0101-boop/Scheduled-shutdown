#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace evening {
using Time = int64_t; // milliseconds
constexpr Time Minute = 60000;
constexpr Time Day = 86400000;
enum class Kind { Once, Weekly, Countdown };
enum class Status { Cancelled, Snoozed, Missed, Requesting, Requested, Failed };
enum class Reconcile { Normal, Startup, Resume, TimeChange };
struct Snapshot {
    Time wall = 0, steady = 0;
};
struct LocalStamp {
    int day = 0, minute = 0, weekday = 0;
};
struct Calendar {
    virtual ~Calendar() = default;
    virtual LocalStamp local(Time utc) const = 0;
    virtual std::optional<Time> resolve(int day, int minute) const = 0;
};
inline int weekday(int day) {
    return ((day + 3) % 7 + 7) % 7;
}
inline int civilDay(int year, unsigned month, unsigned day) {
    using namespace std::chrono;
    return static_cast<int>(
        sys_days{year_month_day{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}}}
            .time_since_epoch()
            .count());
}
struct Task {
    std::string id, name;
    int revision = 1;
    Kind kind = Kind::Weekly;
    bool enabled = true, completed = false;
    int day = 0, minute = 23 * 60, weekdays = 127, minutes = 60;
    int lastHandledDay = -2000000;
    Time onceDue = 0;
};
struct Occurrence {
    std::string key, taskId;
    int revision = 1, day = 0;
    Time originalDue = 0, due = 0;
    bool detached = false;
    // Runtime-only. Never restore an expired warning after a restart.
    Time monotonicDue = 0;
    bool warned = false;
};
struct Log {
    std::string key, name, detail;
    Time at = 0, due = 0;
    Status status = Status::Missed;
};
struct Settings {
    bool paused = false, autostart = false, sound = true, force = false;
    std::string theme = "system";
};
struct State {
    Settings settings;
    std::vector<Task> tasks;
    std::vector<Occurrence> pending;
    std::vector<Log> logs;
};
struct Group {
    std::vector<std::string> keys;
    Time due = 0, monotonicDue = 0;
};
struct TickResult {
    bool changed = false, warningOpened = false;
    std::vector<std::string> dispatch;
};

class Engine {
    const Calendar *calendar_;
    bool finish(const std::string &key, Status status, Snapshot now, const std::string &detail);
    void append(const Occurrence &p, Status s, Snapshot now, const std::string &detail = {});
    bool fill(Snapshot now);
    void cancelTaskWarning(const std::string &id, Snapshot now, const std::string &detail);
    void promoteWarning();

  public:
    State state;
    std::optional<Group> active;
    std::vector<Group> otherWarnings;
    explicit Engine(const Calendar &calendar) : calendar_(&calendar) {}
    Task *task(const std::string &id);
    const Task *task(const std::string &id) const;
    TickResult tick(Snapshot now, Reconcile reason = Reconcile::Normal);
    bool cancel(Snapshot now, const std::string &detail = {});
    bool snooze(Snapshot now, int minutes);
    bool pause(Snapshot now, bool value);
    void upsert(Task value, Snapshot now);
    void erase(const std::string &id, Snapshot now);
    void enable(const std::string &id, bool value, Snapshot now);
    void result(const std::vector<std::string> &keys, bool success, const std::string &detail);
    Time nextWait(Snapshot now) const;
    const Occurrence *next() const;
    Time remaining(Snapshot now) const;
    bool cycleWarning();
    void clearWarnings();
};
const char *kindName(Kind kind);
const char *statusName(Status status);
} // namespace evening
