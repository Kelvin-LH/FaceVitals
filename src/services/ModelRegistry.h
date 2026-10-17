#pragma once
#include "AuthService.h"

namespace fv {
class ModelRegistry final {
  public:
    ModelRegistry(Database &db, AuthService &auth) : db_(db), auth_(auth) {}
    QString importModel(const QString &token, const QString &directory);
    void activate(const QString &token, const QString &id);
    void ensureBundledModel(const QString &directory);
    QString activeId() const;
    QString activePath() const;
    QVector<QVariantMap> list(const QString &token) const;

  private:
    QString importVerified(const QString &actor, const QString &directory);
    void activateVerified(const QString &actor, const QString &id);
    Database &db_;
    AuthService &auth_;
};
} // namespace fv
