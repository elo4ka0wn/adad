#include "inputcontroller.h"

#include <QCoreApplication>
#include <QMutexLocker>
#include <QString>
#include <QtGlobal>

#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kMotionThreshold = 0.5;
}

InputController::InputController(QObject *parent)
    : QThread(parent)
    , m_unitDistribution(0.0, 1.0)
{
    std::random_device device;
    m_randomEngine.seed(device());
}

InputController::~InputController()
{
    stopController();
    wait();
}

void InputController::stopController()
{
    requestInterruption();
}

void InputController::setActivationKeysym(quint32 keysym)
{
    QMutexLocker locker(&m_activationMutex);
    m_pendingActivationKeysym = static_cast<KeySym>(keysym);
    m_activationDirty = true;
}

void InputController::setRandomizerEnabled(bool enabled)
{
    m_randomizerEnabled.store(enabled, std::memory_order_relaxed);
}

void InputController::setRandomizerRange(int minimumPercent, int maximumPercent)
{
    minimumPercent = std::clamp(minimumPercent, 0, 100);
    maximumPercent = std::clamp(maximumPercent, 0, 100);

    m_randomizerMinimum.store(minimumPercent, std::memory_order_relaxed);
    m_randomizerMaximum.store(maximumPercent, std::memory_order_relaxed);
}

void InputController::run()
{
    Display *display = XOpenDisplay(nullptr);
    if (!display) {
        emit errorOccurred(QStringLiteral("Не вдалося підключитися до сервера X11."));
        return;
    }

    int xiOpcode = 0;
    int event = 0;
    int error = 0;
    if (!XQueryExtension(display, "XInputExtension", &xiOpcode, &event, &error)) {
        emit errorOccurred(QStringLiteral("Розширення XInput2 недоступне."));
        XCloseDisplay(display);
        return;
    }

    int major = 2;
    int minor = 3;
    if (XIQueryVersion(display, &major, &minor) == BadRequest) {
        emit errorOccurred(QStringLiteral("Потрібна новіша версія XInput2."));
        XCloseDisplay(display);
        return;
    }

    m_keycodeA = XKeysymToKeycode(display, XK_a);
    m_keycodeD = XKeysymToKeycode(display, XK_d);
    if (m_keycodeA == 0 || m_keycodeD == 0) {
        emit errorOccurred(QStringLiteral("Не вдалося знайти клавіші a/d."));
        XCloseDisplay(display);
        return;
    }

    applyPendingActivation(display);

    Window rootWindow = DefaultRootWindow(display);
    XIEventMask mask;
    unsigned char maskData[3] = {0};
    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = sizeof(maskData);
    XISetMask(mask.mask, XI_RawMotion);
    XISetMask(mask.mask, XI_RawKeyPress);
    XISetMask(mask.mask, XI_RawKeyRelease);

    Status status = XISelectEvents(display, rootWindow, &mask, 1);
    if (status != Success) {
        emit errorOccurred(QStringLiteral("Не вдалося підписатися на події XInput2."));
        XCloseDisplay(display);
        return;
    }

    XFlush(display);
    emit statusChanged(QStringLiteral("Готово. Затисніть клавішу активації."));

    while (!isInterruptionRequested()) {
        applyPendingActivation(display);

        if (!XPending(display)) {
            msleep(5);
            continue;
        }

        XEvent eventData;
        XNextEvent(display, &eventData);

        if (eventData.type != GenericEvent) {
            continue;
        }

        XGenericEventCookie *cookie = &eventData.xcookie;
        if (cookie->extension != xiOpcode) {
            continue;
        }

        if (!XGetEventData(display, cookie)) {
            continue;
        }

        switch (cookie->evtype) {
        case XI_RawMotion:
            handleRawMotion(display, reinterpret_cast<XIRawEvent *>(cookie->data));
            break;
        case XI_RawKeyPress:
            handleRawKey(display, reinterpret_cast<XIRawEvent *>(cookie->data), true);
            break;
        case XI_RawKeyRelease:
            handleRawKey(display, reinterpret_cast<XIRawEvent *>(cookie->data), false);
            break;
        default:
            break;
        }

        XFreeEventData(display, cookie);
    }

    releaseActiveKey(display);
    XFlush(display);
    XCloseDisplay(display);
}

