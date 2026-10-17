#include "MeasurementRepository.h"
#include <QJsonDocument>

namespace fv {
Measurement MeasurementRepository::begin(const QString &token, const QString &subject, const QString &source,
                                         const QString &model, bool synthetic) {
    auto u = auth_.require(token, {"admin", "operator", "user"});
    if (subject.trimmed().isEmpty() || subject.size() > 100)
        fail("请填写 1—100 字符的受试者代号。");
    Measurement m;
    m.id = newId();
    m.owner = u.id;
    m.subject = subject.trimmed();
    m.created = utcNow();
    m.source = source;
    m.modelId = model;
    m.synthetic = synthetic;
    db_.execute("INSERT INTO measurements VALUES(?,?,?,?,?,?)",
                {m.id, m.owner, m.subject, m.created, m.status,
                 QString::fromUtf8(QJsonDocument(m.json()).toJson(QJsonDocument::Compact))});
    db_.audit(u.id, "measurement_start", m.id);
    return m;
}
void MeasurementRepository::finish(const Measurement &m) {
    if (!QStringList{"completed", "partial", "cancelled", "failed"}.contains(m.status))
        fail("测量终态无效。");
    auto old = db_.rows("SELECT owner,status FROM measurements WHERE id=?", {m.id});
    if (old.isEmpty() || old[0]["owner"].toString() != m.owner)
        fail("测量记录不匹配。");
    if (old[0]["status"].toString() != "measuring")
        return;
    db_.execute("UPDATE measurements SET status=?,payload=? WHERE id=?",
                {m.status, QString::fromUtf8(QJsonDocument(m.json()).toJson(QJsonDocument::Compact)), m.id});
    db_.audit(m.owner, "measurement_finish", m.id + ":" + m.status);
}
QVector<QVariantMap> MeasurementRepository::history(const QString &token) const {
    auto u = auth_.require(token);
    QString sql = "SELECT id,subject,created,status FROM measurements";
    if (u.role == "admin")
        return db_.rows(sql + " ORDER BY created DESC");
    return db_.rows(sql + " WHERE owner=? ORDER BY created DESC", {u.id});
}
Measurement MeasurementRepository::get(const QString &token, const QString &id) const {
    auto u = auth_.require(token);
    auto rows = db_.rows("SELECT owner,payload FROM measurements WHERE id=?", {id});
    if (rows.isEmpty() || (u.role != "admin" && u.id != rows[0]["owner"].toString()))
        fail("无权访问此测量。");
    return Measurement::fromJson(QJsonDocument::fromJson(rows[0]["payload"].toString().toUtf8()).object());
}
QVector<Measurement> MeasurementRepository::recoverInterrupted() {
    QVector<Measurement> recovered;
    for (auto row : db_.rows("SELECT payload FROM measurements WHERE status='measuring'")) {
        auto m = Measurement::fromJson(QJsonDocument::fromJson(row["payload"].toString().toUtf8()).object());
        m.status = "failed";
        m.reason = "上次运行中断，信号未完整保存。";
        m.ended = utcNow();
        finish(m);
        recovered.append(m);
    }
    return recovered;
}
} // namespace fv
