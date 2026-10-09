#include "results_export.h"

#include "search_core.h"
#include "tabular_text.h"

#include <QDateTime>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QXmlStreamWriter>

#include <array>

namespace {
QStringList values(const FileRecord &record)
{
    const auto date = [](qint64 time) {
        return time ? QDateTime::fromSecsSinceEpoch(time).toString(Qt::ISODate) : QString{};
    };
    return {
        record.name, record.folder, QString::number(record.size),
        QString::number(record.sizeOnDisk), date(record.modified), date(record.created),
        date(record.accessed), date(record.changed), record.attributes,
        QFileInfo(record.name).suffix(),
        record.group ? QString::number(record.group) : QString{},
        record.duplicateCopy ? QString::number(record.duplicateCopy) : QString{},
        record.type, record.owner, record.path
    };
}

void setError(QString *errorMessage, const QIODevice &device)
{
    if (!errorMessage)
        return;
    const QString detail = device.errorString();
    *errorMessage = detail.isEmpty() || detail == QStringLiteral("Unknown error")
        ? QStringLiteral("Could not write exported data") : detail;
}

bool writeAll(QIODevice &device, const QByteArray &bytes)
{
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = device.write(bytes.constData() + offset,
                                            bytes.size() - offset);
        if (written <= 0)
            return false;
        offset += written;
    }
    return true;
}

QStringList const &xmlFieldNames()
{
    static const QStringList names{
        QStringLiteral("Filename"), QStringLiteral("Folder"), QStringLiteral("Size"),
        QStringLiteral("SizeOnDisk"), QStringLiteral("ModifiedTime"),
        QStringLiteral("CreatedTime"), QStringLiteral("LastAccessedTime"),
        QStringLiteral("EntryModifiedTime"), QStringLiteral("Attributes"),
        QStringLiteral("Extension"), QStringLiteral("DuplicateSet"),
        QStringLiteral("KeeperPriority"), QStringLiteral("Type"),
        QStringLiteral("FileOwner"), QStringLiteral("FullPath")
    };
    return names;
}
}

QString xml10SafeText(QString value)
{
    QString safe;
    safe.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar ch = value.at(i);
        uint codePoint = ch.unicode();
        if (ch.isHighSurrogate()) {
            if (i + 1 < value.size() && value.at(i + 1).isLowSurrogate()) {
                codePoint = QChar::surrogateToUcs4(ch, value.at(++i));
                safe += QChar::highSurrogate(codePoint);
                safe += QChar::lowSurrogate(codePoint);
                continue;
            }
            safe += QChar(0xfffd);
            continue;
        }
        if (ch.isLowSurrogate()) {
            safe += QChar(0xfffd);
            continue;
        }
        const bool allowed = codePoint == 0x9 || codePoint == 0xa || codePoint == 0xd
            || (codePoint >= 0x20 && codePoint <= 0xd7ff)
            || (codePoint >= 0xe000 && codePoint <= 0xfffd);
        safe += allowed ? ch : QChar(0xfffd);
    }
    return safe;
}

bool writeResults(QIODevice &device, const QVector<FileRecord> &rows,
                  const QStringList &headers, const QString &suffix,
                  bool addHeader, QString *errorMessage)
{
    if (!device.isOpen() || !device.isWritable()) {
        setError(errorMessage, device);
        return false;
    }

    const QString format = suffix.toLower();
    if (format == QStringLiteral("json")) {
        QJsonArray array;
        for (const FileRecord &record : rows) {
            const QStringList row = values(record);
            QJsonObject object;
            for (qsizetype i = 0; i < headers.size() && i < row.size(); ++i)
                object.insert(headers.at(i), row.at(i));
            array.append(object);
        }
        if (headers.size() != values(FileRecord{}).size()
            || !writeAll(device, QJsonDocument(array).toJson())) {
            setError(errorMessage, device);
            return false;
        }
        return true;
    }

    if (format == QStringLiteral("xml")) {
        if (headers.size() != xmlFieldNames().size()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unexpected XML field count");
            return false;
        }
        QXmlStreamWriter writer(&device);
        writer.writeStartDocument();
        writer.writeStartElement(QStringLiteral("SearchResults"));
        for (const FileRecord &record : rows) {
            writer.writeStartElement(QStringLiteral("File"));
            const QStringList row = values(record);
            for (qsizetype i = 0; i < row.size(); ++i) {
                writer.writeTextElement(xmlFieldNames().at(i), xml10SafeText(row.at(i)));
            }
            writer.writeEndElement();
        }
        writer.writeEndElement();
        writer.writeEndDocument();
        if (writer.hasError()) {
            setError(errorMessage, device);
            return false;
        }
        return true;
    }

    QTextStream out(&device);
    if (format == QStringLiteral("html")) {
        out << "<!doctype html><meta charset=\"utf-8\"><table><thead><tr>";
        for (const QString &header : headers)
            out << "<th>" << header.toHtmlEscaped() << "</th>";
        out << "</tr></thead><tbody>";
        for (const FileRecord &record : rows) {
            out << "<tr>";
            for (const QString &value : values(record))
                out << "<td>" << value.toHtmlEscaped() << "</td>";
            out << "</tr>";
        }
        out << "</tbody></table>";
    } else {
        const QChar separator = format == QStringLiteral("csv") ? u',' : u'\t';
        if (addHeader)
            out << headers.join(separator) << u'\n';
        for (const FileRecord &record : rows) {
            QStringList row = values(record);
            for (QString &value : row)
                value = spreadsheetSafeCell(value);
            if (separator == u',') {
                for (QString &value : row) {
                    value.replace(u'"', QStringLiteral("\"\""));
                    value = u'"' + value + u'"';
                }
            }
            out << row.join(separator) << u'\n';
        }
    }
    out.flush();
    if (out.status() != QTextStream::Ok) {
        setError(errorMessage, device);
        return false;
    }
    return true;
}
