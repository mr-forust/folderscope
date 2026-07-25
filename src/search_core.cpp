#include "search_core.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <pwd.h>

QStringList patterns(const QString &text)
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

QStringList exclusionPatterns(const QString &text)
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

bool wildcardMatch(const QString &value, const QStringList &items, Qt::CaseSensitivity cs)
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

QString humanSize(qint64 bytes)
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

QString ownerName(uid_t uid)
{
    struct passwd pwd {};
    struct passwd *result = nullptr;
    QByteArray buffer(16384, Qt::Uninitialized);
    if (getpwuid_r(uid, &pwd, buffer.data(), static_cast<size_t>(buffer.size()), &result) == 0 && result)
        return QString::fromLocal8Bit(result->pw_name);
    return QString::number(uid);
}

std::optional<QByteArray> hexNeedle(QString text)
{
    text.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (text.size() % 2 != 0 || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]*$")).match(text).hasMatch())
        return std::nullopt;
    return QByteArray::fromHex(text.toLatin1());
}

bool fileContains(const QString &path, const SearchOptions &options)
{
    if (options.contains.isEmpty())
        return true;

    const QStringList terms = options.multipleValues
        ? patterns(options.contains) : QStringList{options.contains};
    QList<QByteArray> needles;
    for (const QString &term : terms) {
        if (options.binary) {
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
        if (!options.caseSensitive && !options.binary)
            data = data.toLower();
        for (qsizetype i = 0; i < needles.size(); ++i) {
            QByteArray needle = needles[i];
            if (!options.caseSensitive && !options.binary)
                needle = needle.toLower();
            if (!found[i] && data.contains(needle))
                found[i] = true;
        }
        const bool matched = options.multipleAnd
            ? std::all_of(found.cbegin(), found.cend(), [](bool foundTerm) { return foundTerm; })
            : std::any_of(found.cbegin(), found.cend(), [](bool foundTerm) { return foundTerm; });
        if (matched)
            return true;
        tail = data.right(overlap - 1);
    }
    return false;
}

bool filesEqual(const QString &leftPath, const QString &rightPath)
{
    QFile left(leftPath);
    QFile right(rightPath);
    if (!left.open(QIODevice::ReadOnly) || !right.open(QIODevice::ReadOnly)
        || left.size() != right.size())
        return false;
    while (!left.atEnd()) {
        const QByteArray leftData = left.read(1024 * 1024);
        const QByteArray rightData = right.read(leftData.size());
        if (leftData != rightData)
            return false;
    }
    return right.atEnd();
}

QByteArray sha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
        hash.addData(file.read(1024 * 1024));
    return hash.result();
}

bool parseOptionalIsoDate(const QString &text, qint64 &seconds)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        seconds = 0;
        return true;
    }
    const QDateTime parsed = QDateTime::fromString(trimmed, Qt::ISODate);
    if (!parsed.isValid())
        return false;
    seconds = parsed.toSecsSinceEpoch();
    return true;
}
