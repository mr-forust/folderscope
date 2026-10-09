#include "options_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QSettings>
#include <QtTest>

class OptionsDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void preservesOptionsControlledOutsideDialog_data()
    {
        QTest::addColumn<bool>("copiesOnly");
        QTest::newRow("show-all") << false;
        QTest::newRow("copies-only") << true;
    }

    void preservesOptionsControlledOutsideDialog()
    {
        QFETCH(bool, copiesOnly);
        SearchOptions options;
        options.mode = QStringLiteral("Duplicates search");
        options.showDuplicateCopiesOnly = copiesOnly;
        options.includeSubfoldersInSummary = true;
        OptionsDialog dialog(options);
        QCOMPARE(dialog.options().showDuplicateCopiesOnly, copiesOnly);
        QVERIFY(dialog.options().includeSubfoldersInSummary);
    }

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

    void remembersInputHistory()
    {
        QSettings settings;
        settings.remove(QStringLiteral("inputHistory"));

        SearchOptions options;
        OptionsDialog first(options);
        auto *roots = first.findChild<QComboBox *>(QStringLiteral("historyroots"));
        QVERIFY(roots);
        roots->setEditText(QStringLiteral("/tmp/first;/tmp/second"));
        QVERIFY(QMetaObject::invokeMethod(&first, "accept"));

        QCOMPARE(settings.value(QStringLiteral("inputHistory/roots")).toStringList(),
                 QStringList{QStringLiteral("/tmp/first;/tmp/second")});

        OptionsDialog second(options);
        auto *history = second.findChild<QComboBox *>(QStringLiteral("historyroots"));
        QVERIFY(history);
        QCOMPARE(history->itemText(0), QStringLiteral("/tmp/first;/tmp/second"));

        history->setEditText(QStringLiteral("/tmp/new"));
        QVERIFY(QMetaObject::invokeMethod(&second, "accept"));
        QCOMPARE(settings.value(QStringLiteral("inputHistory/roots")).toStringList(),
                 (QStringList{QStringLiteral("/tmp/new"),
                              QStringLiteral("/tmp/first;/tmp/second")}));

        settings.remove(QStringLiteral("inputHistory"));
    }
};

QTEST_MAIN(OptionsDialogTest)

#include "options_dialog_test.moc"
