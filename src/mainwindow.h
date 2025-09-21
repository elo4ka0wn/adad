#pragma once

#include <QMainWindow>
#include <QVector>
#include <QString>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QSlider;
class QFrame;
QT_END_NAMESPACE

class InputController;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void handleActivationChanged(int index);
    void handleRandomizerToggled(bool checked);
    void handleMinRangeChanged(int value);
    void handleMaxRangeChanged(int value);
    void handleThemeChanged(int index);
    void updateStatusLabel(const QString &statusText);
    void presentError(const QString &message);

private:
    enum class Theme {
        Light,
        Dark
    };

    struct KeyOption {
        QString label;
        quint32 keycode;
    };

    void buildInterface();
    void populateKeyOptions();
    void applyTheme(Theme theme);
    void refreshRandomizerControls();
    void syncRangeWithController();
    void updateRangeLabels();

    InputController *m_controller{nullptr};
    QVector<KeyOption> m_keyOptions;

    QFrame *m_cardFrame{nullptr};
    QComboBox *m_activationCombo{nullptr};
    QCheckBox *m_randomizerCheck{nullptr};
    QSlider *m_minSlider{nullptr};
    QSlider *m_maxSlider{nullptr};
    QLabel *m_minLabel{nullptr};
    QLabel *m_maxLabel{nullptr};
    QLabel *m_statusLabel{nullptr};
    QComboBox *m_themeCombo{nullptr};

    Theme m_currentTheme{Theme::Dark};
    int m_minSync{70};
    int m_maxSync{90};
};
