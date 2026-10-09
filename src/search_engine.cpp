#include "search_engine.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
struct HashedRecord {
    QByteArray digest;
    FileRecord record;
};

bool keepFirst(const FileRecord &left, const FileRecord &right,
               const QStringList &priorityRoots)
{
    const int leftRoot = preferredRootIndex(left.path, priorityRoots);
    const int rightRoot = preferredRootIndex(right.path, priorityRoots);
    const int leftPriority = leftRoot < 0 ? priorityRoots.size() : leftRoot;
    const int rightPriority = rightRoot < 0 ? priorityRoots.size() : rightRoot;
    if (leftPriority != rightPriority)
        return leftPriority < rightPriority;
    if (left.modified != right.modified)
        return left.modified > right.modified;
    return left.path < right.path;
}
}

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
    if (!o.contains.isEmpty() && o.binary) {
        const QStringList terms = o.multipleValues
            ? patterns(o.contains) : QStringList{o.contains};
        for (const QString &term : terms) {
            if (!hexNeedle(term)) {
                emit finished({}, tr("Invalid hexadecimal search query"));
                return;
            }
        }
    }

    QVector<FileRecord> candidates;
    QSet<QString> candidatePaths;
    const EffectiveRoots roots = effectiveSearchRoots(o);
    const Qt::CaseSensitivity cs = o.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const QStringList fileMasks = patterns(o.fileWildcards).isEmpty()
        ? QStringList{QStringLiteral("*")} : patterns(o.fileWildcards);
    const QStringList subfolderMasks = patterns(o.subfolderWildcards).isEmpty()
        ? QStringList{QStringLiteral("*")} : patterns(o.subfolderWildcards);
    const QStringList excludedFiles = patterns(o.excludeFiles);
    const QStringList excludedFolders = patterns(o.excludeFolders);
    const QStringList includedFolders = patterns(o.includeFolders);
    const CompiledWildcardMatcher fileMatcher(fileMasks, cs);
    const CompiledWildcardMatcher subfolderMatcher(subfolderMasks, cs);
    const CompiledWildcardMatcher excludedFileMatcher(excludedFiles, cs);
    const CompiledWildcardMatcher excludedFolderMatcher(excludedFolders, cs);
    const CompiledWildcardMatcher includedFolderMatcher(includedFolders, cs);
    const bool canLimitDuringScan = o.mode == QStringLiteral("Standard search")
        && o.contains.isEmpty() && o.maxResults > 0;
    bool incomplete = false;
    quint64 incompleteChecks = 0;
    const auto markIncomplete = [&] {
        incomplete = true;
        ++incompleteChecks;
    };
    quint64 scanned = 0;
    quint64 totalEntries = 0;

    if (o.accurateProgress) {
        emit phaseProgress(tr("Counting files"), 0, 0);
        for (const QString &rootText : roots.paths) {
            if (cancelled_.load(std::memory_order_relaxed))
                break;
            const fs::path root = fs::path(rootText.toStdString());
            fs::directory_options dirOptions = fs::directory_options::none;
            if (o.followLinks)
                dirOptions |= fs::directory_options::follow_directory_symlink;
            if (!o.recursive) {
                std::error_code initError;
                fs::directory_iterator it(root, dirOptions, initError), end;
                if (initError)
                    markIncomplete();
                while (it != end && !cancelled_.load(std::memory_order_relaxed)) {
                    ++totalEntries;
                    if (totalEntries % 2048 == 0)
                        emit phaseProgress(tr("Counting files"), totalEntries, 0);
                    std::error_code incrementError;
                    it.increment(incrementError);
                    if (incrementError) {
                        markIncomplete();
                        it = end;
                    }
                }
                continue;
            }
            std::error_code initError;
            fs::recursive_directory_iterator it(root, dirOptions, initError), end;
            if (initError)
                markIncomplete();
            while (it != end && !cancelled_.load(std::memory_order_relaxed)) {
                std::error_code entryError;
                const bool isDirectory = it->is_directory(entryError);
                if (entryError)
                    markIncomplete();
                if (isDirectory && !entryError) {
                    const QString name = QString::fromStdString(it->path().filename().string());
                    const QString fullPath = QString::fromStdString(it->path().string());
                    if (excludedFolderMatcher.matches(name)
                        || excludedFolderMatcher.matches(fullPath)
                        || (o.maxDepth > 0 && it.depth() >= o.maxDepth - 1)) {
                        it.disable_recursion_pending();
                    }
                }
                ++totalEntries;
                if (totalEntries % 2048 == 0)
                    emit phaseProgress(tr("Counting files"), totalEntries, 0);
                std::error_code incrementError;
                it.increment(incrementError);
                if (incrementError) {
                    markIncomplete();
                    it = end;
                }
            }
        }
        emit phaseProgress(tr("Scanning files"), 0, totalEntries);
    }

    for (const QString &rootText : roots.paths) {
        if (cancelled_.load(std::memory_order_relaxed))
            break;
        fs::path root = fs::path(rootText.toStdString());
        fs::directory_options dirOptions = fs::directory_options::none;
        if (o.followLinks)
            dirOptions |= fs::directory_options::follow_directory_symlink;

        auto process = [&](const fs::directory_entry &entry, const fs::path &baseRoot) {
            if (cancelled_.load(std::memory_order_relaxed))
                return;
            const QString path = QDir::cleanPath(QFileInfo(
                QString::fromStdString(entry.path().string())).absoluteFilePath());
            const QString name = QString::fromStdString(entry.path().filename().string());
            std::error_code entryError;
            const bool isDir = entry.is_directory(entryError);
            if (entryError) {
                markIncomplete();
                return;
            }
            if (isDir && (excludedFolderMatcher.matches(name)
                          || excludedFolderMatcher.matches(path)))
                return;
            if ((isDir && !o.findFolders) || (!isDir && !o.findFiles))
                return;
            const fs::path parentPath = entry.path().parent_path();
            const bool isBaseFolder = parentPath == baseRoot;
            const QString parentName = QString::fromStdString(parentPath.filename().string());
            if (!isBaseFolder && !subfolderMatcher.matches(parentName))
                return;
            if (!fileMatcher.matches(name))
                return;
            if (excludedFileMatcher.matches(name))
                return;
            if (!includedFolders.isEmpty()) {
                const QString folderPath = isDir ? path : QFileInfo(path).absolutePath();
                const QString folderName = isDir ? name : QFileInfo(path).dir().dirName();
                if (!includedFolderMatcher.matches(folderPath)
                    && !includedFolderMatcher.matches(folderName))
                    return;
            }

            struct stat st {};
            const QByteArray nativePath = QFile::encodeName(path);
            if (::stat(nativePath.constData(), &st) != 0) {
                markIncomplete();
                return;
            }
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
            if (candidatePaths.contains(path))
                return;
            candidatePaths.insert(path);

            QString attrs;
            attrs += isDir ? u'd' : u'-';
            attrs += hidden ? u'h' : u'-';
            attrs += readOnly ? u'r' : u'-';
            attrs += executable ? u'x' : u'-';
            const bool isSymlink = entry.is_symlink(entryError);
            if (entryError)
                markIncomplete();
            attrs += isSymlink ? u'l' : u'-';
            candidates.push_back({
                path, name, QFileInfo(path).absolutePath(), size,
                isDir ? 0 : qint64(st.st_blocks) * 512,
                st.st_mtime,
                QFileInfo(path).birthTime().isValid() ? QFileInfo(path).birthTime().toSecsSinceEpoch() : 0,
                st.st_atime, st.st_ctime,
                qint64(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec,
                qint64(st.st_ctim.tv_sec) * 1'000'000'000LL + st.st_ctim.tv_nsec,
                quint64(st.st_dev), quint64(st.st_ino),
                isDir ? QStringLiteral("Folder") : QStringLiteral("File"),
                o.retrieveOwner ? ownerName(st.st_uid) : QString{}, attrs, 0, 0
            });
            if (canLimitDuringScan && candidates.size() >= o.maxResults)
                cancelledByLimit_ = true;
        };

        if (!o.recursive) {
            std::error_code initError;
            fs::directory_iterator it(root, dirOptions, initError), end;
            if (initError)
                markIncomplete();
            while (it != end && !cancelled_.load(std::memory_order_relaxed)
                   && !cancelledByLimit_) {
                const QString currentPath = QString::fromStdString(it->path().string());
                process(*it, root);
                if (++scanned % 512 == 0) {
                    if (totalEntries)
                        emit phaseProgress(tr("Scanning files"), scanned, totalEntries);
                    else
                        emit progress(currentPath, scanned);
                }
                if (cancelledByLimit_)
                    break;
                std::error_code incrementError;
                it.increment(incrementError);
                if (incrementError) {
                    markIncomplete();
                    it = end;
                }
            }
            if (cancelledByLimit_)
                break;
            continue;
        }

        std::error_code initError;
        fs::recursive_directory_iterator it(root, dirOptions, initError), end;
        if (initError)
            markIncomplete();
        while (it != end && !cancelled_.load(std::memory_order_relaxed)
               && !cancelledByLimit_) {
            const QString currentPath = QString::fromStdString(it->path().string());
            std::error_code entryError;
            const bool isDirectory = it->is_directory(entryError);
            if (entryError)
                markIncomplete();
            if (isDirectory && !entryError) {
                const QString name = QString::fromStdString(it->path().filename().string());
                const QString fullPath = QString::fromStdString(it->path().string());
                if (excludedFolderMatcher.matches(name)
                    || excludedFolderMatcher.matches(fullPath)) {
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
                    emit progress(currentPath, scanned);
            }
            if (cancelledByLimit_)
                break;
            std::error_code incrementError;
            it.increment(incrementError);
            if (incrementError) {
                markIncomplete();
                it = end;
            }
        }
        if (cancelledByLimit_)
            break;
    }

    if (totalEntries)
        emit phaseProgress(tr("Scanning files"), std::min(scanned, totalEntries), totalEntries);

    if (finishIfCancelled())
        return;

    if (!o.contains.isEmpty()) {
        emit phaseProgress(tr("Searching file contents"), 0, candidates.size());
        std::atomic<qsizetype> checked = 0;
        std::atomic<quint64> contentCheckFailures = 0;
        const qsizetype total = candidates.size();
        QFuture<FileRecord> future = QtConcurrent::filtered(
            candidates, [this, o, &checked, &contentCheckFailures, total](const FileRecord &r) {
                if (cancelled_.load(std::memory_order_relaxed))
                    return false;
                if (r.type == QStringLiteral("Folder"))
                    return true;
                const FileContentResult contentResult = cachedFileContent(r, o);
                if (contentResult == FileContentResult::Error)
                    contentCheckFailures.fetch_add(1, std::memory_order_relaxed);
                const qsizetype done = checked.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done == total || done % 32 == 0)
                    emit phaseProgress(tr("Searching file contents"), done, total);
                return contentResult == FileContentResult::Match;
            });
        future.waitForFinished();
        if (finishIfCancelled())
            return;
        const quint64 failedContentChecks = contentCheckFailures.load(std::memory_order_relaxed);
        incompleteChecks += failedContentChecks;
        incomplete = incomplete || failedContentChecks > 0;
        candidates = future.results();
    }

    QThreadPool hashPool;
    const int idealThreads = QThread::idealThreadCount();
    hashPool.setMaxThreadCount(std::clamp(
        idealThreads > 0 ? idealThreads / 2 : 1, 1, 4));

    QVector<FileRecord> results;
    QSet<QString> unknownPaths;
    if (o.mode == QStringLiteral("Duplicates search")
        || o.mode == QStringLiteral("Non-Duplicates search")) {
        std::unordered_map<qint64, QVector<FileRecord>> bySize;
        QVector<FileRecord> duplicateInputs;
        QHash<QPair<quint64, quint64>, qsizetype> physicalFiles;
        for (const auto &record : candidates) {
            if (record.type != QStringLiteral("File"))
                continue;
            const auto identity = qMakePair(record.device, record.inode);
            const auto existing = physicalFiles.constFind(identity);
            if (existing == physicalFiles.cend()) {
                physicalFiles.insert(identity, duplicateInputs.size());
                duplicateInputs.push_back(record);
            } else {
                FileRecord &representative = duplicateInputs[*existing];
                const bool existingLink = representative.attributes.endsWith(u'l');
                const bool candidateLink = record.attributes.endsWith(u'l');
                // A link to the keeper is not an independent data copy. Prefer
                // its real path when selecting a representative.
                if ((existingLink && !candidateLink)
                    || (existingLink == candidateLink
                        && keepFirst(record, representative, roots.priorityPaths)))
                    representative = record;
            }
        }
        for (const FileRecord &record : duplicateInputs)
            bySize[record.size].push_back(record);

        QVector<FileRecord> sampleCandidates;
        QVector<FileRecord> fullHashCandidates;
        for (const auto &[size, records] : bySize) {
            if (records.size() < 2)
                continue;
            if (size > duplicateSampleThreshold)
                sampleCandidates += records;
            else
                fullHashCandidates += records;
        }

        std::atomic<qsizetype> sampled = 0;
        const qsizetype sampleTotal = sampleCandidates.size();
        if (sampleTotal)
            emit phaseProgress(tr("Sampling duplicate candidates"), 0, sampleTotal);
        const auto samples = QtConcurrent::blockingMapped(
            &hashPool, sampleCandidates,
            [this, &sampled, sampleTotal](const FileRecord &record) {
                HashedRecord result{cachedSampleSha256(record), record};
                const qsizetype done = sampled.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done == sampleTotal || done % 16 == 0)
                    emit phaseProgress(tr("Sampling duplicate candidates"), done, sampleTotal);
                return result;
            });
        if (finishIfCancelled())
            return;

        QHash<qint64, QHash<QByteArray, QVector<FileRecord>>> bySample;
        for (const auto &sample : samples) {
            if (!sample.digest.isEmpty()) {
                bySample[sample.record.size][sample.digest].push_back(sample.record);
            } else if (!cancelled_.load(std::memory_order_relaxed)) {
                unknownPaths.insert(sample.record.path);
                markIncomplete();
            }
        }
        for (const auto &sameSize : bySample)
            for (const auto &sameSample : sameSize)
                if (sameSample.size() > 1)
                    fullHashCandidates += sameSample;

        std::atomic<qsizetype> hashed = 0;
        const qsizetype hashTotal = fullHashCandidates.size();
        if (hashTotal)
            emit phaseProgress(tr("Hashing duplicate candidates"), 0, hashTotal);
        const auto hashes = QtConcurrent::blockingMapped(
            &hashPool, fullHashCandidates,
            [this, &hashed, hashTotal](const FileRecord &record) {
                HashedRecord result{cachedSha256(record), record};
                const qsizetype done = hashed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done == hashTotal || done % 16 == 0)
                    emit phaseProgress(tr("Hashing duplicate candidates"), done, hashTotal);
                return result;
            });
        if (finishIfCancelled())
            return;

        QHash<qint64, QHash<QByteArray, QVector<FileRecord>>> byHash;
        for (const auto &hash : hashes) {
            if (!hash.digest.isEmpty()) {
                byHash[hash.record.size][hash.digest].push_back(hash.record);
            } else if (!cancelled_.load(std::memory_order_relaxed)) {
                unknownPaths.insert(hash.record.path);
                markIncomplete();
            }
        }

        int group = 1;
        QSet<QString> duplicatedPaths;
        for (const auto &sameSize : byHash) {
            for (const auto &hashCandidates : sameSize) {
                if (cancelled_.load(std::memory_order_relaxed))
                    break;
                if (hashCandidates.size() < 2)
                    continue;
                QVector<QVector<FileRecord>> exactGroups;
                if (!o.strictDuplicateComparison) {
                    exactGroups.push_back(hashCandidates);
                } else {
                    for (const auto &record : hashCandidates) {
                        if (cancelled_.load(std::memory_order_relaxed))
                            break;
                        bool placed = false;
                        for (auto &exact : exactGroups) {
                            const FileCompareResult comparison =
                                compareFiles(exact.front(), record, &cancelled_);
                            if (comparison == FileCompareResult::Equal) {
                                exact.push_back(record);
                                placed = true;
                                break;
                            }
                            if (comparison == FileCompareResult::Error) {
                                unknownPaths.insert(exact.front().path);
                                unknownPaths.insert(record.path);
                                markIncomplete();
                            }
                        }
                        if (!placed)
                            exactGroups.push_back({record});
                    }
                }
                for (auto &exact : exactGroups) {
                    if (cancelled_.load(std::memory_order_relaxed))
                        break;
                    exact.erase(std::remove_if(exact.begin(), exact.end(),
                        [&](const FileRecord &record) {
                            if (fileSignatureMatches(record))
                                return false;
                            unknownPaths.insert(record.path);
                            markIncomplete();
                            return true;
                        }), exact.end());
                    if (o.strictDuplicateComparison
                        && std::any_of(exact.cbegin(), exact.cend(), [&unknownPaths](const FileRecord &record) {
                               return unknownPaths.contains(record.path);
                           }))
                        continue;
                    if (exact.size() < 2)
                        continue;
                    std::stable_sort(exact.begin(), exact.end(),
                                     [&roots](const FileRecord &left,
                                              const FileRecord &right) {
                        return keepFirst(left, right, roots.priorityPaths);
                    });
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
            for (const auto &record : duplicateInputs)
                if (record.type == QStringLiteral("File")
                    && !duplicatedPaths.contains(record.path)
                    && !unknownPaths.contains(record.path))
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
            std::stable_sort(sameName.begin(), sameName.end(),
                             [&roots](const FileRecord &left,
                                      const FileRecord &right) {
                return keepFirst(left, right, roots.priorityPaths);
            });
            if (o.duplicateNameMode.contains(QStringLiteral("identical"), Qt::CaseInsensitive)) {
                bool identical = std::all_of(
                    sameName.cbegin(), sameName.cend(),
                    [&sameName](const FileRecord &record) {
                        return record.size == sameName.front().size;
                    });
                bool comparisonUnknown = false;
                for (const FileRecord &record : sameName) {
                    if (!fileSignatureMatches(record)) {
                        comparisonUnknown = true;
                        markIncomplete();
                    }
                }
                if (identical && !comparisonUnknown && sameName.front().size > duplicateSampleThreshold) {
                    const auto samples = QtConcurrent::blockingMapped(
                        &hashPool, sameName, [this](const FileRecord &record) {
                            return cachedSampleSha256(record);
                        });
                    if (std::any_of(samples.cbegin(), samples.cend(),
                                    [](const QByteArray &digest) { return digest.isEmpty(); })) {
                        comparisonUnknown = true;
                        markIncomplete();
                    } else {
                        identical = !samples.isEmpty()
                            && std::all_of(samples.cbegin(), samples.cend(),
                                [&samples](const QByteArray &digest) {
                                    return digest == samples.front();
                                });
                    }
                }
                QVector<QByteArray> hashes;
                if (identical && !comparisonUnknown) {
                    hashes = QtConcurrent::blockingMapped(
                        &hashPool, sameName, [this](const FileRecord &record) {
                            return cachedSha256(record);
                        });
                    if (std::any_of(hashes.cbegin(), hashes.cend(),
                                    [](const QByteArray &digest) { return digest.isEmpty(); })) {
                        comparisonUnknown = true;
                        markIncomplete();
                    } else {
                        identical = !hashes.isEmpty()
                            && std::all_of(hashes.cbegin(), hashes.cend(),
                                [&hashes](const QByteArray &digest) {
                                    return digest == hashes.front();
                                });
                    }
                }
                if (identical && !comparisonUnknown && o.strictDuplicateComparison) {
                    const FileRecord &first = sameName.front();
                    for (qsizetype index = 1; index < sameName.size(); ++index) {
                        const FileRecord &record = sameName[index];
                        const FileCompareResult comparison =
                            compareFiles(first, record, &cancelled_);
                        if (comparison == FileCompareResult::Error) {
                            comparisonUnknown = true;
                            markIncomplete();
                            break;
                        }
                        if (comparison == FileCompareResult::Different) {
                            identical = false;
                            break;
                        }
                    }
                }
                for (const FileRecord &record : sameName) {
                    if (!fileSignatureMatches(record)) {
                        comparisonUnknown = true;
                        markIncomplete();
                    }
                }
                if (comparisonUnknown)
                    continue;
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
                    for (const QString &root : roots.paths) {
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
                               newest, 0, 0, 0, 0, 0, 0, 0,
                               tr("%1 files").arg(it.value().size()), {}, {}, 0, 0});
        }
        if (finishIfCancelled())
            return;
    } else {
        results = std::move(candidates);
    }

    if (o.mode == QStringLiteral("Non-Duplicates search")) {
        results.erase(std::remove_if(results.begin(), results.end(),
            [&](const FileRecord &record) {
                if (fileSignatureMatches(record))
                    return false;
                markIncomplete();
                return true;
            }), results.end());
    }

    if (o.maxResults > 0 && results.size() > o.maxResults)
        results.resize(o.maxResults);

    if (finishIfCancelled())
        return;
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    cache_.save();
    QString message = tr("%1 results, %2 entries scanned in %3 seconds; cache: %4 hits")
                          .arg(results.size()).arg(scanned).arg(seconds, 0, 'f', 2)
                          .arg(cacheHits_.load(std::memory_order_relaxed));
    if (roots.skipped > 0)
        message += tr("; %1 redundant roots skipped").arg(roots.skipped);
    if (incomplete)
        message += tr("; search incomplete; %1 path/file checks failed")
                       .arg(incompleteChecks);
    emit finished(results, message);
}

