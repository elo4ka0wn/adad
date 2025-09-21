#include "inputcontroller.h"

#include <QMutexLocker>
#include <QString>
#include <QtGlobal>

#include <libinput.h>
#include <libudev.h>

#include <cerrno>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <linux/uinput.h>

namespace
{
constexpr double kMotionThreshold = 0.4;
constexpr auto kIdleReleaseInterval = std::chrono::milliseconds(150);

const libinput_interface kInterface = {
    .open_restricted = &InputController::openRestricted,
    .close_restricted = &InputController::closeRestricted,
};
}

InputController::InputController(QObject *parent)
    : QThread(parent)
    , m_unitDistribution(0.0, 1.0)
{
    std::random_device device;
    m_randomEngine.seed(device());
    m_lastMotion = std::chrono::steady_clock::now();
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

void InputController::setActivationKeycode(quint32 keycode)
{
    QMutexLocker locker(&m_activationMutex);
    m_pendingActivationKeycode = static_cast<uint16_t>(keycode);
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

int InputController::openRestricted(const char *path, int flags, void *userData)
{
    Q_UNUSED(userData);
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    return fd;
}

void InputController::closeRestricted(int fd, void *userData)
{
    Q_UNUSED(userData);
    if (fd >= 0) {
        close(fd);
    }
}

bool InputController::setupUinput()
{
    m_uinputFd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (m_uinputFd < 0) {
        emit errorOccurred(QStringLiteral("Не вдалося відкрити /dev/uinput: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    if (ioctl(m_uinputFd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(m_uinputFd, UI_SET_KEYBIT, m_keycodeA) < 0 ||
        ioctl(m_uinputFd, UI_SET_KEYBIT, m_keycodeD) < 0) {
        emit errorOccurred(QStringLiteral("Не вдалося налаштувати клавіші uinput: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        teardownUinput();
        return false;
    }

    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1337;
    setup.id.product = 0x1337;
    setup.id.version = 1;
    std::strncpy(setup.name, "MouseDirectionBinder", sizeof(setup.name) - 1);

    if (ioctl(m_uinputFd, UI_DEV_SETUP, &setup) < 0) {
        emit errorOccurred(QStringLiteral("Не вдалося створити віртуальний пристрій: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        teardownUinput();
        return false;
    }

    if (ioctl(m_uinputFd, UI_DEV_CREATE) < 0) {
        emit errorOccurred(QStringLiteral("Не вдалося активувати віртуальний пристрій: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        teardownUinput();
        return false;
    }

    return true;
}

void InputController::teardownUinput()
{
    if (m_uinputFd >= 0) {
        ioctl(m_uinputFd, UI_DEV_DESTROY);
        close(m_uinputFd);
        m_uinputFd = -1;
    }
}

bool InputController::setupLibinput()
{
    m_udev = udev_new();
    if (!m_udev) {
        emit errorOccurred(QStringLiteral("Не вдалося створити контекст udev."));
        return false;
    }

    m_libinput = libinput_udev_create_context(&kInterface, this, m_udev);
    if (!m_libinput) {
        emit errorOccurred(QStringLiteral("Не вдалося створити контекст libinput. Переконайтеся, що маєте доступ до /dev/input/*."));
        udev_unref(m_udev);
        m_udev = nullptr;
        return false;
    }

    if (libinput_udev_assign_seat(m_libinput, "seat0") != 0) {
        emit errorOccurred(QStringLiteral("Не вдалося підключитися до seat0. Перевірте доступ seatd чи запустіть додаток з sudo."));
        libinput_unref(m_libinput);
        m_libinput = nullptr;
        udev_unref(m_udev);
        m_udev = nullptr;
        return false;
    }

    libinput_dispatch(m_libinput);
    return true;
}

void InputController::teardownLibinput()
{
    if (m_libinput) {
        libinput_unref(m_libinput);
        m_libinput = nullptr;
    }
    if (m_udev) {
        udev_unref(m_udev);
        m_udev = nullptr;
    }
}

void InputController::run()
{
    emit statusChanged(QStringLiteral("Ініціалізація пристроїв..."));

    if (!setupUinput()) {
        return;
    }
    if (!setupLibinput()) {
        teardownUinput();
        return;
    }

    emit statusChanged(QStringLiteral("Готово. Затисніть клавішу активації."));

    pollfd descriptors[1];
    descriptors[0].fd = libinput_get_fd(m_libinput);
    descriptors[0].events = POLLIN;

    while (!isInterruptionRequested()) {
        applyPendingActivation();

        int ret = poll(descriptors, 1, 50);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            emit errorOccurred(QStringLiteral("Помилка poll(): %1").arg(QString::fromLocal8Bit(strerror(errno))));
            break;
        }

        if (ret > 0 && (descriptors[0].revents & (POLLERR | POLLHUP))) {
            emit errorOccurred(QStringLiteral("Втрачено з'єднання з пристроєм введення."));
            break;
        }

        if (ret > 0 && (descriptors[0].revents & POLLIN)) {
            libinput_dispatch(m_libinput);
            libinput_event *event = nullptr;
            while ((event = libinput_get_event(m_libinput)) != nullptr) {
                processEvent(event);
                libinput_event_destroy(event);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_currentlyPressedKeycode != 0 && now - m_lastMotion > kIdleReleaseInterval) {
            releaseActiveKey();
            emit statusChanged(QStringLiteral("Призупинено."));
        }
    }

    releaseActiveKey();
    teardownLibinput();
    teardownUinput();
}

void InputController::applyPendingActivation()
{
    bool dirty = false;
    uint16_t keycode = KEY_LEFTSHIFT;
    {
        QMutexLocker locker(&m_activationMutex);
        if (m_activationDirty) {
            keycode = m_pendingActivationKeycode;
            m_activationDirty = false;
            dirty = true;
        }
    }

    if (!dirty) {
        return;
    }

    if (keycode == 0) {
        emit errorOccurred(QStringLiteral("Обрана клавіша недоступна."));
        return;
    }

    m_activationKeycode.store(keycode, std::memory_order_relaxed);
    m_activationPressed = false;
    releaseActiveKey();
    emit statusChanged(QStringLiteral("Клавіша активації оновлена."));
}

void InputController::processEvent(libinput_event *event)
{
    switch (libinput_event_get_type(event)) {
    case LIBINPUT_EVENT_POINTER_MOTION:
        handlePointerMotion(libinput_event_get_pointer_event(event));
        break;
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
        handlePointerMotion(libinput_event_get_pointer_event(event));
        break;
    case LIBINPUT_EVENT_KEYBOARD_KEY:
        handleKeyboardKey(libinput_event_get_keyboard_event(event));
        break;
    default:
        break;
    }
}

void InputController::handlePointerMotion(libinput_event_pointer *pointerEvent)
{
    if (!pointerEvent) {
        return;
    }

    if (!m_activationPressed) {
        releaseActiveKey();
        return;
    }

    double deltaX = libinput_event_pointer_get_dx(pointerEvent);
    const double rawDeltaX = libinput_event_pointer_get_dx_unaccelerated(pointerEvent);
    if (std::fabs(rawDeltaX) > std::fabs(deltaX)) {
        deltaX = rawDeltaX;
    }

    if (std::fabs(deltaX) < kMotionThreshold) {
        return;
    }

    m_lastMotion = std::chrono::steady_clock::now();

    if (!shouldApplyMotion()) {
        releaseActiveKey();
        return;
    }

    if (deltaX < 0.0) {
        pressKey(m_keycodeA);
    } else if (deltaX > 0.0) {
        pressKey(m_keycodeD);
    }
}

void InputController::handleKeyboardKey(libinput_event_keyboard *keyboardEvent)
{
    if (!keyboardEvent) {
        return;
    }

    const uint32_t key = libinput_event_keyboard_get_key(keyboardEvent);
    if (key != m_activationKeycode.load(std::memory_order_relaxed)) {
        return;
    }

    const libinput_key_state state = libinput_event_keyboard_get_key_state(keyboardEvent);
    const bool pressed = (state == LIBINPUT_KEY_STATE_PRESSED);
    if (pressed == m_activationPressed) {
        return;
    }

    m_activationPressed = pressed;
    if (!pressed) {
        releaseActiveKey();
        emit statusChanged(QStringLiteral("Призупинено."));
    } else {
        emit statusChanged(QStringLiteral("Активно."));
    }
}

void InputController::pressKey(uint16_t keycode)
{
    if (m_uinputFd < 0 || keycode == 0) {
        return;
    }

    if (m_currentlyPressedKeycode == keycode) {
        return;
    }

    releaseActiveKey();
    sendKeyEvent(keycode, 1);
    m_currentlyPressedKeycode = keycode;

    const QString keyName = (keycode == m_keycodeA) ? QStringLiteral("A") : QStringLiteral("D");
    emit statusChanged(QStringLiteral("Утримується клавіша %1.").arg(keyName));
}

void InputController::releaseKey(uint16_t keycode)
{
    if (m_uinputFd < 0 || keycode == 0) {
        return;
    }

    sendKeyEvent(keycode, 0);
}

void InputController::sendKeyEvent(uint16_t keycode, int value)
{
    if (m_uinputFd < 0) {
        return;
    }

    input_event event{};
    gettimeofday(&event.time, nullptr);
    event.type = EV_KEY;
    event.code = keycode;
    event.value = value;
    if (write(m_uinputFd, &event, sizeof(event)) < 0) {
        emit errorOccurred(QStringLiteral("Помилка запису у uinput: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return;
    }

    input_event sync{};
    gettimeofday(&sync.time, nullptr);
    sync.type = EV_SYN;
    sync.code = SYN_REPORT;
    sync.value = 0;
    if (write(m_uinputFd, &sync, sizeof(sync)) < 0) {
        emit errorOccurred(QStringLiteral("Помилка синхронізації uinput: %1").arg(QString::fromLocal8Bit(strerror(errno))));
    }
}

void InputController::releaseActiveKey()
{
    if (m_currentlyPressedKeycode == 0) {
        return;
    }

    releaseKey(m_currentlyPressedKeycode);
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
