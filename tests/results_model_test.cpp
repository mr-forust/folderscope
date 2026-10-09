#include "results_model.h"
#include <QtTest>

class ResultsModelTest final : public QObject {
    Q_OBJECT
private slots:
    void explainsCopyStatusWithoutChangingSortOrder()
    {
        ResultsModel model;
        QVector<FileRecord> rows(3);
        rows[0].duplicateCopy = 1;
        rows[1].duplicateCopy = 3;
        model.setRows(rows);
        const auto column = ResultsModel::DuplicateGroup;
        QCOMPARE(model.headerData(column, Qt::Horizontal, Qt::DisplayRole).toString(), QString("Copy status"));
        QCOMPARE(model.data(model.index(0, column), Qt::DisplayRole).toString(), QString("Keep this copy"));
        QCOMPARE(model.data(model.index(1, column), Qt::DisplayRole).toString(), QString("Duplicate №2"));
        QCOMPARE(model.data(model.index(1, column), Qt::EditRole).toInt(), 3);
        QVERIFY(model.data(model.index(2, column), Qt::DisplayRole).toString().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ResultsModelTest)
#include "results_model_test.moc"
