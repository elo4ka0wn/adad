#include "inputcontroller.h"

#include <QMutexLocker>
#include <QString>
#include <QStringList>
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
#include <array>

namespace
{
constexpr double kMotionThreshold = 0.4;
constexpr auto kIdleReleaseInterval = std::chrono::milliseconds(150);

const libinput_interface kInterface = {
    .open_restricted = &InputController::openRestricted,
    .close_restricted = &InputController::closeRestricted,
};

const std::array<const char *, 15> kDefaultPointerBrands = {
    "logitech", "steelseries", "razer", "asus", "synaptics",
    "elan", "apple", "microsoft", "lenovo", "hp",
    "dell", "glorious", "zowie", "touchpad", "mouse"
};

const std::array<const char *, 6> kDefaultPointerBlocked = {
    "virtual", "uinput", "seat", "test", "dummy", "mousedirectionbinder"
};

const std::array<const char *, 14> kDefaultKeyboardBrands = {
    "logitech", "steelseries", "razer", "asus", "corsair",
    "msi", "keychron", "anne", "ducky", "vortex",
    "apple", "lenovo", "dell", "keyboard"
};

const std::array<const char *, 6> kDefaultKeyboardBlocked = {
    "virtual", "uinput", "seat", "test", "dummy", "mousedirectionbinder"
};
}

InputController::InputController(QObject *parent)
    : QThread(parent)
    , m_unitDistribution(0.0, 1.0)
{
    std::random_device device;
    m_randomEngine.seed(device());
    m_lastMotion = std::chrono::steady_clock::now();

    for (const char *brand : kDefaultPointerBrands) {
        const QString value = QString::fromUtf8(brand).trimmed().toLower();
        if (!value.isEmpty()) {
            m_pointerAllowedBrands.append(value);
        }
    }
    for (const char *brand : kDefaultPointerBlocked) {
        const QString value = QString::fromUtf8(brand).trimmed().toLower();
        if (!value.isEmpty()) {
            m_pointerBlockedBrands.append(value);
        }
    }
    for (const char *brand : kDefaultKeyboardBrands) {
        const QString value = QString::fromUtf8(brand).trimmed().toLower();
        if (!value.isEmpty()) {
            m_keyboardAllowedBrands.append(value);
        }
    }
    for (const char *brand : kDefaultKeyboardBlocked) {
        const QString value = QString::fromUtf8(brand).trimmed().toLower();
        if (!value.isEmpty()) {
            m_keyboardBlockedBrands.append(value);
        }
    }

    m_pointerAllowedBrands.removeDuplicates();
    m_pointerBlockedBrands.removeDuplicates();
    m_keyboardAllowedBrands.removeDuplicates();
    m_keyboardBlockedBrands.removeDuplicates();
}

InputController::~InputController()
{
    stopController();
    wait();
}

