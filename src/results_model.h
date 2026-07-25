#pragma once

#include <QAbstractTableModel>
#include <QColor>
#include <QStringList>
#include <QVariant>

#include "search_core.h"

class ResultsModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Folder, Size, SizeOnDisk, Modified, Created, Accessed, Changed,
                  Attributes, Extension, DuplicateNumber, DuplicateGroup, Type, Owner,
                  FullPath, ColumnCount };

    explicit ResultsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    void setRows(QVector<FileRecord> rows);
    const QVector<FileRecord> &rows() const { return rows_; }

    void setDisplayOptions(int sizeUnit, bool showGmt, bool markDuplicates, int colorSet);

private:
    QString formattedSize(qint64 bytes) const;

    QVector<FileRecord> rows_;
    int sizeUnit_ = 0;
    bool showGmt_ = false;
    bool markDuplicates_ = true;
    int duplicateColorSet_ = 1;
};
