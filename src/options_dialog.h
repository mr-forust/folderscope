#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include "search_core.h"

class LongLongSpinBox;

class OptionsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit OptionsDialog(const SearchOptions &o, QWidget *parent = nullptr);

    SearchOptions options() const;

protected:
    void accept() override;

private:
    bool eventFilter(QObject *obj, QEvent *event) override;
    static QComboBox *historyCombo(const QString &key, const QString &value);
    static QComboBox *addHistoryCombo(QFormLayout *layout, const QString &label,
                                      const QString &key, const QString &value);
    static LongLongSpinBox *sizeSpin(qint64 value);
    static qint64 dateValue(const QLineEdit *edit);
    bool validateDate(QLineEdit *edit, const QString &label);
    void rememberHistory(const QString &key, const QString &value);
    static QComboBox *attrCombo(const QString &value);

    void setupUi(const SearchOptions &o);
    void saveProfile();
    void loadProfile();

    QComboBox *mode_{};
    QComboBox *duplicateMode_{};
    QCheckBox *duplicateWithoutExtension_{};
    QComboBox *roots_{};
    QComboBox *fileMasks_{};
    QComboBox *subfolderMasks_{};
    QComboBox *excludeFiles_{};
    QComboBox *excludeFolders_{};
    QComboBox *includeFolders_{};
    QTextEdit *contains_{};
    QCheckBox *binary_{};
    QCheckBox *multiple_{};
    QComboBox *multipleMode_{};
    QCheckBox *caseSensitive_{};
    LongLongSpinBox *minSize_{};
    LongLongSpinBox *maxSize_{};
    QComboBox *timeField_{};
    QSpinBox *lastMinutes_{};
    QLineEdit *minTime_{};
    QLineEdit *maxTime_{};
    QCheckBox *recursive_{};
    QCheckBox *findFiles_{};
    QCheckBox *findFolders_{};
    QCheckBox *followLinks_{};
    QCheckBox *retrieveOwner_{};
    QCheckBox *accurateProgress_{};
    QCheckBox *useCache_{};
    QCheckBox *strictDuplicateComparison_{};
    QSpinBox *maxDepth_{};
    QSpinBox *maxResults_{};
    QComboBox *hidden_{};
    QComboBox *readonly_{};
    QComboBox *executable_{};
    QComboBox *profileCombo_{};
};