void InputController::stopController()
{
    requestInterruption();
    {
        QMutexLocker locker(&m_accessMutex);
        if (m_accessDecisionPending) {
            m_accessDecisionPending = false;
            m_accessGranted = false;
            m_accessWait.wakeAll();
        }
    }
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

namespace
{
QStringList normalisedBrands(const QStringList &input)
{
    QStringList output;
    output.reserve(input.size());
    for (const QString &entry : input) {
        const QString trimmed = entry.trimmed().toLower();
        if (!trimmed.isEmpty() && !output.contains(trimmed)) {
            output.append(trimmed);
        }
    }
    return output;
}
}

void InputController::setPointerBrandFilters(const QStringList &allowed, const QStringList &blocked)
{
    QMutexLocker locker(&m_deviceMutex);
    m_pointerAllowedBrands = normalisedBrands(allowed);
    m_pointerBlockedBrands = normalisedBrands(blocked);
    if (!m_pointerBlockedBrands.contains(QStringLiteral("mousedirectionbinder"))) {
        m_pointerBlockedBrands.append(QStringLiteral("mousedirectionbinder"));
    }
}

void InputController::setKeyboardBrandFilters(const QStringList &allowed, const QStringList &blocked)
{
    QMutexLocker locker(&m_deviceMutex);
    m_keyboardAllowedBrands = normalisedBrands(allowed);
    m_keyboardBlockedBrands = normalisedBrands(blocked);
    if (!m_keyboardBlockedBrands.contains(QStringLiteral("mousedirectionbinder"))) {
        m_keyboardBlockedBrands.append(QStringLiteral("mousedirectionbinder"));
    }
}

void InputController::deliverAccessConfirmation(bool granted)
{
    QMutexLocker locker(&m_accessMutex);
    if (!m_accessDecisionPending) {
        return;
    }
    m_accessGranted = granted;
    m_accessDecisionPending = false;
    m_accessWait.wakeAll();
}

int InputController::openRestricted(const char *path, int flags, void *userData)
{
    auto *controller = static_cast<InputController *>(userData);
    const QString devicePath = QString::fromLocal8Bit(path);

    for (;;) {
        int fd = open(path, flags | O_CLOEXEC);
        if (fd >= 0) {
            return fd;
        }

        const int error = errno;
        if (!controller || (error != EACCES && error != EPERM)) {
            return -error;
        }

        if (!controller->requestDeviceAccess(devicePath)) {
            return -error;
        }
    }
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
        if ((errno == EACCES || errno == EPERM) && requestDeviceAccess(QStringLiteral("/dev/uinput"))) {
            m_uinputFd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        }
    }

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
    case LIBINPUT_EVENT_DEVICE_ADDED:
        handleDeviceAdded(event);
        break;
    case LIBINPUT_EVENT_DEVICE_REMOVED:
        handleDeviceRemoved(event);
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

    libinput_event *baseEvent = libinput_event_pointer_get_base_event(pointerEvent);
    libinput_device *device = baseEvent ? libinput_event_get_device(baseEvent) : nullptr;
    if (!isPointerDeviceAllowed(device)) {
        return;
    }

    updatePointerDevice(device);

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

    libinput_event *baseEvent = libinput_event_keyboard_get_base_event(keyboardEvent);
    libinput_device *device = baseEvent ? libinput_event_get_device(baseEvent) : nullptr;
    if (!isKeyboardDeviceAllowed(device)) {
        return;
    }

    updateKeyboardDevice(device);

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

void InputController::handleDeviceAdded(libinput_event *event)
{
    if (!event) {
        return;
    }

    libinput_device *device = libinput_event_get_device(event);
    if (!device) {
        return;
    }

    if (isPointerDeviceAllowed(device)) {
        updatePointerDevice(device);
    }

    if (isKeyboardDeviceAllowed(device)) {
        updateKeyboardDevice(device);
    }
}

void InputController::handleDeviceRemoved(libinput_event *event)
{
    if (!event) {
        return;
    }

    libinput_device *device = libinput_event_get_device(event);
    if (!device) {
        return;
    }

    const QString descriptor = describeDevice(device);
    bool changed = false;
    {
        QMutexLocker locker(&m_deviceMutex);
        if (m_pointerDetected && (!descriptor.isEmpty() && m_pointerDeviceName == descriptor)) {
            m_pointerDetected = false;
            m_pointerDeviceName.clear();
            changed = true;
        }
        if (m_keyboardDetected && (!descriptor.isEmpty() && m_keyboardDeviceName == descriptor)) {
            m_keyboardDetected = false;
            m_keyboardDeviceName.clear();
            changed = true;
        }
    }

    if (changed) {
        refreshDeviceSignal();
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

bool InputController::isPointerDeviceAllowed(libinput_device *device)
{
    if (!device) {
        return false;
    }

    if (!libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER) &&
        !libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_GESTURE)) {
        return false;
    }

    const QString deviceName = QString::fromLocal8Bit(libinput_device_get_name(device)).toLower();
    QStringList allowed;
    QStringList blocked;
    {
        QMutexLocker locker(&m_deviceMutex);
        allowed = m_pointerAllowedBrands;
        blocked = m_pointerBlockedBrands;
    }

    for (const QString &entry : blocked) {
        if (!entry.isEmpty() && deviceName.contains(entry)) {
            return false;
        }
    }

    if (allowed.isEmpty()) {
        return true;
    }

    for (const QString &entry : allowed) {
        if (!entry.isEmpty() && deviceName.contains(entry)) {
            return true;
        }
    }

    return false;
}

bool InputController::isKeyboardDeviceAllowed(libinput_device *device)
{
    if (!device) {
        return false;
    }

    if (!libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
        return false;
    }

    const QString deviceName = QString::fromLocal8Bit(libinput_device_get_name(device)).toLower();
    QStringList allowed;
    QStringList blocked;
    {
        QMutexLocker locker(&m_deviceMutex);
        allowed = m_keyboardAllowedBrands;
        blocked = m_keyboardBlockedBrands;
    }

    for (const QString &entry : blocked) {
        if (!entry.isEmpty() && deviceName.contains(entry)) {
            return false;
        }
    }

    if (allowed.isEmpty()) {
        return true;
    }

    for (const QString &entry : allowed) {
        if (!entry.isEmpty() && deviceName.contains(entry)) {
            return true;
        }
    }

    return false;
}

QString InputController::describeDevice(libinput_device *device) const
{
    if (!device) {
        return QString();
    }

    const QString name = QString::fromLocal8Bit(libinput_device_get_name(device)).trimmed();
    const quint32 vendor = libinput_device_get_id_vendor(device);
    const quint32 product = libinput_device_get_id_product(device);
    const QString vendorText = QStringLiteral("%1").arg(vendor, 4, 16, QLatin1Char('0')).toUpper();
    const QString productText = QStringLiteral("%1").arg(product, 4, 16, QLatin1Char('0')).toUpper();

    if (name.isEmpty()) {
        return QStringLiteral("VID:%1 PID:%2").arg(vendorText, productText);
    }

    return QStringLiteral("%1 (VID:%2 PID:%3)").arg(name, vendorText, productText);
}

void InputController::updatePointerDevice(libinput_device *device)
{
    const QString descriptor = describeDevice(device);
    bool changed = false;
    {
        QMutexLocker locker(&m_deviceMutex);
        if (!descriptor.isEmpty() && m_pointerDeviceName != descriptor) {
            m_pointerDeviceName = descriptor;
            m_pointerDetected = true;
            changed = true;
        } else if (!m_pointerDetected && descriptor.isEmpty()) {
            m_pointerDetected = true;
            changed = true;
        }
    }

    if (changed) {
        refreshDeviceSignal();
    }
}

void InputController::updateKeyboardDevice(libinput_device *device)
{
    const QString descriptor = describeDevice(device);
    bool changed = false;
    {
        QMutexLocker locker(&m_deviceMutex);
        if (!descriptor.isEmpty() && m_keyboardDeviceName != descriptor) {
            m_keyboardDeviceName = descriptor;
            m_keyboardDetected = true;
            changed = true;
        } else if (!m_keyboardDetected && descriptor.isEmpty()) {
            m_keyboardDetected = true;
            changed = true;
        }
    }

    if (changed) {
        refreshDeviceSignal();
    }
}

void InputController::refreshDeviceSignal()
{
    QString pointer;
    QString keyboard;
    {
        QMutexLocker locker(&m_deviceMutex);
        pointer = m_pointerDetected ? m_pointerDeviceName : QString();
        keyboard = m_keyboardDetected ? m_keyboardDeviceName : QString();
    }

    emit devicesDetected(pointer, keyboard);
}

bool InputController::requestDeviceAccess(const QString &devicePath)
{
    {
        QMutexLocker locker(&m_accessMutex);
        while (m_accessDecisionPending && !isInterruptionRequested()) {
            m_accessWait.wait(&m_accessMutex);
        }
        if (isInterruptionRequested()) {
            return false;
        }
        m_accessDecisionPending = true;
        m_accessGranted = false;
        m_pendingAccessPath = devicePath;
    }

    emitAccessRequest(devicePath);

    QMutexLocker locker(&m_accessMutex);
    while (m_accessDecisionPending && !isInterruptionRequested()) {
        m_accessWait.wait(&m_accessMutex);
    }
    return m_accessGranted;
}

void InputController::emitAccessRequest(const QString &path)
{
    emit accessConfirmationRequested(path);
}

