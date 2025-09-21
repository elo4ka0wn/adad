#include "mainwindow.h"

#include "inputcontroller.h"

#include <QApplication>
#include <QCheckBox>
#include <QChar>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QSlider>
#include <QSpacerItem>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

#include <X11/keysym.h>
#include <X11/keysymdef.h>

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

    buildInterface();
    populateKeyOptions();
    applyTheme(m_currentTheme);

    connect(m_controller, &InputController::statusChanged, this, &MainWindow::updateStatusLabel);
    connect(m_controller, &InputController::errorOccurred, this, &MainWindow::presentError);

    m_controller->start();

    int defaultIndex = m_activationCombo->findData(static_cast<quint32>(XK_Shift_L));
    if (defaultIndex < 0) {
        defaultIndex = 0;
    }
    if (m_activationCombo->count() > 0) {
        m_activationCombo->setCurrentIndex(defaultIndex);
        handleActivationChanged(m_activationCombo->currentIndex());
    }

    updateRangeLabels();
    refreshRandomizerControls();
    syncRangeWithController();
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

    const quint32 keysym = m_activationCombo->itemData(index).toUInt();
    m_controller->setActivationKeysym(keysym);
}

void MainWindow::handleRandomizerToggled(bool checked)
{
    m_controller->setRandomizerEnabled(checked);
    refreshRandomizerControls();
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
}

void MainWindow::handleThemeChanged(int index)
{
    Theme target = Theme::Dark;
    if (index == 1) {
        target = Theme::Light;
    }
    applyTheme(target);
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

    auto *subtitle = new QLabel(QStringLiteral("Зв'яжіть напрямок миші з клавішами A та D."), m_cardFrame);
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

    const auto appendOption = [this](const QString &label, KeySym keysym) {
        KeyOption option{label, static_cast<quint32>(keysym)};
        m_keyOptions.push_back(option);
        m_activationCombo->addItem(option.label, QVariant::fromValue(option.keysym));
    };

    appendOption(QStringLiteral("Left Shift"), XK_Shift_L);
    appendOption(QStringLiteral("Right Shift"), XK_Shift_R);
    appendOption(QStringLiteral("Left Control"), XK_Control_L);
    appendOption(QStringLiteral("Right Control"), XK_Control_R);
    appendOption(QStringLiteral("Left Alt"), XK_Alt_L);
    appendOption(QStringLiteral("Right Alt"), XK_Alt_R);
    appendOption(QStringLiteral("Space"), XK_space);
    appendOption(QStringLiteral("Caps Lock"), XK_Caps_Lock);

    for (int i = 0; i < 10; ++i) {
        const QChar digit = QChar(u'0' + i);
        appendOption(QStringLiteral("Клавіша %1").arg(digit), XK_0 + i);
    }

    for (int i = 0; i < 26; ++i) {
        const QChar letter = QChar(u'A' + i);
        appendOption(QString(letter), XK_A + i);
    }

    for (int i = 1; i <= 12; ++i) {
        appendOption(QStringLiteral("F%1").arg(i), XK_F1 + (i - 1));
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

    if (!enabled) {
        m_statusLabel->setText(QStringLiteral("Статус: синхронізація 100%"));
    }

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
