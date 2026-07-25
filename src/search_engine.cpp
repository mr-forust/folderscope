#include "search_engine.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QtConcurrent>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

void SearchEngine::search(const SearchOptions &o)
{
    cancelled_.store(false, std::memory_order_relaxed);
    const auto finishIfCancelled = [this] {
        if (!cancelled_.load(std::memory_order_relaxed))
            return false;
        emit finished({}, tr("Search stopped"));
        return true;
    };
    cancelledByLimit_ = false;
    cache_.setEnabled(o.useCache);
    cacheHits_.store(0, std::memory_order_relaxed);
    cacheMisses_.store(0, std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    QVector<FileRecord> candidates;
    const Qt::CaseSensitivity cs = o.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const QStringList fileMasks = patterns(o.fileWildcards).isEmpty()
        ? QStringList{QStringLiteral("*")} : patterns(o.fileWildcards);
    const QStringList subfolderMasks = patterns(o.subfolderWildcards).isEmpty()
        ? QStringList{QStringLiteral("*")} : patterns(o.subfolderWildcards);
    const QStringList excludedFiles = exclusionPatterns(o.excludeFiles);
    const QStringList excludedFolders = patterns(o.excludeFolders);
    const QStringList includedFolders = patterns(o.includeFolders);
    quint64 scanned = 0;
    quint64 totalEntries = 0;

    if (o.accurateProgress) {
        emit phaseProgress(tr("Counting files"), 0, 0);
        for (const QString &rootText : patterns(o.roots)) {
            if (cancelled_.load(std::memory_order_relaxed))
                break;
            std::error_code ec;
            const fs::path root = fs::path(rootText.toStdString());
            fs::directory_options dirOptions = fs::directory_options::skip_permission_denied;
            if (o.followLinks)
                dirOptions |= fs::directory_options::follow_directory_symlink;
            if (!o.recursive) {
                for (fs::directory_iterator it(root, dirOptions, ec), end;
                     it != end && !ec && !cancelled_.load(std::memory_order_relaxed);
                     it.increment(ec)) {
                    ++totalEntries;
                    if (totalEntries % 2048 == 0)
                        emit phaseProgress(tr("Counting files"), totalEntries, 0);
                }
                continue;
            }
            for (fs::recursive_directory_iterator it(root, dirOptions, ec), end;
                 it != end && !ec && !cancelled_.load(std::memory_order_relaxed); it.increment(ec)) {
                if (it->is_directory(ec)) {
                    const QString name = QString::fromStdString(it->path().filename().string());
                    const QString fullPath = QString::fromStdString(it->path().string());
                    if ((!excludedFolders.isEmpty()
                         && (wildcardMatch(name, excludedFolders, cs)
                             || wildcardMatch(fullPath, excludedFolders, cs)))
                        || (o.maxDepth > 0 && it.depth() >= o.maxDepth - 1)) {
                        it.disable_recursion_pending();
                    }
                }
                ++totalEntries;
                if (totalEntries % 2048 == 0)
                    emit phaseProgress(tr("Counting files"), totalEntries, 0);
            }
        }
        emit phaseProgress(tr("Scanning files"), 0, totalEntries);
    }

    for (const QString &rootText : patterns(o.roots)) {
        if (cancelled_.load(std::memory_order_relaxed))
            break;
        std::error_code ec;
        fs::path root = fs::path(rootText.toStdString());
        fs::directory_options dirOptions = fs::directory_options::skip_permission_denied;
        if (o.followLinks)
            dirOptions |= fs::directory_options::follow_directory_symlink;

        auto process = [&](const fs::directory_entry &entry, const fs::path &baseRoot) {
            if (cancelled_.load(std::memory_order_relaxed))
                return;
            const QString path = QString::fromStdString(entry.path().string());
            const QString name = QString::fromStdString(entry.path().filename().string());
            const bool isDir = entry.is_directory(ec);
            if ((isDir && !o.findFolders) || (!isDir && !o.findFiles))
                return;
            const fs::path parentPath = entry.path().parent_path();
            const bool isBaseFolder = parentPath == baseRoot;
            const QString parentName = QString::fromStdString(parentPath.filename().string());
            if (!isBaseFolder && !wildcardMatch(parentName, subfolderMasks, cs))
                return;
            if (!wildcardMatch(name, fileMasks, cs))
                return;
            if (!excludedFiles.isEmpty() && wildcardMatch(name, excludedFiles, cs))
                return;
            if (!includedFolders.isEmpty()) {
                const QString folderPath = isDir ? path : QFileInfo(path).absolutePath();
                const QString folderName = isDir ? name : QFileInfo(path).dir().dirName();
                if (!wildcardMatch(folderPath, includedFolders, cs)
                    && !wildcardMatch(folderName, includedFolders, cs))
                    return;
            }

            struct stat st {};
            const QByteArray nativePath = QFile::encodeName(path);
            if (::stat(nativePath.constData(), &st) != 0)
                return;
            const qint64 size = isDir ? 0 : st.st_size;
            if (o.minSize > 0 && size < o.minSize)
                return;
            if (o.maxSize > 0 && size > o.maxSize)
                return;
            const qint64 selectedTime = o.timeField == QStringLiteral("Accessed time")
                ? st.st_atime
                : o.timeField == QStringLiteral("Metadata changed time") ? st.st_ctime : st.st_mtime;
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            if (o.lastMinutes > 0 && selectedTime < now - qint64(o.lastMinutes) * 60)
                return;
            if (o.minTime > 0 && selectedTime < o.minTime)
                return;
            if (o.maxTime > 0 && selectedTime > o.maxTime)
                return;
            const bool hidden = name.startsWith(u'.');
            const bool readOnly = !(st.st_mode & S_IWUSR);
            const bool executable = st.st_mode & S_IXUSR;
            const auto attrMatches = [](const QString &wanted, bool actual) {
                return wanted == QStringLiteral("Any") || (wanted == QStringLiteral("Yes")) == actual;
            };
            if (!attrMatches(o.hidden, hidden)
                || !attrMatches(o.readonly, readOnly)
                || !attrMatches(o.executable, executable))
                return;

            QString attrs;
            attrs += isDir ? u'd' : u'-';
            attrs += hidden ? u'h' : u'-';
            attrs += readOnly ? u'r' : u'-';
            attrs += executable ? u'x' : u'-';
            attrs += entry.is_symlink(ec) ? u'l' : u'-';
            candidates.push_back({
                path, name, QFileInfo(path).absolutePath(), size,
                isDir ? 0 : qint64(st.st_blocks) * 512,
                st.st_mtime,
                QFileInfo(path).birthTime().isValid() ? QFileInfo(path).birthTime().toSecsSinceEpoch() : 0,
                st.st_atime, st.st_ctime,
                isDir ? QStringLiteral("Folder") : QStringLiteral("File"),
                o.retrieveOwner ? ownerName(st.st_uid) : QString{}, attrs, 0, 0
            });
            if (o.maxResults > 0 && o.contains.isEmpty() && candidates.size() >= o.maxResults)
                cancelledByLimit_ = true;
        };

        if (!o.recursive) {
            for (fs::directory_iterator it(root, dirOptions, ec), end;
                 it != end && !ec && !cancelled_.load(std::memory_order_relaxed)
                     && !cancelledByLimit_;
                 it.increment(ec)) {
                process(*it, root);
                if (++scanned % 512 == 0) {
                    if (totalEntries)
                        emit phaseProgress(tr("Scanning files"), scanned, totalEntries);
                    else
                        emit progress(QString::fromStdString(it->path().string()), scanned);
                }
            }
            continue;
        }

        for (fs::recursive_directory_iterator it(root, dirOptions, ec), end;
             it != end && !ec && !cancelled_.load(std::memory_order_relaxed)
                 && !cancelledByLimit_; it.increment(ec)) {
            const QString name = QString::fromStdString(it->path().filename().string());
            if (it->is_directory(ec)) {
                const QString fullPath = QString::fromStdString(it->path().string());
                if (!excludedFolders.isEmpty()
                    && (wildcardMatch(name, excludedFolders, cs)
                        || wildcardMatch(fullPath, excludedFolders, cs))) {
                    it.disable_recursion_pending();
                }
                if (o.maxDepth > 0 && it.depth() >= o.maxDepth - 1)
                    it.disable_recursion_pending();
            }
            process(*it, root);
            if (++scanned % 512 == 0) {
                if (totalEntries)
                    emit phaseProgress(tr("Scanning files"), scanned, totalEntries);
                else
                    emit progress(QString::fromStdString(it->path().string()), scanned);
            }
        }
        if (cancelledByLimit_)
            break;
    }

    if (totalEntries)
        emit phaseProgress(tr("Scanning files"), std::min(scanned, totalEntries), totalEntries);

    if (finishIfCancelled())
        return;

    if (!o.contains.isEmpty() && !o.findFolders) {
        emit phaseProgress(tr("Searching file contents"), 0, candidates.size());
        std::atomic<qsizetype> checked = 0;
        const qsizetype total = candidates.size();
        QFuture<FileRecord> future = QtConcurrent::filtered(
            candidates, [this, o, &checked, total](const FileRecord &r) {
                if (cancelled_.load(std::memory_order_relaxed))
                    return false;
                const bool matched = cachedFileContains(r, o);
                const qsizetype done = checked.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done == total || done % 32 == 0)
                    emit phaseProgress(tr("Searching file contents"), done, total);
                return matched;
            });
        future.waitForFinished();
        if (finishIfCancelled())
            return;
        candidates = future.results();
        if (o.maxResults > 0 && candidates.size() > o.maxResults)
            candidates.resize(o.maxResults);
    }

    QVector<FileRecord> results;
    if (o.mode == QStringLiteral("Duplicates search")
        || o.mode == QStringLiteral("Non-Duplicates search")) {
        std::unordered_map<qint64, QVector<FileRecord>> bySize;
        for (const auto &record : candidates)
            if (record.type == QStringLiteral("File"))
                bySize[record.size].push_back(record);
        quint64 hashTotal = 0;
        for (const auto &[size, records] : bySize) {
            Q_UNUSED(size);
            if (records.size() > 1)
                hashTotal += records.size();
        }
        std::atomic<quint64> hashed = 0;
        if (hashTotal)
            emit phaseProgress(tr("Checking duplicates"), 0, hashTotal);
        int group = 1;
        QSet<QString> duplicatedPaths;
        for (auto &[size, sameSize] : bySize) {
            if (cancelled_.load(std::memory_order_relaxed))
                break;
            Q_UNUSED(size);
            if (sameSize.size() < 2)
                continue;
            auto hashes = QtConcurrent::blockingMapped(sameSize, [this, &hashed, hashTotal](const FileRecord &r) {
                auto result = std::pair<QByteArray, FileRecord>{cachedSha256(r), r};
                const quint64 done = hashed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done == hashTotal || done % 16 == 0)
                    emit phaseProgress(tr("Checking duplicates"), done, hashTotal);
                return result;
            });
            if (cancelled_.load(std::memory_order_relaxed))
                break;
            QHash<QByteArray, QVector<FileRecord>> sameHash;
            for (auto &[hash, record] : hashes)
                if (!hash.isEmpty())
                    sameHash[hash].push_back(record);
            for (const auto &hashCandidates : sameHash) {
                if (cancelled_.load(std::memory_order_relaxed))
                    break;
                if (hashCandidates.size() < 2)
                    continue;
                QVector<QVector<FileRecord>> exactGroups;
                for (const auto &record : hashCandidates) {
                    if (cancelled_.load(std::memory_order_relaxed))
                        break;
                    bool placed = false;
                    for (auto &exact : exactGroups) {
                        if (filesEqual(exact.front().path, record.path, &cancelled_)) {
                            exact.push_back(record);
                            placed = true;
                            break;
                        }
                    }
                    if (!placed)
                        exactGroups.push_back({record});
                }
                for (auto &exact : exactGroups) {
                    if (cancelled_.load(std::memory_order_relaxed))
                        break;
                    if (exact.size() < 2)
                        continue;
                    int duplicateCopy = 1;
                    for (auto &record : exact) {
                        record.group = group;
                        record.duplicateCopy = duplicateCopy++;
                        duplicatedPaths.insert(record.path);
                        if (o.mode == QStringLiteral("Duplicates search")
                            && shouldShowDuplicateResult(
                                o.showDuplicateCopiesOnly, record.duplicateCopy)) {
                            results.push_back(record);
                        }
                    }
                    ++group;
                }
            }
        }
        if (finishIfCancelled())
            return;
        if (o.mode == QStringLiteral("Non-Duplicates search")) {
            for (const auto &record : candidates)
                if (record.type == QStringLiteral("File") && !duplicatedPaths.contains(record.path))
                    results.push_back(record);
        }
    } else if (o.mode == QStringLiteral("Duplicate names search")) {
        QHash<QString, QVector<FileRecord>> byName;
        for (const auto &record : candidates) {
            const QString key = o.duplicateNameWithoutExtension
                ? QFileInfo(record.name).completeBaseName().toCaseFolded()
                : record.name.toCaseFolded();
            byName[key].push_back(record);
        }
        int group = 1;
        quint64 namesDone = 0;
        emit phaseProgress(tr("Comparing duplicate names"), 0, byName.size());
        for (auto sameName : byName) {
            if (cancelled_.load(std::memory_order_relaxed))
                break;
            ++namesDone;
            if (namesDone % 32 == 0 || namesDone == quint64(byName.size()))
                emit phaseProgress(tr("Comparing duplicate names"), namesDone, byName.size());
            if (o.duplicateNameMode != QStringLiteral("All files and folders"))
                sameName.erase(std::remove_if(sameName.begin(), sameName.end(),
                    [](const FileRecord &r) { return r.type != QStringLiteral("File"); }), sameName.end());
            if (sameName.size() < 2)
                continue;
            if (o.duplicateNameMode.contains(QStringLiteral("identical"), Qt::CaseInsensitive)) {
                const QByteArray firstHash = cachedSha256(sameName.front());
                bool identical = !firstHash.isEmpty();
                for (qsizetype i = 1; identical && i < sameName.size(); ++i) {
                    identical = cachedSha256(sameName[i]) == firstHash
                        && filesEqual(
                            sameName.front().path, sameName[i].path, &cancelled_);
                }
                const bool wantsNonIdentical = o.duplicateNameMode.startsWith(QStringLiteral("Only non"));
                if (identical == wantsNonIdentical)
                    continue;
            }
            int duplicateCopy = 1;
            for (auto &record : sameName) {
                record.group = group;
                record.duplicateCopy = duplicateCopy++;
                results.push_back(record);
            }
            ++group;
        }
        if (finishIfCancelled())
            return;
    } else if (o.mode == QStringLiteral("Summary")) {
        QHash<QString, QVector<FileRecord>> byFolder;
        for (const auto &record : candidates) {
            if (cancelled_.load(std::memory_order_relaxed))
                break;
            byFolder[record.folder].push_back(record);
            if (o.includeSubfoldersInSummary) {
                QDir parent(record.folder);
                while (parent.cdUp()) {
                    const QString ancestor = parent.absolutePath();
                    bool belongsToRoot = false;
                    for (const QString &root : patterns(o.roots)) {
                        if (ancestor == QDir(root).absolutePath()) {
                            belongsToRoot = true;
                            break;
                        }
                    }
                    byFolder[ancestor].push_back(record);
                    if (belongsToRoot)
                        break;
                }
            }
        }
        for (auto it = byFolder.cbegin(); it != byFolder.cend(); ++it) {
            if (cancelled_.load(std::memory_order_relaxed))
                break;
            qint64 total = 0;
            qint64 totalOnDisk = 0;
            qint64 newest = 0;
            for (const auto &record : it.value()) {
                total += record.size;
                totalOnDisk += record.sizeOnDisk;
                newest = std::max(newest, record.modified);
            }
            const QFileInfo info(it.key());
            results.push_back({it.key(), info.fileName(), info.absolutePath(), total, totalOnDisk,
                               newest, 0, 0, 0, tr("%1 files").arg(it.value().size()),
                               {}, {}, 0, 0});
        }
        if (finishIfCancelled())
            return;
    } else {
        results = std::move(candidates);
    }

    if (finishIfCancelled())
        return;
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    cache_.save();
    emit finished(results, tr("%1 results, %2 entries scanned in %3 seconds; cache: %4 hits")
                              .arg(results.size()).arg(scanned).arg(seconds, 0, 'f', 2)
                              .arg(cacheHits_.load(std::memory_order_relaxed)));
}

