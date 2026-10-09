#include "search_core.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>

#include <algorithm>
#include <array>
#include <numeric>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

QStringList patterns(const QString &text)
{
    QStringList out;
    QString item;
    bool quoted = false;
    for (QChar ch : text) {
        if (ch == u'"') {
            quoted = !quoted;
        } else if (!quoted && (ch == u';' || ch == u',')) {
            if (!item.trimmed().isEmpty())
                out.push_back(item.trimmed());
            item.clear();
        } else {
            item += ch;
        }
    }
    if (!item.trimmed().isEmpty())
        out.push_back(item.trimmed());
    return out;
}

QStringList exclusionPatterns(const QString &text)
{
    QString normalized = text;
    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(";"));
    QStringList result = patterns(normalized);
    for (QString &pattern : result) {
        if (!pattern.contains(u'*') && !pattern.contains(u'?') && !pattern.contains(u'/'))
            pattern = QStringLiteral("*.") + pattern.remove(QRegularExpression(QStringLiteral("^\\.")));
    }
    return result;
}

CompiledWildcardMatcher::CompiledWildcardMatcher(const QStringList &items,
                                                 Qt::CaseSensitivity cs)
{
    expressions_.reserve(items.size());
    for (const QString &pattern : items) {
        expressions_.push_back(QRegularExpression(
            QRegularExpression::wildcardToRegularExpression(pattern),
            cs == Qt::CaseInsensitive ? QRegularExpression::CaseInsensitiveOption
                                      : QRegularExpression::NoPatternOption));
    }
}

bool CompiledWildcardMatcher::matches(const QString &value) const
{
    for (const QRegularExpression &regex : expressions_) {
        if (regex.match(value).hasMatch())
            return true;
    }
    return false;
}

bool wildcardMatch(const QString &value, const QStringList &items, Qt::CaseSensitivity cs)
{
    return CompiledWildcardMatcher(items, cs).matches(value);
}

namespace {
struct RootCandidate {
    QString path;
};

bool isDescendantPath(const QString &parent, const QString &child,
                      QString *relativePath = nullptr)
{
    const QString relative = QDir(parent).relativeFilePath(child);
    const bool descendant = relative != QStringLiteral(".")
        && relative != QStringLiteral("..")
        && !relative.startsWith(QStringLiteral("../"))
        && !QDir::isAbsolutePath(relative);
    if (descendant && relativePath)
        *relativePath = relative;
    return descendant;
}

bool canCollapseInto(const RootCandidate &parent, const RootCandidate &child,
                     const SearchOptions &options, const QStringList &excludedFolders,
                     Qt::CaseSensitivity cs)
{
    if (!QFileInfo(parent.path).isDir())
        return false;

    QString relative;
    if (!isDescendantPath(parent.path, child.path, &relative))
        return false;

    QString current = parent.path;
    const QStringList segments = relative.split(u'/', Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        current = QDir(current).filePath(segment);
        const QFileInfo info(current);
        if ((!options.followLinks && info.isSymLink())
            || (!excludedFolders.isEmpty()
                && (wildcardMatch(info.fileName(), excludedFolders, cs)
                    || wildcardMatch(QDir::cleanPath(current), excludedFolders, cs)))) {
            return false;
        }
    }
    return true;
}
}

