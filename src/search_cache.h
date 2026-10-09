#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>

#include "search_core.h"

struct SearchCacheEntry {
    qint64 size = -1;
    qint64 modifiedNs = -1;
    qint64 changedNs = -1;
    quint64 device = 0;
    quint64 inode = 0;
    qint64 lastUsed = 0;
    QByteArray sampleSha256;
    QByteArray sha256;
    QHash<QByteArray, bool> contentMatches;
};

class SearchCache {
public:
    explicit SearchCache(QString path = {});

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    bool findSampleHash(const FileRecord &record, QByteArray &digest);
    void storeSampleHash(const FileRecord &record, const QByteArray &digest);
    bool findHash(const FileRecord &record, QByteArray &digest);
    void storeHash(const FileRecord &record, const QByteArray &digest);
    std::optional<bool> findContent(const FileRecord &record, const QByteArray &queryKey);
    void storeContent(const FileRecord &record, const QByteArray &queryKey, bool matched);
    void save();
    void clear();

private:
    static bool signatureMatches(const SearchCacheEntry &entry, const FileRecord &record);
    SearchCacheEntry &currentEntry(const FileRecord &record);
    void load();
    void prune();

    QHash<QString, SearchCacheEntry> entries_;
    QMutex mutex_;
    QString path_;
    bool enabled_ = true;
};
