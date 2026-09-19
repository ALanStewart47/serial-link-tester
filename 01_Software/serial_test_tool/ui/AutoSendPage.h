#pragma once

#include "core/AutoSendEngine.h"
#include "core/CommandItem.h"

#include <QElapsedTimer>
#include <QList>
#include <QStringList>
#include <QWidget>

class CommandLibrary;
class IByteTransport;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QShowEvent;
class QEvent;

class AutoSendPage : public QWidget
{
    Q_OBJECT

public:
    AutoSendPage(CommandLibrary *library, AutoSendEngine *engine, IByteTransport *transport,
                 QWidget *parent = nullptr);

signals:
    void runningChanged(bool running);

protected:
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void reloadCommands();
    void onCommandChanged();
    void onLibrarySelectionChanged();
    void onPlaylistSelectionChanged();
    void onLibraryDoubleClicked(QTreeWidgetItem *item, int column);
    void onTimeoutModeChanged();
    void onModeChanged();
    void onStopModeChanged();
    void startTest();
    void stopTest();
    void onEngineState(AutoSendEngine::State state, const QString &text);
    void refreshStats();
    void toggleFavorite();
    void fillExpectedFromLastRx();
    void addCurrentToPlaylist();
    void removeSelectedFromPlaylist();
    void movePlaylistItem(int delta);
    void clearPlaylist();

private:
    void buildUi();
    void setControlsLocked(bool locked);
    void loadSettings();
    void saveSettings();
    void updateFavoriteButton();
    void updateVerdict();
    int durationMsFromUi() const;
    void applyDurationMsToUi(int durationMs);
    bool detectionEnabled() const;
    QString currentCommandId() const;
    QStringList selectedLibraryIds() const;
    QStringList playlistIds() const;
    QList<CommandItem> playlistItems() const;
    void restorePlaylist(const QStringList &ids);
    void refreshPlaylistLabels();
    void refreshSlotTable();
    void highlightCurrentSlot();
    void appendToPlaylist(const QStringList &ids);
    void selectTreeCommand(const QString &commandId);

    CommandLibrary *m_library = nullptr;
    AutoSendEngine *m_engine = nullptr;
    IByteTransport *m_transport = nullptr;
    QTimer *m_refreshTimer = nullptr;
    bool m_previewFromPlaylist = false;

    QLineEdit *m_searchEdit = nullptr;
    QTreeWidget *m_libraryTree = nullptr;
    QPushButton *m_favButton = nullptr;
    QPushButton *m_fillExpectedButton = nullptr;
    QLabel *m_sendPreview = nullptr;
    QLabel *m_replyPreview = nullptr;
    QLabel *m_matchHint = nullptr;
    QListWidget *m_playlist = nullptr;
    QPushButton *m_addToPlaylist = nullptr;
    QPushButton *m_playlistUp = nullptr;
    QPushButton *m_playlistDown = nullptr;
    QPushButton *m_playlistRemove = nullptr;
    QPushButton *m_playlistClear = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QRadioButton *m_stopCountRadio = nullptr;
    QRadioButton *m_stopDurationRadio = nullptr;
    QSpinBox *m_countSpin = nullptr;
    QSpinBox *m_durationSpin = nullptr;
    QComboBox *m_durationUnit = nullptr;
    QRadioButton *m_detectRadio = nullptr;
    QRadioButton *m_stressRadio = nullptr;
    QComboBox *m_timeoutModeCombo = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;
    QDoubleSpinBox *m_passMinSuccess = nullptr;
    QDoubleSpinBox *m_passMaxLoss = nullptr;
    QLabel *m_verdictLabel = nullptr;
    QElapsedTimer m_verdictClock;
    QCheckBox *m_fullLogCheck = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;

    QLabel *m_stateLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_etaLabel = nullptr;
    QLabel *m_lastReplyLabel = nullptr;
    QElapsedTimer m_testTimer;
    QLabel *m_statSent = nullptr;
    QLabel *m_statRecv = nullptr;
    QLabel *m_statMatch = nullptr;
    QLabel *m_statMismatch = nullptr;
    QLabel *m_statTimeout = nullptr;
    QLabel *m_statLoss = nullptr;
    QLabel *m_statCorrect = nullptr;
    QLabel *m_statSuccess = nullptr;
    QLabel *m_statRespMin = nullptr;
    QLabel *m_statRespAvg = nullptr;
    QLabel *m_statRespMax = nullptr;
    QTableWidget *m_slotTable = nullptr;
};