EffectiveRoots effectiveSearchRoots(const SearchOptions &options)
{
    QVector<RootCandidate> unique;
    QSet<QString> identities;
    int skipped = 0;
    for (const QString &rootText : patterns(options.roots)) {
        const QFileInfo info(rootText);
        const QString path = QDir::cleanPath(info.absoluteFilePath());
        const QString canonical = info.canonicalFilePath();
        const QString identity = canonical.isEmpty()
            ? path : QDir::cleanPath(canonical);
        if (identities.contains(identity)) {
            ++skipped;
            continue;
        }
        identities.insert(identity);
        unique.push_back({path});
    }

    const QStringList subfolderMasks = patterns(options.subfolderWildcards).isEmpty()
        ? QStringList{QStringLiteral("*")} : patterns(options.subfolderWildcards);
    const bool universalSubfolderMask =
        subfolderMasks.contains(QStringLiteral("*"));
    if (!options.recursive || options.maxDepth > 0 || options.maxResults > 0
        || !universalSubfolderMask) {
        EffectiveRoots result;
        result.skipped = skipped;
        for (const RootCandidate &root : unique) {
            result.paths.push_back(root.path);
            result.priorityPaths.push_back(root.path);
        }
        return result;
    }

    const Qt::CaseSensitivity cs =
        options.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const QStringList excludedFolders = patterns(options.excludeFolders);
    EffectiveRoots result;
    result.skipped = skipped;
    for (const RootCandidate &root : unique)
        result.priorityPaths.push_back(root.path);
    for (qsizetype childIndex = 0; childIndex < unique.size(); ++childIndex) {
        bool redundant = false;
        for (qsizetype parentIndex = 0; parentIndex < unique.size(); ++parentIndex) {
            if (parentIndex == childIndex)
                continue;
            if (canCollapseInto(unique[parentIndex], unique[childIndex], options,
                                excludedFolders, cs)) {
                redundant = true;
                break;
            }
        }
        if (redundant) {
            ++result.skipped;
        } else {
            result.paths.push_back(unique[childIndex].path);
        }
    }
    return result;
}

int preferredRootIndex(const QString &path, const QStringList &roots)
{
    const QString normalizedPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    int bestIndex = -1;
    qsizetype bestLength = 0;
    for (qsizetype index = 0; index < roots.size(); ++index) {
        const QString root = QDir::cleanPath(QFileInfo(roots[index]).absoluteFilePath());
        if (normalizedPath != root && !isDescendantPath(root, normalizedPath))
            continue;
        if (root.size() > bestLength) {
            bestIndex = int(index);
            bestLength = root.size();
        }
    }
    return bestIndex;
}

bool shouldShowDuplicateResult(bool copiesOnly, int keeperPriority)
{
    if (keeperPriority < 1)
        return false;
    return !copiesOnly || keeperPriority > 1;
}

QString humanSize(qint64 bytes)
{
    static const QStringList units{QStringLiteral("B"), QStringLiteral("KB"),
                                   QStringLiteral("MB"), QStringLiteral("GB"),
                                   QStringLiteral("TB")};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(bytes)
                     : QStringLiteral("%1 %2").arg(value, 0, 'f', 2).arg(units[unit]);
}

QString ownerName(uid_t uid)
{
    struct passwd pwd {};
    struct passwd *result = nullptr;
    QByteArray buffer(16384, Qt::Uninitialized);
    if (getpwuid_r(uid, &pwd, buffer.data(), static_cast<size_t>(buffer.size()), &result) == 0 && result)
        return QString::fromLocal8Bit(result->pw_name);
    return QString::number(uid);
}

std::optional<QByteArray> hexNeedle(QString text)
{
    text.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (text.isEmpty() || text.size() % 2 != 0
        || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]+$")).match(text).hasMatch())
        return std::nullopt;
    return QByteArray::fromHex(text.toLatin1());
}

