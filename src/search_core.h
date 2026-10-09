#pragma once

#include <QByteArray>
#include <QDir>
#include <QMetaType>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <optional>
#include <sys/types.h>

inline constexpr qint64 duplicateSampleBlockSize = 64LL * 1024;
inline constexpr qint64 duplicateSampleThreshold = 4LL * 1024 * 1024;

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
    bool strictDuplicateComparison = false;
    bool showDuplicateCopiesOnly = true;
    bool includeSubfoldersInSummary = false;
    QString hidden = QStringLiteral("Any");
    QString readonly = QStringLiteral("Any");
    QString executable = QStringLiteral("Any");

    bool operator==(const SearchOptions &) const = default;
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
    qint64 modifiedNs = 0;
    qint64 changedNs = 0;
    quint64 device = 0;
    quint64 inode = 0;
    QString type;
    QString owner;
    QString attributes;
    int group = 0;
    int duplicateCopy = 0;
};

struct EffectiveRoots {
    QStringList paths;
    QStringList priorityPaths;
    int skipped = 0;
};

enum class FileContentResult {
    Match,
    NoMatch,
    Error,
};

enum class FileCompareResult {
    Equal,
    Different,
    Error,
};

class CompiledWildcardMatcher {
public:
    explicit CompiledWildcardMatcher(const QStringList &items = {},
                                     Qt::CaseSensitivity cs = Qt::CaseInsensitive);
    bool matches(const QString &value) const;

private:
    QVector<QRegularExpression> expressions_;
};

Q_DECLARE_METATYPE(SearchOptions)
Q_DECLARE_METATYPE(FileRecord)
Q_DECLARE_METATYPE(QVector<FileRecord>)

QStringList patterns(const QString &text);
QStringList exclusionPatterns(const QString &text);
bool wildcardMatch(const QString &value, const QStringList &items, Qt::CaseSensitivity cs);
EffectiveRoots effectiveSearchRoots(const SearchOptions &options);
int preferredRootIndex(const QString &path, const QStringList &roots);
QString humanSize(qint64 bytes);
QString ownerName(uid_t uid);
std::optional<QByteArray> hexNeedle(QString text);
FileContentResult fileContentResult(const QString &path, const SearchOptions &options,
                                    const std::atomic_bool *cancelled = nullptr);
FileContentResult fileContentResult(const FileRecord &record, const SearchOptions &options,
                                    const std::atomic_bool *cancelled = nullptr);
bool fileContains(const QString &path, const SearchOptions &options,
                  const std::atomic_bool *cancelled = nullptr);
bool fileContains(const FileRecord &record, const SearchOptions &options,
                  const std::atomic_bool *cancelled = nullptr);
bool filesEqual(const QString &leftPath, const QString &rightPath,
                const std::atomic_bool *cancelled = nullptr);
bool filesEqual(const FileRecord &left, const FileRecord &right,
                const std::atomic_bool *cancelled = nullptr);
FileCompareResult compareFiles(const FileRecord &left, const FileRecord &right,
                               const std::atomic_bool *cancelled = nullptr);
QByteArray sha256(const QString &path, const std::atomic_bool *cancelled = nullptr);
QByteArray sha256(const FileRecord &record, const std::atomic_bool *cancelled = nullptr);
QByteArray sampleSha256(const FileRecord &record,
                        const std::atomic_bool *cancelled = nullptr);
bool fileSignatureMatches(const FileRecord &record);
bool removePathWithoutFollowingSymlink(const QString &path);
bool parseOptionalIsoDate(const QString &text, qint64 &seconds);
bool shouldShowDuplicateResult(bool copiesOnly, int keeperPriority);
