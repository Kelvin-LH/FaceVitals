#pragma once
#include "Database.h"
#include "domain/Types.h"
#include <QMap>

namespace fv {
class AuthService final {
  public:
    explicit AuthService(Database &db) : db_(db) {}
    bool needsSetup() const;
    void bootstrap(const QString &name, const QString &password);
    QString login(const QString &name, const QString &password);
    void logout(const QString &token);
    User require(const QString &token, const QStringList &roles = {}) const;
    void createUser(const QString &token, const QString &name, const QString &password, const QString &role);
    QVector<QVariantMap> users(const QString &token) const;
    void setEnabled(const QString &token, const QString &id, bool enabled);
    void changePassword(const QString &token, const QString &oldPassword, const QString &newPassword);

  private:
    void insert(const QString &name, const QString &password, const QString &role);
    static QByteArray derive(const QString &password, const QByteArray &salt);
    Database &db_;
    QMap<QString, QPair<QString, qint64>> sessions_;
};
} // namespace fv
