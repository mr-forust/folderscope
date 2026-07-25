#include <QAbstractTableModel>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDataStream>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFuture>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMutex>
#include <QMutexLocker>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableView>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <pwd.h>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

struct SearchOptions {
    QString roots = QDir::homePath();
    QString fileWildcards = QStringLiteral("*");
    QString subfolderWildcards = QStringLiteral("*");
    QString excludeFiles;
    QString excludeFolders;
    QString includeFolders;
    QString contains;
    bool binary = false;
    bool multipleValues = false;
    bool multipleAnd = false;
    bool caseSensitive = false;
    qint64 minSize = 0;
    qint64 maxSize = 0;
    qint64 minTime = 0;
    qint64 maxTime = 0;
    QString timeField = QStringLiteral("Modified time");
    int lastMinutes = 0;
    bool recursive = true;
    bool findFiles = true;
    bool findFolders = false;
    bool followLinks = false;
    bool retrieveOwner = false;
    bool accurateProgress = true;
    bool useCache = true;
    int maxDepth = 0;
    int maxResults = 0;
    QString mode = QStringLiteral("Standard search");
    QString duplicateNameMode = QStringLiteral("All files and folders");
    bool duplicateNameWithoutExtension = false;
    bool showOnlyDuplicateFiles = true;
    bool includeSubfoldersInSummary = false;
    QString hidden = QStringLiteral("Any");
    QString readonly = QStringLiteral("Any");
    QString executable = QStringLiteral("Any");
};

struct FileRecord {
    QString path;
    QString name;
    QString folder;
    qint64 size = 0;
    qint64 sizeOnDisk = 0;
    qint64 modified = 0;
    qint64 created = 0;
    qint64 accessed = 0;
    qint64 changed = 0;
    QString type;
    QString owner;
    QString attributes;
    int group = 0;
    int duplicateCopy = 0;
};

Q_DECLARE_METATYPE(SearchOptions)
Q_DECLARE_METATYPE(FileRecord)
Q_DECLARE_METATYPE(QVector<FileRecord>)

static QStringList patterns(const QString &text)
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

static QStringList exclusionPatterns(const QString &text)
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

static bool wildcardMatch(const QString &value, const QStringList &items, Qt::CaseSensitivity cs)
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

static QString humanSize(qint64 bytes)
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

static QString ownerName(uid_t uid)
{
    struct passwd pwd {};
    struct passwd *result = nullptr;
    QByteArray buffer(16384, Qt::Uninitialized);
    if (getpwuid_r(uid, &pwd, buffer.data(), static_cast<size_t>(buffer.size()), &result) == 0 && result)
        return QString::fromLocal8Bit(result->pw_name);
    return QString::number(uid);
}

static std::optional<QByteArray> hexNeedle(QString text)
{
    text.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (text.size() % 2 != 0 || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]*$")).match(text).hasMatch())
        return std::nullopt;
    return QByteArray::fromHex(text.toLatin1());
}

static bool fileContains(const QString &path, const SearchOptions &o)
{
    if (o.contains.isEmpty())
        return true;

    QStringList terms = o.multipleValues ? patterns(o.contains) : QStringList{o.contains};
    QList<QByteArray> needles;
    for (const QString &term : terms) {
        if (o.binary) {
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
    while (!file.atEnd()) {
        QByteArray data = tail + file.read(1024 * 1024);
        if (!o.caseSensitive && !o.binary)
            data = data.toLower();
        for (qsizetype i = 0; i < needles.size(); ++i) {
            QByteArray needle = needles[i];
            if (!o.caseSensitive && !o.binary)
                needle = needle.toLower();
            if (!found[i] && data.contains(needle))
                found[i] = true;
        }
        const bool matched = o.multipleAnd
            ? std::all_of(found.cbegin(), found.cend(), [](bool x) { return x; })
            : std::any_of(found.cbegin(), found.cend(), [](bool x) { return x; });
        if (matched)
            return true;
        tail = data.right(overlap - 1);
    }
    return false;
}

static bool filesEqual(const QString &leftPath, const QString &rightPath)
{
    QFile left(leftPath);
    QFile right(rightPath);
    if (!left.open(QIODevice::ReadOnly) || !right.open(QIODevice::ReadOnly)
        || left.size() != right.size())
        return false;
    while (!left.atEnd()) {
        const QByteArray a = left.read(1024 * 1024);
        const QByteArray b = right.read(a.size());
        if (a != b)
            return false;
    }
    return right.atEnd();
}

static QByteArray sha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
        hash.addData(file.read(1024 * 1024));
    return hash.result();
}

struct SearchCacheEntry {
    qint64 size = -1;
    qint64 modified = -1;
    qint64 lastUsed = 0;
    QByteArray sha256;
    QHash<QByteArray, bool> contentMatches;
};

class SearchCache {
public:
    SearchCache()
    {
        const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(cacheRoot);
        path_ = QDir(cacheRoot).filePath(QStringLiteral("search-cache-v1.dat"));
        load();
    }

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    bool findHash(const FileRecord &record, QByteArray &digest)
    {
        if (!enabled_)
            return false;
        QMutexLocker lock(&mutex_);
        auto it = entries_.find(record.path);
        if (it == entries_.end() || !signatureMatches(*it, record) || it->sha256.isEmpty())
            return false;
        it->lastUsed = QDateTime::currentSecsSinceEpoch();
        digest = it->sha256;
        return true;
    }

    void storeHash(const FileRecord &record, const QByteArray &digest)
    {
        if (!enabled_ || digest.isEmpty())
            return;
        QMutexLocker lock(&mutex_);
        SearchCacheEntry &entry = currentEntry(record);
        entry.sha256 = digest;
    }

    std::optional<bool> findContent(const FileRecord &record, const QByteArray &queryKey)
    {
        if (!enabled_)
            return std::nullopt;
        QMutexLocker lock(&mutex_);
        auto it = entries_.find(record.path);
        if (it == entries_.end() || !signatureMatches(*it, record))
            return std::nullopt;
        const auto match = it->contentMatches.constFind(queryKey);
        if (match == it->contentMatches.cend())
            return std::nullopt;
        it->lastUsed = QDateTime::currentSecsSinceEpoch();
        return *match;
    }

    void storeContent(const FileRecord &record, const QByteArray &queryKey, bool matched)
    {
        if (!enabled_)
            return;
        QMutexLocker lock(&mutex_);
        SearchCacheEntry &entry = currentEntry(record);
        entry.contentMatches.insert(queryKey, matched);
    }

    void save()
    {
        if (!enabled_)
            return;
        QMutexLocker lock(&mutex_);
        prune();
        QSaveFile file(path_);
        if (!file.open(QIODevice::WriteOnly))
            return;
        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << quint32(0x46534348) << quint32(1) << quint32(entries_.size());
        for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
            stream << it.key() << it->size << it->modified << it->lastUsed << it->sha256;
            stream << quint32(it->contentMatches.size());
            for (auto match = it->contentMatches.cbegin(); match != it->contentMatches.cend(); ++match)
                stream << match.key() << match.value();
        }
        if (stream.status() == QDataStream::Ok)
            file.commit();
    }

    void clear()
    {
        QMutexLocker lock(&mutex_);
        entries_.clear();
        QFile::remove(path_);
    }

private:
    static bool signatureMatches(const SearchCacheEntry &entry, const FileRecord &record)
    {
        return entry.size == record.size && entry.modified == record.modified;
    }

    SearchCacheEntry &currentEntry(const FileRecord &record)
    {
        SearchCacheEntry &entry = entries_[record.path];
        if (!signatureMatches(entry, record)) {
            entry = {};
            entry.size = record.size;
            entry.modified = record.modified;
        }
        entry.lastUsed = QDateTime::currentSecsSinceEpoch();
        return entry;
    }

    void load()
    {
        QFile file(path_);
        if (!file.open(QIODevice::ReadOnly))
            return;
        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);
        quint32 magic = 0;
        quint32 version = 0;
        quint32 count = 0;
        stream >> magic >> version >> count;
        if (magic != 0x46534348 || version != 1 || count > 200'000)
            return;
        for (quint32 index = 0; index < count && stream.status() == QDataStream::Ok; ++index) {
            QString path;
            SearchCacheEntry entry;
            quint32 contentCount = 0;
            stream >> path >> entry.size >> entry.modified >> entry.lastUsed >> entry.sha256;
            stream >> contentCount;
            if (contentCount > 256)
                return;
            for (quint32 contentIndex = 0; contentIndex < contentCount; ++contentIndex) {
                QByteArray key;
                bool matched = false;
                stream >> key >> matched;
                entry.contentMatches.insert(key, matched);
            }
            entries_.insert(path, std::move(entry));
        }
        if (stream.status() != QDataStream::Ok)
            entries_.clear();
    }

    void prune()
    {
        constexpr qsizetype maxEntries = 100'000;
        if (entries_.size() <= maxEntries)
            return;
        QVector<QPair<qint64, QString>> ages;
        ages.reserve(entries_.size());
        for (auto it = entries_.cbegin(); it != entries_.cend(); ++it)
            ages.push_back({it->lastUsed, it.key()});
        std::sort(ages.begin(), ages.end(),
                  [](const auto &left, const auto &right) { return left.first > right.first; });
        for (qsizetype index = maxEntries; index < ages.size(); ++index)
            entries_.remove(ages[index].second);
    }

    QHash<QString, SearchCacheEntry> entries_;
    QMutex mutex_;
    QString path_;
    bool enabled_ = true;
};

