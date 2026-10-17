#include "Database.h"
#include "domain/Types.h"
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

namespace fv {
Database::Database(const QString &root) : root_(root), connection_(newId()) {
    if (!QDir().mkpath(root_))
        fail("无法创建数据目录。");
    db_ = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db_.setDatabaseName(root_ + "/facevitals.sqlite3");
    if (!db_.open())
        fail(db_.lastError().text());
    execute("PRAGMA journal_mode=WAL");
    execute("PRAGMA busy_timeout=5000");
    execute("PRAGMA foreign_keys=ON");
    execute("CREATE TABLE IF NOT EXISTS users(id TEXT PRIMARY KEY,name TEXT UNIQUE NOT NULL,salt BLOB NOT "
            "NULL,digest BLOB NOT NULL,role TEXT NOT NULL,enabled INTEGER NOT NULL DEFAULT 1)");
    execute("CREATE TABLE IF NOT EXISTS attempts(name TEXT PRIMARY KEY,count INTEGER,until_time INTEGER)");
    execute("CREATE TABLE IF NOT EXISTS audit(id INTEGER PRIMARY KEY,time TEXT,actor TEXT,action TEXT,detail "
            "TEXT)");
    execute("CREATE TABLE IF NOT EXISTS measurements(id TEXT PRIMARY KEY,owner TEXT,subject TEXT,created "
            "TEXT,status TEXT,payload TEXT)");
    execute("CREATE TABLE IF NOT EXISTS models(id TEXT PRIMARY KEY,created TEXT,path TEXT,manifest TEXT)");
    execute("CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT)");
}
Database::~Database() {
    db_.close();
    db_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection_);
}
void Database::execute(const QString &sql, const QVariantList &args) {
    QSqlQuery q(db_);
    if (!q.prepare(sql))
        fail(q.lastError().text());
    for (const auto &a : args)
        q.addBindValue(a);
    if (!q.exec())
        fail(q.lastError().text());
}
QVector<QVariantMap> Database::rows(const QString &sql, const QVariantList &args) const {
    QSqlQuery q(db_);
    if (!q.prepare(sql))
        fail(q.lastError().text());
    for (const auto &a : args)
        q.addBindValue(a);
    if (!q.exec())
        fail(q.lastError().text());
    QVector<QVariantMap> result;
    while (q.next()) {
        QVariantMap row;
        for (int i = 0; i < q.record().count(); ++i)
            row[q.record().fieldName(i)] = q.value(i);
        result.append(row);
    }
    return result;
}
QVariant Database::scalar(const QString &sql, const QVariantList &args) const {
    QSqlQuery q(db_);
    q.prepare(sql);
    for (const auto &a : args)
        q.addBindValue(a);
    if (!q.exec())
        fail(q.lastError().text());
    return q.next() ? q.value(0) : QVariant();
}
void Database::audit(const QString &actor, const QString &action, const QString &detail) {
    execute("INSERT INTO audit(time,actor,action,detail) VALUES(?,?,?,?)", {utcNow(), actor, action, detail});
}
} // namespace fv
