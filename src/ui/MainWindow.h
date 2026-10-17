#pragma once
#include "PlotWidget.h"
#include "EmotionRadar.h"
#include "capture/CaptureWorker.h"
#include "services/MeasurementRepository.h"
#include "services/ModelRegistry.h"
#include <QDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <functional>

namespace fv {
QString loginDialog(AuthService &auth, QWidget *parent = nullptr);
class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    MainWindow(Database &db, AuthService &auth, QString token, QString assets, QWidget *parent = nullptr);
    ~MainWindow() override;
    void startSyntheticForTest(int seconds);

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    QWidget *measurePage();
    QWidget *historyPage();
    QWidget *modelPage();
    QWidget *usersPage();
    QWidget *auditPage();
    void protect(const std::function<void()> &action);
    void refreshCameras();
    void refreshHistory();
    void refreshModels();
    void refreshUsers();
    void start(int maximumSeconds = 0);
    void updatePreview();
    void finish(Measurement m);
    QString selected(QTableWidget *table) const;
    Database &db_;
    AuthService &auth_;
    QString token_, assets_;
    MeasurementRepository repository_;
    ModelRegistry registry_;
    QPointer<CaptureWorker> capture_;
    QLabel *video_ = nullptr, *hint_ = nullptr, *hr_ = nullptr, *emotion_ = nullptr, *active_ = nullptr;
    QComboBox *cameras_ = nullptr, *cameraProfile_ = nullptr;
    QLabel *elapsed_ = nullptr;
    QLineEdit *subject_ = nullptr;
    QCheckBox *simulation_ = nullptr, *recording_ = nullptr;
    QPushButton *start_ = nullptr, *stop_ = nullptr;
    PlotWidget *plot_ = nullptr;
    EmotionRadar *radar_ = nullptr;
    QTableWidget *history_ = nullptr, *models_ = nullptr, *users_ = nullptr, *audits_ = nullptr;
    QLineEdit *newName_ = nullptr, *newPassword_ = nullptr;
    QComboBox *newRole_ = nullptr;
    QDialog *settings_ = nullptr;
    QTimer timer_;
    QString lastReport_;
    bool closePending_ = false;
};
} // namespace fv
