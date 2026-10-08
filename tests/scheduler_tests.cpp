#include "core.hpp"
#include <iostream>
#include <stdexcept>
using namespace evening;
struct UTC : Calendar {
    int gapDay = -999999, gapMinute = -1;
    LocalStamp local(Time value) const override {
        int d = static_cast<int>(value / Day);
        return {d, static_cast<int>((value % Day) / Minute), weekday(d)};
    }
    std::optional<Time> resolve(int d, int m) const override {
        if (d == gapDay && m == gapMinute)
            return {};
        return d * Day + m * Minute;
    }
};
int checks = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok)
        throw std::runtime_error(label);
}
Task weekly(std::string id, int minute, int mask = 127) {
    Task t;
    t.id = id;
    t.name = id;
    t.minute = minute;
    t.weekdays = mask;
    return t;
}
Task once(std::string id, Time due) {
    Task t;
    t.id = id;
    t.name = id;
    t.kind = Kind::Once;
    t.onceDue = due;
    t.day = static_cast<int>(due / Day);
    t.minute = static_cast<int>((due % Day) / Minute);
    return t;
}
Snapshot at(int d, int minute, Time extra = 0) {
    return {d * Day + minute * Minute + extra, d * Day + minute * Minute + extra};
}
int main() {
    try {
        UTC cal;
        int d = civilDay(2026, 10, 8);
        {
            Engine e(cal);
            e.upsert(weekly("daily", 1380), at(d, 1200));
            check(e.next()->due == at(d, 1380).wall, "daily due");
            auto r = e.tick(at(d, 1379));
            check(r.warningOpened && e.active.has_value(), "60 second warning");
            check(e.cancel(at(d, 1379, 10000)), "cancel");
            check(e.next()->due == at(d + 1, 1380).wall, "cancel only this occurrence");
            Engine restarted(cal);
            restarted.state = e.state;
            restarted.tick(at(d, 1379, 20000), Reconcile::Startup);
            check(!restarted.active && restarted.next()->due == at(d + 1, 1380).wall,
                  "cancel survives restart");
            restarted.tick(at(d - 1, 1379), Reconcile::TimeChange);
            check(!restarted.active, "rollback does not resurrect cancellation");
        }
        {
            Engine e(cal);
            e.upsert(once("early", at(d, 1380).wall), at(d, 1200));
            e.upsert(once("later", at(d, 1380, 30000).wall), at(d, 1200));
            e.tick(at(d, 1379));
            e.tick(at(d, 1379, 30000));
            check(e.otherWarnings.size() == 1, "adjacent tasks warn independently");
            auto laterDeadline = e.otherWarnings.front().monotonicDue;
            e.cancel(at(d, 1379, 59000));
            check(e.active && e.active->monotonicDue == laterDeadline &&
                      e.remaining(at(d, 1379, 59000)) == 31000,
                  "cancel does not delay adjacent task");
            check(e.tick(at(d, 1380, 29999)).dispatch.empty(), "adjacent target not early");
            check(e.tick(at(d, 1380, 30000)).dispatch.size() == 1, "adjacent target retained");
        }
        {
            Engine e(cal);
            e.upsert(once("early", at(d, 1380).wall), at(d, 1200));
            e.upsert(once("later", at(d, 1380, 30000).wall), at(d, 1200));
            e.tick(at(d, 1379));
            e.tick(at(d, 1379, 30000));
            auto laterDeadline = e.otherWarnings.front().monotonicDue;
            e.snooze(at(d, 1379, 40000), 10);
            check(e.active && e.active->monotonicDue == laterDeadline, "snooze keeps adjacent deadline");
            e.pause(at(d, 1379, 45000), true);
            check(!e.active && e.otherWarnings.empty(), "pause clears all concurrent alerts");
        }
        {
            Engine e(cal);
            e.upsert(once("early", at(d, 1380).wall), at(d, 1200));
            e.upsert(once("later", at(d, 1380, 30000).wall), at(d, 1200));
            e.tick(at(d, 1379));
            e.tick(at(d, 1379, 30000));
            auto earlyDeadline = e.active->monotonicDue;
            check(e.cycleWarning() && e.active->keys.front().starts_with("later/"),
                  "select another warning group");
            check(e.otherWarnings.front().monotonicDue == earlyDeadline, "switch preserves earlier deadline");
            auto result = e.tick(at(d, 1380));
            check(result.dispatch.size() == 1 && result.dispatch.front().starts_with("early/") && e.active &&
                      e.active->keys.front().starts_with("later/"),
                  "nonselected warning still dispatches on time");
            e.enable("later", false, at(d, 1380, 10000));
            check(!e.active && e.otherWarnings.empty(), "disable removes concurrent warning");
        }
        {
            UTC zone;
            auto t = once("zone-gap", at(d, 120).wall);
            Engine e(zone);
            e.upsert(t, at(d, 60));
            zone.gapDay = d;
            zone.gapMinute = 120;
            e.tick(at(d, 60), Reconcile::TimeChange);
            check(e.task("zone-gap")->completed && !e.active && e.state.logs.front().status == Status::Missed,
                  "one-off gap after timezone change skips safely");
        }
        {
            Engine e(cal);
            e.upsert(weekly("a", 1380), at(d, 1200));
            e.upsert(weekly("b", 1380), at(d, 1200));
            e.tick(at(d, 1379));
            check(e.active->keys.size() == 2, "same-time group");
            e.snooze(at(d, 1379, 5000), 10);
            check(e.state.pending.size() == 4, "snooze preserves next repetition");
            check(e.task("a")->minute == 1380, "snooze keeps schedule");
            e.tick(at(d, 1388, 5000));
            check(e.active && e.active->keys.size() == 2, "snoozed group warning");
            auto r = e.tick(at(d, 1389, 5000));
            check(r.dispatch.size() == 2, "group dispatch");
            check(e.state.pending.size() == 2, "future tasks preserved");
            check(e.tick(at(d, 1389, 6000)).dispatch.empty(), "dispatch not repeated");
            e.result(r.dispatch, true, "simulation");
            check(e.state.logs[0].status == Status::Requested, "request result");
            check(e.state.logs[2].status == Status::Snoozed, "snooze history kept");
        }
        {
            Engine e(cal);
            e.upsert(once("once", at(d, 1380).wall), at(d, 1200));
            e.tick(at(d, 1379));
            e.cancel(at(d, 1379, 59999));
            check(e.tick(at(d, 1380)).dispatch.empty(), "last-millisecond cancel");
            check(e.task("once")->completed && e.state.pending.empty(), "one-off completes");
        }
        {
            Engine e(cal);
            e.upsert(weekly("work", 1380, 31), at(d + 1, 1381));
            check(cal.local(e.next()->due).weekday == 0, "Friday schedules Monday");
            Engine w(cal);
            w.upsert(weekly("weekend", 60, 96), at(d + 1, 1380));
            check(cal.local(w.next()->due).weekday == 5, "weekend Saturday");
        }
        {
            Engine e(cal);
            e.upsert(weekly("miss", 1380), at(d, 1200));
            e.tick(at(d, 1381), Reconcile::Resume);
            check(!e.active && e.next()->due == at(d + 1, 1380).wall, "sleep overshoot skipped");
            check(e.state.logs.front().status == Status::Missed, "miss recorded");
            Engine one(cal);
            one.upsert(once("past", at(d, 1380).wall), at(d, 1381));
            check(one.task("past")->completed, "past once missed");
        }
        {
            Engine e(cal);
            e.upsert(once("near", at(d, 1380).wall), at(d, 1379, 30000));
            auto r = e.tick(at(d, 1379, 30000), Reconcile::Startup);
            check(r.warningOpened && e.remaining(at(d, 1379, 30000)) == Minute, "startup grants full minute");
            check(e.tick(at(d, 1380)).dispatch.empty(), "not premature");
            check(e.tick(at(d, 1380, 30000)).dispatch.size() == 1, "extended warning dispatch");
        }
        {
            Engine e(cal);
            e.upsert(once("pause-future", at(d, 1380).wall), at(d, 1200));
            e.tick(at(d, 1379));
            e.pause(at(d, 1379, 10000), true);
            check(!e.task("pause-future")->completed && !e.active, "pause retains a future one-off task");
            e.pause(at(d, 1379, 20000), false);
            check(e.active && e.remaining(at(d, 1379, 20000)) == Minute,
                  "resume before target gives full warning");
            e.pause(at(d, 1379, 25000), true);
            e.pause(at(d, 1381), false);
            check(e.task("pause-future")->completed && !e.active &&
                      e.state.logs.front().status == Status::Missed,
                  "resume after target skips paused occurrence");
        }
        {
            Engine e(cal);
            e.upsert(weekly("pause", 1380), at(d, 1200));
            e.tick(at(d, 1379));
            e.pause(at(d, 1379, 1000), true);
            check(!e.active && e.state.settings.paused, "pause clears warning");
            e.tick(at(d + 1, 1381));
            e.pause(at(d + 1, 1381), false);
            check(!e.active && e.next()->due == at(d + 2, 1380).wall, "pause no catchup");
        }
        {
            Engine e(cal);
            e.upsert(weekly("edit", 1380), at(d, 1200));
            e.tick(at(d, 1379));
            auto t = *e.task("edit");
            t.minute = 1390;
            e.upsert(t, at(d, 1379, 1000));
            check(!e.active && e.next()->due == at(d, 1390).wall, "edit clears warning");
            e.enable("edit", false, at(d, 1380));
            check(e.state.pending.empty(), "disable clears");
            e.enable("edit", true, at(d, 1380));
            check(e.next()->due == at(d, 1390).wall, "reenable future");
            e.erase("edit", at(d, 1380));
            check(e.state.tasks.empty() && e.state.pending.empty(), "delete");
        }
        {
            Engine e(cal);
            auto t = once("count", at(d, 1380).wall);
            t.kind = Kind::Countdown;
            e.upsert(t, at(d, 1370));
            Snapshot jump{at(d, 1370).wall + 10 * Day, at(d, 1370).steady};
            e.tick(jump, Reconcile::TimeChange);
            check(e.next()->due - jump.wall == 10 * Minute, "countdown survives clock jump");
            e.tick({jump.wall + 11 * Minute, jump.steady + 11 * Minute}, Reconcile::Resume);
            check(e.task("count")->completed && !e.active, "expired countdown skipped");
        }
        {
            UTC dst;
            dst.gapDay = d;
            dst.gapMinute = 120;
            Engine e(dst);
            e.upsert(weekly("dst", 120), at(d, 60));
            check(e.next()->due == at(d + 1, 120).wall, "DST gap skipped");
        }
        {
            Engine e(cal);
            e.upsert(once("a", at(d, 1380).wall), at(d, 1200));
            e.upsert(once("b", at(d, 1380, 30000).wall), at(d, 1200));
            e.tick(at(d, 1379));
            e.cancel(at(d, 1379, 40000));
            e.tick(at(d, 1379, 40000));
            check(e.active && e.active->keys.size() == 1, "nearby remains");
            check(e.remaining(at(d, 1379, 40000)) == Minute, "nearby full minute");
        }
        {
            Engine e(cal);
            e.upsert(weekly("a", 1380), at(d, 1200));
            e.upsert(weekly("b", 1380), at(d, 1200));
            e.tick(at(d, 1379));
            auto t = *e.task("a");
            t.minute = 1390;
            e.upsert(t, at(d, 1379, 1000));
            check(e.active && e.active->keys.size() == 1, "editing one grouped task preserves the other");
            auto r = e.tick(at(d, 1380));
            check(r.dispatch.size() == 1, "other grouped task still dispatches");
            e.tick(at(d, 1389));
            e.erase("a", at(d, 1389, 1000));
            check(!e.active, "delete active task");
        }
        {
            Engine e(cal);
            e.upsert(weekly("a", 1380), at(d, 1200));
            e.upsert(weekly("b", 1380), at(d, 1200));
            e.tick(at(d, 1379));
            e.enable("a", false, at(d, 1379, 1000));
            check(e.active && e.active->keys.size() == 1, "disabling one grouped task preserves the other");
        }
        {
            Engine e(cal);
            e.upsert(weekly("rename", 1380), at(d, 1200));
            e.tick(at(d, 1379));
            e.cancel(at(d, 1379, 1000));
            auto t = *e.task("rename");
            t.name = "new name";
            e.upsert(t, at(d, 1379, 2000));
            check(e.next()->due == at(d + 1, 1380).wall, "rename preserves cancellation");
            e.tick(at(d + 1, 1379));
            auto deadline = e.active->monotonicDue;
            t = *e.task("rename");
            t.name = "another name";
            e.upsert(t, at(d + 1, 1379, 1000));
            check(e.active && e.active->monotonicDue == deadline, "rename preserves active countdown");
        }
        {
            struct Offset : Calendar {
                Time offset = 0;
                LocalStamp local(Time value) const override {
                    Time v = value + offset;
                    int d = int(v / Day);
                    return {d, int(v % Day / Minute), weekday(d)};
                }
                std::optional<Time> resolve(int d, int m) const override {
                    return d * Day + m * Minute - offset;
                }
            } zone;
            zone.offset = 8 * 60 * Minute;
            Engine e(zone);
            auto t = once("zone", zone.resolve(d, 1380).value());
            t.day = d;
            t.minute = 1380;
            t.enabled = false;
            e.upsert(t, at(d, 600));
            zone.offset = -7 * 60 * Minute;
            e.tick(at(d, 600), Reconcile::Startup);
            e.enable("zone", true, at(d, 600));
            check(e.next() && e.next()->due == zone.resolve(d, 1380).value(),
                  "disabled one-off follows local zone on restart");
        }
        std::cout << "PASS: " << checks << " scheduler assertions\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
