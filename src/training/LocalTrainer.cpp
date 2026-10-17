#include "LocalTrainer.h"
#include "algorithms/EmotionModel.h"
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <random>

namespace fv {
namespace {
struct Row {
    QVector<double> feature;
    int label;
    QString split;
};
} // namespace
void LocalTrainer::run() {
    try {
        auto dir = train(manifest_, output_, epochs_, cancelled_, [this](QString x) { emit progress(x); });
        emit completed(dir, {});
    } catch (const std::exception &e) {
        emit completed({}, QString::fromUtf8(e.what()));
    }
}
QString LocalTrainer::train(const QString &manifest, const QString &output, int epochs,
                            std::atomic_bool &cancelled, const std::function<void(QString)> &progress) {
    if (epochs < 1 || epochs > 1000)
        fail("训练轮数应为 1—1000。");
    auto config = readJson(manifest);
    auto rows = config["rows"].toArray(), labels = config["labels"].toArray();
    if (config["schema"].toInt() != 1 || config["authorization"].toString().trimmed().isEmpty())
        fail("数据清单需要 schema=1 和 authorization 授权说明。");
    if (labels.size() < 2 || labels.size() > 16 || rows.size() < 4 || rows.size() > 100000)
        fail("数据集需要 2—16 类，4—100000 条记录。");
    QSet<QString> labelSet;
    for (auto x : labels) {
        if (!x.isString() || x.toString().isEmpty() || x.toString().size() > 40)
            fail("类别名称无效。");
        labelSet.insert(x.toString());
    }
    if (labelSet.size() != labels.size())
        fail("类别名称重复。");
    QString base = QFileInfo(manifest).absoluteDir().canonicalPath();
    QMap<QString, QString> subjectSplit, hashSplit;
    QVector<Row> data;
    QVector<int> trainIndices, valIndices, trainCount(labels.size(), 0), valCount(labels.size(), 0);
    QJsonArray provenance;
    for (auto value : rows) {
        if (cancelled)
            fail("训练已取消。");
        auto r = value.toObject();
        QString subject = r["subject"].toString(), split = r["split"].toString(),
                relative = r["path"].toString();
        int label = r["label"].toInt(-1);
        if (subject.isEmpty() || !QStringList{"train", "val"}.contains(split) || label < 0 ||
            label >= labels.size())
            fail("每条数据需要有效 subject、train/val split 与类别索引。");
        if (subjectSplit.contains(subject) && subjectSplit[subject] != split)
            fail("检测到同一受试者跨训练/验证集泄漏。");
        subjectSplit[subject] = split;
        QFileInfo file(QDir(base).filePath(relative));
        QString path = file.canonicalFilePath();
        if (QDir::isAbsolutePath(relative) || file.isSymLink() || !path.startsWith(base + "/") ||
            file.size() > 8 * 1024 * 1024)
            fail("图像必须在数据清单目录内，且不超过 8 MiB。");
        auto hash = fileHash(path);
        if (hashSplit.contains(hash) && hashSplit[hash] != split)
            fail("检测到跨划分重复图像。");
        hashSplit[hash] = split;
        QImageReader reader(path);
        auto size = reader.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 4096 * 4096)
            fail("图像尺寸无效或过大。");
        auto image = reader.read();
        if (image.isNull())
            fail("无法解码图像：" + relative);
        data.append({LinearEmotionModel::features(image), label, split});
        int i = data.size() - 1;
        if (split == "train") {
            trainIndices.append(i);
            ++trainCount[label];
        } else {
            valIndices.append(i);
            ++valCount[label];
        }
        provenance.append(QJsonObject{
            {"path", relative}, {"subject", subject}, {"split", split}, {"label", label}, {"sha256", hash}});
    }
    for (int i = 0; i < labels.size(); ++i)
        if (!trainCount[i] || !valCount[i])
            fail("每个类别都需要训练和独立主体的验证图像。");
    QVector<QVector<double>> weights(labels.size(), QVector<double>(LinearEmotionModel::dimensions + 1, 0)),
        bestWeights = weights;
    std::mt19937 rng(42);
    double bestLoss = 1e100;
    int bestEpoch = 0;
    QJsonArray logs;
    for (int epoch = 1; epoch <= epochs; ++epoch) {
        std::shuffle(trainIndices.begin(), trainIndices.end(), rng);
        double loss = 0;
        for (int index : trainIndices) {
            if (cancelled)
                fail("训练已取消。");
            const auto &row = data[index];
            auto p = LinearEmotionModel::probabilities(row.feature, weights);
            loss -= std::log(std::max(1e-12, p[row.label]));
            for (int c = 0; c < labels.size(); ++c) {
                double error = p[c] - (c == row.label ? 1 : 0);
                for (int f = 0; f < LinearEmotionModel::dimensions; ++f)
                    weights[c][f] -= .02 * (error * row.feature[f] + .001 * weights[c][f]);
                weights[c].last() -= .02 * error;
            }
        }
        double valLoss = 0;
        int correct = 0;
        for (int i : valIndices) {
            auto p = LinearEmotionModel::probabilities(data[i].feature, weights);
            valLoss -= std::log(std::max(1e-12, p[data[i].label]));
            correct += int(std::distance(p.begin(), std::max_element(p.begin(), p.end()))) == data[i].label;
        }
        valLoss /= valIndices.size();
        if (valLoss < bestLoss) {
            bestLoss = valLoss;
            bestEpoch = epoch;
            bestWeights = weights;
        }
        QJsonObject log{{"epoch", epoch},
                        {"train_loss", loss / trainIndices.size()},
                        {"val_loss", valLoss},
                        {"val_accuracy", double(correct) / valIndices.size()}};
        logs.append(log);
        progress(QString("轮次 %1/%2 · 训练 loss %3 · 验证 loss %4 · 验证准确率 %5")
                     .arg(epoch)
                     .arg(epochs)
                     .arg(loss / trainIndices.size(), 0, 'f', 4)
                     .arg(valLoss, 0, 'f', 4)
                     .arg(double(correct) / valIndices.size(), 0, 'f', 3));
    }
    if (cancelled)
        fail("训练已取消。");
    QJsonArray matrix;
    QVector<QVector<int>> confusion(labels.size(), QVector<int>(labels.size(), 0));
    for (int i : valIndices) {
        auto p = LinearEmotionModel::probabilities(data[i].feature, bestWeights);
        ++confusion[data[i].label][int(std::distance(p.begin(), std::max_element(p.begin(), p.end())))];
    }
    double f1 = 0, uar = 0;
    for (int c = 0; c < labels.size(); ++c) {
        int tp = confusion[c][c], actual = 0, predicted = 0;
        QJsonArray line;
        for (int k = 0; k < labels.size(); ++k) {
            actual += confusion[c][k];
            predicted += confusion[k][c];
            line.append(confusion[c][k]);
        }
        f1 += actual + predicted > 0 ? 2.0 * tp / (actual + predicted) : 0;
        uar += double(tp) / actual;
        matrix.append(line);
    }
    QString id = "linear-" + newId();
    QString dest = output + "/" + id;
    if (!QDir().mkpath(dest))
        fail("不能创建训练输出目录。");
    QJsonArray w;
    for (const auto &row : bestWeights) {
        QJsonArray line;
        for (double x : row)
            line.append(x);
        w.append(line);
    }
    writeFile(
        dest + "/model.json",
        QJsonDocument(QJsonObject{{"features", "gray-gradient-128-v1"}, {"labels", labels}, {"weights", w}})
            .toJson());
    QJsonObject m{{"schema", 1},
                  {"id", id},
                  {"task", "emotion"},
                  {"format", "linear-v1"},
                  {"file", "model.json"},
                  {"sha256", fileHash(dest + "/model.json")},
                  {"labels", labels},
                  {"validation",
                   "Research baseline; validation selected checkpoint; not independent test performance"}};
    writeFile(dest + "/manifest.json", QJsonDocument(m).toJson());
    QJsonObject summary{{"run_id", id},
                        {"seed", 42},
                        {"backend", "C++ CPU softmax"},
                        {"features", "gray-gradient-128-v1"},
                        {"epochs", epochs},
                        {"best_epoch", bestEpoch},
                        {"val_loss", bestLoss},
                        {"val_macro_f1", f1 / labels.size()},
                        {"val_uar", uar / labels.size()},
                        {"confusion", matrix},
                        {"history", logs},
                        {"dataset_manifest_sha256", fileHash(manifest)},
                        {"authorization", config["authorization"]},
                        {"data", provenance},
                        {"created", utcNow()}};
    writeFile(dest + "/training.json", QJsonDocument(summary).toJson());
    return dest;
}
} // namespace fv