namespace {
bool isCancelled(const std::atomic_bool *cancelled)
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

qint64 modifiedNanoseconds(const struct stat &st)
{
    return qint64(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec;
}

qint64 changedNanoseconds(const struct stat &st)
{
    return qint64(st.st_ctim.tv_sec) * 1'000'000'000LL + st.st_ctim.tv_nsec;
}

bool signatureMatches(const FileRecord &record, const struct stat &st)
{
    return record.size == st.st_size
        && record.modifiedNs == modifiedNanoseconds(st)
        && record.changedNs == changedNanoseconds(st)
        && record.device == quint64(st.st_dev)
        && record.inode == quint64(st.st_ino);
}

bool statMatches(const FileRecord &record)
{
    struct stat st {};
    const QByteArray nativePath = QFile::encodeName(record.path);
    return ::stat(nativePath.constData(), &st) == 0 && signatureMatches(record, st);
}

bool sameFileState(const struct stat &left, const struct stat &right)
{
    return S_ISREG(left.st_mode) && S_ISREG(right.st_mode)
        && left.st_dev == right.st_dev
        && left.st_ino == right.st_ino
        && left.st_size == right.st_size
        && modifiedNanoseconds(left) == modifiedNanoseconds(right)
        && changedNanoseconds(left) == changedNanoseconds(right);
}

bool openRegularFile(QFile &file, const QString &path, struct stat &openedState,
                     const FileRecord *expected = nullptr)
{
    const QByteArray nativePath = QFile::encodeName(path);
    struct stat pathStat {};
    if (::stat(nativePath.constData(), &pathStat) != 0 || !S_ISREG(pathStat.st_mode))
        return false;
    if (expected && !signatureMatches(*expected, pathStat))
        return false;

    const int fd = ::open(nativePath.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return false;

    struct stat openedStat {};
    if (::fstat(fd, &openedStat) != 0 || !S_ISREG(openedStat.st_mode)) {
        ::close(fd);
        return false;
    }
    if (expected ? !signatureMatches(*expected, openedStat)
                 : !sameFileState(pathStat, openedStat)) {
        ::close(fd);
        return false;
    }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return false;
    }
    openedState = openedStat;
    return true;
}

bool regularFileUnchanged(QFile &file, const QString &path,
                          const struct stat &openedState,
                          const FileRecord *expected = nullptr)
{
    struct stat descriptorState {};
    if (::fstat(file.handle(), &descriptorState) != 0
        || !sameFileState(openedState, descriptorState))
        return false;
    if (expected)
        return signatureMatches(*expected, descriptorState) && statMatches(*expected);

    struct stat pathState {};
    const QByteArray nativePath = QFile::encodeName(path);
    return ::stat(nativePath.constData(), &pathState) == 0
        && sameFileState(openedState, pathState);
}

FileContentResult fileContainsImpl(const QString &path, const SearchOptions &options,
                                   const std::atomic_bool *cancelled,
                                   const FileRecord *expected)
{
    if (isCancelled(cancelled))
        return FileContentResult::Error;
    if (options.contains.isEmpty())
        return !expected || statMatches(*expected)
            ? FileContentResult::Match : FileContentResult::Error;

    const QStringList terms = options.multipleValues
        ? patterns(options.contains) : QStringList{options.contains};
    QList<QByteArray> byteNeedles;
    QList<QString> textNeedles;
    for (const QString &term : terms) {
        if (options.binary) {
            const auto bytes = hexNeedle(term);
            if (!bytes)
                return FileContentResult::Error;
            byteNeedles.push_back(*bytes);
        } else if (options.caseSensitive) {
            byteNeedles.push_back(term.toUtf8());
        } else {
            textNeedles.push_back(term.toCaseFolded());
        }
    }
    if (byteNeedles.isEmpty() && textNeedles.isEmpty())
        return !expected || statMatches(*expected)
            ? FileContentResult::Match : FileContentResult::Error;

    QFile file;
    struct stat openedState {};
    if (!openRegularFile(file, path, openedState, expected))
        return FileContentResult::Error;

    const bool textMode = !options.binary && !options.caseSensitive;
    const qsizetype needleCount = textMode ? textNeedles.size() : byteNeedles.size();
    QVector<bool> found(needleCount, false);
    const qsizetype overlap = !textMode
        ? std::accumulate(byteNeedles.cbegin(), byteNeedles.cend(), qsizetype{1},
                          [](qsizetype size, const QByteArray &needle) {
                              return std::max(size, needle.size());
                          })
        : std::accumulate(textNeedles.cbegin(), textNeedles.cend(), qsizetype{1},
                          [](qsizetype size, const QString &needle) {
                              return std::max(size, needle.size());
                          });
    const auto allMatched = [&] {
        return options.multipleAnd
            ? std::all_of(found.cbegin(), found.cend(), [](bool value) { return value; })
            : std::any_of(found.cbegin(), found.cend(), [](bool value) { return value; });
    };

    QByteArray byteTail;
    QString textTail;
    QStringDecoder decoder(QStringDecoder::Utf8);
    while (!file.atEnd() && !isCancelled(cancelled)) {
        QByteArray chunk = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError)
            return FileContentResult::Error;
        if (chunk.isEmpty()) {
            if (!file.atEnd())
                return FileContentResult::Error;
            break;
        }

        if (!textMode) {
            QByteArray data = byteTail + chunk;
            for (qsizetype i = 0; i < byteNeedles.size(); ++i)
                if (!found[i] && data.contains(byteNeedles[i]))
                    found[i] = true;
            if (allMatched())
                return regularFileUnchanged(file, path, openedState, expected)
                    ? FileContentResult::Match : FileContentResult::Error;
            byteTail = data.right(overlap - 1);
        } else {
            const QString decodedText = decoder.decode(chunk);
            const QString decoded = decodedText.toCaseFolded();
            if (decoder.hasError())
                return FileContentResult::Error;
            const QString data = textTail + decoded;
            for (qsizetype i = 0; i < textNeedles.size(); ++i)
                if (!found[i] && data.contains(textNeedles[i]))
                    found[i] = true;
            if (allMatched())
                return regularFileUnchanged(file, path, openedState, expected)
                    ? FileContentResult::Match : FileContentResult::Error;

            qsizetype keep = overlap - 1;
            if (keep > 0 && keep < data.size() && data.at(data.size() - keep).isLowSurrogate())
                ++keep;
            textTail = data.right(keep);
        }
    }

    if (isCancelled(cancelled))
        return FileContentResult::Error;
    if (textMode) {
        // A sentinel forces any pending UTF-8 sequence to be decoded. This
        // also works with Qt versions that do not provide finalize().
        const QString sentinel = decoder.decode(QByteArray(1, '\0'));
        Q_UNUSED(sentinel);
        if (decoder.hasError())
            return FileContentResult::Error;
    }
    if (!regularFileUnchanged(file, path, openedState, expected))
        return FileContentResult::Error;
    return allMatched() ? FileContentResult::Match : FileContentResult::NoMatch;
}

FileCompareResult filesEqualImpl(const QString &leftPath, const QString &rightPath,
                                const std::atomic_bool *cancelled,
                                const FileRecord *leftExpected,
                                const FileRecord *rightExpected)
{
    if (isCancelled(cancelled))
        return FileCompareResult::Error;
    QFile left;
    QFile right;
    struct stat leftOpenedState {};
    struct stat rightOpenedState {};
    if (!openRegularFile(left, leftPath, leftOpenedState, leftExpected)
        || !openRegularFile(right, rightPath, rightOpenedState, rightExpected))
        return FileCompareResult::Error;
    const auto unchanged = [&] {
        return regularFileUnchanged(left, leftPath, leftOpenedState, leftExpected)
            && regularFileUnchanged(right, rightPath, rightOpenedState, rightExpected);
    };
    if (left.size() != right.size())
        return unchanged() ? FileCompareResult::Different : FileCompareResult::Error;
    while (!left.atEnd() && !isCancelled(cancelled)) {
        const QByteArray leftData = left.read(1024 * 1024);
        const QByteArray rightData = right.read(leftData.size());
        if (left.error() != QFileDevice::NoError || right.error() != QFileDevice::NoError
            || (leftData.isEmpty() && !left.atEnd())
            || (rightData.isEmpty() && !right.atEnd()))
            return FileCompareResult::Error;
        if (leftData != rightData)
            return unchanged() ? FileCompareResult::Different : FileCompareResult::Error;
    }
    if (isCancelled(cancelled) || !unchanged())
        return FileCompareResult::Error;
    return right.atEnd() ? FileCompareResult::Equal : FileCompareResult::Different;
}
}

