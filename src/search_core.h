#pragma once

#include <QByteArray>
#include <QDir>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>
#include <sys/types.h>

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
    bool showDuplicateCopiesOnly = true;
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

QStringList patterns(const QString &text);
QStringList exclusionPatterns(const QString &text);
bool wildcardMatch(const QString &value, const QStringList &items, Qt::CaseSensitivity cs);
QString humanSize(qint64 bytes);
QString ownerName(uid_t uid);
std::optional<QByteArray> hexNeedle(QString text);
bool fileContains(const QString &path, const SearchOptions &options);
bool filesEqual(const QString &leftPath, const QString &rightPath);
QByteArray sha256(const QString &path);
bool parseOptionalIsoDate(const QString &text, qint64 &seconds);
bool shouldShowDuplicateResult(bool copiesOnly, int keeperPriority);