class SearchEngine final : public QObject {
    Q_OBJECT
public:
    explicit SearchEngine(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    void stop() { cancelled_.store(true, std::memory_order_relaxed); }
    void clearCache()
    {
        cache_.clear();
        emit cacheCleared();
    }

    void search(const SearchOptions &o)
    {
        cancelled_.store(false, std::memory_order_relaxed);
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
                         it != end && !ec; it.increment(ec)) {
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
                const QFileInfo fileInfo(path);
                candidates.push_back({
                    path, name, QFileInfo(path).absolutePath(), size,
                    isDir ? 0 : qint64(st.st_blocks) * 512,
                    st.st_mtime,
                    fileInfo.birthTime().isValid() ? fileInfo.birthTime().toSecsSinceEpoch() : 0,
                    st.st_atime, st.st_ctime,
                    isDir ? QStringLiteral("Folder") : QStringLiteral("File"),
                    o.retrieveOwner ? ownerName(st.st_uid) : QString{}, attrs, 0, 0
                });
                if (o.maxResults > 0 && o.contains.isEmpty() && candidates.size() >= o.maxResults)
                    cancelledByLimit_ = true;
            };

            if (!o.recursive) {
                for (fs::directory_iterator it(root, dirOptions, ec), end;
                     it != end && !ec && !cancelledByLimit_; it.increment(ec)) {
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

        if (cancelled_.load(std::memory_order_relaxed)) {
            emit finished({}, tr("Search stopped"));
            return;
        }

        if (!o.contains.isEmpty() && !o.findFolders) {
            emit phaseProgress(tr("Searching file contents"), 0, candidates.size());
            std::atomic<qsizetype> checked = 0;
            const qsizetype total = candidates.size();
            QFuture<FileRecord> future = QtConcurrent::filtered(
                candidates, [this, o, &checked, total](const FileRecord &r) {
                    const bool matched = cachedFileContains(r, o);
                    const qsizetype done = checked.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done == total || done % 32 == 0)
                        emit phaseProgress(tr("Searching file contents"), done, total);
                    return matched;
                });
            future.waitForFinished();
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
                QHash<QByteArray, QVector<FileRecord>> sameHash;
                for (auto &[hash, record] : hashes)
                    if (!hash.isEmpty())
                        sameHash[hash].push_back(record);
                for (const auto &hashCandidates : sameHash) {
                    if (hashCandidates.size() < 2)
                        continue;
                    QVector<QVector<FileRecord>> exactGroups;
                    for (const auto &record : hashCandidates) {
                        bool placed = false;
                        for (auto &exact : exactGroups) {
                            if (filesEqual(exact.front().path, record.path)) {
                                exact.push_back(record);
                                placed = true;
                                break;
                            }
                        }
                        if (!placed)
                            exactGroups.push_back({record});
                    }
                    for (auto &exact : exactGroups) {
                        if (exact.size() < 2)
                            continue;
                        int duplicateCopy = 1;
                        for (auto &record : exact) {
                            record.group = group;
                            record.duplicateCopy = duplicateCopy++;
                            duplicatedPaths.insert(record.path);
                            if (o.mode == QStringLiteral("Duplicates search"))
                                results.push_back(record);
                        }
                        ++group;
                    }
                }
            }
            if (o.mode == QStringLiteral("Non-Duplicates search")) {
                for (const auto &record : candidates)
                    if (record.type == QStringLiteral("File") && !duplicatedPaths.contains(record.path))
                        results.push_back(record);
            } else if (!o.showOnlyDuplicateFiles) {
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
                            && filesEqual(sameName.front().path, sameName[i].path);
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
        } else if (o.mode == QStringLiteral("Summary")) {
            QHash<QString, QVector<FileRecord>> byFolder;
            for (const auto &record : candidates) {
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
        } else {
            results = std::move(candidates);
        }

        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        cache_.save();
        emit finished(results, tr("%1 results, %2 entries scanned in %3 seconds; cache: %4 hits")
                                  .arg(results.size()).arg(scanned).arg(seconds, 0, 'f', 2)
                                  .arg(cacheHits_.load(std::memory_order_relaxed)));
    }

signals:
    void progress(const QString &path, quint64 scanned);
    void phaseProgress(const QString &phase, quint64 completed, quint64 total);
    void cacheCleared();
    void finished(const QVector<FileRecord> &results, const QString &message);

private:
    QByteArray cachedSha256(const FileRecord &record)
    {
        QByteArray digest;
        if (cache_.findHash(record, digest)) {
            cacheHits_.fetch_add(1, std::memory_order_relaxed);
            return digest;
        }
        cacheMisses_.fetch_add(1, std::memory_order_relaxed);
        digest = sha256(record.path);
        cache_.storeHash(record, digest);
        return digest;
    }

    bool cachedFileContains(const FileRecord &record, const SearchOptions &o)
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
        const bool matched = fileContains(record.path, o);
        cache_.storeContent(record, queryKey, matched);
        return matched;
    }

    SearchCache cache_;
    std::atomic<quint64> cacheHits_ = 0;
    std::atomic<quint64> cacheMisses_ = 0;
    std::atomic_bool cancelled_ = false;
    bool cancelledByLimit_ = false;
};

class ResultsModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Folder, Size, SizeOnDisk, Modified, Created, Accessed, Changed,
                  Attributes, Extension, DuplicateNumber, DuplicateGroup, Type, Owner,
                  FullPath, ColumnCount };
    explicit ResultsModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : rows_.size(); }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : ColumnCount; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        static const QStringList names{
            tr("Filename"), tr("Folder"), tr("Size"), tr("Size On Disk"), tr("Modified Time"),
            tr("Created Time"), tr("Last Accessed Time"), tr("Entry Modified Time"),
            tr("Attributes"), tr("Extension"), tr("Duplicate Number"), tr("Duplicate Group"),
            tr("Type"), tr("File Owner"), tr("Full Path")
        };
        return names.value(section);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= rows_.size())
            return {};
        const FileRecord &r = rows_[index.row()];
        if (role == Qt::UserRole)
            return r.path;
        if (role == Qt::BackgroundRole && r.group && markDuplicates_) {
            const int colorIndex = (r.group - 1) % 64;
            return duplicateColorSet_ == 1
                ? QColor::fromHsv((colorIndex * 137) % 360, 38 + (colorIndex % 3) * 8, 255)
                : QColor::fromHsv((colorIndex * 137) % 360, 85, 205);
        }
        if (role == Qt::ForegroundRole && r.group && markDuplicates_)
            return duplicateColorSet_ == 1 ? QColor(Qt::black) : QColor(Qt::white);
        if (role == Qt::TextAlignmentRole && index.column() == Size)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        if (role == Qt::EditRole) {
            if (index.column() == Size) return r.size;
            if (index.column() == SizeOnDisk) return r.sizeOnDisk;
            if (index.column() == Modified) return r.modified;
            if (index.column() == Created) return r.created;
            if (index.column() == Accessed) return r.accessed;
            if (index.column() == Changed) return r.changed;
            if (index.column() == DuplicateNumber) return r.group;
            if (index.column() == DuplicateGroup) return r.duplicateCopy;
            return data(index, Qt::DisplayRole);
        }
        if (role != Qt::DisplayRole)
            return {};
        const auto time = [this](qint64 value) {
            QDateTime dateTime = QDateTime::fromSecsSinceEpoch(value);
            if (showGmt_)
                dateTime = dateTime.toUTC();
            return value ? dateTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                         : QString{};
        };
        switch (index.column()) {
        case Name: return r.name;
        case Folder: return r.folder;
        case Size: return formattedSize(r.size);
        case SizeOnDisk: return formattedSize(r.sizeOnDisk);
        case Modified: return time(r.modified);
        case Created: return time(r.created);
        case Accessed: return time(r.accessed);
        case Changed: return time(r.changed);
        case Attributes: return r.attributes;
        case Extension: return QFileInfo(r.name).suffix();
        case DuplicateNumber: return r.group ? QVariant(r.group) : QVariant{};
        case DuplicateGroup: return r.duplicateCopy ? QVariant(r.duplicateCopy) : QVariant{};
        case Type: return r.type;
        case Owner: return r.owner;
        case FullPath: return r.path;
        default: return {};
        }
    }

    void setRows(QVector<FileRecord> rows)
    {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }

    const QVector<FileRecord> &rows() const { return rows_; }

    void setDisplayOptions(int sizeUnit, bool showGmt, bool markDuplicates, int colorSet)
    {
        sizeUnit_ = sizeUnit;
        showGmt_ = showGmt;
        markDuplicates_ = markDuplicates;
        duplicateColorSet_ = colorSet;
        if (!rows_.isEmpty())
            emit dataChanged(index(0, 0), index(rows_.size() - 1, ColumnCount - 1));
    }

private:
    QString formattedSize(qint64 bytes) const
    {
        if (sizeUnit_ == 0)
            return humanSize(bytes);
        static const double divisors[]{1.0, 1024.0, 1024.0 * 1024.0, 1024.0 * 1024.0 * 1024.0};
        static const char *units[]{"Bytes", "KB", "MB", "GB"};
        const int index = std::clamp(sizeUnit_ - 1, 0, 3);
        return index == 0
            ? QStringLiteral("%1 Bytes").arg(bytes)
            : QStringLiteral("%1 %2").arg(double(bytes) / divisors[index], 0, 'f', 2)
                  .arg(QString::fromLatin1(units[index]));
    }

    QVector<FileRecord> rows_;
    int sizeUnit_ = 0;
    bool showGmt_ = false;
    bool markDuplicates_ = true;
    int duplicateColorSet_ = 1;
};

class OptionsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit OptionsDialog(const SearchOptions &o, QWidget *parent = nullptr) : QDialog(parent)
    {
        setWindowTitle(tr("Search Options"));
        resize(760, 560);
        auto *layout = new QVBoxLayout(this);
        auto *modeGroup = new QGroupBox(tr("Search mode"));
        auto *modeLayout = new QHBoxLayout(modeGroup);
        mode_ = new QComboBox;
        mode_->addItems({tr("Standard search"), tr("Duplicates search"), tr("Non-Duplicates search"),
                         tr("Summary"), tr("Duplicate names search")});
        mode_->setCurrentText(o.mode);
        duplicateMode_ = new QComboBox;
        duplicateMode_->addItems({tr("All files and folders"), tr("Only files"),
                                  tr("Only identical content"), tr("Only non-identical content")});
        duplicateMode_->setCurrentText(o.duplicateNameMode);
        duplicateWithoutExtension_ = new QCheckBox(tr("Compare names without extension"));
        duplicateWithoutExtension_->setChecked(o.duplicateNameWithoutExtension);
        modeLayout->addWidget(mode_);
        modeLayout->addWidget(new QLabel(tr("Duplicate-name mode:")));
        modeLayout->addWidget(duplicateMode_);
        modeLayout->addWidget(duplicateWithoutExtension_);
        layout->addWidget(modeGroup);

        auto *tabs = new QTabWidget;
        layout->addWidget(tabs);
        auto *filesPage = new QWidget;
        auto *filesForm = new QFormLayout(filesPage);
        roots_ = new QLineEdit(o.roots);
        auto *rootRow = new QWidget;
        auto *rootLayout = new QHBoxLayout(rootRow);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->addWidget(roots_);
        auto *browse = new QPushButton(tr("Browse…"));
        rootLayout->addWidget(browse);
        filesForm->addRow(tr("Base folders (separate with ;):"), rootRow);
        fileMasks_ = addLine(filesForm, tr("Files wildcard:"), o.fileWildcards);
        subfolderMasks_ = addLine(filesForm, tr("Subfolders wildcard:"), o.subfolderWildcards);
        excludeFiles_ = addLine(filesForm, tr("Exclude files:"), o.excludeFiles);
        excludeFolders_ = addLine(filesForm, tr("Exclude folders:"), o.excludeFolders);
        includeFolders_ = addLine(filesForm, tr("Include only folders:"), o.includeFolders);
        recursive_ = new QCheckBox(tr("Search subfolders")); recursive_->setChecked(o.recursive);
        findFiles_ = new QCheckBox(tr("Find files")); findFiles_->setChecked(o.findFiles);
        findFolders_ = new QCheckBox(tr("Find folders")); findFolders_->setChecked(o.findFolders);
        followLinks_ = new QCheckBox(tr("Follow symbolic links")); followLinks_->setChecked(o.followLinks);
        retrieveOwner_ = new QCheckBox(tr("Retrieve file owner (slower)")); retrieveOwner_->setChecked(o.retrieveOwner);
        accurateProgress_ = new QCheckBox(tr("Count files first for accurate progress"));
        accurateProgress_->setChecked(o.accurateProgress);
        useCache_ = new QCheckBox(tr("Use persistent cache for hashes and content searches"));
        useCache_->setChecked(o.useCache);
        maxDepth_ = new QSpinBox; maxDepth_->setRange(0, 1024); maxDepth_->setValue(o.maxDepth);
        maxDepth_->setSpecialValueText(tr("Unlimited"));
        maxResults_ = new QSpinBox; maxResults_->setRange(0, 100'000'000); maxResults_->setValue(o.maxResults);
        maxResults_->setSpecialValueText(tr("Unlimited"));
        filesForm->addRow(recursive_);
        filesForm->addRow(tr("Subfolders depth:"), maxDepth_);
        filesForm->addRow(findFiles_);
        filesForm->addRow(findFolders_);
        filesForm->addRow(followLinks_);
        filesForm->addRow(retrieveOwner_);
        filesForm->addRow(accurateProgress_);
        filesForm->addRow(useCache_);
        filesForm->addRow(tr("Stop after finding:"), maxResults_);
        tabs->addTab(filesPage, tr("Files & folders"));

        auto *contentPage = new QWidget;
        auto *contentForm = new QFormLayout(contentPage);
        contains_ = new QTextEdit(o.contains);
        contains_->setMaximumHeight(110);
        contentForm->addRow(tr("File contains:"), contains_);
        binary_ = new QCheckBox(tr("Binary search (hex bytes, e.g. A2 C5 2F)")); binary_->setChecked(o.binary);
        multiple_ = new QCheckBox(tr("Search multiple values (comma separated)")); multiple_->setChecked(o.multipleValues);
        multipleMode_ = new QComboBox; multipleMode_->addItems({tr("Or"), tr("And")});
        multipleMode_->setCurrentText(o.multipleAnd ? tr("And") : tr("Or"));
        caseSensitive_ = new QCheckBox(tr("Case sensitive")); caseSensitive_->setChecked(o.caseSensitive);
        contentForm->addRow(binary_);
        contentForm->addRow(multiple_);
        contentForm->addRow(tr("Multiple values match:"), multipleMode_);
        contentForm->addRow(caseSensitive_);
        tabs->addTab(contentPage, tr("File contents"));

        auto *filterPage = new QWidget;
        auto *filterForm = new QFormLayout(filterPage);
        minSize_ = sizeSpin(o.minSize);
        maxSize_ = sizeSpin(o.maxSize);
        maxSize_->setSuffix(tr(" bytes (0 = any)"));
        filterForm->addRow(tr("Minimum size:"), minSize_);
        filterForm->addRow(tr("Maximum size:"), maxSize_);
        timeField_ = new QComboBox;
        timeField_->addItems({tr("Modified time"), tr("Accessed time"), tr("Metadata changed time")});
        timeField_->setCurrentText(o.timeField);
        filterForm->addRow(tr("Time field:"), timeField_);
        lastMinutes_ = new QSpinBox;
        lastMinutes_->setRange(0, 10'000'000);
        lastMinutes_->setSuffix(tr(" minutes (0 = any)"));
        lastMinutes_->setValue(o.lastMinutes);
        filterForm->addRow(tr("Within last:"), lastMinutes_);
        minTime_ = new QLineEdit(o.minTime ? QDateTime::fromSecsSinceEpoch(o.minTime).toString(Qt::ISODate) : QString{});
        maxTime_ = new QLineEdit(o.maxTime ? QDateTime::fromSecsSinceEpoch(o.maxTime).toString(Qt::ISODate) : QString{});
        minTime_->setPlaceholderText(tr("YYYY-MM-DDTHH:MM:SS"));
        maxTime_->setPlaceholderText(tr("YYYY-MM-DDTHH:MM:SS"));
        filterForm->addRow(tr("From time:"), minTime_);
        filterForm->addRow(tr("To time:"), maxTime_);
        hidden_ = attrCombo(o.hidden);
        readonly_ = attrCombo(o.readonly);
        executable_ = attrCombo(o.executable);
        filterForm->addRow(tr("Hidden:"), hidden_);
        filterForm->addRow(tr("Read-only:"), readonly_);
        filterForm->addRow(tr("Executable:"), executable_);
        tabs->addTab(filterPage, tr("Size, time & attributes"));

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QStringList existingFolders = patterns(roots_->text());
            const QString startFolder = existingFolders.isEmpty()
                ? QDir::homePath() : existingFolders.constLast();
            const QString folder = QFileDialog::getExistingDirectory(
                this, tr("Choose base folder"), startFolder);
            if (folder.isEmpty())
                return;

            QStringList folders = existingFolders;
            const QString cleanPath = QDir::cleanPath(folder);
            if (!folders.contains(cleanPath))
                folders.push_back(cleanPath);
            roots_->setText(folders.join(u';'));
        });
        layout->addWidget(buttons);
    }

    SearchOptions options() const
    {
        SearchOptions o;
        o.roots = roots_->text();
        o.fileWildcards = fileMasks_->text();
        o.subfolderWildcards = subfolderMasks_->text();
        o.excludeFiles = excludeFiles_->text();
        o.excludeFolders = excludeFolders_->text();
        o.includeFolders = includeFolders_->text();
        o.contains = contains_->toPlainText();
        o.binary = binary_->isChecked();
        o.multipleValues = multiple_->isChecked();
        o.multipleAnd = multipleMode_->currentText() == tr("And");
        o.caseSensitive = caseSensitive_->isChecked();
        o.minSize = minSize_->value();
        o.maxSize = maxSize_->value();
        o.timeField = timeField_->currentText();
        o.lastMinutes = lastMinutes_->value();
        o.minTime = QDateTime::fromString(minTime_->text(), Qt::ISODate).toSecsSinceEpoch();
        o.maxTime = QDateTime::fromString(maxTime_->text(), Qt::ISODate).toSecsSinceEpoch();
        o.recursive = recursive_->isChecked();
        o.findFiles = findFiles_->isChecked();
        o.findFolders = findFolders_->isChecked();
        o.followLinks = followLinks_->isChecked();
        o.retrieveOwner = retrieveOwner_->isChecked();
        o.accurateProgress = accurateProgress_->isChecked();
        o.useCache = useCache_->isChecked();
        o.maxDepth = maxDepth_->value();
        o.maxResults = maxResults_->value();
        o.mode = mode_->currentText();
        o.duplicateNameMode = duplicateMode_->currentText();
        o.duplicateNameWithoutExtension = duplicateWithoutExtension_->isChecked();
        o.hidden = hidden_->currentText();
        o.readonly = readonly_->currentText();
        o.executable = executable_->currentText();
        return o;
    }