QByteArray SearchEngine::cachedSampleSha256(const FileRecord &record)
{
    QByteArray digest;
    if (cache_.findSampleHash(record, digest)) {
        if (!fileSignatureMatches(record))
            return {};
        cacheHits_.fetch_add(1, std::memory_order_relaxed);
        return digest;
    }
    cacheMisses_.fetch_add(1, std::memory_order_relaxed);
    digest = sampleSha256(record, &cancelled_);
    if (!digest.isEmpty() && !cancelled_.load(std::memory_order_relaxed))
        cache_.storeSampleHash(record, digest);
    return digest;
}

QByteArray SearchEngine::cachedSha256(const FileRecord &record)
{
    QByteArray digest;
    if (cache_.findHash(record, digest)) {
        if (!fileSignatureMatches(record))
            return {};
        cacheHits_.fetch_add(1, std::memory_order_relaxed);
        return digest;
    }
    cacheMisses_.fetch_add(1, std::memory_order_relaxed);
    digest = sha256(record, &cancelled_);
    if (!digest.isEmpty() && !cancelled_.load(std::memory_order_relaxed))
        cache_.storeHash(record, digest);
    return digest;
}

FileContentResult SearchEngine::cachedFileContent(const FileRecord &record,
                                                 const SearchOptions &o)
{
    QByteArray queryData;
    QDataStream stream(&queryData, QIODevice::WriteOnly);
    stream << o.contains << o.binary << o.multipleValues << o.multipleAnd << o.caseSensitive;
    const QByteArray queryKey = QCryptographicHash::hash(queryData, QCryptographicHash::Sha256);
    if (const auto cached = cache_.findContent(record, queryKey)) {
        if (!fileSignatureMatches(record))
            return FileContentResult::Error;
        cacheHits_.fetch_add(1, std::memory_order_relaxed);
        return *cached ? FileContentResult::Match : FileContentResult::NoMatch;
    }
    cacheMisses_.fetch_add(1, std::memory_order_relaxed);
    const FileContentResult result = fileContentResult(record, o, &cancelled_);
    if (cancelled_.load(std::memory_order_relaxed)
        || result == FileContentResult::Error || !fileSignatureMatches(record))
        return FileContentResult::Error;
    cache_.storeContent(record, queryKey, result == FileContentResult::Match);
    return result;
}
