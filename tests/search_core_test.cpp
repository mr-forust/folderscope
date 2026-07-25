#include "search_core.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class SearchCoreTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesPatternsAndExclusions()
    {
        QCOMPARE(patterns(QStringLiteral("/a; \"/b,with-comma\" , /c")),
                 QStringList({QStringLiteral("/a"), QStringLiteral("/b,with-comma"),
                              QStringLiteral("/c")}));
        QCOMPARE(exclusionPatterns(QStringLiteral("tmp log *.bak")),
                 QStringList({QStringLiteral("*.tmp"), QStringLiteral("*.log"),
                              QStringLiteral("*.bak")}));
        QVERIFY(wildcardMatch(QStringLiteral("Report.TXT"), {QStringLiteral("*.txt")},
                              Qt::CaseInsensitive));
        QVERIFY(!wildcardMatch(QStringLiteral("Report.TXT"), {QStringLiteral("*.txt")},
                               Qt::CaseSensitive));
    }

    void parsesOptionalIsoDates()
    {
        qint64 seconds = -1;
        QVERIFY(parseOptionalIsoDate(QString{}, seconds));
        QCOMPARE(seconds, 0);
        QVERIFY(parseOptionalIsoDate(QStringLiteral("2026-07-25T20:30:00"), seconds));
        QVERIFY(seconds > 0);
        QVERIFY(!parseOptionalIsoDate(QStringLiteral("2026-not-a-date"), seconds));
    }

    void selectsDuplicateRowsForDisplay()
    {
        QVERIFY(!shouldShowDuplicateResult(true, 0));
        QVERIFY(!shouldShowDuplicateResult(true, 1));
        QVERIFY(shouldShowDuplicateResult(true, 2));

        QVERIFY(!shouldShowDuplicateResult(false, 0));
        QVERIFY(shouldShowDuplicateResult(false, 1));
        QVERIFY(shouldShowDuplicateResult(false, 2));
    }

    void searchesContentsAndHashes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstPath = directory.filePath(QStringLiteral("first.bin"));
        const QString secondPath = directory.filePath(QStringLiteral("second.bin"));
        const QString differentPath = directory.filePath(QStringLiteral("different.bin"));
        const QByteArray content = QByteArray(1024 * 1024 - 2, 'x')
            + QByteArray("Alpha needle BETA");

        QFile first(firstPath);
        QVERIFY(first.open(QIODevice::WriteOnly));
        QCOMPARE(first.write(content), content.size());
        first.close();

        QFile second(secondPath);
        QVERIFY(second.open(QIODevice::WriteOnly));
        QCOMPARE(second.write(content), content.size());
        second.close();

        QFile different(differentPath);
        QVERIFY(different.open(QIODevice::WriteOnly));
        QCOMPARE(different.write("different"), qint64(9));
        different.close();

        SearchOptions options;
        options.contains = QStringLiteral("needle");
        QVERIFY(fileContains(firstPath, options));
        options.contains = QStringLiteral("alpha,beta");
        options.multipleValues = true;
        options.multipleAnd = true;
        QVERIFY(fileContains(firstPath, options));
        options.caseSensitive = true;
        QVERIFY(!fileContains(firstPath, options));
        options.binary = true;
        options.multipleValues = false;
        options.multipleAnd = false;
        options.contains = QStringLiteral("41 6c 70 68 61");
        QVERIFY(fileContains(firstPath, options));
        options.contains = QStringLiteral("ABC");
        QVERIFY(!fileContains(firstPath, options));

        QVERIFY(filesEqual(firstPath, secondPath));
        QVERIFY(!filesEqual(firstPath, differentPath));
        QCOMPARE(sha256(firstPath), QCryptographicHash::hash(content, QCryptographicHash::Sha256));
    }
};

QTEST_GUILESS_MAIN(SearchCoreTest)

#include "search_core_test.moc"
