#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>

#include "search_core.h"

struct SearchCacheEntry {
    qint64 size = -1;
    qint64 modified = -1;
    qint64 lastUsed = 0;
    QByteArray sha256;
    QHash<QByteArray, bool> contentMatches;
};

class SearchCache {
public:
    SearchCache();

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

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
