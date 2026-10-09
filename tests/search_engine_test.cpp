#include "search_engine.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(data) == data.size();
}

bool rewriteFilePreservingMtime(const QString &path, const QByteArray &data,
                                struct stat &before, struct stat &after)
{
    const QByteArray nativePath = QFile::encodeName(path);
    if (::stat(nativePath.constData(), &before) != 0)
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(data) != data.size())
        return false;
    file.close();

    const timespec times[2]{{0, UTIME_OMIT}, before.st_mtim};
    if (::utimensat(AT_FDCWD, nativePath.constData(), times, 0) != 0
        || ::stat(nativePath.constData(), &after) != 0)
        return false;
    return before.st_size == after.st_size
        && before.st_mtim.tv_sec == after.st_mtim.tv_sec
        && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec
        && before.st_dev == after.st_dev
        && before.st_ino == after.st_ino
        && (before.st_ctim.tv_sec != after.st_ctim.tv_sec
            || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec);
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
    void matchesIndependentByteOracle_data()
    {
        QTest::addColumn<bool>("strict");
        QTest::addColumn<bool>("cache");
        QTest::addColumn<bool>("copiesOnly");
        for (bool strict : {false, true})
            for (bool cache : {false, true})
                for (bool copiesOnly : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("strict-%1-cache-%2-copies-%3")
                        .arg(strict).arg(cache).arg(copiesOnly))) << strict << cache << copiesOnly;
    }

    void matchesIndependentByteOracle()
    {
        QFETCH(bool, strict);
        QFETCH(bool, cache);
        QFETCH(bool, copiesOnly);
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        const QByteArray boundary(duplicateSampleThreshold, 'x');
        const QByteArray large(duplicateSampleThreshold + 1, 'z');
        QByteArray sameSamplesDifferentContent = large;
        sameSamplesDifferentContent[1024 * 1024] = 'q';
        const QVector<QByteArray> contents{
            {}, {}, QByteArray("same"), QByteArray("same"), QByteArray("diff"),
            QByteArray("unique-size"), boundary, boundary,
            QByteArray(duplicateSampleThreshold, 'y'), large, large,
            sameSamplesDifferentContent
        };
        QStringList paths;
        for (qsizetype index = 0; index < contents.size(); ++index) {
            const QString path = directory.filePath(
                QStringLiteral("fixture-%1.bin").arg(index, 2, 10, QChar(u'0')));
            QVERIFY(writeFile(path, contents[index]));
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(file.setFileTime(QDateTime::fromSecsSinceEpoch(1000),
                                     QFileDevice::FileModificationTime));
            paths.push_back(path);
        }
        QSet<QString> expectedDuplicates;
        QSet<QString> expectedUnique;
        QVector<int> priorities(contents.size());
        for (qsizetype index = 0; index < contents.size(); ++index) {
            int copies = 0;
            int priority = 0;
            for (qsizetype other = 0; other < contents.size(); ++other) {
                if (contents[index] == contents[other]) {
                    ++copies;
                    if (other <= index)
                        ++priority;
                }
            }
            priorities[index] = priority;
            if (copies == 1)
                expectedUnique.insert(paths[index]);
            else if (!copiesOnly || priority > 1)
                expectedDuplicates.insert(paths[index]);
        }
        const auto resultPaths = [](const QVector<FileRecord> &records) {
            QSet<QString> result;
            for (const FileRecord &record : records)
                result.insert(record.path);
            return result;
        };
        for (int pass = 0; pass < (cache ? 2 : 1); ++pass) {
            // Reopen the engine to cover persisted hashes, not only in-memory entries.
            SearchEngine engine(nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
            SearchOptions options = optionsFor(directory.path(), QStringLiteral("Duplicates search"));
            options.strictDuplicateComparison = strict;
            options.useCache = cache;
            options.showDuplicateCopiesOnly = copiesOnly;
            const QVector<FileRecord> duplicates = runSearch(engine, options);
            QCOMPARE(resultPaths(duplicates), expectedDuplicates);
            QCOMPARE(duplicates.size(), expectedDuplicates.size());
            for (const FileRecord &record : duplicates) {
                const qsizetype index = paths.indexOf(record.path);
                QVERIFY(index >= 0);
                QCOMPARE(record.duplicateCopy, priorities[index]);
                QVERIFY(record.group > 0);
                for (const FileRecord &other : duplicates) {
                    const qsizetype otherIndex = paths.indexOf(other.path);
                    QCOMPARE(record.group == other.group,
                             contents[index] == contents[otherIndex]);
                }
            }
            options.mode = QStringLiteral("Non-Duplicates search");
            QCOMPARE(resultPaths(runSearch(engine, options)), expectedUnique);
        }
    }

    void doesNotTreatSymlinkAliasAsIndependentDuplicate()
    {
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        const QString target = directory.filePath(QStringLiteral("z-target.bin"));
        const QString alias = directory.filePath(QStringLiteral("a-alias.bin"));
        const QString copy = directory.filePath(QStringLiteral("copy.bin"));
        QVERIFY(writeFile(target, QByteArray("only physical copy")));
        QVERIFY(::symlink(QFile::encodeName(target).constData(),
                          QFile::encodeName(alias).constData()) == 0);
        SearchEngine engine(nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(directory.path(), QStringLiteral("Duplicates search"));
        for (bool followLinks : {false, true}) {
            options.followLinks = followLinks;
            QVERIFY2(runSearch(engine, options).isEmpty(),
                     "A symlink and its target must not form a duplicate group");
        }
        QVERIFY(writeFile(copy, QByteArray("only physical copy")));
        const QVector<FileRecord> results = runSearch(engine, options);
        QCOMPARE(namesOf(results), QSet<QString>({QStringLiteral("z-target.bin"),
                                                  QStringLiteral("copy.bin")}));
    }

    void doesNotTreatHardlinksAsIndependentCopies()
    {
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        const QString first = directory.filePath(QStringLiteral("a.bin"));
        const QString alias = directory.filePath(QStringLiteral("b.bin"));
        QVERIFY(writeFile(first, QByteArray("data")));
        QVERIFY(::link(QFile::encodeName(first).constData(),
                       QFile::encodeName(alias).constData()) == 0);
        SearchEngine engine(nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(directory.path(), QStringLiteral("Duplicates search"));
        QVERIFY(runSearch(engine, options).isEmpty());
        QVERIFY(writeFile(directory.filePath(QStringLiteral("c.bin")), QByteArray("data")));
        const auto results = runSearch(engine, options);
        QCOMPARE(results.size(), 2);
        QVERIFY(results[0].inode != results[1].inode);
        QCOMPARE(results[0].group, results[1].group);
    }

    void rejectsChangedFilesInNameComparison_data()
    {
        QTest::addColumn<QString>("filter");
        QTest::newRow("identical") << QStringLiteral("Only identical content");
        QTest::newRow("different") << QStringLiteral("Only non-identical content");
    }

    void rejectsChangedFilesInNameComparison()
    {
        QFETCH(QString, filter);
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("sub")));
        const QString second = directory.filePath(QStringLiteral("sub/same.bin"));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("same.bin")), QByteArray("same")));
        QVERIFY(writeFile(second, QByteArray("same")));
        SearchEngine engine(nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        bool changed = false;
        connect(&engine, &SearchEngine::phaseProgress, &engine,
                [&](const QString &phase, quint64 done, quint64 total) {
            if (phase == QStringLiteral("Comparing duplicate names") && total && done == total)
                changed = writeFile(second, QByteArray("different size"));
        }, Qt::DirectConnection);
        SearchOptions options = optionsFor(directory.path(), QStringLiteral("Duplicate names search"));
        options.duplicateNameMode = filter;
        QSignalSpy finished(&engine, &SearchEngine::finished);
        engine.search(options);
        QVERIFY(changed);
        QCOMPARE(finished.size(), 1);
        const auto result = finished.takeFirst();
        QVERIFY(result.at(0).value<QVector<FileRecord>>().isEmpty());
        QVERIFY(result.at(1).toString().contains(QStringLiteral("search incomplete")));
    }

    void rejectsFilesChangedAfterHashing_data()
    {
        QTest::addColumn<bool>("strict");
        QTest::newRow("hash") << false;
        QTest::newRow("byte-comparison") << true;
    }

    void rejectsFilesChangedAfterHashing()
    {
        QFETCH(bool, strict);
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        const QString first = directory.filePath(QStringLiteral("first.bin"));
        const QString second = directory.filePath(QStringLiteral("second.bin"));
        QVERIFY(writeFile(first, QByteArray("same")));
        QVERIFY(writeFile(second, QByteArray("same")));
        SearchEngine engine(nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        std::atomic_bool changed = false;
        std::atomic_bool writeSucceeded = false;
        connect(&engine, &SearchEngine::phaseProgress, &engine,
                [&](const QString &phase, quint64 done, quint64 total) {
            if (phase == QStringLiteral("Hashing duplicate candidates")
                && total > 0 && done == total && !changed.exchange(true))
                writeSucceeded.store(writeFile(second, QByteArray("diff")));
        }, Qt::DirectConnection);
        SearchOptions options = optionsFor(directory.path(), QStringLiteral("Duplicates search"));
        options.strictDuplicateComparison = strict;
        QSignalSpy finished(&engine, &SearchEngine::finished);
        engine.search(options);
        QVERIFY(changed.load());
        QVERIFY(writeSucceeded.load());
        QCOMPARE(finished.size(), 1);
        const auto result = finished.takeFirst();
        QVERIFY(result.at(0).value<QVector<FileRecord>>().isEmpty());
        QVERIFY(result.at(1).toString().contains(QStringLiteral("search incomplete")));
    }

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

    void appliesResultLimitAfterDuplicateClassification()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(writeFile(directory.filePath(QStringLiteral("duplicate-a.bin")),
                          QByteArray("duplicate")));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("duplicate-b.bin")),
                          QByteArray("duplicate")));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("unique.bin")),
                          QByteArray("unique-size")));

        SearchEngine engine(nullptr, directory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Duplicates search"));
        options.maxResults = 1;
        QCOMPARE(runSearch(engine, options).size(), 1);

        options.mode = QStringLiteral("Non-Duplicates search");
        const QVector<FileRecord> nonDuplicates = runSearch(engine, options);
        QCOMPARE(nonDuplicates.size(), 1);
        QCOMPARE(nonDuplicates.front().name, QStringLiteral("unique.bin"));

        options.mode = QStringLiteral("Duplicate names search");
        options.duplicateNameMode = QStringLiteral("All files and folders");
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("one")));
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("two")));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("one/same-name.bin")),
                          QByteArray("left")));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("two/same-name.bin")),
                          QByteArray("right")));
        options.fileWildcards = QStringLiteral("same-name.bin");
        options.maxResults = 1;
        QCOMPARE(runSearch(engine, options).size(), 1);
    }

    void appliesStandardSearchLimitAcrossNonRecursiveRoots()
    {
        QTemporaryDir first;
        QTemporaryDir second;
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        QVERIFY(writeFile(first.filePath(QStringLiteral("first.txt")), QByteArray("one")));
        QVERIFY(writeFile(second.filePath(QStringLiteral("second.txt")), QByteArray("two")));

        SearchEngine engine(nullptr, first.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            first.path() + u';' + second.path(), QStringLiteral("Standard search"));
        options.recursive = false;
        options.maxResults = 1;
        QCOMPARE(runSearch(engine, options).size(), 1);
    }

    void contentFilterKeepsFoldersWithoutDisablingFileFiltering()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("folder-entry")));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("match.txt")),
                          QByteArray("needle here")));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("miss.txt")),
                          QByteArray("other text")));

        SearchEngine engine(nullptr, directory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Standard search"));
        options.findFolders = true;
        options.contains = QStringLiteral("needle");
        const QSet<QString> names = namesOf(runSearch(engine, options));
        QVERIFY(names.contains(QStringLiteral("folder-entry")));
        QVERIFY(names.contains(QStringLiteral("match.txt")));
        QVERIFY(!names.contains(QStringLiteral("miss.txt")));
    }

    void rejectsWhitespaceOnlyBinaryQueriesBeforeScanning()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(writeFile(directory.filePath(QStringLiteral("file.bin")),
                          QByteArray("anything")));
        SearchEngine engine(nullptr, directory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Standard search"));
        options.binary = true;
        options.contains = QStringLiteral(" \t  ");
        QSignalSpy finished(&engine, &SearchEngine::finished);
        engine.search(options);
        QCOMPARE(finished.size(), 1);
        const QList<QVariant> result = finished.takeFirst();
        QVERIFY(result.at(0).value<QVector<FileRecord>>().isEmpty());
        QCOMPARE(result.at(1).toString(), QStringLiteral("Invalid hexadecimal search query"));
    }

    void excludedFoldersAreNeverReturned()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("excluded")));
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("included")));

        SearchEngine engine(nullptr, directory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Standard search"));
        options.findFiles = false;
        options.findFolders = true;
        options.excludeFolders = QStringLiteral("excluded");
        QCOMPARE(namesOf(runSearch(engine, options)),
                 QSet<QString>({QStringLiteral("included")}));

        options.recursive = false;
        QCOMPARE(namesOf(runSearch(engine, options)),
                 QSet<QString>({QStringLiteral("included")}));
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
            QVERIFY(QFile::setPermissions(first, {}));
            QVERIFY(QFile::setPermissions(second, {}));
            options.strictDuplicateComparison = true;
            finished.clear();
            engine.search(options);
            QCOMPARE(finished.size(), 1);
            QVERIFY(finished.takeFirst().at(0).value<QVector<FileRecord>>().isEmpty());
        }
        QFile::remove(cachePath);
    }

    void invalidatesCachedHashesAndContentMatchesWhenCtimeChanges()
    {
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        const QString first = directory.filePath(QStringLiteral("first.bin"));
        const QString second = directory.filePath(QStringLiteral("second.bin"));
        QVERIFY(writeFile(first, QByteArray("same")));
        QVERIFY(writeFile(second, QByteArray("same")));

        SearchEngine engine(nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions duplicateOptions = optionsFor(
            directory.path(), QStringLiteral("Duplicates search"));
        duplicateOptions.useCache = true;
        QCOMPARE(runSearch(engine, duplicateOptions).size(), 2);

        SearchOptions contentOptions = optionsFor(
            directory.path(), QStringLiteral("File search"));
        contentOptions.useCache = true;
        contentOptions.contains = QStringLiteral("same");
        QCOMPARE(runSearch(engine, contentOptions).size(), 2);

        QThread::msleep(20);
        struct stat before {};
        struct stat after {};
        QVERIFY(rewriteFilePreservingMtime(second, QByteArray("diff"), before, after));

        QVERIFY(runSearch(engine, duplicateOptions).isEmpty());
        const QVector<FileRecord> contentResults = runSearch(engine, contentOptions);
        QCOMPARE(namesOf(contentResults), QSet<QString>({QStringLiteral("first.bin")}));
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

    void unreadableDuplicateCandidatesAreUnknownAndReported()
    {
        if (::geteuid() == 0)
            QSKIP("Root can read files regardless of their permission bits");
        QTemporaryDir directory;
        QTemporaryDir cacheDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(cacheDirectory.isValid());
        const QString first = directory.filePath(QStringLiteral("first.bin"));
        const QString second = directory.filePath(QStringLiteral("second.bin"));
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("one")));
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("two")));
        const QString sameNameFirst = directory.filePath(QStringLiteral("one/report.bin"));
        const QString sameNameSecond = directory.filePath(QStringLiteral("two/report.bin"));
        QVERIFY(writeFile(first, QByteArray("blocked")));
        QVERIFY(writeFile(second, QByteArray("blocked")));
        QVERIFY(writeFile(sameNameFirst, QByteArray("blocked")));
        QVERIFY(writeFile(sameNameSecond, QByteArray("blocked")));
        QVERIFY(QFile::setPermissions(first, {}));
        QVERIFY(QFile::setPermissions(second, {}));
        QVERIFY(QFile::setPermissions(sameNameFirst, {}));
        QVERIFY(QFile::setPermissions(sameNameSecond, {}));

        SearchEngine engine(
            nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            directory.path(), QStringLiteral("Duplicates search"));
        QSignalSpy finished(&engine, &SearchEngine::finished);
        engine.search(options);
        QCOMPARE(finished.size(), 1);
        QList<QVariant> result = finished.takeFirst();
        QVERIFY(result.at(0).value<QVector<FileRecord>>().isEmpty());
        const QString duplicateMessage = result.at(1).toString();
        QVERIFY(duplicateMessage.contains(QStringLiteral("search incomplete")));
        QVERIFY(QRegularExpression(QStringLiteral(
            "[1-9][0-9]* path/file checks failed")).match(duplicateMessage).hasMatch());

        options.mode = QStringLiteral("Duplicate names search");
        options.fileWildcards = QStringLiteral("report.bin");
        options.duplicateNameMode = QStringLiteral("Only non-identical content");
        engine.search(options);
        QCOMPARE(finished.size(), 1);
        result = finished.takeFirst();
        QVERIFY(result.at(0).value<QVector<FileRecord>>().isEmpty());
        QVERIFY(result.at(1).toString().contains(QStringLiteral("search incomplete")));

        options.mode = QStringLiteral("Non-Duplicates search");
        engine.search(options);
        QCOMPARE(finished.size(), 1);
        result = finished.takeFirst();
        QVERIFY(result.at(0).value<QVector<FileRecord>>().isEmpty());
        QVERIFY(result.at(1).toString().contains(QStringLiteral("search incomplete")));

        options.mode = QStringLiteral("Standard search");
        options.fileWildcards = QStringLiteral("*");
        options.contains = QStringLiteral("blocked");
        engine.search(options);
        QCOMPARE(finished.size(), 1);
        result = finished.takeFirst();
        QVERIFY(result.at(0).value<QVector<FileRecord>>().isEmpty());
        QVERIFY(result.at(1).toString().contains(QStringLiteral("search incomplete")));
    }

    void reportsUnreadableSearchRoots()
    {
        QTemporaryDir cacheDirectory;
        QVERIFY(cacheDirectory.isValid());
        SearchEngine engine(
            nullptr, cacheDirectory.filePath(QStringLiteral("cache.dat")));
        SearchOptions options = optionsFor(
            QStringLiteral("/path/that/does/not/exist/folderscope-audit"),
            QStringLiteral("Standard search"));
        QSignalSpy finished(&engine, &SearchEngine::finished);
        engine.search(options);
        QCOMPARE(finished.size(), 1);
        const QList<QVariant> result = finished.takeFirst();
        QVERIFY(result.at(0).value<QVector<FileRecord>>().isEmpty());
        const QString message = result.at(1).toString();
        QVERIFY(message.contains(QStringLiteral("search incomplete")));
        QVERIFY(QRegularExpression(QStringLiteral(
            "[1-9][0-9]* path/file checks failed")).match(message).hasMatch());
    }
};

QTEST_GUILESS_MAIN(SearchEngineTest)

#include "search_engine_test.moc"
