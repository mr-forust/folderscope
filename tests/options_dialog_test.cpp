#include "options_dialog.h"

#include <QCheckBox>
#include <QtTest>

class OptionsDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void exposesStrictDuplicateComparison()
    {
        SearchOptions options;
        QVERIFY(!options.strictDuplicateComparison);
        options.strictDuplicateComparison = true;

        OptionsDialog dialog(options);
        auto *strict = dialog.findChild<QCheckBox *>(
            QStringLiteral("strictDuplicateComparison"));
        QVERIFY(strict);
        QVERIFY(strict->isChecked());
        QVERIFY(dialog.options().strictDuplicateComparison);

        strict->setChecked(false);
        QVERIFY(!dialog.options().strictDuplicateComparison);
    }
};

QTEST_MAIN(OptionsDialogTest)

#include "options_dialog_test.moc"
