#include "mainwindow.h"

#include "inputcontroller.h"

#include <QApplication>
#include <QCheckBox>
#include <QChar>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpacerItem>
#include <QStandardPaths>
#include <QVariant>
#include <QVBoxLayout>
#include <QFileInfo>

#include <QtGlobal>

#include <algorithm>

#include <linux/input-event-codes.h>

namespace
{
QString formatPercentLabel(const QString &label, int value)
{
    return QStringLiteral("%1: %2%")
        .arg(label)
        .arg(value);
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_controller(new InputController(this))
{
    setWindowTitle(QStringLiteral("Mouse Direction Sync"));
    resize(520, 560);

    const QString configPath = configFilePath();
    QFileInfo configInfo(configPath);
    QDir().mkpath(configInfo.absolutePath());

    m_settings = new QSettings(configPath, QSettings::IniFormat, this);
    m_settings->setFallbacksEnabled(false);
    loadSettings();

    buildInterface();
    populateKeyOptions();
    applyTheme(m_currentTheme);

    connect(m_controller, &InputController::statusChanged, this, &MainWindow::updateStatusLabel);
    connect(m_controller, &InputController::errorOccurred, this, &MainWindow::presentError);
    connect(m_controller, &InputController::accessConfirmationRequested, this, &MainWindow::showAccessPrompt);
    connect(m_controller, &InputController::devicesDetected, this, &MainWindow::updateDeviceLabels);

    m_controller->setPointerBrandFilters(m_pointerAllowedBrands, m_pointerBlockedBrands);
    m_controller->setKeyboardBrandFilters(m_keyboardAllowedBrands, m_keyboardBlockedBrands);
    m_controller->start();

    restoreSettings();
}

MainWindow::~MainWindow()
{
    if (m_controller) {
        m_controller->stopController();
        m_controller->wait();
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_controller) {
        m_controller->stopController();
        m_controller->wait(1000);
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::handleActivationChanged(int index)
{
    if (index < 0 || index >= m_activationCombo->count()) {
        return;
    }

    const quint32 keycode = m_activationCombo->itemData(index).toUInt();
    m_controller->setActivationKeycode(keycode);
    m_initialActivationKey = keycode;
    saveSettings();
}

void MainWindow::handleRandomizerToggled(bool checked)
{
    m_randomizerInitiallyEnabled = checked;
    refreshRandomizerControls();
    saveSettings();
}

void MainWindow::handleMinRangeChanged(int value)
{
    m_minSync = value;
    if (m_minSync > m_maxSync) {
        m_maxSync = m_minSync;
        m_maxSlider->blockSignals(true);
        m_maxSlider->setValue(m_maxSync);
        m_maxSlider->blockSignals(false);
    }
    updateRangeLabels();
    syncRangeWithController();
    saveSettings();
}

void MainWindow::handleMaxRangeChanged(int value)
{
    m_maxSync = value;
    if (m_maxSync < m_minSync) {
        m_minSync = m_maxSync;
        m_minSlider->blockSignals(true);
        m_minSlider->setValue(m_minSync);
        m_minSlider->blockSignals(false);
    }
    updateRangeLabels();
    syncRangeWithController();
    saveSettings();
}

void MainWindow::handleThemeChanged(int index)
{
    Theme target = Theme::Dark;
    if (index == 1) {
        target = Theme::Light;
    }
    applyTheme(target);
    saveSettings();
}

void MainWindow::updateStatusLabel(const QString &statusText)
{
    if (!m_statusLabel) {
        return;
    }

    m_statusLabel->setText(QStringLiteral("Статус: %1").arg(statusText));
}

void MainWindow::presentError(const QString &message)
{
    updateStatusLabel(message);
    QMessageBox::critical(this, QStringLiteral("Помилка"), message);
}

void MainWindow::showAccessPrompt(const QString &devicePath)
{
    if (!m_controller) {
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("Потрібні права доступу"));
    box.setText(QStringLiteral("Програмі потрібен тимчасовий доступ до %1.").arg(devicePath));
    box.setInformativeText(QStringLiteral("Натисніть \"Надати доступ\", щоб відкрити полкіт-підтвердження та додати ACL для вашого користувача."));
    QPushButton *grantButton = box.addButton(QStringLiteral("Надати доступ"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(grantButton);
    box.exec();

    if (box.clickedButton() == grantButton) {
        const bool success = grantAccessWithPkexec(devicePath);
        m_controller->deliverAccessConfirmation(success);
        if (success) {
            updateStatusLabel(QStringLiteral("Доступ надано. Повторюємо підключення..."));
        }
        if (!success) {
            updateStatusLabel(QStringLiteral("Доступ не було надано."));
        }
    } else {
        m_controller->deliverAccessConfirmation(false);
        updateStatusLabel(QStringLiteral("Доступ не було надано."));
    }
}

void MainWindow::updateDeviceLabels(const QString &pointerName, const QString &keyboardName)
{
    if (m_pointerDeviceLabel) {
        const QString pointerText = pointerName.isEmpty()
            ? QStringLiteral("Миша/тачпад: не знайдено")
            : QStringLiteral("Миша/тачпад: %1").arg(pointerName);
        m_pointerDeviceLabel->setText(pointerText);
    }
    if (m_keyboardDeviceLabel) {
        const QString keyboardText = keyboardName.isEmpty()
            ? QStringLiteral("Клавіатура: не знайдено")
            : QStringLiteral("Клавіатура: %1").arg(keyboardName);
        m_keyboardDeviceLabel->setText(keyboardText);
    }
}

void MainWindow::buildInterface()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    auto *outerLayout = new QVBoxLayout(central);
    outerLayout->setContentsMargins(28, 28, 28, 28);
    outerLayout->setSpacing(16);

    m_cardFrame = new QFrame(this);
    m_cardFrame->setObjectName(QStringLiteral("card"));
    auto *cardLayout = new QVBoxLayout(m_cardFrame);
    cardLayout->setContentsMargins(24, 24, 24, 24);
    cardLayout->setSpacing(18);

    auto *titleLabel = new QLabel(QStringLiteral("Розумне затискання клавіш"), m_cardFrame);
    titleLabel->setObjectName(QStringLiteral("title"));
    cardLayout->addWidget(titleLabel);

    auto *subtitle = new QLabel(QStringLiteral("Зв'яжіть напрямок миші з клавішами A та D у Wayland."), m_cardFrame);
    subtitle->setWordWrap(true);
    subtitle->setObjectName(QStringLiteral("subtitle"));
    cardLayout->addWidget(subtitle);

    auto *activationLabel = new QLabel(QStringLiteral("Клавіша для активації"), m_cardFrame);
    cardLayout->addWidget(activationLabel);

    m_activationCombo = new QComboBox(m_cardFrame);
    cardLayout->addWidget(m_activationCombo);

    m_randomizerCheck = new QCheckBox(QStringLiteral("Увімкнути рандомізацію синхронізації"), m_cardFrame);
    cardLayout->addWidget(m_randomizerCheck);

    m_minLabel = new QLabel(m_cardFrame);
    m_minLabel->setObjectName(QStringLiteral("rangeLabel"));
    cardLayout->addWidget(m_minLabel);

    m_minSlider = new QSlider(Qt::Horizontal, m_cardFrame);
    m_minSlider->setRange(0, 100);
    cardLayout->addWidget(m_minSlider);

    m_maxLabel = new QLabel(m_cardFrame);
    m_maxLabel->setObjectName(QStringLiteral("rangeLabel"));
    cardLayout->addWidget(m_maxLabel);

    m_maxSlider = new QSlider(Qt::Horizontal, m_cardFrame);
    m_maxSlider->setRange(0, 100);
    cardLayout->addWidget(m_maxSlider);

    auto *themeLabel = new QLabel(QStringLiteral("Тема оформлення"), m_cardFrame);
    cardLayout->addWidget(themeLabel);

    m_themeCombo = new QComboBox(m_cardFrame);
    m_themeCombo->addItem(QStringLiteral("Темна тема"));
    m_themeCombo->addItem(QStringLiteral("Світла тема"));
    cardLayout->addWidget(m_themeCombo);

    auto *devicesHeader = new QLabel(QStringLiteral("Автовизначені пристрої"), m_cardFrame);
    devicesHeader->setObjectName(QStringLiteral("devicesTitle"));
    cardLayout->addWidget(devicesHeader);

    m_pointerDeviceLabel = new QLabel(QStringLiteral("Миша/тачпад: очікування..."), m_cardFrame);
    m_pointerDeviceLabel->setObjectName(QStringLiteral("deviceValue"));
    m_pointerDeviceLabel->setWordWrap(true);
    cardLayout->addWidget(m_pointerDeviceLabel);

    m_keyboardDeviceLabel = new QLabel(QStringLiteral("Клавіатура: очікування..."), m_cardFrame);
    m_keyboardDeviceLabel->setObjectName(QStringLiteral("deviceValue"));
    m_keyboardDeviceLabel->setWordWrap(true);
    cardLayout->addWidget(m_keyboardDeviceLabel);

    m_statusLabel = new QLabel(QStringLiteral("Статус: ініціалізація..."), m_cardFrame);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
    m_statusLabel->setWordWrap(true);
    cardLayout->addWidget(m_statusLabel);

    cardLayout->addStretch(1);

    outerLayout->addWidget(m_cardFrame);
    outerLayout->addStretch(1);

    connect(m_activationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::handleActivationChanged);
    connect(m_randomizerCheck, &QCheckBox::toggled, this, &MainWindow::handleRandomizerToggled);
    connect(m_minSlider, &QSlider::valueChanged, this, &MainWindow::handleMinRangeChanged);
    connect(m_maxSlider, &QSlider::valueChanged, this, &MainWindow::handleMaxRangeChanged);
    connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::handleThemeChanged);

    m_minSlider->setValue(m_minSync);
    m_maxSlider->setValue(m_maxSync);
    m_randomizerCheck->setChecked(false);
    m_themeCombo->setCurrentIndex(m_currentTheme == Theme::Dark ? 0 : 1);
}

void MainWindow::populateKeyOptions()
{
    m_keyOptions.clear();
    m_activationCombo->clear();

    const auto appendOption = [this](const QString &label, quint32 keycode) {
        KeyOption option{label, keycode};
        m_keyOptions.push_back(option);
        m_activationCombo->addItem(option.label, QVariant::fromValue(option.keycode));
    };

    appendOption(QStringLiteral("Left Shift"), KEY_LEFTSHIFT);
    appendOption(QStringLiteral("Right Shift"), KEY_RIGHTSHIFT);
    appendOption(QStringLiteral("Left Control"), KEY_LEFTCTRL);
    appendOption(QStringLiteral("Right Control"), KEY_RIGHTCTRL);
    appendOption(QStringLiteral("Left Alt"), KEY_LEFTALT);
    appendOption(QStringLiteral("Right Alt"), KEY_RIGHTALT);
    appendOption(QStringLiteral("Space"), KEY_SPACE);
    appendOption(QStringLiteral("Caps Lock"), KEY_CAPSLOCK);

    appendOption(QStringLiteral("Tab"), KEY_TAB);
    appendOption(QStringLiteral("Escape"), KEY_ESC);

    for (int i = 1; i <= 10; ++i) {
        const QChar digit = (i % 10 == 0) ? QChar(u'0') : QChar(u'0' + i);
        appendOption(QStringLiteral("Клавіша %1").arg(digit), (i % 10 == 0) ? KEY_0 : static_cast<quint32>(KEY_1 + (i - 1)));
    }

    for (int i = 0; i < 26; ++i) {
        const QChar letter = QChar(u'A' + i);
        appendOption(QString(letter), static_cast<quint32>(KEY_A + i));
    }

    for (int i = 1; i <= 12; ++i) {
        appendOption(QStringLiteral("F%1").arg(i), static_cast<quint32>(KEY_F1 + (i - 1)));
    }
}

void MainWindow::applyTheme(Theme theme)
{
    m_currentTheme = theme;

    const bool dark = (theme == Theme::Dark);
    const QString background = dark ? QStringLiteral("#101014") : QStringLiteral("#f5f7fb");
    const QString card = dark ? QStringLiteral("#1f1f2b") : QStringLiteral("#ffffff");
    const QString border = dark ? QStringLiteral("#2f3142") : QStringLiteral("#d5d9e6");
    const QString text = dark ? QStringLiteral("#f5f5f5") : QStringLiteral("#1b1d29");
    const QString accent = dark ? QStringLiteral("#7aa2f7") : QStringLiteral("#356fd1");
    const QString secondaryText = dark ? QStringLiteral("#d2d6e0") : QStringLiteral("#4b5162");

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(background));
    palette.setColor(QPalette::WindowText, QColor(text));
    palette.setColor(QPalette::Base, QColor(card));
    palette.setColor(QPalette::AlternateBase, QColor(card));
    palette.setColor(QPalette::Text, QColor(text));
    palette.setColor(QPalette::Button, QColor(card));
    palette.setColor(QPalette::ButtonText, QColor(text));
    palette.setColor(QPalette::Highlight, QColor(accent));
    palette.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    qApp->setPalette(palette);

    QString style = QStringLiteral(
        "QFrame#card { background-color: %1; border-radius: 20px; border: 1px solid %2; }"
        "QLabel { color: %3; font-size: 14px; }"
        "QLabel#title { font-size: 22px; font-weight: 600; margin-bottom: 4px; }"
        "QLabel#subtitle { color: %4; font-size: 13px; margin-bottom: 18px; }"
        "QLabel#rangeLabel { font-size: 13px; color: %4; }"
        "QLabel#devicesTitle { font-size: 14px; font-weight: 600; margin-top: 12px; margin-bottom: 4px; color: %3; }"
        "QLabel#deviceValue { font-size: 12px; color: %4; }"
        "QLabel#statusLabel { font-size: 13px; color: %5; font-weight: 600; }"
        "QComboBox { padding: 6px 10px; border-radius: 8px; border: 1px solid %2; background-color: %6; color: %3; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background-color: %6; color: %3; selection-background-color: %5; selection-color: #ffffff; }"
        "QCheckBox { spacing: 8px; color: %3; font-size: 14px; }"
        "QSlider::groove:horizontal { height: 6px; border-radius: 4px; background: %2; }"
        "QSlider::handle:horizontal { background: %5; width: 18px; margin: -6px 0; border-radius: 9px; }"
        "QSlider::sub-page:horizontal { background: %5; border-radius: 4px; }"
    ).arg(card, border, text, secondaryText, accent, card);

    qApp->setStyleSheet(style);

    if (m_cardFrame) {
        m_cardFrame->setStyleSheet(QString());
    }

    if (m_themeCombo) {
        const int desiredIndex = dark ? 0 : 1;
        if (m_themeCombo->currentIndex() != desiredIndex) {
            m_themeCombo->blockSignals(true);
            m_themeCombo->setCurrentIndex(desiredIndex);
            m_themeCombo->blockSignals(false);
        }
    }
}

void MainWindow::refreshRandomizerControls()
{
    const bool enabled = m_randomizerCheck->isChecked();
    m_minSlider->setEnabled(enabled);
    m_maxSlider->setEnabled(enabled);
    m_minLabel->setEnabled(enabled);
    m_maxLabel->setEnabled(enabled);

    m_controller->setRandomizerEnabled(enabled);
    syncRangeWithController();
}

void MainWindow::syncRangeWithController()
{
    if (!m_randomizerCheck->isChecked()) {
        m_controller->setRandomizerRange(100, 100);
        return;
    }

    m_controller->setRandomizerRange(m_minSync, m_maxSync);
}

void MainWindow::updateRangeLabels()
{
    if (m_minLabel) {
        m_minLabel->setText(formatPercentLabel(QStringLiteral("Мінімальна синхронізація"), m_minSync));
    }
    if (m_maxLabel) {
        m_maxLabel->setText(formatPercentLabel(QStringLiteral("Максимальна синхронізація"), m_maxSync));
    }
}

void MainWindow::loadSettings()
{
    if (!m_settings) {
        return;
    }

    m_initialActivationKey = m_settings->value(QStringLiteral("Input/ActivationKey"), static_cast<quint32>(KEY_LEFTSHIFT)).toUInt();
    m_randomizerInitiallyEnabled = m_settings->value(QStringLiteral("Randomizer/Enabled"), false).toBool();
    m_minSync = std::clamp(m_settings->value(QStringLiteral("Randomizer/Minimum"), 70).toInt(), 0, 100);
    m_maxSync = std::clamp(m_settings->value(QStringLiteral("Randomizer/Maximum"), 90).toInt(), 0, 100);
    if (m_maxSync < m_minSync) {
        std::swap(m_maxSync, m_minSync);
    }

    const QString themeValue = m_settings->value(QStringLiteral("Appearance/Theme"), QStringLiteral("Dark")).toString();
    m_currentTheme = (themeValue.compare(QStringLiteral("Light"), Qt::CaseInsensitive) == 0) ? Theme::Light : Theme::Dark;

    m_pointerAllowedBrands = readBrandList(QStringLiteral("Devices/PointerAllow"), defaultPointerBrands());
    m_pointerBlockedBrands = readBrandList(QStringLiteral("Devices/PointerBlock"), defaultBlockedBrands());
    m_keyboardAllowedBrands = readBrandList(QStringLiteral("Devices/KeyboardAllow"), defaultKeyboardBrands());
    m_keyboardBlockedBrands = readBrandList(QStringLiteral("Devices/KeyboardBlock"), defaultBlockedBrands());

    const auto ensureBinder = [](QStringList &list) {
        for (const QString &entry : list) {
            if (entry.compare(QStringLiteral("MouseDirectionBinder"), Qt::CaseInsensitive) == 0) {
                return;
            }
        }
        list.append(QStringLiteral("MouseDirectionBinder"));
    };
    ensureBinder(m_pointerBlockedBrands);
    ensureBinder(m_keyboardBlockedBrands);

    if (!m_settings->contains(QStringLiteral("Devices/PointerAllow"))) {
        writeBrandList(QStringLiteral("Devices/PointerAllow"), m_pointerAllowedBrands);
    }
    if (!m_settings->contains(QStringLiteral("Devices/PointerBlock"))) {
        writeBrandList(QStringLiteral("Devices/PointerBlock"), m_pointerBlockedBrands);
    }
    if (!m_settings->contains(QStringLiteral("Devices/KeyboardAllow"))) {
        writeBrandList(QStringLiteral("Devices/KeyboardAllow"), m_keyboardAllowedBrands);
    }
    if (!m_settings->contains(QStringLiteral("Devices/KeyboardBlock"))) {
        writeBrandList(QStringLiteral("Devices/KeyboardBlock"), m_keyboardBlockedBrands);
    }

    m_settings->sync();
}

void MainWindow::restoreSettings()
{
    m_isRestoring = true;

    if (m_activationCombo) {
        m_activationCombo->blockSignals(true);
        int index = m_activationCombo->findData(m_initialActivationKey);
        if (index < 0 && m_activationCombo->count() > 0) {
            index = 0;
            m_initialActivationKey = m_activationCombo->itemData(index).toUInt();
        }
        if (index >= 0) {
            m_activationCombo->setCurrentIndex(index);
        }
        m_activationCombo->blockSignals(false);
    }

    if (m_randomizerCheck) {
        m_randomizerCheck->blockSignals(true);
        m_randomizerCheck->setChecked(m_randomizerInitiallyEnabled);
        m_randomizerCheck->blockSignals(false);
    }

    if (m_minSlider) {
        m_minSlider->blockSignals(true);
        m_minSlider->setValue(m_minSync);
        m_minSlider->blockSignals(false);
    }
    if (m_maxSlider) {
        m_maxSlider->blockSignals(true);
        m_maxSlider->setValue(m_maxSync);
        m_maxSlider->blockSignals(false);
    }

    if (m_themeCombo) {
        m_themeCombo->blockSignals(true);
        m_themeCombo->setCurrentIndex(m_currentTheme == Theme::Dark ? 0 : 1);
        m_themeCombo->blockSignals(false);
    }

    updateRangeLabels();

    m_isRestoring = false;

    if (m_activationCombo && m_activationCombo->count() > 0) {
        handleActivationChanged(m_activationCombo->currentIndex());
    }

    refreshRandomizerControls();
    applyTheme(m_currentTheme);
    saveSettings();
}

void MainWindow::saveSettings()
{
    if (!m_settings || m_isRestoring) {
        return;
    }

    m_settings->setValue(QStringLiteral("Input/ActivationKey"), static_cast<quint32>(m_initialActivationKey));
    m_settings->setValue(QStringLiteral("Randomizer/Enabled"), m_randomizerInitiallyEnabled);
    m_settings->setValue(QStringLiteral("Randomizer/Minimum"), m_minSync);
    m_settings->setValue(QStringLiteral("Randomizer/Maximum"), m_maxSync);
    const QString themeValue = (m_currentTheme == Theme::Dark) ? QStringLiteral("Dark") : QStringLiteral("Light");
    m_settings->setValue(QStringLiteral("Appearance/Theme"), themeValue);
    writeBrandList(QStringLiteral("Devices/PointerAllow"), m_pointerAllowedBrands);
    writeBrandList(QStringLiteral("Devices/PointerBlock"), m_pointerBlockedBrands);
    writeBrandList(QStringLiteral("Devices/KeyboardAllow"), m_keyboardAllowedBrands);
    writeBrandList(QStringLiteral("Devices/KeyboardBlock"), m_keyboardBlockedBrands);
    m_settings->sync();
}

QString MainWindow::configFilePath() const
{
    const QString home = QDir::homePath();
    QDir configDir(home + QStringLiteral("/.config"));
    return configDir.filePath(QStringLiteral("Mouse→A_D Helper.ini"));
}

QStringList MainWindow::defaultPointerBrands() const
{
    return {
        QStringLiteral("Logitech"), QStringLiteral("SteelSeries"), QStringLiteral("Razer"),
        QStringLiteral("ASUS"), QStringLiteral("Synaptics"), QStringLiteral("ELAN"),
        QStringLiteral("Apple"), QStringLiteral("Microsoft"), QStringLiteral("Lenovo"),
        QStringLiteral("HP"), QStringLiteral("Dell"), QStringLiteral("Glorious"),
        QStringLiteral("Zowie"), QStringLiteral("Touchpad"), QStringLiteral("Mouse")
    };
}

QStringList MainWindow::defaultKeyboardBrands() const
{
    return {
        QStringLiteral("Logitech"), QStringLiteral("SteelSeries"), QStringLiteral("Razer"),
        QStringLiteral("ASUS"), QStringLiteral("Corsair"), QStringLiteral("MSI"),
        QStringLiteral("Keychron"), QStringLiteral("Anne"), QStringLiteral("Ducky"),
        QStringLiteral("Vortex"), QStringLiteral("Apple"), QStringLiteral("Lenovo"),
        QStringLiteral("Dell"), QStringLiteral("Keyboard")
    };
}

QStringList MainWindow::defaultBlockedBrands() const
{
    return {
        QStringLiteral("Virtual"), QStringLiteral("uinput"), QStringLiteral("seat"),
        QStringLiteral("test"), QStringLiteral("dummy"), QStringLiteral("MouseDirectionBinder")
    };
}

QStringList MainWindow::readBrandList(const QString &key, const QStringList &fallback) const
{
    if (!m_settings) {
        return fallback;
    }

    const QString raw = m_settings->value(key).toString();
    if (raw.trimmed().isEmpty()) {
        return fallback;
    }

    QStringList parsed = parseBrandString(raw);
    if (parsed.isEmpty()) {
        return fallback;
    }
    return parsed;
}

void MainWindow::writeBrandList(const QString &key, const QStringList &values)
{
    if (!m_settings) {
        return;
    }
    m_settings->setValue(key, brandsToString(values));
}

QStringList MainWindow::parseBrandString(const QString &value) const
{
    QString normalised = value;
    normalised.replace(QLatin1Char(';'), QLatin1Char(','));
    QStringList parts = normalised.split(QLatin1Char(','), Qt::SkipEmptyParts);
    QStringList result;
    result.reserve(parts.size());
    for (QString part : parts) {
        part = part.trimmed();
        if (!part.isEmpty() && !result.contains(part, Qt::CaseInsensitive)) {
            result.append(part);
        }
    }
    return result;
}

QString MainWindow::brandsToString(const QStringList &values) const
{
    QStringList cleaned;
    cleaned.reserve(values.size());
    for (QString value : values) {
        value = value.trimmed();
        if (!value.isEmpty() && !cleaned.contains(value, Qt::CaseInsensitive)) {
            cleaned.append(value);
        }
    }
    return cleaned.join(QStringLiteral(", "));
}

bool MainWindow::grantAccessWithPkexec(const QString &devicePath)
{
    const QString pkexecPath = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexecPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("pkexec не знайдено"), QStringLiteral("Не вдалося знайти утиліту pkexec. Встановіть polkit і повторіть спробу."));
        return false;
    }

    const QString setfaclPath = QStandardPaths::findExecutable(QStringLiteral("setfacl"));
    if (setfaclPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("setfacl не знайдено"), QStringLiteral("Не вдалося знайти утиліту setfacl. Встановіть пакет acl."));
        return false;
    }

    QString user = QString::fromLocal8Bit(qgetenv("USER"));
    if (user.isEmpty()) {
        user = QString::fromLocal8Bit(qgetenv("USERNAME"));
    }
    if (user.isEmpty()) {
        user = QString::fromLocal8Bit(qgetenv("LOGNAME"));
    }
    if (user.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Невідомий користувач"), QStringLiteral("Не вдалося визначити ім'я користувача для призначення прав доступу."));
        return false;
    }

    QProcess process;
    QStringList arguments;
    arguments << setfaclPath
              << QStringLiteral("-m")
              << QStringLiteral("u:%1:rw").arg(user)
              << devicePath;

    process.start(pkexecPath, arguments);
    if (!process.waitForStarted()) {
        QMessageBox::warning(this, QStringLiteral("Не вдалося запустити pkexec"), QStringLiteral("Процес pkexec не стартував."));
        return false;
    }

    process.waitForFinished(-1);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString errorOutput = QString::fromLocal8Bit(process.readAllStandardError());
        QMessageBox::warning(this, QStringLiteral("Доступ не надано"),
                             errorOutput.isEmpty()
                                 ? QStringLiteral("Користувач скасував операцію або доступ не було надано.")
                                 : errorOutput);
        return false;
    }

    return true;
}
