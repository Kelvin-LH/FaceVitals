#pragma once
#include "AuthService.h"

namespace fv {
class MeasurementRepository final {
  public:
    MeasurementRepository(Database &db, AuthService &auth) : db_(db), auth_(auth) {}
    Measurement begin(const QString &token, const QString &subject, const QString &source,
                      const QString &model, bool synthetic);
    void finish(const Measurement &measurement);
    QVector<QVariantMap> history(const QString &token) const;
    Measurement get(const QString &token, const QString &id) const;
    QVector<Measurement> recoverInterrupted();

  private:
    Database &db_;
    AuthService &auth_;
};
} // namespace fv
