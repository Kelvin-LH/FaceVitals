#include "MainWindow.h"
#include "capture/ICameraSource.h"
#include "reports/ReportWriter.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QListWidget>
#include <QTabWidget>
#include <QGridLayout>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QPainter>
#include <QSplitter>
#include <QStackedWidget>
#include <QUrl>

namespace fv {
namespace {
QString pulseLabel(const PulseResult &pulse, double elapsed) {
    const auto status = pulse.heartRate.status;
    if (status == "valid" || status == "recent") {
        const auto caption = status == "recent" ? QString("最近有效 · %1 秒前").arg(std::max(0.,elapsed-pulse.measuredAt),0,'f',1) : QString("心率 · bpm");
        return QString("<span style='color:%1'>%2</span><br/><span style='font-size:13px'>%3</span>")
            .arg(status=="recent"?"#f2c66e":"#51edcf").arg(pulse.heartRate.value,0,'f',1).arg(caption);
    }
    if (pulse.windowSeconds < 20) return QString("采集中<br/><span style='font-size:13px'>有效片段 %1 / 20 秒</span>").arg(pulse.windowSeconds,0,'f',1);
    return "待稳定<br/><span style='font-size:13px'>"+pulse.heartRate.reason.toHtmlEscaped()+"</span>";
}
QPushButton *button(const QString &text, QLayout *layout, const std::function<void()> &action) {
    auto *b = new QPushButton(text);
    layout->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, b, action);
    return b;
}
QLabel *title(const QString &text) {
    auto *l = new QLabel(text);
    l->setStyleSheet("font-size:22px;font-weight:600;color:#cbeef8;margin:6px 0;");
    return l;
}
QTableWidget *table(const QStringList &columns, QVBoxLayout *layout) {
    auto *t = new QTableWidget(0, columns.size());
    t->setHorizontalHeaderLabels(columns);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->verticalHeader()->hide();
    layout->addWidget(t);
    return t;
}
void fill(QTableWidget *t, const QVector<QVariantMap> &rows, const QStringList &keys) {
    t->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r)
        for (int c = 0; c < keys.size(); ++c)
            t->setItem(r, c, new QTableWidgetItem(rows[r].value(keys[c]).toString()));
}
QString elapsedText(double seconds) {
    const auto total = qint64(seconds);
    return QString("%1:%2:%3").arg(total / 3600, 2, 10, QChar('0')).arg(total / 60 % 60, 2, 10, QChar('0')).arg(total % 60, 2, 10, QChar('0'));
}
} // namespace
QString loginDialog(AuthService &auth, QWidget *parent) {
    QDialog dialog(parent);
    dialog.setStyleSheet("QDialog{background:#0c182a;}QLabel{color:#cbe5ef;}QLineEdit{background:#14283e;color:#edfaff;border:1px solid #38667b;padding:8px;}QPushButton{background:#176b7b;color:white;padding:8px 18px;}");
    dialog.setWindowTitle("FaceVitals · 本地账号");
    dialog.setMinimumWidth(410);
    auto *l = new QVBoxLayout(&dialog);
    bool setup = auth.needsSetup();
    l->addWidget(title(setup ? "创建首个管理员" : "登录 FaceVitals"));
    auto *info =
        new QLabel(setup ? "密码至少 10 个字符。账号与结果保存在本机。" : "原生桌面 · 数据保存在本机");
    info->setWordWrap(true);
    l->addWidget(info);
    QLineEdit name, password, confirm;
    password.setEchoMode(QLineEdit::Password);
    confirm.setEchoMode(QLineEdit::Password);
    auto *form = new QFormLayout;
    l->addLayout(form);
    form->addRow("账号", &name);
    form->addRow("密码", &password);
    if (setup)
        form->addRow("确认密码", &confirm);
    QLabel error;
    error.setWordWrap(true);
    error.setStyleSheet("color:#bb4b40");
    l->addWidget(&error);
    QDialogButtonBox actions(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    actions.button(QDialogButtonBox::Ok)->setText(setup ? "创建并登录" : "登录");
    l->addWidget(&actions);
    QString token;
    QObject::connect(&actions, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            if (setup) {
                if (password.text() != confirm.text())
                    fail("两次密码不一致。");
                auth.bootstrap(name.text(), password.text());
                setup = false;
            }
            token = auth.login(name.text(), password.text());
            dialog.accept();
        } catch (const std::exception &e) {
            error.setText(QString::fromUtf8(e.what()));
        }
    });
    QObject::connect(&actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
    return token;
}
MainWindow::MainWindow(Database &db, AuthService &auth, QString token, QString assets, QWidget *parent)
    : QMainWindow(parent), db_(db), auth_(auth), token_(std::move(token)), assets_(std::move(assets)),
      repository_(db, auth), registry_(db, auth) {
    setWindowTitle("FaceVitals · 面部表情与生理信号研究系统");
    const auto screen = QGuiApplication::primaryScreen()->availableGeometry();
    resize(std::min(1360, screen.width() - 40), std::min(990, screen.height() - 70));
    setMinimumSize(1100, 760);
    setStyleSheet(R"(
        QMainWindow,QDialog{background:#06102b;}
        QWidget{font-family:'Microsoft YaHei','Noto Sans CJK SC';font-size:13px;color:#d2e3f2;}
        QPushButton{background:#14273d;border:1px solid #28455f;border-radius:7px;padding:9px 14px;}
        QPushButton:hover{background:#1b3a50;border-color:#30d9d0;}
        QPushButton:disabled{color:#607288;background:#111e30;border-color:#1b2d43;}
        QLineEdit,QComboBox,QSpinBox,QTextEdit{background:#0d1b2e;border:1px solid #28435d;border-radius:6px;padding:8px;selection-background-color:#17687b;}
        QAbstractItemView{background:#0d1b2e;selection-background-color:#174b61;color:#d2e3f2;}
        QTableWidget{background:#0d1b2e;gridline-color:#1e334a;border:1px solid #28435d;}
        QHeaderView::section{background:#142a40;color:#72d8e8;padding:10px;border:0;}
        QGroupBox{background:#0d1b2e;border:1px solid #28435d;border-radius:8px;margin-top:14px;padding:12px;}
        QGroupBox::title{subcontrol-origin:margin;left:12px;color:#72d8e8;}
        QProgressBar{background:#111f33;border:1px solid #233a52;border-radius:4px;text-align:center;color:#c3eaf2;}
        QProgressBar::chunk{background:#176b82;border-radius:4px;}
        QCheckBox::indicator{width:15px;height:15px;border:1px solid #50687e;background:#0d1b2e;border-radius:3px;}
        QCheckBox::indicator:checked{background:#23c5be;border-color:#77ede1;}
        QToolTip{background:#163047;color:#e4f6ff;border:1px solid #357088;}
        QTabBar::tab{background:#0a1b40;color:#7ea8d8;padding:10px 28px;border-bottom:2px solid #183b69;}
        QTabBar::tab:selected{background:#113268;color:#b9f6ff;border-bottom:2px solid #39dcff;}
        QTabBar::tab:hover{color:white;background:#163767;}
        QTabWidget::pane{background:#0d1b2e;border:1px solid #28435d;}
        QWidget#settingsPage{background:#0d1b2e;}
    )");
    auto *center = new QWidget;
    setCentralWidget(center);
    auto *root = new QVBoxLayout(center);
    root->setContentsMargins(14, 10, 14, 10);
    root->setSpacing(8);
    auto *header = new QWidget;
    header->setStyleSheet("background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #0a1740,stop:0.5 #164b84,stop:1 #0a1740);border-bottom:1px solid #287db2;");
    auto *side = new QHBoxLayout(header);
    side->setContentsMargins(14, 7, 14, 7);
    auto *brand = new QLabel;
    brand->setAlignment(Qt::AlignCenter);
    brand->setPixmap(QPixmap(":/brand/facevitals.png").scaled(44, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    brand->setStyleSheet("background:transparent;border:0;");
    side->addWidget(brand);
    auto *logo = new QLabel("FACEVITALS  /  面部表情与生理信号监测");
    logo->setStyleSheet("font-size:23px;font-weight:700;color:#d5f6ff;border:0;background:transparent;");
    side->addWidget(logo, 1);
    auto user = auth_.require(token_);
    auto *who = new QLabel(user.name + " · " + user.role);
    who->setStyleSheet("color:#8fbce1;background:transparent;border:0;");
    who->setWordWrap(true);
    side->addWidget(who);
    button("退出登录", side, [this] {
        if (capture_) {
            QMessageBox::information(this, "任务运行中", "请先停止测量。");
            return;
        }
        auth_.logout(token_);
        close();
    });
    root->addWidget(header);
    auto *monitor = new QScrollArea;
    monitor->setWidgetResizable(true);
    monitor->setFrameShape(QFrame::NoFrame);
    monitor->setWidget(measurePage());
    root->addWidget(monitor, 1);
    settings_ = new QDialog(this);
    settings_->setObjectName("settingsDialog");
    settings_->setWindowTitle("设置");
    settings_->resize(1040, 680);
    auto *settingsLayout = new QVBoxLayout(settings_);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("settingsTabs");
    settingsLayout->addWidget(tabs);
    auto *account = new QWidget;
    auto *accountLayout = new QVBoxLayout(account);
    accountLayout->addWidget(title("账号"));
    accountLayout->addWidget(new QLabel(user.name + " · " + user.role));
    button("更改密码", accountLayout, [this] {
        protect([this] {
            QDialog d(this);
            d.setWindowTitle("修改密码");
            auto *f = new QFormLayout(&d);
            QLineEdit old, p, again;
            for (auto *x : {&old, &p, &again})
                x->setEchoMode(QLineEdit::Password);
            f->addRow("原密码", &old);
            f->addRow("新密码", &p);
            f->addRow("确认", &again);
            QDialogButtonBox b(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            f->addRow(&b);
            connect(&b, &QDialogButtonBox::accepted, &d, &QDialog::accept);
            connect(&b, &QDialogButtonBox::rejected, &d, &QDialog::reject);
            if (d.exec() == QDialog::Accepted) {
                if (p.text() != again.text())
                    fail("密码不一致。");
                auth_.changePassword(token_, old.text(), p.text());
            }
        });
    });
    accountLayout->addStretch();
    tabs->addTab(account, "账号");
    auto *cameraSettings = new QWidget;
    auto *cameraLayout = new QVBoxLayout(cameraSettings);
    cameraLayout->addWidget(title("摄像头设置"));
    cameraProfile_ = new QComboBox;
    cameraProfile_->setObjectName("cameraProfile");
    cameraProfile_->addItems({"标准 · 优先 1280×720 / 30 fps", "兼容 · 优先 640×480 / 30 fps", "高清 · 优先 1920×1080 / 30 fps"});
    cameraLayout->addWidget(cameraProfile_);
    auto *cameraHelp = new QLabel("不支持首选模式时自动尝试设备提供的其他模式。\nOBS：先启动虚拟摄像头，再刷新设备；建议关闭美颜、色彩滤镜与背景替换。\n户外：面向柔和光源，避开直射阳光及移动树影，固定相机并保持面部稳定。\n心率需要至少 20 秒连续有效信号；过曝、运动或色温突变时会重新积累。\n实时画面下方显示实际采集模式；更改设置后重新开始测量生效。");
    cameraHelp->setWordWrap(true);
    cameraLayout->addWidget(cameraHelp);
    cameraLayout->addStretch();
    tabs->addTab(cameraSettings, "摄像头");
    tabs->addTab(historyPage(), "历史与报告");
    if (user.role == "admin" || user.role == "researcher") {
        tabs->addTab(modelPage(), "模型中心");
        auto *advanced = new QWidget;
        auto *advancedLayout = new QVBoxLayout(advanced);
        advancedLayout->addWidget(title("调试选项"));
        advancedLayout->addWidget(simulation_);
        simulation_->show();
        advancedLayout->addStretch();
        tabs->addTab(advanced, "高级");
    }
    if (user.role == "admin") {
        tabs->addTab(usersPage(), "账号管理");
        tabs->addTab(auditPage(), "审计日志");
    }
    for (int i = 0; i < tabs->count(); ++i) tabs->widget(i)->setObjectName("settingsPage");
    button("设置", side, [this] { protect([this] {
        auth_.require(token_);
        refreshHistory();
        refreshModels();
        settings_->show();
        settings_->raise();
        settings_->activateWindow();
    }); });
    start_->setEnabled(user.role != "researcher");
    connect(&timer_, &QTimer::timeout, this, &MainWindow::updatePreview);
    timer_.start(100);
    refreshHistory();
    refreshModels();
    // 枚举设备不启动摄像头。
    QTimer::singleShot(0, this, [this] { protect([this] { refreshCameras(); }); });
    for (const auto &m : repository_.recoverInterrupted()) {
        try {
            ReportWriter(db_.root()).generate(m);
        } catch (...) {
        }
    }
}
MainWindow::~MainWindow() {
    if (capture_) {
        capture_->cancel();
        capture_->wait();
    }
}
void MainWindow::protect(const std::function<void()> &action) {
    try {
        action();
    } catch (const std::exception &e) {
        QMessageBox::warning(this, "操作未完成", QString::fromUtf8(e.what()));
    }
}
QWidget *MainWindow::measurePage() {
    auto *w = new QWidget;
    w->setObjectName("monitorPage");
    w->setStyleSheet("QWidget#monitorPage{background:#06102b;}");
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(4, 4, 4, 0);
    l->setSpacing(9);
    auto *options = new QHBoxLayout;
    subject_ = new QLineEdit;
    subject_->setPlaceholderText("受试者代号");
    cameras_ = new QComboBox;
    cameras_->setMinimumWidth(180);
    options->addWidget(subject_, 1);
    options->addWidget(cameras_, 1);
    button("刷新设备", options, [this] {
        protect([this] {
            if (capture_)
                fail("测量时不能切换设备。");
            refreshCameras();
        });
    });
    l->addLayout(options);
    auto *body = new QHBoxLayout;
    auto *cameraPanel = new QGroupBox("01  /  视频采集");
    cameraPanel->setFixedHeight(335);
    auto *cameraLayout = new QVBoxLayout(cameraPanel);
    video_ = new QLabel("摄像头待机");
    video_->setAlignment(Qt::AlignCenter);
    video_->setMinimumSize(430, 265);
    video_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    video_->setMaximumHeight(300);
    video_->setStyleSheet("background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #102e53,stop:1 #050f28);color:#76b6dc;border:1px solid #245986;border-radius:3px;");
    cameraLayout->addWidget(video_);
    body->addWidget(cameraPanel, 5);
    auto *summaryPanel = new QGroupBox("02  /  实时分析");
    summaryPanel->setFixedHeight(335);
    auto *analysisLayout = new QHBoxLayout(summaryPanel);
    auto *cards = new QVBoxLayout;
    hr_ = new QLabel("—\n心率 bpm");
    hr_->setAlignment(Qt::AlignCenter);
    hr_->setStyleSheet(
        "background:#092e47;color:#51edcf;border:1px solid #216480;border-radius:3px;font-size:28px;font-weight:600;padding:10px;");
    cards->addWidget(hr_, 1);
    emotion_ = new QLabel(registry_.activeId().isEmpty() ? "模型未启用" : "已就绪\n表情识别");
    emotion_->setAlignment(Qt::AlignCenter);
    emotion_->setWordWrap(true);
    emotion_->setStyleSheet("background:#122653;color:#aecbff;border:1px solid #355e96;border-radius:3px;font-size:18px;padding:10px;");
    cards->addWidget(emotion_, 1);
    auto *disabled = new QLabel("血压 / 血氧\n当前摄像头暂无可靠测量");
    disabled->setAlignment(Qt::AlignCenter);
    disabled->setToolTip("这两项尚无经过校准验证的测量模型。心率不能换算成血压或血氧。");
    disabled->setStyleSheet("color:#8aa8c7;background:#0c1c3a;border:1px solid #264467;padding:10px;border-radius:3px;");
    cards->addWidget(disabled);
    analysisLayout->addLayout(cards, 2);
    radar_ = new EmotionRadar;
    analysisLayout->addWidget(radar_, 3);
    body->addWidget(summaryPanel, 5);
    l->addLayout(body);
    hint_ = new QLabel("就绪");
    hint_->setWordWrap(true);
    hint_->setStyleSheet("color:#8ac7e6;padding:2px;");
    auto *tracePanel = new QGroupBox("03  /  信号波形");
    auto *traceLayout = new QVBoxLayout(tracePanel);
    traceLayout->setContentsMargins(6, 10, 6, 4);
    plot_ = new PlotWidget;
    traceLayout->addWidget(plot_);
    l->addWidget(tracePanel, 1);
    l->addWidget(hint_);
    elapsed_ = new QLabel("未开始");
    elapsed_->setObjectName("elapsedTime");
    l->addWidget(elapsed_);
    auto *actions = new QHBoxLayout;
    simulation_ = new QCheckBox("模拟信号", this);
    simulation_->hide();
    recording_ = new QCheckBox("实时录像");
    recording_->setObjectName("recordingToggle");
    actions->addWidget(recording_);
    connect(recording_, &QCheckBox::toggled, this, [this](bool enabled) {
        protect([&] { if (capture_) {
            capture_->setRecording(enabled);
            db_.audit(auth_.require(token_).id, enabled ? "recording_on" : "recording_off", "active measurement");
        } });
    });
    actions->addStretch();
    start_ = button("开始测量", actions, [this] { protect([this] { start(); }); });
    start_->setObjectName("startMeasurement");
    start_->setStyleSheet("QPushButton{background:#147c81;color:#e5fffc;border:1px solid #3edbcc;font-weight:600;}QPushButton:disabled{background:#17323e;color:#628087;border-color:#294654;}");
    stop_ = button("停止", actions, [this] {
        if (capture_)
            capture_->cancel();
    });
    stop_->setEnabled(false);
    stop_->setObjectName("stopMeasurement");
    l->addLayout(actions);
    return w;
}
QWidget *MainWindow::historyPage() {
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(26, 20, 26, 20);
    l->addWidget(title("历史与报告"));
    history_ = table({"编号", "受试者代号", "UTC 时间", "状态"}, l);
    auto *a = new QHBoxLayout;
    button("刷新", a, [this] { protect([this] { refreshHistory(); }); });
    button("打开 PDF / 重试生成", a, [this] {
        protect([this] {
            auto m = repository_.get(token_, selected(history_));
            auto path = ReportWriter(db_.root()).generate(m);
            db_.audit(auth_.require(token_).id, "report_open", m.id);
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
    });
    button("导出报告包", a, [this] {
        protect([this] {
            auto m = repository_.get(token_, selected(history_));
            auto pdf = ReportWriter(db_.root()).generate(m);
            QString dest = QFileDialog::getExistingDirectory(this, "选择导出位置");
            if (dest.isEmpty())
                return;
            dest += "/" + m.id;
            if (QFileInfo::exists(dest))
                fail("目标报告目录已存在，未覆盖。");
            QDir().mkpath(dest);
            QString source = QFileInfo(pdf).absolutePath();
            auto files = readJson(source + "/manifest.json")["files"].toObject().keys();
            files.append("manifest.json");
            for (const auto &file : files) copyFileChecked(source + "/" + file, dest + "/" + file);
            db_.audit(auth_.require(token_).id, "report_export", m.id);
        });
    });
    button("打开录像目录", a, [this] { protect([this] {
        auto m = repository_.get(token_, selected(history_));
        auto path = db_.root() + "/recordings/" + m.id;
        if (m.recordings.isEmpty()) fail("此记录没有已完成的录像。");
        db_.audit(auth_.require(token_).id, "recording_open", m.id);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }); });
    button("校验完整性", a, [this] {
        protect([this] {
            auto m = repository_.get(token_, selected(history_));
            bool valid = ReportWriter::verify(db_.root() + "/reports/" + m.id);
            QMessageBox::information(this, "报告校验",
                                     valid ? "SHA-256 校验通过。该清单未做数字签名。"
                                           : "校验失败，或报告尚未生成。");
        });
    });
    l->addLayout(a);
    return w;
}
QWidget *MainWindow::modelPage() {
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(26, 20, 26, 20);
    l->addWidget(title("模型中心"));
    active_ = new QLabel;
    l->addWidget(active_);
    l->addWidget(new QLabel("导入验证后的模型可以发布或回滚；进行中的测量保持原版本。"));
    models_ = table({"版本 ID", "导入 UTC 时间", "本地目录"}, l);
    auto *a = new QHBoxLayout;
    button("导入模型目录", a, [this] {
        protect([this] {
            QString dir = QFileDialog::getExistingDirectory(this, "选择含 manifest.json 的模型目录");
            if (dir.isEmpty())
                return;
            registry_.importModel(token_, dir);
            refreshModels();
        });
    });
    if (auth_.require(token_).role == "admin") button("发布 / 回滚至选中版本", a, [this] {
        protect([this] {
            registry_.activate(token_, selected(models_));
            refreshModels();
        });
    });
    button("刷新", a, [this] { protect([this] { refreshModels(); }); });
    l->addLayout(a);
    return w;
}
QWidget *MainWindow::usersPage() {
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(26, 20, 26, 20);
    l->addWidget(title("账号与权限"));
    users_ = table({"ID", "账号", "角色", "启用"}, l);
    auto *row = new QHBoxLayout;
    newName_ = new QLineEdit;
    newName_->setPlaceholderText("新账号");
    newPassword_ = new QLineEdit;
    newPassword_->setEchoMode(QLineEdit::Password);
    newPassword_->setPlaceholderText("至少 10 字符密码");
    newRole_ = new QComboBox;
    newRole_->addItems({"user", "operator", "researcher", "admin"});
    row->addWidget(newName_);
    row->addWidget(newPassword_);
    row->addWidget(newRole_);
    button("创建", row, [this] {
        protect([this] {
            auth_.createUser(token_, newName_->text(), newPassword_->text(), newRole_->currentText());
            newPassword_->clear();
            refreshUsers();
        });
    });
    l->addLayout(row);
    auto *a = new QHBoxLayout;
    button("启用选中账号", a, [this] {
        protect([this] {
            auth_.setEnabled(token_, selected(users_), true);
            refreshUsers();
        });
    });
    button("禁用选中账号", a, [this] {
        protect([this] {
            auth_.setEnabled(token_, selected(users_), false);
            refreshUsers();
        });
    });
    l->addLayout(a);
    refreshUsers();
    return w;
}
QWidget *MainWindow::auditPage() {
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(26, 20, 26, 20);
    l->addWidget(title("本地审计日志"));
    audits_ = table({"UTC 时间", "操作者", "事件", "详情"}, l);
    button("刷新", l, [this] {
        protect([this] {
            auth_.require(token_, {"admin"});
            fill(audits_, db_.rows("SELECT time,actor,action,detail FROM audit ORDER BY id DESC LIMIT 300"),
                 {"time", "actor", "action", "detail"});
        });
    });
    return w;
}
void MainWindow::refreshCameras() {
    const auto previous = cameras_->currentData();
    cameras_->clear();
    for (auto c : enumerateCameras())
        cameras_->addItem(c.name, c.id);
    if (!cameras_->count())
        cameras_->addItem("未发现摄像头", QString());
    const int selected = cameras_->findData(previous);
    if (selected >= 0) cameras_->setCurrentIndex(selected);
}
void MainWindow::refreshHistory() {
    fill(history_, repository_.history(token_), {"id", "subject", "created", "status"});
}
void MainWindow::refreshModels() {
    if (!models_) return;
    fill(models_, registry_.list(token_), {"id", "created", "path"});
    active_->setText("当前表情模型：" + (registry_.activeId().isEmpty() ? QString("未启用；请导入并发布")
                                                                        : registry_.activeId()));
}
void MainWindow::refreshUsers() {
    fill(users_, auth_.users(token_), {"id", "name", "role", "enabled"});
}
QString MainWindow::selected(QTableWidget *t) const {
    if (t->currentRow() < 0)
        fail("请先选中一行。");
    return t->item(t->currentRow(), 0)->text();
}
void MainWindow::start(int maximumSeconds) {
    if (capture_)
        fail("请等待当前测量结束。");
    const bool demo = simulation_->isChecked();
    QString id = cameras_->currentData().toString();
    if (!demo && id.isEmpty())
        fail("没有可用摄像头。");
    QString detector = assets_ + "/face.onnx";
    if (!demo) {
        if (registry_.activeId().isEmpty() || registry_.activePath().isEmpty())
            fail("表情模型未启用，请重启软件完成初始化，或联系管理员。");
        if (!QFileInfo::exists(detector))
            fail("缺少人脸检测模型。");
        if (fileHash(detector) != "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4")
            fail("人脸检测模型校验失败。");
    }
    auto record =
        repository_.begin(token_, subject_->text(), demo ? "synthetic-72bpm" : cameras_->currentText(),
                          registry_.activeId(), demo);
    capture_ = new CaptureWorker(record, id, detector, registry_.activePath(), maximumSeconds, this);
    capture_->configureRecording(db_.root() + "/recordings/" + record.id, recording_->isChecked());
    capture_->configureArchive(db_.root() + "/signals/" + record.id);
    const int mode = cameraProfile_->currentIndex();
    capture_->configureCamera(mode == 1 ? CameraOptions{640,480,30} : mode == 2 ? CameraOptions{1920,1080,30} : CameraOptions{});
    cameraProfile_->setEnabled(false);
    recording_->setEnabled(true);
    elapsed_->setText("已运行 00:00:00");
    plot_->setSignals({}, {});
    radar_->setResult({});
    start_->setEnabled(false);
    stop_->setEnabled(true);
    simulation_->setEnabled(false);
    subject_->setEnabled(false);
    cameras_->setEnabled(false);
    connect(capture_, &CaptureWorker::completed, this, &MainWindow::finish);
    connect(capture_, &QThread::finished, capture_, &QObject::deleteLater);
    capture_->start();
}
void MainWindow::startSyntheticForTest(int seconds) {
    subject_->setText("SIMULATION");
    simulation_->setChecked(true);
    start(seconds);
}
void MainWindow::updatePreview() {
    if (!capture_)
        return;
    auto v = capture_->latest();
    if (!v.recordingError.isEmpty()) {
        QSignalBlocker blocker(recording_);
        recording_->setChecked(false);
        recording_->setEnabled(false);
    }
    if (!v.image.isNull()) {
        QImage frame = v.image.copy();
        QPainter p(&frame);
        if (v.emotion.status == "synthetic") {
            p.setPen(QColor("#75dfff"));
            p.setFont(QFont("Microsoft YaHei", 22));
            p.drawText(frame.rect(), Qt::AlignCenter, "SIMULATION");
        }
        if (!v.face.isEmpty()) {
            p.setPen(QPen(QColor("#38d4ad"), 3));
            p.drawRect(v.face);
        }
        p.end();
        video_->setPixmap(
            QPixmap::fromImage(frame).scaled(video_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else { video_->setPixmap({}); video_->setText("等待摄像头画面"); }
    hint_->setText(v.guidance);
    elapsed_->setText("已运行 " + elapsedText(v.elapsed));
    hr_->setText(pulseLabel(v.pulse,v.elapsed));
    emotion_->setText(v.emotion.status == "synthetic" ? "模拟模式" : v.emotion.status=="poor_image" ? "面部图像质量不足" :
        v.emotion.status=="uncertain" ? "表情暂不确定" : v.emotion.label);
    plot_->setSignals(v.pulse, v.samples);
    radar_->setResult(v.emotion);
}
void MainWindow::finish(Measurement m) {
    if (capture_)
        capture_->wait();
    hr_->setText(m.pulse.heartRate.status == "valid"
                     ? QString::number(m.pulse.heartRate.value, 'f', 1) + "\nbpm"
                     : "—\n心率 bpm");
    if(m.pulse.heartRate.status!="valid" && m.diagnostics.contains("last_valid_heart_rate")) {
        const auto last=m.diagnostics["last_valid_heart_rate"].toObject();
        hr_->setText(QString("%1<br/><span style='font-size:13px'>本次最近有效 · %2 秒时</span>").arg(last["bpm"].toDouble(),0,'f',1).arg(last["at_seconds"].toDouble(),0,'f',1));
    }
    plot_->setSignals(m.pulse, m.samples);
    elapsed_->setText("已停止 · " + elapsedText(m.samples.isEmpty() ? 0 : m.samples.last().t));
    protect([&] {
        repository_.finish(m);
        lastReport_ = ReportWriter(db_.root()).generate(m);
        refreshHistory();
        const QString state = m.status == "completed" ? "已完成" : m.status == "cancelled" ? "已停止" : m.status == "failed" ? "采集失败" : "有效数据不足";
        hint_->setText(state + (m.recordingError.isEmpty() ? QString() : " · " + m.recordingError));
    });
    capture_ = nullptr;
    start_->setEnabled(auth_.require(token_).role != "researcher");
    stop_->setEnabled(false);
    simulation_->setEnabled(true);
    subject_->setEnabled(true);
    cameras_->setEnabled(true);
    cameraProfile_->setEnabled(true);
    recording_->setEnabled(true);
    if (closePending_)
        QTimer::singleShot(100, this, &QWidget::close);
}
void MainWindow::closeEvent(QCloseEvent *e) {
    if (capture_) {
        closePending_ = true;
        if (capture_)
            capture_->cancel();
        hint_->setText("正在停止后台任务并保存结果…");
        e->ignore();
        return;
    }
    e->accept();
}
} // namespace fv