FileContentResult fileContentResult(const QString &path, const SearchOptions &options,
                                    const std::atomic_bool *cancelled)
{
    return fileContainsImpl(path, options, cancelled, nullptr);
}

FileContentResult fileContentResult(const FileRecord &record, const SearchOptions &options,
                                    const std::atomic_bool *cancelled)
{
    return fileContainsImpl(record.path, options, cancelled, &record);
}

bool fileContains(const QString &path, const SearchOptions &options,
                  const std::atomic_bool *cancelled)
{
    return fileContentResult(path, options, cancelled) == FileContentResult::Match;
}

bool fileContains(const FileRecord &record, const SearchOptions &options,
                  const std::atomic_bool *cancelled)
{
    return fileContentResult(record, options, cancelled) == FileContentResult::Match;
}

bool filesEqual(const QString &leftPath, const QString &rightPath,
                const std::atomic_bool *cancelled)
{
    return filesEqualImpl(leftPath, rightPath, cancelled, nullptr, nullptr)
        == FileCompareResult::Equal;
}

bool filesEqual(const FileRecord &left, const FileRecord &right,
                const std::atomic_bool *cancelled)
{
    return compareFiles(left, right, cancelled) == FileCompareResult::Equal;
}

FileCompareResult compareFiles(const FileRecord &left, const FileRecord &right,
                               const std::atomic_bool *cancelled)
{
    return filesEqualImpl(left.path, right.path, cancelled, &left, &right);
}

