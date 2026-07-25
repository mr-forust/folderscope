#include "search_cache.h"

#include <QDateTime>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

SearchCache::SearchCache()
{
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheRoot);
    path_ = QDir(cacheRoot).filePath(QStringLiteral("search-cache-v1.dat"));
    load();
}

bool SearchCache::findHash(const FileRecord &record, QByteArray &digest)
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

void SearchCache::storeHash(const FileRecord &record, const QByteArray &digest)
{
    if (!enabled_ || digest.isEmpty())
        return;
    QMutexLocker lock(&mutex_);
    SearchCacheEntry &entry = currentEntry(record);
    entry.sha256 = digest;
}

std::optional<bool> SearchCache::findContent(const FileRecord &record, const QByteArray &queryKey)
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

void SearchCache::storeContent(const FileRecord &record, const QByteArray &queryKey, bool matched)
{
    if (!enabled_)
        return;
    QMutexLocker lock(&mutex_);
    SearchCacheEntry &entry = currentEntry(record);
    entry.contentMatches.insert(queryKey, matched);
}

void SearchCache::save()
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

void SearchCache::clear()
{
    QMutexLocker lock(&mutex_);
    entries_.clear();
    QFile::remove(path_);
}

bool SearchCache::signatureMatches(const SearchCacheEntry &entry, const FileRecord &record)
{
    return entry.size == record.size && entry.modified == record.modified;
}

SearchCacheEntry &SearchCache::currentEntry(const FileRecord &record)
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

void SearchCache::load()
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

void SearchCache::prune()
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
