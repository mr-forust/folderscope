#include "search_cache.h"

#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class SearchCacheTest final : public QObject {
    Q_OBJECT

private:
    static FileRecord record()
    {
        FileRecord value;
        value.path = QStringLiteral("/tmp/cache-test.bin");
        value.size = 1234;
        value.modifiedNs = 5678;
        value.device = 9;
        value.inode = 10;
        return value;
    }

private slots:
    void storesAndReloadsBothHashes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("cache.dat"));
        const FileRecord value = record();
        const QByteArray sample("sample");
        const QByteArray full("full");
        const QByteArray query("query");

        {
            SearchCache cache(path);
            cache.storeSampleHash(value, sample);
            cache.storeHash(value, full);
            cache.storeContent(value, query, true);
            cache.save();
        }

        SearchCache cache(path);
        QByteArray digest;
        QVERIFY(cache.findSampleHash(value, digest));
        QCOMPARE(digest, sample);
        QVERIFY(cache.findHash(value, digest));
        QCOMPARE(digest, full);
        QCOMPARE(cache.findContent(value, query), std::optional<bool>(true));
    }

    void invalidatesEverySignatureField()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        SearchCache cache(directory.filePath(QStringLiteral("cache.dat")));
        const FileRecord value = record();
        cache.storeSampleHash(value, QByteArray("sample"));
        cache.storeHash(value, QByteArray("full"));

        const auto misses = [&cache](FileRecord changed) {
            QByteArray digest;
            return !cache.findSampleHash(changed, digest)
                && !cache.findHash(changed, digest);
        };

        FileRecord changed = value;
        ++changed.size;
        QVERIFY(misses(changed));
        changed = value;
        ++changed.modifiedNs;
        QVERIFY(misses(changed));
        changed = value;
        ++changed.device;
        QVERIFY(misses(changed));
        changed = value;
        ++changed.inode;
        QVERIFY(misses(changed));
    }

    void ignoresVersionOneData()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("cache.dat"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << quint32(0x46534348) << quint32(1) << quint32(0);
        file.close();

        SearchCache cache(path);
        QByteArray digest;
        QVERIFY(!cache.findHash(record(), digest));
        cache.storeHash(record(), QByteArray("full"));
        cache.save();

        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QDataStream savedStream(&saved);
        savedStream.setVersion(QDataStream::Qt_6_0);
        quint32 magic = 0;
        quint32 version = 0;
        savedStream >> magic >> version;
        QCOMPARE(magic, quint32(0x46534348));
        QCOMPARE(version, quint32(2));
    }
};

QTEST_GUILESS_MAIN(SearchCacheTest)

#include "search_cache_test.moc"