private:
    static QLineEdit *addLine(QFormLayout *layout, const QString &label, const QString &value)
    {
        auto *edit = new QLineEdit(value);
        layout->addRow(label, edit);
        return edit;
    }
    static QSpinBox *sizeSpin(qint64 value)
    {
        auto *spin = new QSpinBox;
        spin->setRange(0, 2'000'000'000);
        spin->setSuffix(QObject::tr(" bytes"));
        spin->setValue(static_cast<int>(std::min<qint64>(value, spin->maximum())));
        return spin;
    }
    static QComboBox *attrCombo(const QString &value)
    {
        auto *combo = new QComboBox;
        combo->addItems({QObject::tr("Any"), QObject::tr("Yes"), QObject::tr("No")});
        combo->setCurrentText(value);
        return combo;
    }

    QComboBox *mode_{};
    QComboBox *duplicateMode_{};
    QCheckBox *duplicateWithoutExtension_{};
    QLineEdit *roots_{};
    QLineEdit *fileMasks_{};
    QLineEdit *subfolderMasks_{};
    QLineEdit *excludeFiles_{};
    QLineEdit *excludeFolders_{};
    QLineEdit *includeFolders_{};
    QTextEdit *contains_{};
    QCheckBox *binary_{};
    QCheckBox *multiple_{};
    QComboBox *multipleMode_{};
    QCheckBox *caseSensitive_{};
    QSpinBox *minSize_{};
    QSpinBox *maxSize_{};
    QComboBox *timeField_{};
    QSpinBox *lastMinutes_{};
    QLineEdit *minTime_{};
    QLineEdit *maxTime_{};
    QCheckBox *recursive_{};
    QCheckBox *findFiles_{};
    QCheckBox *findFolders_{};
    QCheckBox *followLinks_{};
    QCheckBox *retrieveOwner_{};
    QCheckBox *accurateProgress_{};
    QCheckBox *useCache_{};
    QSpinBox *maxDepth_{};
    QSpinBox *maxResults_{};
    QComboBox *hidden_{};
    QComboBox *readonly_{};
    QComboBox *executable_{};
};

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow()
    {
        loadOptions();
        setWindowTitle(tr("SearchMyFiles for Linux"));
        resize(1280, 760);
        model_ = new ResultsModel(this);
        model_->setDisplayOptions(sizeUnit_, showGmt_, markDuplicates_, duplicateColorSet_);
        proxy_ = new QSortFilterProxyModel(this);
        proxy_->setSourceModel(model_);
        proxy_->setSortRole(Qt::EditRole);
        table_ = new QTableView;
        table_->setModel(proxy_);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setSortingEnabled(true);
        table_->setAlternatingRowColors(true);
        table_->setContextMenuPolicy(Qt::CustomContextMenu);
        table_->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
        table_->horizontalHeader()->setSectionResizeMode(ResultsModel::Name, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(ResultsModel::Folder, QHeaderView::Stretch);
        setCentralWidget(table_);
        {
            QSettings settings;
            restoreGeometry(settings.value(QStringLiteral("ui/windowGeometry")).toByteArray());
            table_->horizontalHeader()->restoreState(
                settings.value(QStringLiteral("ui/headerState")).toByteArray());
            table_->setShowGrid(showGrid_);
            table_->viewport()->setAttribute(Qt::WA_AlwaysShowToolTips, showTooltips_);
        }

        auto *toolbar = addToolBar(tr("Main"));
        searchAction_ = toolbar->addAction(QIcon::fromTheme(QStringLiteral("system-search")), tr("Search options…"));
        searchAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F9));
        stopAction_ = toolbar->addAction(QIcon::fromTheme(QStringLiteral("process-stop")), tr("Stop"));
        stopAction_->setShortcut(Qt::Key_Escape);
        stopAction_->setEnabled(false);
        refreshAction_ = toolbar->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), tr("Refresh"));
        refreshAction_->setShortcut(Qt::Key_F5);
        toolbar->addSeparator();
        auto *exportAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-save")), tr("Export…"));
        exportAction->setShortcut(QKeySequence::Save);
        auto *openAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("Open"));
        openAction->setShortcut(Qt::Key_F9);
        auto *folderAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Open folder"));
        folderAction->setShortcut(Qt::Key_F8);
        auto *openWithAction = new QAction(tr("Open With…"), this);
        openWithAction->setShortcut(Qt::Key_F7);
        auto *selectInFolderAction = new QAction(tr("Select File In File Manager"), this);
        auto *trashAction = new QAction(tr("Move To Trash"), this);
        trashAction->setShortcut(Qt::Key_Delete);
        auto *deleteAction = new QAction(tr("Delete Selected Files"), this);
        deleteAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
        auto *renameAction = new QAction(tr("Rename"), this);
        renameAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
        auto *propertiesAction = new QAction(tr("Properties"), this);
        propertiesAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
        for (QAction *action : {openWithAction, selectInFolderAction, trashAction,
                                deleteAction, renameAction, propertiesAction})
            addAction(action);

        auto *fileMenu = menuBar()->addMenu(tr("&File"));
        fileMenu->addAction(searchAction_);
        fileMenu->addAction(stopAction_);
        fileMenu->addAction(refreshAction_);
        fileMenu->addSeparator();
        fileMenu->addAction(openAction);
        fileMenu->addAction(folderAction);
        fileMenu->addAction(openWithAction);
        fileMenu->addAction(selectInFolderAction);
        fileMenu->addSeparator();
        fileMenu->addAction(trashAction);
        fileMenu->addAction(deleteAction);
        fileMenu->addAction(renameAction);
        fileMenu->addSeparator();
        fileMenu->addAction(exportAction);
        fileMenu->addAction(propertiesAction);
        fileMenu->addSeparator();
        fileMenu->addAction(tr("Exit"), QKeySequence::Quit, this, &QWidget::close);

        auto *editMenu = menuBar()->addMenu(tr("&Edit"));
        auto *findAction = editMenu->addAction(tr("Find…"));
        findAction->setShortcut(QKeySequence::Find);
        auto *findNextAction = editMenu->addAction(tr("Find next"));
        findNextAction->setShortcut(Qt::Key_F3);
        editMenu->addSeparator();
        auto *copyInfoAction = editMenu->addAction(tr("Copy files information"));
        copyInfoAction->setShortcut(QKeySequence::Copy);
        auto *copyPathsAction = editMenu->addAction(tr("Copy full filenames path"));
        copyPathsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
        auto *explorerCopyAction = editMenu->addAction(tr("Explorer copy"));
        explorerCopyAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
        auto *explorerCutAction = editMenu->addAction(tr("Explorer cut"));
        explorerCutAction->setShortcut(QKeySequence::Cut);
        editMenu->addSeparator();
        editMenu->addAction(tr("Select all"), QKeySequence::SelectAll, table_, &QTableView::selectAll);
        auto *deselectAction = editMenu->addAction(tr("Deselect all"));
        deselectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));

        auto *viewMenu = menuBar()->addMenu(tr("&View"));
        auto *gridAction = viewMenu->addAction(tr("Show grid lines"));
        gridAction->setCheckable(true);
        gridAction->setChecked(showGrid_);
        auto *tooltipsAction = viewMenu->addAction(tr("Show tooltips"));
        tooltipsAction->setCheckable(true);
        tooltipsAction->setChecked(showTooltips_);
        viewMenu->addSeparator();
        auto *autoSizeAction = viewMenu->addAction(tr("Auto size columns"), this, [this] {
            table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
            table_->resizeColumnsToContents();
            saveUiSettings();
        });
        autoSizeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus));
        auto *columnsMenu = viewMenu->addMenu(tr("Choose columns"));
        for (int column = 0; column < ResultsModel::ColumnCount; ++column) {
            auto *action = columnsMenu->addAction(
                model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
            action->setCheckable(true);
            action->setChecked(!table_->isColumnHidden(column));
            connect(action, &QAction::toggled, this, [this, column](bool visible) {
                table_->setColumnHidden(column, !visible);
                saveUiSettings();
            });
        }

        auto *optionsMenu = menuBar()->addMenu(tr("&Options"));
        const auto addChoiceMenu = [this, optionsMenu](
                                       const QString &title,
                                       const QStringList &choices,
                                       const QString &selected,
                                       const std::function<void(const QString &)> &changed) {
            auto *menu = optionsMenu->addMenu(title);
            auto *group = new QActionGroup(menu);
            group->setExclusive(true);
            for (const QString &choice : choices) {
                auto *action = menu->addAction(choice);
                action->setCheckable(true);
                action->setChecked(choice == selected);
                group->addAction(action);
                connect(action, &QAction::triggered, this, [this, choice, changed] {
                    changed(choice);
                    saveUiSettings();
                });
            }
            return menu;
        };

        addChoiceMenu(tr("File Size Unit"),
                      {tr("Automatic"), tr("Bytes"), tr("KB"), tr("MB"), tr("GB")},
                      sizeUnitName(), [this](const QString &choice) {
            sizeUnit_ = sizeUnitFromName(choice);
            applyDisplayOptions();
        });
        addChoiceMenu(tr("Summary File Size Unit"),
                      {tr("Automatic"), tr("Bytes"), tr("KB"), tr("MB"), tr("GB")},
                      summarySizeUnitName_, [this](const QString &choice) {
            summarySizeUnitName_ = choice;
            if (options_.mode == QStringLiteral("Summary"))
                applyDisplayOptions();
        });

        auto *duplicateOptions = optionsMenu->addMenu(tr("Duplicate Search Options"));
        auto *duplicateOptionsGroup = new QActionGroup(duplicateOptions);
        auto *showAllDuplicates = duplicateOptions->addAction(tr("Show All Files"));
        auto *showOnlyDuplicates = duplicateOptions->addAction(tr("Show Only Duplicate Files"));
        for (QAction *action : {showAllDuplicates, showOnlyDuplicates}) {
            action->setCheckable(true);
            duplicateOptionsGroup->addAction(action);
        }
        showOnlyDuplicates->setChecked(options_.showOnlyDuplicateFiles);
        showAllDuplicates->setChecked(!options_.showOnlyDuplicateFiles);
        connect(showOnlyDuplicates, &QAction::triggered, this, [this] {
            options_.showOnlyDuplicateFiles = true;
            saveOptions();
        });
        connect(showAllDuplicates, &QAction::triggered, this, [this] {
            options_.showOnlyDuplicateFiles = false;
            saveOptions();
        });

        addChoiceMenu(tr("Duplicate Mark Color Set"),
                      {tr("Color Set 1"), tr("Color Set 2")},
                      duplicateColorSet_ == 1 ? tr("Color Set 1") : tr("Color Set 2"),
                      [this](const QString &choice) {
            duplicateColorSet_ = choice.endsWith(u'1') ? 1 : 2;
            applyDisplayOptions();
        });
        addChoiceMenu(tr("Double-Click Action"), actionChoices(), doubleClickAction_,
                      [this](const QString &choice) { doubleClickAction_ = choice; });
        addChoiceMenu(tr("Enter Key Action"), actionChoices(), enterKeyAction_,
                      [this](const QString &choice) { enterKeyAction_ = choice; });

        auto addToggle = [this, optionsMenu](const QString &label, bool checked,
                                             const std::function<void(bool)> &changed) {
            auto *action = optionsMenu->addAction(label);
            action->setCheckable(true);
            action->setChecked(checked);
            connect(action, &QAction::toggled, this, [this, changed](bool value) {
                changed(value);
                saveUiSettings();
            });
            return action;
        };
        addToggle(tr("Include Subfolders in Summary Totals"), options_.includeSubfoldersInSummary,
                  [this](bool value) { options_.includeSubfoldersInSummary = value; saveOptions(); });
        addToggle(tr("Show Time In GMT"), showGmt_, [this](bool value) {
            showGmt_ = value; applyDisplayOptions();
        });
        optionsMenu->addSeparator();
        addToggle(tr("Mark Duplicate Files"), markDuplicates_, [this](bool value) {
            markDuplicates_ = value; applyDisplayOptions();
        });
        addToggle(tr("Add Header Line To CSV/Tab-Delimited File"), addExportHeader_,
                  [this](bool value) { addExportHeader_ = value; });
        addToggle(tr("Retrieve File Owner"), options_.retrieveOwner,
                  [this](bool value) { options_.retrieveOwner = value; saveOptions(); });
        addToggle(tr("Accurate Progress (Count Files First)"), options_.accurateProgress,
                  [this](bool value) { options_.accurateProgress = value; saveOptions(); });
        addToggle(tr("Use Persistent Search Cache"), options_.useCache,
                  [this](bool value) { options_.useCache = value; saveOptions(); });
        optionsMenu->addAction(tr("Clear Search Cache"), this, [this] {
            emit clearCacheRequested();
        });
        addToggle(tr("Set Focus On Search Start"), focusOnSearchStart_,
                  [this](bool value) { focusOnSearchStart_ = value; });
        addToggle(tr("Set Focus On Search End"), focusOnSearchEnd_,
                  [this](bool value) { focusOnSearchEnd_ = value; });
        addToggle(tr("Auto Size Columns On Search End"), autoSizeOnSearchEnd_,
                  [this](bool value) { autoSizeOnSearchEnd_ = value; });

        menuBar()->addMenu(tr("&Help"))->addAction(tr("About"), this, [this] {
            QMessageBox::about(this, tr("About"),
                tr("FolderScope %1\n\nFast unindexed file search.\nC++20 + Qt 6.")
                    .arg(QStringLiteral(FOLDERSCOPE_VERSION)));
        });

        progress_ = new QProgressBar;
        progress_->setRange(0, 0);
        progress_->setMinimumWidth(280);
        progress_->setMaximumWidth(420);
        progress_->setTextVisible(true);
        progress_->hide();
        statusBar()->addPermanentWidget(progress_);
        statusBar()->showMessage(tr("Press Ctrl+F9 to configure and start a search"));

        engine_ = new SearchEngine;
        engine_->moveToThread(&workerThread_);
        connect(&workerThread_, &QThread::finished, engine_, &QObject::deleteLater);
        connect(this, &MainWindow::startRequested, engine_, &SearchEngine::search);
        connect(this, &MainWindow::stopRequested, engine_, &SearchEngine::stop, Qt::DirectConnection);
        connect(this, &MainWindow::clearCacheRequested, engine_, &SearchEngine::clearCache);
        connect(engine_, &SearchEngine::cacheCleared, this, [this] {
            statusBar()->showMessage(tr("Search cache cleared"), 5000);
        });
        connect(engine_, &SearchEngine::progress, this, [this](const QString &path, quint64 count) {
            progress_->setRange(0, 0);
            progress_->setFormat(tr("Scanning… %1 entries").arg(count));
            statusBar()->showMessage(tr("Scanning %1 (%2 entries)").arg(path).arg(count));
        });
        connect(engine_, &SearchEngine::phaseProgress, this,
                [this](const QString &phase, quint64 completed, quint64 total) {
            if (total == 0) {
                progress_->setRange(0, 0);
                progress_->setFormat(completed
                    ? tr("%1… %2").arg(phase).arg(completed)
                    : phase);
                statusBar()->showMessage(completed
                    ? tr("%1: %2").arg(phase).arg(completed)
                    : phase);
                return;
            }
            const int percent = int(std::min<quint64>(100, completed * 100 / total));
            progress_->setRange(0, 100);
            progress_->setValue(percent);
            progress_->setFormat(tr("%1% — %2").arg(percent).arg(phase));
            statusBar()->showMessage(
                tr("%1: %2 / %3").arg(phase).arg(completed).arg(total));
        });
        connect(engine_, &SearchEngine::finished, this, &MainWindow::searchFinished);
        workerThread_.start();

        connect(searchAction_, &QAction::triggered, this, &MainWindow::showOptions);
        connect(stopAction_, &QAction::triggered, this, [this] { emit stopRequested(); });
        connect(refreshAction_, &QAction::triggered, this, &MainWindow::refreshSearch);
        connect(exportAction, &QAction::triggered, this, &MainWindow::exportResults);
        connect(openAction, &QAction::triggered, this, &MainWindow::openSelected);
        connect(folderAction, &QAction::triggered, this, &MainWindow::openFolder);
        connect(openWithAction, &QAction::triggered, this, &MainWindow::openWith);
        connect(selectInFolderAction, &QAction::triggered, this, &MainWindow::selectInFileManager);
        connect(trashAction, &QAction::triggered, this, &MainWindow::moveToTrash);
        connect(deleteAction, &QAction::triggered, this, &MainWindow::deleteSelected);
        connect(renameAction, &QAction::triggered, this, &MainWindow::renameSelected);
        connect(propertiesAction, &QAction::triggered, this, &MainWindow::showProperties);
        connect(findAction, &QAction::triggered, this, &MainWindow::findText);
        connect(findNextAction, &QAction::triggered, this, &MainWindow::findNext);
        connect(copyInfoAction, &QAction::triggered, this, &MainWindow::copyInformation);
        connect(copyPathsAction, &QAction::triggered, this, &MainWindow::copyPaths);
        connect(explorerCopyAction, &QAction::triggered, this, &MainWindow::explorerCopy);
        connect(explorerCutAction, &QAction::triggered, this, &MainWindow::explorerCut);
        connect(deselectAction, &QAction::triggered, table_, &QTableView::clearSelection);
        connect(gridAction, &QAction::toggled, this, [this](bool enabled) {
            showGrid_ = enabled;
            table_->setShowGrid(enabled);
            saveUiSettings();
        });
        connect(tooltipsAction, &QAction::toggled, this, [this](bool enabled) {
            showTooltips_ = enabled;
            table_->viewport()->setAttribute(Qt::WA_AlwaysShowToolTips, enabled);
            saveUiSettings();
        });
        connect(table_, &QTableView::doubleClicked, this, [this] {
            executeConfiguredAction(doubleClickAction_);
        });
        connect(table_, &QWidget::customContextMenuRequested, this, &MainWindow::contextMenu);
        connect(table_->horizontalHeader(), &QHeaderView::customContextMenuRequested,
                this, &MainWindow::headerContextMenu);
    }

    ~MainWindow() override
    {
        emit stopRequested();
        workerThread_.quit();
        workerThread_.wait();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        saveUiSettings();
        QMainWindow::closeEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && event->modifiers() == Qt::NoModifier) {
            executeConfiguredAction(enterKeyAction_);
            event->accept();
            return;
        }
        QMainWindow::keyPressEvent(event);
    }

