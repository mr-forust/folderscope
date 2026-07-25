#pragma once

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDesktopServices>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QProcess>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStatusBar>
#include <QTableView>
#include <QThread>
#include <QToolBar>
#include <QUrl>

#include <optional>

#include "results_model.h"
#include "search_core.h"

class SearchEngine;
class QTextEdit;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

signals:
    void startRequested(const SearchOptions &options);
    void stopRequested();
    void clearCacheRequested();

private slots:
    void showOptions();
    void refreshSearch();
    void findText();
    void findNext();
    void copyInformation();
    void explorerCopy();
    void explorerCut();
    void startCurrentSearch();
    void searchFinished(const QVector<FileRecord> &results, const QString &message);
    void openSelected();
    void openFolder();
    void openWith();
    void selectInFileManager();
    void moveToTrash();
    void deleteSelected();
    void renameSelected();
    void showProperties();
    void copyPaths();
    void contextMenu(const QPoint &point);
    void headerContextMenu(const QPoint &point);
    void exportResults();

private:
    std::optional<FileRecord> currentRecord() const;
    QStringList actionChoices() const;
    void executeConfiguredAction(const QString &action);
    QString sizeUnitName() const;
    int sizeUnitFromName(const QString &name) const;
    void applyDisplayOptions();
    void saveUiSettings();
    QStringList selectedPaths() const;
    void saveOptions();
    void updateStaleSearchWarning();
    void loadOptions();

    SearchOptions options_;
    ResultsModel *model_{};
    QSortFilterProxyModel *proxy_{};
    QTableView *table_{};
    QAction *searchAction_{};
    QAction *stopAction_{};
    QAction *refreshAction_{};
    QLabel *staleSearchWarning_{};
    QProgressBar *progress_{};
    QThread workerThread_;
    SearchEngine *engine_{};
    std::optional<SearchOptions> lastSearchOptions_;
    QString findQuery_;
    QString summarySizeUnitName_ = QStringLiteral("Automatic");
    QString doubleClickAction_ = QStringLiteral("Open Properties Window");
    QString enterKeyAction_ = QStringLiteral("Open Selected File");
    int sizeUnit_ = 0;
    int duplicateColorSet_ = 1;
    bool showGmt_ = false;
    bool markDuplicates_ = true;
    bool addExportHeader_ = true;
    bool focusOnSearchStart_ = true;
    bool focusOnSearchEnd_ = true;
    bool autoSizeOnSearchEnd_ = false;
    bool showGrid_ = true;
    bool showTooltips_ = true;
    bool isSearching_ = false;
    bool isStopping_ = false;
};
