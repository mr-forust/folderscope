#include "results_model.h"

#include <QDateTime>
#include <QFileInfo>

ResultsModel::ResultsModel(QObject *parent) : QAbstractTableModel(parent) {}

int ResultsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

int ResultsModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ResultsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::ToolTipRole) {
        if (section == DuplicateNumber)
            return tr("Identifies one set of identical files");
        if (section == DuplicateGroup)
            return tr("Most-specific base folder wins; ties use newer files, then path. 1 = preferred copy to keep");
        return {};
    }
    if (role != Qt::DisplayRole)
        return {};
    static const QStringList names{
        tr("Filename"), tr("Folder"), tr("Size"), tr("Size On Disk"), tr("Modified Time"),
        tr("Created Time"), tr("Last Accessed Time"), tr("Entry Modified Time"),
        tr("Attributes"), tr("Extension"), tr("Duplicate Set"), tr("Keeper Priority"),
        tr("Type"), tr("File Owner"), tr("Full Path")
    };
    return names.value(section);
}

QVariant ResultsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rows_.size())
        return {};
    const FileRecord &r = rows_[index.row()];
    if (role == Qt::UserRole)
        return r.path;
    if (role == Qt::BackgroundRole && r.group && markDuplicates_) {
        const int colorIndex = (r.group - 1) % 64;
        return duplicateColorSet_ == 1
            ? QColor::fromHsv((colorIndex * 137) % 360, 38 + (colorIndex % 3) * 8, 255)
            : QColor::fromHsv((colorIndex * 137) % 360, 85, 205);
    }
    if (role == Qt::ForegroundRole && r.group && markDuplicates_)
        return duplicateColorSet_ == 1 ? QColor(Qt::black) : QColor(Qt::white);
    if (role == Qt::TextAlignmentRole && index.column() == Size)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::EditRole) {
        if (index.column() == Size) return r.size;
        if (index.column() == SizeOnDisk) return r.sizeOnDisk;
        if (index.column() == Modified) return r.modified;
        if (index.column() == Created) return r.created;
        if (index.column() == Accessed) return r.accessed;
        if (index.column() == Changed) return r.changed;
        if (index.column() == DuplicateNumber) return r.group;
        if (index.column() == DuplicateGroup) return r.duplicateCopy;
        return data(index, Qt::DisplayRole);
    }
    if (role != Qt::DisplayRole)
        return {};
    const auto time = [this](qint64 value) {
        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(value);
        if (showGmt_)
            dateTime = dateTime.toUTC();
        return value ? dateTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                     : QString{};
    };
    switch (index.column()) {
    case Name: return r.name;
    case Folder: return r.folder;
    case Size: return formattedSize(r.size);
    case SizeOnDisk: return formattedSize(r.sizeOnDisk);
    case Modified: return time(r.modified);
    case Created: return time(r.created);
    case Accessed: return time(r.accessed);
    case Changed: return time(r.changed);
    case Attributes: return r.attributes;
    case Extension: return QFileInfo(r.name).suffix();
    case DuplicateNumber: return r.group ? QVariant(r.group) : QVariant{};
    case DuplicateGroup: return r.duplicateCopy ? QVariant(r.duplicateCopy) : QVariant{};
    case Type: return r.type;
    case Owner: return r.owner;
    case FullPath: return r.path;
    default: return {};
    }
}

void ResultsModel::setRows(QVector<FileRecord> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

void ResultsModel::setDisplayOptions(int sizeUnit, bool showGmt, bool markDuplicates, int colorSet)
{
    sizeUnit_ = sizeUnit;
    showGmt_ = showGmt;
    markDuplicates_ = markDuplicates;
    duplicateColorSet_ = colorSet;
    if (!rows_.isEmpty())
        emit dataChanged(index(0, 0), index(rows_.size() - 1, ColumnCount - 1));
}

QString ResultsModel::formattedSize(qint64 bytes) const
{
    if (sizeUnit_ == 0)
        return humanSize(bytes);
    static const double divisors[]{1.0, 1024.0, 1024.0 * 1024.0, 1024.0 * 1024.0 * 1024.0};
    static const char *units[]{"Bytes", "KB", "MB", "GB"};
    const int index = std::clamp(sizeUnit_ - 1, 0, 3);
    return index == 0
        ? QStringLiteral("%1 Bytes").arg(bytes)
        : QStringLiteral("%1 %2").arg(double(bytes) / divisors[index], 0, 'f', 2)
              .arg(QString::fromLatin1(units[index]));
}
