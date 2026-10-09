#include "search_core.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>
#include <unistd.h>

namespace {
FileRecord recordFor(const QString &path)
{
    struct stat st {};
    const QByteArray nativePath = QFile::encodeName(path);
    if (::stat(nativePath.constData(), &st) != 0)
        return {};
    FileRecord record;
    record.path = path;
    record.size = st.st_size;
    record.modified = st.st_mtime;
    record.modifiedNs =
        qint64(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec;
    record.changedNs =
        qint64(st.st_ctim.tv_sec) * 1'000'000'000LL + st.st_ctim.tv_nsec;
    record.device = st.st_dev;
    record.inode = st.st_ino;
    record.type = QStringLiteral("File");
    return record;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(data) == data.size();
}
}

class SearchCoreTest final : public QObject {
    Q_OBJECT

private slots:
    void rejectsIncompleteUtf8AtEndOfFile()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("truncated.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray("text\xe2\x82", 6)), 6);
        file.close();
        SearchOptions options;
        options.contains = QStringLiteral("absent");
        options.caseSensitive = false;
        QCOMPARE(fileContentResult(path, options), FileContentResult::Error);
    }

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

    void collapsesRedundantSearchRoots()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("child")));
        QVERIFY(root.mkpath(QStringLiteral("misc")));

        SearchOptions options;
        const QString child = directory.filePath(QStringLiteral("child"));
        const QString misc = directory.filePath(QStringLiteral("misc"));
        options.roots = QStringLiteral("%1;%2/;%1;%3")
                            .arg(child, directory.path(), misc);
        options.excludeFolders = QStringLiteral("unrelated");

        const EffectiveRoots effective = effectiveSearchRoots(options);
        QCOMPARE(effective.paths, QStringList({directory.path()}));
        QCOMPARE(effective.skipped, 3);
    }

    void preservesNestedRootsThatExpandCoverage()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("child")));
        const QString child = directory.filePath(QStringLiteral("child"));

        SearchOptions options;
        options.roots = directory.path() + u';' + child;

        options.recursive = false;
        QCOMPARE(effectiveSearchRoots(options).paths.size(), 2);

        options.recursive = true;
        options.maxDepth = 1;
        QCOMPARE(effectiveSearchRoots(options).paths.size(), 2);

        options.maxDepth = 0;
        options.maxResults = 1;
        QCOMPARE(effectiveSearchRoots(options).paths.size(), 2);

        options.maxResults = 0;
        options.subfolderWildcards = QStringLiteral("matched-*");
        QCOMPARE(effectiveSearchRoots(options).paths.size(), 2);

        options.subfolderWildcards = QStringLiteral("*");
        options.excludeFolders = QStringLiteral("child");
        QCOMPARE(effectiveSearchRoots(options).paths.size(), 2);

        options.excludeFolders = QStringLiteral("unrelated");
        QCOMPARE(effectiveSearchRoots(options).paths,
                 QStringList({directory.path()}));
    }

    void respectsSymlinksWhenCollapsingRoots()
    {
        QTemporaryDir directory;
        QTemporaryDir external;
        QVERIFY(directory.isValid());
        QVERIFY(external.isValid());
        QDir externalRoot(external.path());
        QVERIFY(externalRoot.mkpath(QStringLiteral("child")));
        const QString link = directory.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(external.path(), link));
        const QString linkedChild = QDir(link).filePath(QStringLiteral("child"));

        SearchOptions options;
        options.roots = directory.path() + u';' + linkedChild;
        options.followLinks = false;
        QCOMPARE(effectiveSearchRoots(options).paths.size(), 2);

        options.followLinks = true;
        QCOMPARE(effectiveSearchRoots(options).paths,
                 QStringList({directory.path()}));

        options.roots = external.path() + u';' + link;
        const EffectiveRoots aliases = effectiveSearchRoots(options);
        QCOMPARE(aliases.paths.size(), 1);
        QCOMPARE(aliases.skipped, 1);
    }

    void prefersTheMostSpecificConfiguredRoot()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("Temp")));
        QVERIFY(root.mkpath(QStringLiteral("misc")));
        const QString temp = directory.filePath(QStringLiteral("Temp"));
        const QString misc = directory.filePath(QStringLiteral("misc"));
        const QStringList roots{directory.path(), temp, misc};

        QCOMPARE(preferredRootIndex(
                     directory.filePath(QStringLiteral("base.mp4")), roots), 0);
        QCOMPARE(preferredRootIndex(
                     QDir(temp).filePath(QStringLiteral("copy.mp4")), roots), 1);
        QCOMPARE(preferredRootIndex(
                     QDir(misc).filePath(QStringLiteral("copy.mp4")), roots), 2);
        QCOMPARE(preferredRootIndex(
                     directory.filePath(QStringLiteral("unrelated/file.mp4")), roots), 0);

        const QStringList similarRoots{directory.filePath(QStringLiteral("foo"))};
        QCOMPARE(preferredRootIndex(
                     directory.filePath(QStringLiteral("foobar/file.mp4")), similarRoots), -1);
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

    void caseInsensitiveTextSearchUsesUnicodeCaseFoldingAcrossChunks()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("unicode.txt"));
        const QByteArray content = QByteArray(1024 * 1024 - 1, 'x')
            + QByteArray("\xc3\x84") + QByteArray(" stra\xc3\x9f" "e");
        QVERIFY(writeFile(path, content));

        SearchOptions options;
        options.contains = QStringLiteral("ä");
        QVERIFY(fileContains(path, options));
        options.contains = QStringLiteral("STRAßE");
        QVERIFY(fileContains(path, options));
        options.caseSensitive = true;
        QVERIFY(!fileContains(path, options));
    }

    void rejectsEmptyHexNeedles()
    {
        QVERIFY(!hexNeedle(QString{}));
        QVERIFY(!hexNeedle(QStringLiteral(" \t\n ")));
        QVERIFY(!hexNeedle(QStringLiteral("abc")));
        QVERIFY(!hexNeedle(QStringLiteral("0g")));
        const auto valid = hexNeedle(QStringLiteral("41 42"));
        QVERIFY(valid.has_value());
        QCOMPARE(*valid, QByteArray("AB"));
    }

    void deletesDirectorySymlinkWithoutTouchingTarget()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString target = directory.filePath(QStringLiteral("target"));
        QVERIFY(QDir().mkpath(QDir(target).filePath(QStringLiteral("child"))));
        const QString targetFile = QDir(target).filePath(QStringLiteral("keep.txt"));
        QVERIFY(writeFile(targetFile, QByteArray("keep")));

        const QString link = directory.filePath(QStringLiteral("target-link"));
        const QByteArray nativeTarget = QFile::encodeName(target);
        const QByteArray nativeLink = QFile::encodeName(link);
        QVERIFY(::symlink(nativeTarget.constData(), nativeLink.constData()) == 0);
        QVERIFY(QFileInfo(link).isSymLink());

        QVERIFY(removePathWithoutFollowingSymlink(link));
        QVERIFY(!QFileInfo(link).isSymLink());
        QVERIFY(QFileInfo(targetFile).isFile());

        const QString normalDirectory = directory.filePath(QStringLiteral("normal"));
        QVERIFY(QDir().mkpath(QDir(normalDirectory).filePath(QStringLiteral("child"))));
        const QString normalFile = QDir(normalDirectory).filePath(QStringLiteral("child/file.txt"));
        QVERIFY(writeFile(normalFile, QByteArray("remove")));
        QVERIFY(removePathWithoutFollowingSymlink(normalDirectory));
        QVERIFY(!QFileInfo::exists(normalDirectory));
    }

    void rejectsFifoReadsWithoutBlocking()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("waiting.fifo"));
        const QByteArray nativePath = QFile::encodeName(path);
        QVERIFY(::mkfifo(nativePath.constData(), 0600) == 0);

        SearchOptions options;
        options.contains = QStringLiteral("needle");
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!fileContains(path, options));
        QVERIFY(!filesEqual(path, path));
        QVERIFY(sha256(path).isEmpty());
        QVERIFY(sha256(recordFor(path)).isEmpty());
        QVERIFY(timer.elapsed() < 1000);
    }

    void recordBoundReadsRejectRetargetedSymlinks()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString original = directory.filePath(QStringLiteral("original.txt"));
        const QString alternate = directory.filePath(QStringLiteral("alternate.txt"));
        const QString link = directory.filePath(QStringLiteral("selected.txt"));
        QVERIFY(writeFile(original, QByteArray("match")));
        QVERIFY(writeFile(alternate, QByteArray("other")));

        const QByteArray nativeOriginal = QFile::encodeName(original);
        const QByteArray nativeAlternate = QFile::encodeName(alternate);
        const QByteArray nativeLink = QFile::encodeName(link);
        QVERIFY(::symlink(nativeOriginal.constData(), nativeLink.constData()) == 0);
        const FileRecord record = recordFor(link);
        const QByteArray originalDigest = sha256(record);
        QVERIFY(!originalDigest.isEmpty());

        SearchOptions options;
        options.contains = QStringLiteral("match");
        QVERIFY(fileContains(record, options));

        QVERIFY(QFile::remove(link));
        QVERIFY(::symlink(nativeAlternate.constData(), nativeLink.constData()) == 0);
        QVERIFY(!fileContains(record, options));
        QVERIFY(sha256(record).isEmpty());
        QVERIFY(!filesEqual(record, recordFor(alternate)));

        options.contains = QStringLiteral("other");
        QVERIFY(fileContains(link, options));

        QVERIFY(QFile::remove(link));
        QVERIFY(::symlink(nativeOriginal.constData(), nativeLink.constData()) == 0);
        options.contains = QStringLiteral("match");
        QVERIFY(fileContains(record, options));
        QCOMPARE(sha256(record), originalDigest);
    }

    void cancelsFileOperations()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("file.bin"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("needle"), qint64(6));
        file.close();

        std::atomic_bool cancelled = true;
        SearchOptions options;
        options.contains = QStringLiteral("needle");
        QVERIFY(!fileContains(path, options, &cancelled));
        QVERIFY(!filesEqual(path, path, &cancelled));
        QVERIFY(sha256(path, &cancelled).isEmpty());
    }

    void samplesLargeFilesAndValidatesSignatures()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray content(duplicateSampleThreshold + 1024 * 1024, 'x');
        const QString firstPath = directory.filePath(QStringLiteral("first.bin"));
        const QString samePath = directory.filePath(QStringLiteral("same.bin"));
        const QString outsidePath = directory.filePath(QStringLiteral("outside.bin"));
        const QString startPath = directory.filePath(QStringLiteral("start.bin"));
        const QString middlePath = directory.filePath(QStringLiteral("middle.bin"));
        const QString endPath = directory.filePath(QStringLiteral("end.bin"));

        QVERIFY(writeFile(firstPath, content));
        QVERIFY(writeFile(samePath, content));

        QByteArray outside = content;
        outside[1024 * 1024] = 'y';
        QVERIFY(writeFile(outsidePath, outside));
        QByteArray start = content;
        start[0] = 'y';
        QVERIFY(writeFile(startPath, start));
        QByteArray middle = content;
        middle[(content.size() - duplicateSampleBlockSize) / 2] = 'y';
        QVERIFY(writeFile(middlePath, middle));
        QByteArray end = content;
        end[end.size() - 1] = 'y';
        QVERIFY(writeFile(endPath, end));

        const FileRecord first = recordFor(firstPath);
        const QByteArray sample = sampleSha256(first);
        QVERIFY(!sample.isEmpty());
        QCOMPARE(sampleSha256(recordFor(samePath)), sample);
        QCOMPARE(sampleSha256(recordFor(outsidePath)), sample);
        QVERIFY(sampleSha256(recordFor(startPath)) != sample);
        QVERIFY(sampleSha256(recordFor(middlePath)) != sample);
        QVERIFY(sampleSha256(recordFor(endPath)) != sample);
        QVERIFY(sha256(recordFor(outsidePath)) != sha256(first));

        std::atomic_bool cancelled = true;
        QVERIFY(sampleSha256(first, &cancelled).isEmpty());

        const QString shortPath = directory.filePath(QStringLiteral("short.bin"));
        QVERIFY(writeFile(shortPath, QByteArray("short")));
        QVERIFY(sampleSha256(recordFor(shortPath)).isEmpty());
        QVERIFY(!sha256(recordFor(shortPath)).isEmpty());

        const QString emptyPath = directory.filePath(QStringLiteral("empty.bin"));
        QVERIFY(writeFile(emptyPath, {}));
        QVERIFY(sampleSha256(recordFor(emptyPath)).isEmpty());
        QVERIFY(!sha256(recordFor(emptyPath)).isEmpty());

        const QString replacedPath = directory.filePath(QStringLiteral("replaced.bin"));
        QVERIFY(writeFile(replacedPath, content));
        const FileRecord replaced = recordFor(replacedPath);
        QVERIFY(QFile::remove(replacedPath));
        QVERIFY(writeFile(replacedPath, content));
        QVERIFY(!fileSignatureMatches(replaced));
        QVERIFY(sha256(replaced).isEmpty());
        QVERIFY(sampleSha256(replaced).isEmpty());

        FileRecord missing = first;
        missing.path = directory.filePath(QStringLiteral("missing.bin"));
        QVERIFY(sha256(missing).isEmpty());
        QVERIFY(sampleSha256(missing).isEmpty());
    }
};

QTEST_GUILESS_MAIN(SearchCoreTest)

#include "search_core_test.moc"
