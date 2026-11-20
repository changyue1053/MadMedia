#ifndef MADMEDIA_IOS_JOURNAL_PROBE_H
#define MADMEDIA_IOS_JOURNAL_PROBE_H
#include "core/journal/journal.h"

// The only task is a synthetic persisted row; no engine, scheduler or transfer.
inline bool exerciseInstalledJournal(const std::string& path) {
    using namespace immd;
    auto first=Journal::open(path,JournalDurability::Full);
    if (!first.ok()) return false; // production Journal validates WAL/FULL
    TaskRecord row{};
    row.task_id="fixture-only";
    row.kind=TaskKind::Upload;
    row.state=TaskState::Queued;
    row.bytes_done=0;
    if (first.value()->save(row)) return false; // SQLite autocommit transaction
    first.value().reset();
    auto next=Journal::open(path,JournalDurability::Full);
    if (!next.ok()) return false;
    auto loaded=next.value()->load(row.task_id);
    if (!loaded.ok() || loaded.value().task_id!=row.task_id ||
        loaded.value().state!=TaskState::Queued || loaded.value().bytes_done!=0) return false;
    row.state=TaskState::Ready;
    row.bytes_done=37;
    if (next.value()->save(row)) return false;
    next.value().reset();
    auto last=Journal::open(path,JournalDurability::Full);
    if (!last.ok()) return false;
    auto updated=last.value()->load(row.task_id);
    if (!updated.ok() || updated.value().state!=TaskState::Ready ||
        updated.value().bytes_done!=37 || last.value()->remove(row.task_id)) return false;
    last.value().reset();
    auto final=Journal::open(path,JournalDurability::Full);
    if (!final.ok()) return false;
    auto rows=final.value()->load_all();
    return rows.ok() && rows.value().empty();
}
#endif