void InputController::applyPendingActivation(Display *display)
{
    bool dirty = false;
    KeySym keysym = NoSymbol;
    {
        QMutexLocker locker(&m_activationMutex);
        if (m_activationDirty) {
            dirty = true;
            keysym = m_pendingActivationKeysym;
            m_activationDirty = false;
        }
    }

    if (!dirty) {
        return;
    }

    int keycode = XKeysymToKeycode(display, keysym);
    if (keycode == 0) {
        emit errorOccurred(QStringLiteral("Обрана клавіша недоступна у поточній розкладці."));
        return;
    }

    m_activationKeycode = keycode;
    m_activationPressed = false;
    releaseActiveKey(display);
    emit statusChanged(QStringLiteral("Клавіша активації оновлена."));
}

void InputController::handleRawMotion(Display *display, const XIRawEvent *event)
{
    double deltaX = 0.0;
    int valueIndex = 0;
    for (int axis = 0; axis < event->valuators.mask_len * 8; ++axis) {
        if (!XIMaskIsSet(event->valuators.mask, axis)) {
            continue;
        }

        double value = event->raw_values[valueIndex++];
        if (axis == 0) {
            deltaX = value;
        }
    }

    if (!m_activationPressed) {
        releaseActiveKey(display);
        return;
    }

    if (std::fabs(deltaX) < kMotionThreshold) {
        releaseActiveKey(display);
        return;
    }

    if (!shouldApplyMotion()) {
        return;
    }

    if (deltaX < 0.0) {
        pressKey(display, m_keycodeA);
    } else if (deltaX > 0.0) {
        pressKey(display, m_keycodeD);
    }
}

void InputController::handleRawKey(Display *display, const XIRawEvent *event, bool pressed)
{
    if (event->detail != m_activationKeycode) {
        return;
    }

    if (m_activationPressed == pressed) {
        return;
    }

    m_activationPressed = pressed;

    if (!pressed) {
        releaseActiveKey(display);
        emit statusChanged(QStringLiteral("Призупинено."));
    } else {
        emit statusChanged(QStringLiteral("Активно."));
    }
}

void InputController::pressKey(Display *display, int keycode)
{
    if (keycode == 0) {
        return;
    }

    if (m_currentlyPressedKeycode == keycode) {
        return;
    }

    releaseActiveKey(display);

    XTestFakeKeyEvent(display, keycode, True, CurrentTime);
    XFlush(display);
    m_currentlyPressedKeycode = keycode;

    const QString keyName = (keycode == m_keycodeA) ? QStringLiteral("A") : QStringLiteral("D");
    emit statusChanged(QStringLiteral("Утримується клавіша %1.").arg(keyName));
}

void InputController::releaseKey(Display *display, int keycode)
{
    if (keycode == 0) {
        return;
    }

    XTestFakeKeyEvent(display, keycode, False, CurrentTime);
    XFlush(display);
}

void InputController::releaseActiveKey(Display *display)
{
    if (m_currentlyPressedKeycode == 0) {
        return;
    }

    releaseKey(display, m_currentlyPressedKeycode);
    m_currentlyPressedKeycode = 0;
}

bool InputController::shouldApplyMotion()
{
    if (!m_randomizerEnabled.load(std::memory_order_relaxed)) {
        return true;
    }

    int minimum = m_randomizerMinimum.load(std::memory_order_relaxed);
    int maximum = m_randomizerMaximum.load(std::memory_order_relaxed);
    if (maximum < minimum) {
        std::swap(minimum, maximum);
    }

    minimum = std::clamp(minimum, 0, 100);
    maximum = std::clamp(maximum, 0, 100);

    if (maximum == 0) {
        return false;
    }

    std::uniform_int_distribution<int> percentDistribution(minimum, maximum);
    const int percent = percentDistribution(m_randomEngine);
    const double probability = static_cast<double>(percent) / 100.0;

    return m_unitDistribution(m_randomEngine) <= probability;
}
