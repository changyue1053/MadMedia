#ifndef MADMEDIA_IOS_JOURNAL_ROLLBACK_H
#define MADMEDIA_IOS_JOURNAL_ROLLBACK_H
#include "core/journal/journal.h"
#include <sqlite3.h>

// Private synthetic fixture only. The app creates a unique directory before
// calling this helper; no product schema or persisted user data is modified.
inline bool exerciseInstalledJournalRollback(const std::string& path) {
    using namespace immd;
    auto seed=Journal::open(path,JournalDurability::Full);
    if (!seed.ok()) return false;
    TaskRecord row{};
    row.task_id="rollback-fixture";
    row.kind=TaskKind::Upload;
    row.state=TaskState::Queued;
    row.bytes_done=11;
    if (seed.value()->save(row)) return false;
    seed.value().reset();

    // Create a deterministic rejection in the exact existing journal schema.
    // Bootstrap is closed before the Journal prepares its upsert statement.
    sqlite3 *bootstrap=nullptr;
    int opened=sqlite3_open_v2(path.c_str(),&bootstrap,SQLITE_OPEN_READWRITE,nullptr);
    const char *sql=R"sql(
CREATE TRIGGER reject_fixture_update BEFORE UPDATE ON transfer_task
WHEN NEW.task_id='rollback-fixture' AND NEW.bytes_done=99
BEGIN SELECT RAISE(ABORT,'synthetic rejection'); END;
)sql";
    const bool configured=opened==SQLITE_OK &&
        sqlite3_exec(bootstrap,sql,nullptr,nullptr,nullptr)==SQLITE_OK;
    const int closed=bootstrap?sqlite3_close(bootstrap):SQLITE_OK;
    if (!configured || closed!=SQLITE_OK) return false;

    auto journal=Journal::open(path,JournalDurability::Full);
    if (!journal.ok()) return false;
    row.state=TaskState::Ready;
    row.bytes_done=99;
    if (journal.value()->save(row).code!=Err::Io) return false;
    auto prior=journal.value()->load(row.task_id);
    if (!prior.ok() || prior.value().task_id!=row.task_id ||
        prior.value().bytes_done!=11 || prior.value().state!=TaskState::Queued) return false;

    // No reopen, trigger deletion or statement replacement before this save.
    // Only the value changes, so the very same Journal/upsert must recover.
    row.bytes_done=37;
    if (journal.value()->save(row)) return false;
    journal.value().reset();
    auto reopened=Journal::open(path,JournalDurability::Full);
    if (!reopened.ok()) return false;
    auto rows=reopened.value()->load_all();
    if (!rows.ok() || rows.value().size()!=1) return false;
    const auto& saved=rows.value().front();
    return saved.task_id==row.task_id && saved.bytes_done==37 && saved.state==TaskState::Ready;
}
#endif
