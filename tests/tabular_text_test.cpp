#include "tabular_text.h"

#include <QtTest>

class TabularTextTest final : public QObject {
    Q_OBJECT

private slots:
    void prefixesFormulaLikeCells()
    {
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("=1+1")), QStringLiteral("'=1+1"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("  +SUM(A1:A2)")),
                 QStringLiteral("'  +SUM(A1:A2)"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("-1+2")), QStringLiteral("'-1+2"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("@SUM(A1)")), QStringLiteral("'@SUM(A1)"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("＝1+1")), QStringLiteral("'＝1+1"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("＋1+1")), QStringLiteral("'＋1+1"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("－1+2")), QStringLiteral("'－1+2"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("＠SUM(A1)")), QStringLiteral("'＠SUM(A1)"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("\ufeff=1+1")),
                 QStringLiteral("'\ufeff=1+1"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("\ufeff  +SUM(A1:A2)")),
                 QStringLiteral("'\ufeff  +SUM(A1:A2)"));
    }

    void escapesCellAndRecordSeparators()
    {
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("name\t=1\r\nnext")),
                 QStringLiteral("name\\t=1\\r\\nnext"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("\t=1+1")), QStringLiteral("\\t=1+1"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("\n=1+1")), QStringLiteral("\\n=1+1"));
        QCOMPARE(spreadsheetSafeCell(QStringLiteral("plain name")), QStringLiteral("plain name"));

        const QString withNull(QChar(0));
        QCOMPARE(spreadsheetSafeCell(withNull), QStringLiteral("\\u0000"));
    }
};

QTEST_GUILESS_MAIN(TabularTextTest)

#include "tabular_text_test.moc"
