// This independently linked target fixture is never run by the verifier.
#include "sqlite3.h"
#include <string.h>

#if !defined(__ANDROID__) || !defined(__aarch64__) || __ANDROID_API__ != 28
#error Android ARM64 API 28 required
#endif
_Static_assert(SQLITE_VERSION_NUMBER == 3054001, "exact SQLite version required");

static int text_is(sqlite3 *db, const char *sql, const char *expected) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)!=SQLITE_OK) return 0;
    int ok = 0;
    if (sqlite3_step(stmt)==SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt,0);
        ok = text && strcmp((const char*)text,expected)==0 && sqlite3_step(stmt)==SQLITE_DONE;
    }
    return sqlite3_finalize(stmt)==SQLITE_OK && ok;
}

int main(int argc, char **argv) {
    // For a future separately authorized runtime only: one disposable app path.
    if (argc!=2 || sqlite3_libversion_number()!=SQLITE_VERSION_NUMBER ||
        strcmp(sqlite3_sourceid(),SQLITE_SOURCE_ID)!=0 || !sqlite3_threadsafe() ||
        !sqlite3_compileoption_used("OMIT_LOAD_EXTENSION")) return 1;
    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_PRIVATECACHE;
    if (sqlite3_open_v2(argv[1],&db,flags,NULL)!=SQLITE_OK) {
        sqlite3_close(db);
        return 2;
    }
    int ok = text_is(db,"PRAGMA journal_mode=WAL","wal") &&
        sqlite3_exec(db,"PRAGMA synchronous=FULL; CREATE TABLE probe(v TEXT);"
                        "BEGIN; INSERT INTO probe VALUES('synthetic'); COMMIT;",
                     NULL,NULL,NULL)==SQLITE_OK &&
        sqlite3_wal_checkpoint_v2(db,NULL,SQLITE_CHECKPOINT_FULL,NULL,NULL)==SQLITE_OK;
    if (sqlite3_close(db)!=SQLITE_OK || !ok) return 3;
    db = NULL;
    if (sqlite3_open_v2(argv[1],&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_PRIVATECACHE,NULL)!=SQLITE_OK) {
        sqlite3_close(db);
        return 4;
    }
    ok = text_is(db,"PRAGMA journal_mode","wal") && text_is(db,"SELECT v FROM probe","synthetic");
    return sqlite3_close(db)==SQLITE_OK && ok ? 0 : 5;
}
