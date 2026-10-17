#include "AuthService.h"
#include <QPasswordDigestor>
#include <QRandomGenerator>

namespace fv {
namespace {
bool constantEqual(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size())
        return false;
    unsigned char different = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        different |= static_cast<unsigned char>(a[i] ^ b[i]);
    return different == 0;
}
} // namespace
QByteArray AuthService::derive(const QString &p, const QByteArray &salt) {
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, p.toUtf8(), salt, 600000, 32);
}
bool AuthService::needsSetup() const {
    return db_.scalar("SELECT count(*) FROM users").toInt() == 0;
}
void AuthService::insert(const QString &name, const QString &password, const QString &role) {
    if (name.trimmed().isEmpty() || name.size() > 64 || password.size() < 10 || password.size() > 256 ||
        !QStringList{"admin", "operator", "researcher", "user"}.contains(role))
        fail("用户名 1—64 字符，密码 10—256 字符；角色必须有效。");
    QByteArray salt(32, '\0');
    for (int i = 0; i < 32; ++i)
        salt[i] = static_cast<char>(QRandomGenerator::system()->generate() & 255);
    if (db_.scalar("SELECT count(*) FROM users WHERE name=?", {name.trimmed()}).toInt())
        fail("用户名已存在。");
    db_.execute("INSERT INTO users(id,name,salt,digest,role) VALUES(?,?,?,?,?)",
                {newId(), name.trimmed(), salt, derive(password, salt), role});
}
void AuthService::bootstrap(const QString &name, const QString &password) {
    if (!needsSetup())
        fail("管理员已经创建。");
    insert(name, password, "admin");
    db_.audit("setup", "create_admin", name);
}
QString AuthService::login(const QString &input, const QString &password) {
    const QString name = input.trimmed();
    if (name.size() > 64 || password.size() > 256)
        fail("凭据无效。");
    const auto now = QDateTime::currentSecsSinceEpoch();
    auto attempts = db_.rows("SELECT * FROM attempts WHERE name=?", {name});
    if (!attempts.isEmpty() && attempts[0]["until_time"].toLongLong() > now)
        fail("失败次数过多，请 60 秒后重试。");
    auto rows = db_.rows("SELECT * FROM users WHERE name=?", {name});
    // 不存在的账号也执行同等 KDF，减少时序差异。
    QByteArray salt = rows.isEmpty() ? QByteArray(32, 'x') : rows[0]["salt"].toByteArray();
    QByteArray digest = derive(password, salt);
    if (rows.isEmpty() || !rows[0]["enabled"].toBool() ||
        !constantEqual(digest, rows[0]["digest"].toByteArray())) {
        int n = attempts.isEmpty() || attempts[0]["until_time"].toLongLong() > 0
                    ? 1
                    : attempts[0]["count"].toInt() + 1;
        db_.execute("INSERT OR REPLACE INTO attempts VALUES(?,?,?)", {name, n, n >= 5 ? now + 60 : 0});
        db_.audit(name, "login_failed");
        fail("用户名或密码错误，或账号已禁用。");
    }
    db_.execute("DELETE FROM attempts WHERE name=?", {name});
    QString token = newId() + newId();
    sessions_[token] = {rows[0]["id"].toString(), now + 8 * 3600};
    db_.audit(rows[0]["id"].toString(), "login");
    return token;
}
User AuthService::require(const QString &token, const QStringList &roles) const {
    if (!sessions_.contains(token) || sessions_[token].second < QDateTime::currentSecsSinceEpoch())
        fail("登录已失效。");
    auto rows = db_.rows("SELECT id,name,role,enabled FROM users WHERE id=?", {sessions_[token].first});
    if (rows.isEmpty() || !rows[0]["enabled"].toBool())
        fail("账号不可用。");
    const auto &u = rows[0];
    if (!roles.isEmpty() && !roles.contains(u["role"].toString()))
        fail("没有执行此操作的权限。");
    return {u["id"].toString(), u["name"].toString(), u["role"].toString(), true};
}
void AuthService::logout(const QString &token) {
    if (sessions_.contains(token))
        db_.audit(sessions_[token].first, "logout");
    sessions_.remove(token);
}
void AuthService::createUser(const QString &token, const QString &n, const QString &p, const QString &r) {
    auto actor = require(token, {"admin"});
    insert(n, p, r);
    db_.audit(actor.id, "create_user", n + ":" + r);
}
QVector<QVariantMap> AuthService::users(const QString &token) const {
    require(token, {"admin"});
    return db_.rows("SELECT id,name,role,enabled FROM users ORDER BY name");
}
void AuthService::setEnabled(const QString &token, const QString &id, bool enabled) {
    auto u = require(token, {"admin"});
    if (u.id == id)
        fail("不能禁用当前管理员自身。");
    db_.execute("UPDATE users SET enabled=? WHERE id=?", {enabled, id});
    db_.audit(u.id, "set_enabled", id + ":" + QString::number(enabled));
}
void AuthService::changePassword(const QString &token, const QString &oldPassword, const QString &p) {
    auto u = require(token);
    if (p.size() < 10 || p.size() > 256)
        fail("密码长度需要 10—256 字符。");
    auto r = db_.rows("SELECT salt,digest FROM users WHERE id=?", {u.id}).first();
    if (!constantEqual(derive(oldPassword, r["salt"].toByteArray()), r["digest"].toByteArray()))
        fail("原密码不正确。");
    QByteArray salt(32, '\0');
    for (int i = 0; i < 32; ++i)
        salt[i] = static_cast<char>(QRandomGenerator::system()->generate() & 255);
    db_.execute("UPDATE users SET salt=?,digest=? WHERE id=?", {salt, derive(p, salt), u.id});
    db_.audit(u.id, "password_changed");
    for (auto i = sessions_.begin(); i != sessions_.end();) {
        if (i.value().first == u.id && i.key() != token)
            i = sessions_.erase(i);
        else
            ++i;
    }
}
} // namespace fv
