#include "search_core.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>
#include <pwd.h>
#include <sys/stat.h>

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

bool wildcardMatch(const QString &value, const QStringList &items, Qt::CaseSensitivity cs)
{
    for (const QString &pattern : items) {
        const auto regex = QRegularExpression(
            QRegularExpression::wildcardToRegularExpression(pattern),
            cs == Qt::CaseInsensitive ? QRegularExpression::CaseInsensitiveOption
                                      : QRegularExpression::NoPatternOption);
        if (regex.match(value).hasMatch())
            return true;
    }
    return false;
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
    if (text.size() % 2 != 0 || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]*$")).match(text).hasMatch())
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

bool signatureMatches(const FileRecord &record, const struct stat &st)
{
    return record.size == st.st_size
        && record.modifiedNs == modifiedNanoseconds(st)
        && record.device == quint64(st.st_dev)
        && record.inode == quint64(st.st_ino);
}

bool statMatches(const FileRecord &record)
{
    struct stat st {};
    const QByteArray nativePath = QFile::encodeName(record.path);
    return ::stat(nativePath.constData(), &st) == 0 && signatureMatches(record, st);
}
}

bool fileContains(const QString &path, const SearchOptions &options,
                  const std::atomic_bool *cancelled)
{
    if (isCancelled(cancelled))
        return false;
    if (options.contains.isEmpty())
        return true;

    const QStringList terms = options.multipleValues
        ? patterns(options.contains) : QStringList{options.contains};
    QList<QByteArray> needles;
    for (const QString &term : terms) {
        if (options.binary) {
            const auto bytes = hexNeedle(term);
            if (!bytes)
                return false;
            needles.push_back(*bytes);
        } else {
            needles.push_back(term.toUtf8());
        }
    }
    if (needles.isEmpty())
        return true;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QVector<bool> found(needles.size(), false);
    qsizetype overlap = 1;
    for (const QByteArray &needle : needles)
        overlap = std::max(overlap, needle.size());
    QByteArray tail;
    while (!file.atEnd() && !isCancelled(cancelled)) {
        QByteArray data = tail + file.read(1024 * 1024);
        if (!options.caseSensitive && !options.binary)
            data = data.toLower();
        for (qsizetype i = 0; i < needles.size(); ++i) {
            QByteArray needle = needles[i];
            if (!options.caseSensitive && !options.binary)
                needle = needle.toLower();
            if (!found[i] && data.contains(needle))
                found[i] = true;
        }
        const bool matched = options.multipleAnd
            ? std::all_of(found.cbegin(), found.cend(), [](bool foundTerm) { return foundTerm; })
            : std::any_of(found.cbegin(), found.cend(), [](bool foundTerm) { return foundTerm; });
        if (matched)
            return true;
        tail = data.right(overlap - 1);
    }
    return false;
}

bool filesEqual(const QString &leftPath, const QString &rightPath,
                const std::atomic_bool *cancelled)
{
    if (isCancelled(cancelled))
        return false;
    QFile left(leftPath);
    QFile right(rightPath);
    if (!left.open(QIODevice::ReadOnly) || !right.open(QIODevice::ReadOnly)
        || left.size() != right.size())
        return false;
    while (!left.atEnd() && !isCancelled(cancelled)) {
        const QByteArray leftData = left.read(1024 * 1024);
        const QByteArray rightData = right.read(leftData.size());
        if (leftData != rightData)
            return false;
    }
    return !isCancelled(cancelled) && right.atEnd();
}

QByteArray sha256(const QString &path, const std::atomic_bool *cancelled)
{
    if (isCancelled(cancelled))
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd() && !isCancelled(cancelled))
        hash.addData(file.read(1024 * 1024));
    return isCancelled(cancelled) ? QByteArray{} : hash.result();
}

bool fileSignatureMatches(const FileRecord &record)
{
    return statMatches(record);
}

QByteArray sha256(const FileRecord &record, const std::atomic_bool *cancelled)
{
    if (isCancelled(cancelled) || !statMatches(record))
        return {};
    const QByteArray digest = sha256(record.path, cancelled);
    return !digest.isEmpty() && statMatches(record) ? digest : QByteArray{};
}

QByteArray sampleSha256(const FileRecord &record, const std::atomic_bool *cancelled)
{
    if (isCancelled(cancelled) || record.size <= duplicateSampleThreshold
        || !statMatches(record))
        return {};

    QFile file(record.path);
    if (!file.open(QIODevice::ReadOnly))
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
        if (block.size() != duplicateSampleBlockSize)
            return {};
        hash.addData(block);
    }
    const QByteArray digest = hash.result();
    return !isCancelled(cancelled) && statMatches(record) ? digest : QByteArray{};
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
