#pragma once

#include <QObject>
#include <QVector>
#include <QString>

#include <atomic>
#include <chrono>

#include "search_core.h"
#include "search_cache.h"

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

    void search(const SearchOptions &o);

signals:
    void progress(const QString &path, quint64 scanned);
    void phaseProgress(const QString &phase, quint64 completed, quint64 total);
    void cacheCleared();
    void finished(const QVector<FileRecord> &results, const QString &message);

private:
    QByteArray cachedSha256(const FileRecord &record);
    bool cachedFileContains(const FileRecord &record, const SearchOptions &o);

    SearchCache cache_;
    std::atomic<quint64> cacheHits_ = 0;
    std::atomic<quint64> cacheMisses_ = 0;
    std::atomic_bool cancelled_ = false;
    bool cancelledByLimit_ = false;
};
