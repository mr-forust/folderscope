#include "results_export.h"

#include "search_core.h"

#include <QBuffer>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

#include <algorithm>

namespace {
QStringList headers()
{
    return {
        QStringLiteral("Filename"), QStringLiteral("Folder"), QStringLiteral("Size"),
        QStringLiteral("Size On Disk"), QStringLiteral("Modified Time"),
        QStringLiteral("Created Time"), QStringLiteral("Last Accessed Time"),
        QStringLiteral("Entry Modified Time"), QStringLiteral("Attributes"),
        QStringLiteral("Extension"), QStringLiteral("Duplicate Set"),
        QStringLiteral("Keeper Priority"), QStringLiteral("Type"),
        QStringLiteral("File Owner"), QStringLiteral("Full Path")
    };
}

class FailingDevice final : public QIODevice {
public:
    explicit FailingDevice(qint64 limit) : limit_(limit) {}
    QByteArray bytes;

protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 size) override
    {
        if (bytes.size() >= limit_) {
            setErrorString(QStringLiteral("simulated write failure"));
            return -1;
        }
        const qint64 accepted = std::min(size, limit_ - bytes.size());
        bytes.append(data, accepted);
        return accepted;
    }

private:
    qint64 limit_;
};
}

class ResultsExportTest final : public QObject {
    Q_OBJECT

private slots:
    void writesWellFormedXmlWithInvalidCharactersReplaced()
    {
        FileRecord record;
        record.name = QStringLiteral("control") + QChar(1) + QStringLiteral("&.txt");
        record.folder = QStringLiteral("/tmp");
        record.path = QStringLiteral("/tmp/") + record.name;
        record.size = 7;
        record.type = QStringLiteral("File");

        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QString error;
        QVERIFY(writeResults(buffer, {record}, headers(), QStringLiteral("xml"), true,
                             &error));

        QXmlStreamReader reader(buffer.data());
        while (!reader.atEnd())
            reader.readNext();
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
        QVERIFY(buffer.data().contains("\xef\xbf\xbd"));
        QVERIFY(!buffer.data().contains('\x01'));
    }

    void reportsWriteFailures()
    {
        FileRecord record;
        record.name = QString(1000, u'x');
        record.path = QStringLiteral("/tmp/") + record.name;
        record.type = QStringLiteral("File");
        FailingDevice device(32);
        QVERIFY(device.open(QIODevice::WriteOnly));
        QString error;
        QVERIFY(!writeResults(device, {record}, headers(), QStringLiteral("csv"), true,
                              &error));
        QVERIFY(!error.isEmpty());
    }

    void writesSupportedTabularAndJsonFormats()
    {
        FileRecord record;
        record.name = QStringLiteral("report.txt");
        record.folder = QStringLiteral("/tmp");
        record.path = QStringLiteral("/tmp/report.txt");
        record.size = 10;
        record.type = QStringLiteral("File");

        for (const QString &suffix : {QStringLiteral("csv"), QStringLiteral("tsv"),
                                      QStringLiteral("html"), QStringLiteral("json")}) {
            QBuffer buffer;
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QString error;
            QVERIFY2(writeResults(buffer, {record}, headers(), suffix, true, &error),
                     qPrintable(error));
            QVERIFY(!buffer.data().isEmpty());
            if (suffix == QStringLiteral("json")) {
                const QJsonDocument document = QJsonDocument::fromJson(buffer.data());
                QVERIFY(document.isArray());
                QCOMPARE(document.array().size(), 1);
            }
        }
    }

    void sanitizesUnpairedSurrogates()
    {
        const QString invalid(1, QChar(0xd800));
        QCOMPARE(xml10SafeText(invalid), QString(QChar(0xfffd)));
    }
};

QTEST_GUILESS_MAIN(ResultsExportTest)

#include "results_export_test.moc"