QByteArray SearchEngine::cachedSha256(const FileRecord &record)
{
    QByteArray digest;
    if (cache_.findHash(record, digest)) {
        cacheHits_.fetch_add(1, std::memory_order_relaxed);
        return digest;
    }
    cacheMisses_.fetch_add(1, std::memory_order_relaxed);
    digest = sha256(record.path, &cancelled_);
    if (!cancelled_.load(std::memory_order_relaxed))
        cache_.storeHash(record, digest);
    return digest;
}

bool SearchEngine::cachedFileContains(const FileRecord &record, const SearchOptions &o)
{
    QByteArray queryData;
    QDataStream stream(&queryData, QIODevice::WriteOnly);
    stream << o.contains << o.binary << o.multipleValues << o.multipleAnd << o.caseSensitive;
    const QByteArray queryKey = QCryptographicHash::hash(queryData, QCryptographicHash::Sha256);
    if (const auto cached = cache_.findContent(record, queryKey)) {
        cacheHits_.fetch_add(1, std::memory_order_relaxed);
        return *cached;
    }
    cacheMisses_.fetch_add(1, std::memory_order_relaxed);
    const bool matched = fileContains(record.path, o, &cancelled_);
    if (!cancelled_.load(std::memory_order_relaxed))
        cache_.storeContent(record, queryKey, matched);
    return matched;
}
