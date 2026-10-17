#pragma once
#include <QSqlDatabase>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace fv {
class Database final {
  public:
    explicit Database(const QString &root);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    void execute(const QString &sql, const QVariantList &args = {});
    QVector<QVariantMap> rows(const QString &sql, const QVariantList &args = {}) const;
    QVariant scalar(const QString &sql, const QVariantList &args = {}) const;
    void audit(const QString &actor, const QString &action, const QString &detail = {});
    const QString &root() const {
        return root_;
    }

  private:
    QString root_, connection_;
    QSqlDatabase db_;
};
} // namespace fv
