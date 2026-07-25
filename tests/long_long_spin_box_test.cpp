#include "long_long_spin_box.h"

#include <QLineEdit>
#include <QtTest>

class LongLongSpinBoxTest final : public QObject {
    Q_OBJECT

private slots:
    void preservesValuesAboveTwoGigabytes()
    {
        LongLongSpinBox spin;
        spin.setRange(0, std::numeric_limits<qint64>::max());
        spin.setSuffix(QStringLiteral(" bytes"));
        spin.setValue(5'000'000'000);
        QCOMPARE(spin.value(), qint64(5'000'000'000));
        QCOMPARE(spin.findChild<QLineEdit *>()->text(), QStringLiteral("5000000000 bytes"));
        spin.stepUp();
        QCOMPARE(spin.value(), qint64(5'000'000'001));

        spin.findChild<QLineEdit *>()->setText(QStringLiteral("6000000000 bytes"));
        QVERIFY(QMetaObject::invokeMethod(&spin, "editingFinished", Qt::DirectConnection));
        QCOMPARE(spin.value(), qint64(6'000'000'000));
    }

    void clampsAtRangeEdges()
    {
        LongLongSpinBox spin;
        spin.setRange(0, 10);
        spin.setValue(10);
        spin.stepUp();
        QCOMPARE(spin.value(), qint64(10));
        spin.setValue(-50);
        QCOMPARE(spin.value(), qint64(0));
    }
};

QTEST_MAIN(LongLongSpinBoxTest)

#include "long_long_spin_box_test.moc"