signals:
    void startRequested(const SearchOptions &options);
    void stopRequested();
    void clearCacheRequested();

private slots:
    void showOptions()
    {
        OptionsDialog dialog(options_, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        options_ = dialog.options();
        saveOptions();
        startCurrentSearch();
    }

    void refreshSearch()
    {
        if (!isSearching_)
            startCurrentSearch();
    }

    void findText()
    {
        bool accepted = false;
        const QString query = QInputDialog::getText(
            this, tr("Find"), tr("Find text:"), QLineEdit::Normal, findQuery_, &accepted);
        if (!accepted || query.isEmpty())
            return;
        findQuery_ = query;
        findNext();
    }

    void findNext()
    {
        if (findQuery_.isEmpty() || proxy_->rowCount() == 0)
            return;
        int start = 0;
        const QModelIndex current = table_->currentIndex();
        if (current.isValid())
            start = (current.row() + 1) % proxy_->rowCount();
        for (int offset = 0; offset < proxy_->rowCount(); ++offset) {
            const int row = (start + offset) % proxy_->rowCount();
            QStringList values;
            for (int column = 0; column < proxy_->columnCount(); ++column)
                values.push_back(proxy_->index(row, column).data(Qt::DisplayRole).toString());
            if (values.join(u'\t').contains(findQuery_, Qt::CaseInsensitive)) {
                const QModelIndex match = proxy_->index(row, 0);
                table_->selectionModel()->select(
                    match, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                table_->setCurrentIndex(match);
                table_->scrollTo(match, QAbstractItemView::PositionAtCenter);
                return;
            }
        }
        statusBar()->showMessage(tr("Text not found: %1").arg(findQuery_), 4000);
    }

    void copyInformation()
    {
        QModelIndexList selected = table_->selectionModel()->selectedRows();
        std::sort(selected.begin(), selected.end(),
                  [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
        if (selected.isEmpty())
            return;
        QStringList lines;
        QStringList headers;
        for (int column = 0; column < proxy_->columnCount(); ++column) {
            if (!table_->isColumnHidden(column))
                headers.push_back(proxy_->headerData(column, Qt::Horizontal).toString());
        }
        lines.push_back(headers.join(u'\t'));
        for (const QModelIndex &rowIndex : selected) {
            QStringList values;
            for (int column = 0; column < proxy_->columnCount(); ++column) {
                if (!table_->isColumnHidden(column))
                    values.push_back(proxy_->index(rowIndex.row(), column).data().toString());
            }
            lines.push_back(values.join(u'\t'));
        }
        QApplication::clipboard()->setText(lines.join(u'\n'));
    }

    void explorerCopy()
    {
        const QStringList paths = selectedPaths();
        if (paths.isEmpty())
            return;
        auto *mime = new QMimeData;
        QList<QUrl> urls;
        for (const QString &path : paths)
            urls.push_back(QUrl::fromLocalFile(path));
        mime->setUrls(urls);
        mime->setText(paths.join(u'\n'));
        QApplication::clipboard()->setMimeData(mime);
    }

    void explorerCut()
    {
        const QStringList paths = selectedPaths();
        if (paths.isEmpty())
            return;
        auto *mime = new QMimeData;
        QList<QUrl> urls;
        QByteArray gnomeData("cut\n");
        for (const QString &path : paths) {
            const QUrl url = QUrl::fromLocalFile(path);
            urls.push_back(url);
            gnomeData += url.toEncoded() + '\n';
        }
        mime->setUrls(urls);
        mime->setText(paths.join(u'\n'));
        mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeData);
        QApplication::clipboard()->setMimeData(mime);
    }

    void startCurrentSearch()
    {
        if (isSearching_ || options_.roots.trimmed().isEmpty())
            return;
        isSearching_ = true;
        applyDisplayOptions();
        model_->setRows({});
        searchAction_->setEnabled(false);
        stopAction_->setEnabled(true);
        refreshAction_->setEnabled(false);
        progress_->show();
        progress_->setRange(0, 0);
        progress_->setFormat(tr("Scanning…"));
        setWindowTitle(tr("%1 — %2 — SearchMyFiles for Linux")
                           .arg(options_.roots, options_.mode));
        if (focusOnSearchStart_)
            table_->setFocus();
        emit startRequested(options_);
    }

    void searchFinished(const QVector<FileRecord> &results, const QString &message)
    {
        isSearching_ = false;
        model_->setRows(results);
        searchAction_->setEnabled(true);
        stopAction_->setEnabled(false);
        refreshAction_->setEnabled(true);
        progress_->hide();
        statusBar()->showMessage(message);
        if (autoSizeOnSearchEnd_) {
            table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
            table_->resizeColumnsToContents();
        }
        if (focusOnSearchEnd_)
            table_->setFocus();
    }

    void openSelected()
    {
        for (const QString &path : selectedPaths())
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }

    void openFolder()
    {
        const QStringList paths = selectedPaths();
        if (!paths.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.front()).absolutePath()));
    }

    void openWith()
    {
        const QStringList paths = selectedPaths();
        if (!paths.isEmpty())
            QProcess::startDetached(QStringLiteral("gio"), {QStringLiteral("open"), paths.front()});
    }

    void selectInFileManager()
    {
        const QStringList paths = selectedPaths();
        if (paths.isEmpty())
            return;
        QStringList urls;
        for (const QString &path : paths)
            urls.push_back(QUrl::fromLocalFile(path).toString());
        QDBusInterface fileManager(
            QStringLiteral("org.freedesktop.FileManager1"),
            QStringLiteral("/org/freedesktop/FileManager1"),
            QStringLiteral("org.freedesktop.FileManager1"),
            QDBusConnection::sessionBus());
        const QDBusMessage reply = fileManager.call(QStringLiteral("ShowItems"), urls, QString{});
        if (!fileManager.isValid() || reply.type() == QDBusMessage::ErrorMessage)
            openFolder();
    }

    void moveToTrash()
    {
        const QStringList paths = selectedPaths();
        if (paths.isEmpty()
            || QMessageBox::question(
                   this, tr("Move To Trash"),
                   tr("Move %1 selected item(s) to Trash?").arg(paths.size()),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        int failed = 0;
        for (const QString &path : paths)
            if (!QFile::moveToTrash(path))
                ++failed;
        if (failed)
            QMessageBox::warning(this, tr("Move To Trash"),
                                 tr("Failed to move %1 item(s) to Trash.").arg(failed));
        refreshSearch();
    }

    void deleteSelected()
    {
        const QStringList paths = selectedPaths();
        if (paths.isEmpty()
            || QMessageBox::warning(
                   this, tr("Delete Selected Files"),
                   tr("Permanently delete %1 selected item(s)?\n\nThis cannot be undone.")
                       .arg(paths.size()),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        int failed = 0;
        for (const QString &path : paths) {
            const QFileInfo info(path);
            const bool removed = info.isDir() ? QDir().rmdir(path) : QFile::remove(path);
            if (!removed)
                ++failed;
        }
        if (failed)
            QMessageBox::warning(this, tr("Delete Selected Files"),
                                 tr("Failed to delete %1 item(s).").arg(failed));
        refreshSearch();
    }

    void renameSelected()
    {
        const auto record = currentRecord();
        if (!record)
            return;
        bool accepted = false;
        const QString newName = QInputDialog::getText(
            this, tr("Rename"), tr("New filename:"), QLineEdit::Normal,
            record->name, &accepted);
        if (!accepted || newName.isEmpty() || newName == record->name)
            return;
        const QString newPath = QDir(record->folder).filePath(newName);
        if (!QFile::rename(record->path, newPath))
            QMessageBox::critical(this, tr("Rename"), tr("Failed to rename this item."));
        else
            refreshSearch();
    }

    void showProperties()
    {
        const auto record = currentRecord();
        if (!record)
            return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Properties"));
        dialog.resize(780, 540);
        auto *layout = new QVBoxLayout(&dialog);
        auto *form = new QFormLayout;
        const auto addValue = [form](const QString &label, const QString &value) {
            auto *edit = new QLineEdit(value);
            edit->setReadOnly(true);
            edit->setCursorPosition(0);
            form->addRow(label, edit);
        };
        const auto date = [this](qint64 timestamp) {
            if (!timestamp)
                return QString{};
            QDateTime value = QDateTime::fromSecsSinceEpoch(timestamp);
            if (showGmt_)
                value = value.toUTC();
            return value.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        };
        addValue(tr("Filename:"), record->name);
        addValue(tr("Folder:"), record->folder);
        addValue(tr("Full Path:"), record->path);
        addValue(tr("Size:"), QString::number(record->size));
        addValue(tr("Size On Disk:"), QString::number(record->sizeOnDisk));
        addValue(tr("Modified Time:"), date(record->modified));
        addValue(tr("Created Time:"), date(record->created));
        addValue(tr("Last Accessed Time:"), date(record->accessed));
        addValue(tr("Entry Modified Time:"), date(record->changed));
        addValue(tr("Attributes:"), record->attributes);
        addValue(tr("Extension:"), QFileInfo(record->name).suffix());
        addValue(tr("Duplicate Number:"), record->group ? QString::number(record->group) : QString{});
        addValue(tr("Duplicate Group:"),
                 record->duplicateCopy ? QString::number(record->duplicateCopy) : QString{});
        addValue(tr("File Owner:"), record->owner);
        layout->addLayout(form);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        layout->addWidget(buttons);
        dialog.exec();
    }

    void copyPaths()
    {
        QApplication::clipboard()->setText(selectedPaths().join(u'\n'));
    }

    void contextMenu(const QPoint &point)
    {
        QMenu menu(this);
        menu.addAction(tr("Open Selected File"), QKeySequence(Qt::Key_F9), this, &MainWindow::openSelected);
        menu.addAction(tr("Open Folder"), QKeySequence(Qt::Key_F8), this, &MainWindow::openFolder);
        menu.addAction(tr("Open With…"), QKeySequence(Qt::Key_F7), this, &MainWindow::openWith);
        menu.addAction(tr("Select File In File Manager"), this, &MainWindow::selectInFileManager);
        menu.addSeparator();
        menu.addAction(tr("Explorer Copy"), QKeySequence(Qt::CTRL | Qt::Key_E),
                       this, &MainWindow::explorerCopy);
        menu.addAction(tr("Explorer Cut"), QKeySequence::Cut, this, &MainWindow::explorerCut);
        menu.addSeparator();
        menu.addAction(tr("Move To Trash"), QKeySequence(Qt::Key_Delete),
                       this, &MainWindow::moveToTrash);
        menu.addAction(tr("Delete Selected Files"), QKeySequence(Qt::SHIFT | Qt::Key_Delete),
                       this, &MainWindow::deleteSelected);
        menu.addAction(tr("Rename"), QKeySequence(Qt::CTRL | Qt::Key_R),
                       this, &MainWindow::renameSelected);
        menu.addSeparator();
        menu.addAction(tr("Save Files Information"), QKeySequence::Save,
                       this, &MainWindow::exportResults);
        menu.addAction(tr("Copy Files Information"), QKeySequence::Copy,
                       this, &MainWindow::copyInformation);
        menu.addSeparator();
        auto *columns = menu.addMenu(tr("Choose Columns"));
        for (int column = 0; column < ResultsModel::ColumnCount; ++column) {
            auto *action = columns->addAction(
                model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
            action->setCheckable(true);
            action->setChecked(!table_->isColumnHidden(column));
            connect(action, &QAction::toggled, this, [this, column](bool visible) {
                table_->setColumnHidden(column, !visible);
                saveUiSettings();
            });
        }
        menu.addAction(tr("Auto Size Columns"), QKeySequence(Qt::CTRL | Qt::Key_Plus), this, [this] {
            table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
            table_->resizeColumnsToContents();
        });
        menu.addSeparator();
        menu.addAction(tr("Properties"), QKeySequence(Qt::ALT | Qt::Key_Return),
                       this, &MainWindow::showProperties);
        menu.addSeparator();
        menu.addAction(tr("Refresh"), QKeySequence(Qt::Key_F5), this, &MainWindow::refreshSearch);
        menu.exec(table_->viewport()->mapToGlobal(point));
    }

    void headerContextMenu(const QPoint &point)
    {
        QHeaderView *header = table_->horizontalHeader();
        const int clickedColumn = header->logicalIndexAt(point);
        QMenu menu(this);
        auto *autoWidthColumn = menu.addAction(tr("Auto Width This Column"));
        autoWidthColumn->setEnabled(clickedColumn >= 0);
        connect(autoWidthColumn, &QAction::triggered, this, [this, clickedColumn] {
            table_->horizontalHeader()->setSectionResizeMode(clickedColumn, QHeaderView::Interactive);
            table_->resizeColumnToContents(clickedColumn);
            saveUiSettings();
        });
        menu.addAction(tr("Auto Width All Columns"), this, [this] {
            table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
            table_->resizeColumnsToContents();
            saveUiSettings();
        });
        menu.addSeparator();
        auto *hideColumn = menu.addAction(tr("Hide This Column"));
        hideColumn->setEnabled(clickedColumn >= 0);
        connect(hideColumn, &QAction::triggered, this, [this, clickedColumn] {
            table_->setColumnHidden(clickedColumn, true);
            saveUiSettings();
        });
        auto *columns = menu.addMenu(tr("Choose Columns"));
        for (int column = 0; column < ResultsModel::ColumnCount; ++column) {
            auto *action = columns->addAction(
                model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
            action->setCheckable(true);
            action->setChecked(!table_->isColumnHidden(column));
            connect(action, &QAction::toggled, this, [this, column](bool visible) {
                table_->setColumnHidden(column, !visible);
                saveUiSettings();
            });
        }
        menu.exec(header->mapToGlobal(point));
    }

    void exportResults()
    {
        QVector<FileRecord> rows;
        const QStringList selectedList = selectedPaths();
        const QSet<QString> selected(selectedList.cbegin(), selectedList.cend());
        for (const auto &record : model_->rows())
            if (selected.isEmpty() || selected.contains(record.path))
                rows.push_back(record);
        if (rows.isEmpty())
            return;
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export results"), QStringLiteral("search-results.csv"),
            tr("CSV (*.csv);;Text (*.txt *.tsv);;HTML (*.html);;XML (*.xml);;JSON (*.json)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::critical(this, tr("Export failed"), file.errorString());
            return;
        }
        const auto quoted = [](QString value) {
            value.replace(u'"', QStringLiteral("\"\""));
            return u'"' + value + u'"';
        };
        const auto values = [](const FileRecord &r) {
            const auto date = [](qint64 t) { return t ? QDateTime::fromSecsSinceEpoch(t).toString(Qt::ISODate) : QString{}; };
            return QStringList{
                r.name, r.folder, QString::number(r.size), QString::number(r.sizeOnDisk),
                date(r.modified), date(r.created), date(r.accessed), date(r.changed),
                r.attributes, QFileInfo(r.name).suffix(),
                r.group ? QString::number(r.group) : QString{},
                r.duplicateCopy ? QString::number(r.duplicateCopy) : QString{},
                r.type, r.owner, r.path
            };
        };
        const QStringList headers{
            tr("Filename"), tr("Folder"), tr("Size"), tr("Size On Disk"), tr("Modified Time"),
            tr("Created Time"), tr("Last Accessed Time"), tr("Entry Modified Time"),
            tr("Attributes"), tr("Extension"), tr("Duplicate Number"), tr("Duplicate Group"),
            tr("Type"), tr("File Owner"), tr("Full Path")
        };
        QTextStream out(&file);
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == QStringLiteral("json")) {
            QJsonArray array;
            for (const auto &record : rows) {
                QJsonObject object;
                const QStringList row = values(record);
                for (int i = 0; i < headers.size(); ++i)
                    object.insert(headers[i], row[i]);
                array.append(object);
            }
            file.write(QJsonDocument(array).toJson());
        } else if (suffix == QStringLiteral("html")) {
            out << "<!doctype html><meta charset=\"utf-8\"><table><thead><tr>";
            for (const auto &header : headers) out << "<th>" << header.toHtmlEscaped() << "</th>";
            out << "</tr></thead><tbody>";
            for (const auto &record : rows) {
                out << "<tr>";
                for (const auto &value : values(record)) out << "<td>" << value.toHtmlEscaped() << "</td>";
                out << "</tr>";
            }
            out << "</tbody></table>";
        } else if (suffix == QStringLiteral("xml")) {
            out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><SearchResults>";
            for (const auto &record : rows) {
                out << "<File>";
                const auto row = values(record);
                for (int i = 0; i < headers.size(); ++i) {
                    QString tag = headers[i];
                    tag.remove(u' ');
                    out << "<" << tag << ">" << row[i].toHtmlEscaped()
                        << "</" << tag << ">";
                }
                out << "</File>";
            }
            out << "</SearchResults>";
        } else {
            const QChar separator = suffix == QStringLiteral("csv") ? u',' : u'\t';
            if (addExportHeader_)
                out << headers.join(separator) << u'\n';
            for (const auto &record : rows) {
                QStringList row = values(record);
                if (separator == u',')
                    for (QString &value : row) value = quoted(value);
                out << row.join(separator) << u'\n';
            }
        }
        statusBar()->showMessage(tr("Exported %1 results to %2").arg(rows.size()).arg(path), 5000);
    }

private:
    std::optional<FileRecord> currentRecord() const
    {
        const QModelIndex current = table_->currentIndex();
        if (!current.isValid())
            return std::nullopt;
        const int row = proxy_->mapToSource(current).row();
        if (row < 0 || row >= model_->rows().size())
            return std::nullopt;
        return model_->rows().at(row);
    }

    QStringList actionChoices() const
    {
        return {tr("No Action"), tr("Open Properties Window"), tr("Open Selected File"),
                tr("Open With…"), tr("Open Folder"), tr("Select File In File Manager")};
    }

    void executeConfiguredAction(const QString &action)
    {
        if (action == tr("Open Properties Window"))
            showProperties();
        else if (action == tr("Open Selected File"))
            openSelected();
        else if (action == tr("Open With…"))
            openWith();
        else if (action == tr("Open Folder"))
            openFolder();
        else if (action == tr("Select File In File Manager"))
            selectInFileManager();
    }

    QString sizeUnitName() const
    {
        static const QStringList units{tr("Automatic"), tr("Bytes"), tr("KB"), tr("MB"), tr("GB")};
        return units.value(sizeUnit_, units.front());
    }

    int sizeUnitFromName(const QString &name) const
    {
        const QStringList units{tr("Automatic"), tr("Bytes"), tr("KB"), tr("MB"), tr("GB")};
        return std::max(0, int(units.indexOf(name)));
    }

    void applyDisplayOptions()
    {
        const int unit = options_.mode == QStringLiteral("Summary")
            ? sizeUnitFromName(summarySizeUnitName_) : sizeUnit_;
        model_->setDisplayOptions(unit, showGmt_, markDuplicates_, duplicateColorSet_);
    }

    void saveUiSettings()
    {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/sizeUnit"), sizeUnit_);
        settings.setValue(QStringLiteral("ui/summarySizeUnit"), summarySizeUnitName_);
        settings.setValue(QStringLiteral("ui/showGmt"), showGmt_);
        settings.setValue(QStringLiteral("ui/markDuplicates"), markDuplicates_);
        settings.setValue(QStringLiteral("ui/duplicateColorSet"), duplicateColorSet_);
        settings.setValue(QStringLiteral("ui/doubleClickAction"), doubleClickAction_);
        settings.setValue(QStringLiteral("ui/enterKeyAction"), enterKeyAction_);
        settings.setValue(QStringLiteral("ui/addExportHeader"), addExportHeader_);
        settings.setValue(QStringLiteral("ui/focusOnSearchStart"), focusOnSearchStart_);
        settings.setValue(QStringLiteral("ui/focusOnSearchEnd"), focusOnSearchEnd_);
        settings.setValue(QStringLiteral("ui/autoSizeOnSearchEnd"), autoSizeOnSearchEnd_);
        settings.setValue(QStringLiteral("ui/showGrid"), showGrid_);
        settings.setValue(QStringLiteral("ui/showTooltips"), showTooltips_);
        settings.setValue(QStringLiteral("ui/windowGeometry"), saveGeometry());
        if (table_)
            settings.setValue(QStringLiteral("ui/headerState"), table_->horizontalHeader()->saveState());
    }

    QStringList selectedPaths() const
    {
        QStringList paths;
        QSet<int> sourceRows;
        for (const QModelIndex &index : table_->selectionModel()->selectedRows())
            sourceRows.insert(proxy_->mapToSource(index).row());
        for (int row : sourceRows)
            paths.push_back(model_->data(model_->index(row, 0), Qt::UserRole).toString());
        return paths;
    }

    void saveOptions()
    {
        QSettings settings;
        settings.setValue(QStringLiteral("roots"), options_.roots);
        settings.setValue(QStringLiteral("fileWildcards"), options_.fileWildcards);
        settings.setValue(QStringLiteral("subfolderWildcards"), options_.subfolderWildcards);
        settings.setValue(QStringLiteral("excludeFiles"), options_.excludeFiles);
        settings.setValue(QStringLiteral("excludeFolders"), options_.excludeFolders);
        settings.setValue(QStringLiteral("includeFolders"), options_.includeFolders);
        settings.setValue(QStringLiteral("contains"), options_.contains);
        settings.setValue(QStringLiteral("binary"), options_.binary);
        settings.setValue(QStringLiteral("multipleValues"), options_.multipleValues);
        settings.setValue(QStringLiteral("multipleAnd"), options_.multipleAnd);
        settings.setValue(QStringLiteral("caseSensitive"), options_.caseSensitive);
        settings.setValue(QStringLiteral("minSize"), options_.minSize);
        settings.setValue(QStringLiteral("maxSize"), options_.maxSize);
        settings.setValue(QStringLiteral("timeField"), options_.timeField);
        settings.setValue(QStringLiteral("lastMinutes"), options_.lastMinutes);
        settings.setValue(QStringLiteral("recursive"), options_.recursive);
        settings.setValue(QStringLiteral("findFiles"), options_.findFiles);
        settings.setValue(QStringLiteral("findFolders"), options_.findFolders);
        settings.setValue(QStringLiteral("followLinks"), options_.followLinks);
        settings.setValue(QStringLiteral("retrieveOwner"), options_.retrieveOwner);
        settings.setValue(QStringLiteral("accurateProgress"), options_.accurateProgress);
        settings.setValue(QStringLiteral("useCache"), options_.useCache);
        settings.setValue(QStringLiteral("maxDepth"), options_.maxDepth);
        settings.setValue(QStringLiteral("maxResults"), options_.maxResults);
        settings.setValue(QStringLiteral("mode"), options_.mode);
        settings.setValue(QStringLiteral("duplicateNameMode"), options_.duplicateNameMode);
        settings.setValue(QStringLiteral("duplicateNameWithoutExtension"), options_.duplicateNameWithoutExtension);
        settings.setValue(QStringLiteral("showOnlyDuplicateFiles"), options_.showOnlyDuplicateFiles);
        settings.setValue(QStringLiteral("includeSubfoldersInSummary"), options_.includeSubfoldersInSummary);
        settings.setValue(QStringLiteral("hidden"), options_.hidden);
        settings.setValue(QStringLiteral("readonly"), options_.readonly);
        settings.setValue(QStringLiteral("executable"), options_.executable);
    }

    void loadOptions()
    {
        QSettings s;
        options_.roots = s.value(QStringLiteral("roots"), options_.roots).toString();
        options_.fileWildcards = s.value(QStringLiteral("fileWildcards"), options_.fileWildcards).toString();
        options_.subfolderWildcards = s.value(QStringLiteral("subfolderWildcards"), options_.subfolderWildcards).toString();
        options_.excludeFiles = s.value(QStringLiteral("excludeFiles")).toString();
        options_.excludeFolders = s.value(QStringLiteral("excludeFolders")).toString();
        options_.includeFolders = s.value(QStringLiteral("includeFolders")).toString();
        options_.contains = s.value(QStringLiteral("contains")).toString();
        options_.binary = s.value(QStringLiteral("binary"), false).toBool();
        options_.multipleValues = s.value(QStringLiteral("multipleValues"), false).toBool();
        options_.multipleAnd = s.value(QStringLiteral("multipleAnd"), false).toBool();
        options_.caseSensitive = s.value(QStringLiteral("caseSensitive"), false).toBool();
        options_.minSize = s.value(QStringLiteral("minSize"), 0).toLongLong();
        options_.maxSize = s.value(QStringLiteral("maxSize"), 0).toLongLong();
        options_.timeField = s.value(QStringLiteral("timeField"), options_.timeField).toString();
        options_.lastMinutes = s.value(QStringLiteral("lastMinutes"), 0).toInt();
        options_.recursive = s.value(QStringLiteral("recursive"), true).toBool();
        options_.findFiles = s.value(QStringLiteral("findFiles"), true).toBool();
        options_.findFolders = s.value(QStringLiteral("findFolders"), false).toBool();
        options_.followLinks = s.value(QStringLiteral("followLinks"), false).toBool();
        options_.retrieveOwner = s.value(QStringLiteral("retrieveOwner"), false).toBool();
        options_.accurateProgress = s.value(QStringLiteral("accurateProgress"), true).toBool();
        options_.useCache = s.value(QStringLiteral("useCache"), true).toBool();
        options_.maxDepth = s.value(QStringLiteral("maxDepth"), 0).toInt();
        options_.maxResults = s.value(QStringLiteral("maxResults"), 0).toInt();
        options_.mode = s.value(QStringLiteral("mode"), options_.mode).toString();
        options_.duplicateNameMode = s.value(QStringLiteral("duplicateNameMode"), options_.duplicateNameMode).toString();
        options_.duplicateNameWithoutExtension = s.value(QStringLiteral("duplicateNameWithoutExtension"), false).toBool();
        options_.showOnlyDuplicateFiles = s.value(QStringLiteral("showOnlyDuplicateFiles"), true).toBool();
        options_.includeSubfoldersInSummary = s.value(QStringLiteral("includeSubfoldersInSummary"), false).toBool();
        options_.hidden = s.value(QStringLiteral("hidden"), options_.hidden).toString();
        options_.readonly = s.value(QStringLiteral("readonly"), options_.readonly).toString();
        options_.executable = s.value(QStringLiteral("executable"), options_.executable).toString();
        sizeUnit_ = s.value(QStringLiteral("ui/sizeUnit"), 0).toInt();
        summarySizeUnitName_ = s.value(QStringLiteral("ui/summarySizeUnit"), tr("Automatic")).toString();
        showGmt_ = s.value(QStringLiteral("ui/showGmt"), false).toBool();
        markDuplicates_ = s.value(QStringLiteral("ui/markDuplicates"), true).toBool();
        duplicateColorSet_ = s.value(QStringLiteral("ui/duplicateColorSet"), 1).toInt();
        doubleClickAction_ = s.value(
            QStringLiteral("ui/doubleClickAction"), tr("Open Properties Window")).toString();
        enterKeyAction_ = s.value(
            QStringLiteral("ui/enterKeyAction"), tr("Open Selected File")).toString();
        addExportHeader_ = s.value(QStringLiteral("ui/addExportHeader"), true).toBool();
        focusOnSearchStart_ = s.value(QStringLiteral("ui/focusOnSearchStart"), true).toBool();
        focusOnSearchEnd_ = s.value(QStringLiteral("ui/focusOnSearchEnd"), true).toBool();
        autoSizeOnSearchEnd_ = s.value(QStringLiteral("ui/autoSizeOnSearchEnd"), false).toBool();
        showGrid_ = s.value(QStringLiteral("ui/showGrid"), true).toBool();
        showTooltips_ = s.value(QStringLiteral("ui/showTooltips"), true).toBool();
    }

    SearchOptions options_;
    ResultsModel *model_{};
    QSortFilterProxyModel *proxy_{};
    QTableView *table_{};
    QAction *searchAction_{};
    QAction *stopAction_{};
    QAction *refreshAction_{};
    QProgressBar *progress_{};
    QThread workerThread_;
    SearchEngine *engine_{};
    QString findQuery_;
    QString summarySizeUnitName_ = QStringLiteral("Automatic");
    QString doubleClickAction_ = QStringLiteral("Open Properties Window");
    QString enterKeyAction_ = QStringLiteral("Open Selected File");
    int sizeUnit_ = 0;
    int duplicateColorSet_ = 1;
    bool showGmt_ = false;
    bool markDuplicates_ = true;
    bool addExportHeader_ = true;
    bool focusOnSearchStart_ = true;
    bool focusOnSearchEnd_ = true;
    bool autoSizeOnSearchEnd_ = false;
    bool showGrid_ = true;
    bool showTooltips_ = true;
    bool isSearching_ = false;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SearchMyFilesLinux"));
    QCoreApplication::setApplicationName(QStringLiteral("SearchMyFiles"));
    QApplication::setStyle(QStringLiteral("Fusion"));
    qRegisterMetaType<SearchOptions>();
    qRegisterMetaType<QVector<FileRecord>>();
    MainWindow window;
    window.show();
    QTimer::singleShot(0, &window, [&window] {
        QMetaObject::invokeMethod(&window, "showOptions", Qt::QueuedConnection);
    });
    return app.exec();
}

#include "main.moc"
