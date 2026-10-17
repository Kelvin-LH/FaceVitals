#include "ModelRegistry.h"
#include "algorithms/EmotionModel.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace fv {
QString ModelRegistry::importModel(const QString &token, const QString &directory) {
    auto user = auth_.require(token, {"admin", "researcher"});
    return importVerified(user.id, directory);
}
QString ModelRegistry::importVerified(const QString &actor, const QString &directory) {
    auto m = readJson(directory + "/manifest.json");
    const auto filename = m["file"].toString(), id = m["id"].toString();
    if (!QRegularExpression("^[a-zA-Z0-9_-]{1,64}$").match(id).hasMatch() ||
        !QRegularExpression("^[a-zA-Z0-9_.-]{1,80}$").match(filename).hasMatch() || filename == ".." ||
        filename == ".")
        fail("模型 ID 或文件名无效。");
    if (m["schema"].toInt() != 1 || m["task"].toString() != "emotion")
        fail("仅支持 schema=1 的表情模型。");
    auto labels = m["labels"].toArray();
    QSet<QString> unique;
    for (auto x : labels) {
        if (!x.isString() || x.toString().isEmpty() || x.toString().size() > 40)
            fail("类别名称无效。");
        unique.insert(x.toString());
    }
    if (labels.size() < 2 || labels.size() > 16 || unique.size() != labels.size())
        fail("类别必须为 2—16 个不重复名称。");
    if (db_.scalar("SELECT count(*) FROM models WHERE id=?", {id}).toInt())
        fail("版本已经存在，请使用新 ID。");
    const QString file = directory + "/" + filename;
    if (QFileInfo(file).isSymLink() || QFileInfo(file).size() > 64 * 1024 * 1024 ||
        QFileInfo(file).size() < 8)
        fail("模型文件不合法或超过 64 MiB。");
    if (fileHash(file) != m["sha256"].toString())
        fail("模型校验和不匹配。");
    const auto format = m["format"].toString();
    if (format != "linear-v1" && format != "onnx-emotion-v1")
        fail("未支持的模型格式。");
    if (format == "onnx-emotion-v1" &&
        (m["input_size"].toInt() != 224 || m["preprocess"].toString() != "rgb-imagenet-v1"))
        fail("ONNX 输入契约不匹配。");
    if (format == "linear-v1" && readJson(file)["labels"].toArray() != labels)
        fail("模型内部类别和清单不一致。");
    // 在隔离版本目录完成验证；只复制约定的数据文件，不执行模型包脚本。
    QString dest = db_.root() + "/models/" + id;
    QDir().mkpath(dest);
    writeFile(dest + "/" + filename, readFile(file, 64 * 1024 * 1024));
    writeFile(dest + "/manifest.json", QJsonDocument(m).toJson());
    auto model = makeEmotionModel(dest);
    QImage blank(224, 224, QImage::Format_RGB888);
    blank.fill(Qt::gray);
    auto result = model->predict(blank);
    if (result.scores.size() != labels.size())
        fail("输出类别不匹配。");
    db_.execute("INSERT INTO models VALUES(?,?,?,?)",
                {id, utcNow(), dest, QString::fromUtf8(QJsonDocument(m).toJson(QJsonDocument::Compact))});
    db_.audit(actor, "model_import", id);
    return id;
}
void ModelRegistry::activate(const QString &token, const QString &id) {
    auto user = auth_.require(token, {"admin"});
    activateVerified(user.id, id);
}
void ModelRegistry::activateVerified(const QString &actor, const QString &id) {
    auto rows = db_.rows("SELECT path FROM models WHERE id=?", {id});
    if (rows.isEmpty())
        fail("未找到已验证的模型版本。");
    auto model = makeEmotionModel(rows[0]["path"].toString());
    Q_UNUSED(model);
    db_.execute("INSERT OR REPLACE INTO settings VALUES('active_emotion',?)", {id});
    db_.audit(actor, "model_activate", id);
}
void ModelRegistry::ensureBundledModel(const QString &directory) {
    if (!activeId().isEmpty()) {
        auto model = makeEmotionModel(activePath());
        if (!model) fail("当前表情模型不可用，请由管理员重新发布模型。");
        return;
    }
    // 只允许自动安装随程序提供的固定模型包；自定义模型仍需管理员发布。
    const QString manifestHash = "49362ba8855bf9067a17f26c4db8ede5019f734ce6c47b344054f5d658d3f9dc";
    if (fileHash(directory + "/manifest.json") != manifestHash)
        fail("内置表情模型清单校验失败。");
    const auto manifest = readJson(directory + "/manifest.json");
    const auto id = manifest["id"].toString();
    auto rows = db_.rows("SELECT path FROM models WHERE id=?", {id});
    if (rows.isEmpty()) importVerified("system:bundled", directory);
    else if (readJson(rows.first()["path"].toString() + "/manifest.json") != manifest)
        fail("已导入的内置版本清单不一致，请检查模型目录。");
    activateVerified("system:bundled", id);
}
QString ModelRegistry::activeId() const {
    return db_.scalar("SELECT value FROM settings WHERE key='active_emotion'").toString();
}
QString ModelRegistry::activePath() const {
    return db_.scalar("SELECT path FROM models WHERE id=?", {activeId()}).toString();
}
QVector<QVariantMap> ModelRegistry::list(const QString &token) const {
    auth_.require(token);
    return db_.rows("SELECT id,created,path FROM models ORDER BY created DESC");
}
} // namespace fv