QByteArray sha256(const QString &path, const std::atomic_bool *cancelled)
{
    if (isCancelled(cancelled))
        return {};
    QFile file;
    struct stat openedState {};
    if (!openRegularFile(file, path, openedState))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd() && !isCancelled(cancelled)) {
        const QByteArray block = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError
            || (block.isEmpty() && !file.atEnd()))
            return {};
        hash.addData(block);
    }
    return !isCancelled(cancelled) && file.atEnd()
            && file.error() == QFileDevice::NoError
            && regularFileUnchanged(file, path, openedState)
        ? hash.result() : QByteArray{};
}

bool fileSignatureMatches(const FileRecord &record)
{
    return statMatches(record);
}

bool removePathWithoutFollowingSymlink(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink())
        return QFile::remove(path);
    if (info.isDir())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}

QByteArray sha256(const FileRecord &record, const std::atomic_bool *cancelled)
{
    if (isCancelled(cancelled))
        return {};
    QFile file;
    struct stat openedState {};
    if (!openRegularFile(file, record.path, openedState, &record))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd() && !isCancelled(cancelled)) {
        const QByteArray block = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError
            || (block.isEmpty() && !file.atEnd()))
            return {};
        hash.addData(block);
    }
    return !isCancelled(cancelled) && file.atEnd()
            && file.error() == QFileDevice::NoError
            && regularFileUnchanged(file, record.path, openedState, &record)
        ? hash.result() : QByteArray{};
}

QByteArray sampleSha256(const FileRecord &record, const std::atomic_bool *cancelled)
{
    if (isCancelled(cancelled) || record.size <= duplicateSampleThreshold
        || !statMatches(record))
        return {};

    QFile file;
    struct stat openedState {};
    if (!openRegularFile(file, record.path, openedState, &record))
        return {};

    const qint64 middleOffset = (record.size - duplicateSampleBlockSize) / 2;
    const std::array<qint64, 3> offsets{
        0, middleOffset, record.size - duplicateSampleBlockSize
    };
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const qint64 offset : offsets) {
        if (isCancelled(cancelled) || !file.seek(offset))
            return {};
        const QByteArray block = file.read(duplicateSampleBlockSize);
        if (file.error() != QFileDevice::NoError
            || block.size() != duplicateSampleBlockSize)
            return {};
        hash.addData(block);
    }
    const QByteArray digest = hash.result();
    return !isCancelled(cancelled)
            && regularFileUnchanged(file, record.path, openedState, &record)
        ? digest : QByteArray{};
}

bool parseOptionalIsoDate(const QString &text, qint64 &seconds)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        seconds = 0;
        return true;
    }
    const QDateTime parsed = QDateTime::fromString(trimmed, Qt::ISODate);
    if (!parsed.isValid())
        return false;
    seconds = parsed.toSecsSinceEpoch();
    return true;
}
