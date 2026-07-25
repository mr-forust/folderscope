#include <QAbstractTableModel>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
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
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStatusBar>
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
    int maxDepth = 0;
    int maxResults = 0;
    QString mode = QStringLiteral("Standard search");
    QString duplicateNameMode = QStringLiteral("All files and folders");
    bool duplicateNameWithoutExtension = false;
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

class SearchEngine final : public QObject {
    Q_OBJECT
public:
    explicit SearchEngine(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    void stop() { cancelled_.store(true, std::memory_order_relaxed); }

    void search(const SearchOptions &o)
    {
        cancelled_.store(false, std::memory_order_relaxed);
        cancelledByLimit_ = false;
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
                    const bool matched = fileContains(r.path, o);
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
                    auto result = std::pair<QByteArray, FileRecord>{sha256(r.path), r};
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
                    const QByteArray firstHash = sha256(sameName.front().path);
                    bool identical = !firstHash.isEmpty();
                    for (qsizetype i = 1; identical && i < sameName.size(); ++i) {
                        identical = sha256(sameName[i].path) == firstHash
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
            for (const auto &record : candidates)
                byFolder[record.folder].push_back(record);
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
        emit finished(results, tr("%1 results, %2 entries scanned in %3 seconds")
                                  .arg(results.size()).arg(scanned).arg(seconds, 0, 'f', 2));
    }

signals:
    void progress(const QString &path, quint64 scanned);
    void phaseProgress(const QString &phase, quint64 completed, quint64 total);
    void finished(const QVector<FileRecord> &results, const QString &message);

private:
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
        table_->horizontalHeader()->setSectionResizeMode(ResultsModel::Name, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(ResultsModel::Folder, QHeaderView::Stretch);
        setCentralWidget(table_);

        auto *toolbar = addToolBar(tr("Main"));
        searchAction_ = toolbar->addAction(QIcon::fromTheme(QStringLiteral("system-search")), tr("Search options…"));
        searchAction_->setShortcut(Qt::Key_F9);
        stopAction_ = toolbar->addAction(QIcon::fromTheme(QStringLiteral("process-stop")), tr("Stop"));
        stopAction_->setShortcut(Qt::Key_Escape);
        stopAction_->setEnabled(false);
        refreshAction_ = toolbar->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), tr("Refresh"));
        refreshAction_->setShortcut(Qt::Key_F5);
        toolbar->addSeparator();
        auto *exportAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-save")), tr("Export…"));
        exportAction->setShortcut(QKeySequence::Save);
        auto *openAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("Open"));
        openAction->setShortcut(Qt::Key_Return);
        auto *folderAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Open folder"));

        auto *fileMenu = menuBar()->addMenu(tr("&File"));
        fileMenu->addAction(searchAction_);
        fileMenu->addAction(stopAction_);
        fileMenu->addAction(refreshAction_);
        fileMenu->addSeparator();
        fileMenu->addAction(openAction);
        fileMenu->addAction(folderAction);
        fileMenu->addSeparator();
        fileMenu->addAction(exportAction);
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
        editMenu->addSeparator();
        editMenu->addAction(tr("Select all"), QKeySequence::SelectAll, table_, &QTableView::selectAll);
        auto *deselectAction = editMenu->addAction(tr("Deselect all"));
        deselectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));

        auto *viewMenu = menuBar()->addMenu(tr("&View"));
        auto *gridAction = viewMenu->addAction(tr("Show grid lines"));
        gridAction->setCheckable(true);
        gridAction->setChecked(true);
        auto *tooltipsAction = viewMenu->addAction(tr("Show tooltips"));
        tooltipsAction->setCheckable(true);
        tooltipsAction->setChecked(true);
        viewMenu->addSeparator();
        viewMenu->addAction(tr("Auto size columns"), this, [this] {
            table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
            table_->resizeColumnsToContents();
        });
        auto *columnsMenu = viewMenu->addMenu(tr("Choose columns"));
        for (int column = 0; column < ResultsModel::ColumnCount; ++column) {
            auto *action = columnsMenu->addAction(
                model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
            action->setCheckable(true);
            action->setChecked(true);
            connect(action, &QAction::toggled, this, [this, column](bool visible) {
                table_->setColumnHidden(column, !visible);
            });
        }

        menuBar()->addMenu(tr("&Help"))->addAction(tr("About"), this, [this] {
            QMessageBox::about(this, tr("About"),
                tr("SearchMyFiles for Linux\n\nFast unindexed file search.\nC++20 + Qt 6."));
        });

        progress_ = new QProgressBar;
        progress_->setRange(0, 0);
        progress_->setMinimumWidth(280);
        progress_->setMaximumWidth(420);
        progress_->setTextVisible(true);
        progress_->hide();
        statusBar()->addPermanentWidget(progress_);
        statusBar()->showMessage(tr("Press F9 to configure and start a search"));

        engine_ = new SearchEngine;
        engine_->moveToThread(&workerThread_);
        connect(&workerThread_, &QThread::finished, engine_, &QObject::deleteLater);
        connect(this, &MainWindow::startRequested, engine_, &SearchEngine::search);
        connect(this, &MainWindow::stopRequested, engine_, &SearchEngine::stop, Qt::DirectConnection);
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
        connect(findAction, &QAction::triggered, this, &MainWindow::findText);
        connect(findNextAction, &QAction::triggered, this, &MainWindow::findNext);
        connect(copyInfoAction, &QAction::triggered, this, &MainWindow::copyInformation);
        connect(copyPathsAction, &QAction::triggered, this, &MainWindow::copyPaths);
        connect(explorerCopyAction, &QAction::triggered, this, &MainWindow::explorerCopy);
        connect(deselectAction, &QAction::triggered, table_, &QTableView::clearSelection);
        connect(gridAction, &QAction::toggled, table_, &QTableView::setShowGrid);
        connect(tooltipsAction, &QAction::toggled, this, [this](bool enabled) {
            table_->viewport()->setAttribute(Qt::WA_AlwaysShowToolTips, enabled);
        });
        connect(table_, &QTableView::doubleClicked, this, &MainWindow::openSelected);
        connect(table_, &QWidget::customContextMenuRequested, this, &MainWindow::contextMenu);
    }

    ~MainWindow() override
    {
        emit stopRequested();
        workerThread_.quit();
        workerThread_.wait();
    }

