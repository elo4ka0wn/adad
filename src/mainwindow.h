#pragma once

#include <QMainWindow>
#include <QVector>
#include <QString>
#include <QStringList>

#include <linux/input-event-codes.h>

class QSettings;

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
    void showAccessPrompt(const QString &devicePath);
    void updateDeviceLabels(const QString &pointerName, const QString &keyboardName);

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
    void loadSettings();
    void restoreSettings();
    void saveSettings();
    QString configFilePath() const;
    QStringList defaultPointerBrands() const;
    QStringList defaultKeyboardBrands() const;
    QStringList defaultBlockedBrands() const;
    QStringList readBrandList(const QString &key, const QStringList &fallback) const;
    void writeBrandList(const QString &key, const QStringList &values);
    QStringList parseBrandString(const QString &value) const;
    QString brandsToString(const QStringList &values) const;
    bool grantAccessWithPkexec(const QString &devicePath);

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
    QLabel *m_pointerDeviceLabel{nullptr};
    QLabel *m_keyboardDeviceLabel{nullptr};

    Theme m_currentTheme{Theme::Dark};
    int m_minSync{70};
    int m_maxSync{90};
    bool m_randomizerInitiallyEnabled{false};
    quint32 m_initialActivationKey{KEY_LEFTSHIFT};

    QSettings *m_settings{nullptr};
    bool m_isRestoring{false};

    QStringList m_pointerAllowedBrands;
    QStringList m_pointerBlockedBrands;
    QStringList m_keyboardAllowedBrands;
    QStringList m_keyboardBlockedBrands;
};
