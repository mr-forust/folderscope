#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct FileRecord;
class QIODevice;

QString xml10SafeText(QString value);
bool writeResults(QIODevice &device, const QVector<FileRecord> &rows,
                  const QStringList &headers, const QString &suffix,
                  bool addHeader, QString *errorMessage = nullptr);