signals:
    void startRequested(const SearchOptions &options);
    void stopRequested();

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

    void startCurrentSearch()
    {
        if (isSearching_ || options_.roots.trimmed().isEmpty())
            return;
        isSearching_ = true;
        model_->setRows({});
        searchAction_->setEnabled(false);
        stopAction_->setEnabled(true);
        refreshAction_->setEnabled(false);
        progress_->show();
        progress_->setRange(0, 0);
        progress_->setFormat(tr("Scanning…"));
        setWindowTitle(tr("%1 — %2 — SearchMyFiles for Linux")
                           .arg(options_.roots, options_.mode));
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

    void copyPaths()
    {
        QApplication::clipboard()->setText(selectedPaths().join(u'\n'));
    }

    void contextMenu(const QPoint &point)
    {
        QMenu menu(this);
        menu.addAction(tr("Open"), this, &MainWindow::openSelected);
        menu.addAction(tr("Open containing folder"), this, &MainWindow::openFolder);
        menu.addSeparator();
        menu.addAction(tr("Copy paths"), this, &MainWindow::copyPaths);
        menu.addAction(tr("Export selected…"), this, &MainWindow::exportResults);
        menu.addSeparator();
        menu.addAction(tr("Refresh"), QKeySequence(Qt::Key_F5), this, &MainWindow::refreshSearch);
        menu.exec(table_->viewport()->mapToGlobal(point));
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
            return QStringList{r.name, r.folder, QString::number(r.size), QString::number(r.sizeOnDisk), date(r.modified),
                               date(r.accessed), date(r.changed), r.type, r.owner, r.attributes,
                               r.group ? QString::number(r.group) : QString{}, r.path};
        };
        const QStringList headers{tr("Name"), tr("Folder"), tr("Size"), tr("Size On Disk"), tr("Modified Time"),
                                  tr("Accessed Time"), tr("Changed Time"), tr("Type"),
                                  tr("Owner"), tr("Attributes"), tr("Duplicate Number"), tr("Full Path")};
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
        settings.setValue(QStringLiteral("maxDepth"), options_.maxDepth);
        settings.setValue(QStringLiteral("maxResults"), options_.maxResults);
        settings.setValue(QStringLiteral("mode"), options_.mode);
        settings.setValue(QStringLiteral("duplicateNameMode"), options_.duplicateNameMode);
        settings.setValue(QStringLiteral("duplicateNameWithoutExtension"), options_.duplicateNameWithoutExtension);
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
        options_.maxDepth = s.value(QStringLiteral("maxDepth"), 0).toInt();
        options_.maxResults = s.value(QStringLiteral("maxResults"), 0).toInt();
        options_.mode = s.value(QStringLiteral("mode"), options_.mode).toString();
        options_.duplicateNameMode = s.value(QStringLiteral("duplicateNameMode"), options_.duplicateNameMode).toString();
        options_.duplicateNameWithoutExtension = s.value(QStringLiteral("duplicateNameWithoutExtension"), false).toBool();
        options_.hidden = s.value(QStringLiteral("hidden"), options_.hidden).toString();
        options_.readonly = s.value(QStringLiteral("readonly"), options_.readonly).toString();
        options_.executable = s.value(QStringLiteral("executable"), options_.executable).toString();
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
