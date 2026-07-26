#include "search_engine.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <unistd.h>

namespace {
bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(data) == data.size();
}

QVector<FileRecord> runSearch(SearchEngine &engine, SearchOptions options)
{
    QSignalSpy finished(&engine, &SearchEngine::finished);
    engine.search(options);
    if (finished.size() != 1)
        return {};
    return finished.takeFirst().at(0).value<QVector<FileRecord>>();
}

QSet<QString> namesOf(const QVector<FileRecord> &records)
{
    QSet<QString> names;
    for (const auto &record : records)
        names.insert(record.name);
    return names;
}

SearchOptions optionsFor(const QString &root, const QString &mode)
{
    SearchOptions options;
    options.roots = root;
    options.mode = mode;
    options.recursive = true;
    options.accurateProgress = false;
    options.showDuplicateCopiesOnly = false;
    options.useCache = false;
    return options;
}
}

class SearchEngineTest final : public QObject {
    Q_OBJECT

private slots:
    void filtersBySampleThenFullHashAndSupportsStrictMode()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(writeFile(directory.filePath(QStringLiteral("duplicate-a.bin")),
                          QByteArray("same content")));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("duplicate-b.bin")),
                          QByteArray("same content")));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("unique-size.bin")),
                          QByteArray("unique")));

        const QByteArray large(duplicateSampleThreshold + 1024 * 1024, 'x');
        QByteArray different = large;
        different[1024 * 1024] = 'y';
        QVERIFY(writeFile(directory.filePath(QStringLiteral("sample-a.bin")), large));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("sample-b.bin")), different));

        SearchEngine engine(nullptr, directory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Duplicates search"));
        QVector<FileRecord> results = runSearch(engine, options);
        QCOMPARE(namesOf(results),
                 QSet<QString>({QStringLiteral("duplicate-a.bin"),
                                QStringLiteral("duplicate-b.bin")}));

        options.strictDuplicateComparison = true;
        results = runSearch(engine, options);
        QCOMPARE(namesOf(results),
                 QSet<QString>({QStringLiteral("duplicate-a.bin"),
                                QStringLiteral("duplicate-b.bin")}));

        options.mode = QStringLiteral("Non-Duplicates search");
        results = runSearch(engine, options);
        QCOMPARE(namesOf(results),
                 QSet<QString>({QStringLiteral("unique-size.bin"),
                                QStringLiteral("sample-a.bin"),
                                QStringLiteral("sample-b.bin")}));
    }

    void appliesContentPipelineToDuplicateNames()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("one")));
        QVERIFY(root.mkpath(QStringLiteral("two")));
        const QString first =
            directory.filePath(QStringLiteral("one/report.bin"));
        const QString second =
            directory.filePath(QStringLiteral("two/report.bin"));
        QVERIFY(writeFile(first, QByteArray("same")));
        QVERIFY(writeFile(second, QByteArray("same")));

        SearchEngine engine(nullptr, directory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Duplicate names search"));
        options.duplicateNameMode = QStringLiteral("Only identical content");
        QVector<FileRecord> results = runSearch(engine, options);
        QCOMPARE(results.size(), 2);

        QVERIFY(writeFile(second, QByteArray("diff")));
        results = runSearch(engine, options);
        QVERIFY(results.isEmpty());

        options.duplicateNameMode = QStringLiteral("Only non-identical content");
        results = runSearch(engine, options);
        QCOMPARE(results.size(), 2);

        options.strictDuplicateComparison = true;
        results = runSearch(engine, options);
        QCOMPARE(results.size(), 2);
    }

    void assignsKeeperPriorityFromConfiguredRoots()
    {
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("Temp")));
        QVERIFY(root.mkpath(QStringLiteral("misc")));
        const QString temp = directory.filePath(QStringLiteral("Temp"));
        const QString misc = directory.filePath(QStringLiteral("misc"));
        const QByteArray content("same");
        const QString basePath = directory.filePath(QStringLiteral("base.mp4"));
        const QString tempPath = QDir(temp).filePath(QStringLiteral("temp.mp4"));
        const QString miscPath = QDir(misc).filePath(QStringLiteral("misc.mp4"));
        QVERIFY(writeFile(basePath, content));
        QVERIFY(writeFile(tempPath, content));
        QVERIFY(writeFile(miscPath, content));

        SearchEngine engine(nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            QStringList({directory.path(), temp, misc}).join(u';'),
            QStringLiteral("Duplicates search"));
        QVector<FileRecord> results = runSearch(engine, options);
        QCOMPARE(results.size(), 3);

        QHash<QString, int> priorities;
        for (const FileRecord &record : results)
            priorities.insert(record.path, record.duplicateCopy);
        QCOMPARE(priorities.value(basePath), 1);
        QCOMPARE(priorities.value(tempPath), 2);
        QCOMPARE(priorities.value(miscPath), 3);
    }

    void prefersNewerFilesWhenRootPriorityTies()
    {
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("child")));
        const QString older = QDir(directory.filePath(QStringLiteral("child")))
                                  .filePath(QStringLiteral("older.mp4"));
        const QString newer = directory.filePath(QStringLiteral("newer.mp4"));
        QVERIFY(writeFile(older, QByteArray("same")));
        QVERIFY(writeFile(newer, QByteArray("same")));
        QFile olderFile(older);
        QVERIFY(olderFile.open(QIODevice::ReadOnly));
        QVERIFY(olderFile.setFileTime(QDateTime::fromSecsSinceEpoch(1),
                                      QFileDevice::FileModificationTime));
        olderFile.close();

        SearchEngine engine(nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Duplicates search"));
        const QVector<FileRecord> results = runSearch(engine, options);
        QCOMPARE(results.size(), 2);
        QHash<QString, int> priorities;
        for (const FileRecord &record : results)
            priorities.insert(record.path, record.duplicateCopy);
        QCOMPARE(priorities.value(newer), 1);
        QCOMPARE(priorities.value(older), 2);
    }

    void reusesSampleAndFullHashesOnWarmSearch()
    {
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        const QByteArray content(duplicateSampleThreshold + 1024 * 1024, 'x');
        const QString first = directory.filePath(QStringLiteral("first.bin"));
        const QString second = directory.filePath(QStringLiteral("second.bin"));
        QVERIFY(writeFile(first, content));
        QVERIFY(writeFile(second, content));
        const QString cachePath =
            cacheDirectory.filePath(QStringLiteral("duplicate-cache.dat"));
        SearchEngine engine(nullptr, cachePath);
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Duplicates search"));
        options.useCache = true;

        QSignalSpy finished(&engine, &SearchEngine::finished);
        QElapsedTimer timer;
        timer.start();
        engine.search(options);
        const qint64 coldMilliseconds = timer.elapsed();
        QCOMPARE(finished.size(), 1);
        finished.clear();

        if (::geteuid() != 0) {
            QVERIFY(QFile::setPermissions(first, {}));
            QVERIFY(QFile::setPermissions(second, {}));
        }
        timer.restart();
        engine.search(options);
        const qint64 warmMilliseconds = timer.elapsed();
        QCOMPARE(finished.size(), 1);
        const QList<QVariant> arguments = finished.takeFirst();
        QCOMPARE(arguments.at(0).value<QVector<FileRecord>>().size(), 2);
        QVERIFY(arguments.at(1).toString().contains(QStringLiteral("cache: 4 hits")));
        qInfo().noquote()
            << QStringLiteral("duplicate benchmark: cold %1 ms, warm %2 ms")
                   .arg(coldMilliseconds)
                   .arg(warmMilliseconds);

        if (::geteuid() != 0) {
            options.strictDuplicateComparison = true;
            finished.clear();
            engine.search(options);
            QCOMPARE(finished.size(), 1);
            QVERIFY(finished.takeFirst().at(0).value<QVector<FileRecord>>().isEmpty());
        }
        QFile::remove(cachePath);
    }

    void doesNotFullyHashDifferentSamplesOrSizes()
    {
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        const QByteArray firstContent(
            duplicateSampleThreshold + 1024 * 1024, 'x');
        QByteArray secondContent = firstContent;
        secondContent[0] = 'y';
        QVERIFY(writeFile(directory.filePath(QStringLiteral("large-a.bin")),
                          firstContent));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("large-b.bin")),
                          secondContent));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("small-a.bin")),
                          QByteArray("one")));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("small-b.bin")),
                          QByteArray("different size")));

        SearchEngine engine(
            nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Duplicates search"));
        options.useCache = true;
        QSignalSpy finished(&engine, &SearchEngine::finished);
        engine.search(options);
        QCOMPARE(finished.size(), 1);
        finished.clear();

        engine.search(options);
        QCOMPARE(finished.size(), 1);
        const QList<QVariant> arguments = finished.takeFirst();
        QVERIFY(arguments.at(0).value<QVector<FileRecord>>().isEmpty());
        QVERIFY(arguments.at(1).toString().contains(QStringLiteral("cache: 2 hits")));
    }

    void collapsesCurrentOverlappingRootLayoutBeforeHashing()
    {
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        QDir root(directory.path());
        const QStringList children{
            QStringLiteral(".Prt"), QStringLiteral("misc"), QStringLiteral("Music"),
            QStringLiteral("Буфер"), QStringLiteral("Temp")
        };
        QStringList roots{directory.path()};
        for (qsizetype index = 0; index < children.size(); ++index) {
            QVERIFY(root.mkpath(children[index]));
            const QString child = directory.filePath(children[index]);
            roots.push_back(child);
            QVERIFY(writeFile(
                QDir(child).filePath(QStringLiteral("unique.bin")),
                QByteArray(duplicateSampleThreshold + index + 1,
                           char('a' + index))));
        }

        SearchEngine engine(
            nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            roots.join(u';'), QStringLiteral("Duplicates search"));
        options.accurateProgress = true;
        options.excludeFolders =
            directory.filePath(QStringLiteral("Meta"));
        QSignalSpy phases(&engine, &SearchEngine::phaseProgress);
        QSignalSpy finished(&engine, &SearchEngine::finished);
        QElapsedTimer timer;
        timer.start();
        engine.search(options);
        const qint64 elapsedMilliseconds = timer.elapsed();

        QCOMPARE(finished.size(), 1);
        const QList<QVariant> arguments = finished.takeFirst();
        QVERIFY(arguments.at(0).value<QVector<FileRecord>>().isEmpty());
        QVERIFY(arguments.at(1).toString().contains(
            QStringLiteral("5 redundant roots skipped")));
        quint64 scanningCompleted = 0;
        quint64 scanningTotal = 0;
        for (const QList<QVariant> &phase : phases) {
            const QString name = phase.at(0).toString();
            QVERIFY(name != QStringLiteral("Sampling duplicate candidates"));
            QVERIFY(name != QStringLiteral("Hashing duplicate candidates"));
            if (name == QStringLiteral("Scanning files")) {
                scanningCompleted =
                    std::max(scanningCompleted, phase.at(1).toULongLong());
                scanningTotal = phase.at(2).toULongLong();
            }
        }
        QVERIFY(scanningTotal > 0);
        QCOMPARE(scanningCompleted, scanningTotal);
        qInfo().noquote()
            << QStringLiteral("overlapping-roots benchmark: %1 ms, 5 large files, 0 hash phases")
                   .arg(elapsedMilliseconds);
    }

    void deduplicatesCandidatesWhenNestedRootsCannotBeCollapsed()
    {
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("child")));
        const QString child = directory.filePath(QStringLiteral("child"));
        QVERIFY(writeFile(QDir(child).filePath(QStringLiteral("only.bin")),
                          QByteArray(duplicateSampleThreshold + 1, 'x')));

        SearchEngine engine(
            nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path() + u';' + child, QStringLiteral("Standard search"));
        options.maxDepth = 2;
        QVector<FileRecord> results = runSearch(engine, options);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results.front().name, QStringLiteral("only.bin"));

        options.mode = QStringLiteral("Duplicates search");
        QSignalSpy phases(&engine, &SearchEngine::phaseProgress);
        QSignalSpy finished(&engine, &SearchEngine::finished);
        engine.search(options);
        QCOMPARE(finished.size(), 1);
        const QList<QVariant> arguments = finished.takeFirst();
        QVERIFY(arguments.at(0).value<QVector<FileRecord>>().isEmpty());
        QVERIFY(!arguments.at(1).toString().contains(
            QStringLiteral("redundant roots skipped")));
        for (const QList<QVariant> &phase : phases) {
            const QString name = phase.at(0).toString();
            QVERIFY(name != QStringLiteral("Sampling duplicate candidates"));
            QVERIFY(name != QStringLiteral("Hashing duplicate candidates"));
        }
    }

    void leavesUnreadableCandidatesAsNonDuplicates()
    {
        if (::geteuid() == 0)
            QSKIP("Root can read files regardless of their permission bits");
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        const QString first = directory.filePath(QStringLiteral("first.bin"));
        const QString second = directory.filePath(QStringLiteral("second.bin"));
        QVERIFY(writeFile(first, QByteArray("blocked")));
        QVERIFY(writeFile(second, QByteArray("blocked")));
        QVERIFY(QFile::setPermissions(first, {}));
        QVERIFY(QFile::setPermissions(second, {}));

        SearchEngine engine(
            nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Duplicates search"));
        QVERIFY(runSearch(engine, options).isEmpty());
        options.mode = QStringLiteral("Non-Duplicates search");
        QCOMPARE(namesOf(runSearch(engine, options)),
                 QSet<QString>({QStringLiteral("first.bin"),
                                QStringLiteral("second.bin")}));
    }
};

QTEST_GUILESS_MAIN(SearchEngineTest)

#include "search_engine_test.moc"
