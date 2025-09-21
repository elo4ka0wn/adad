#pragma once

#include <QMutex>
#include <QObject>
#include <QThread>
#include <QWaitCondition>
#include <QStringList>
#include <QString>

#include <atomic>
#include <chrono>
#include <random>

#include <linux/input-event-codes.h>

struct libinput;
struct udev;
struct libinput_event;
struct libinput_event_pointer;
struct libinput_event_keyboard;

class InputController : public QThread
{
    Q_OBJECT
public:
    explicit InputController(QObject *parent = nullptr);
    ~InputController() override;

    void stopController();

signals:
    void statusChanged(const QString &statusText);
    void errorOccurred(const QString &errorText);
    void devicesDetected(const QString &pointerName, const QString &keyboardName);
    void accessConfirmationRequested(const QString &devicePath);

public slots:
    void setActivationKeycode(quint32 keycode);
    void setRandomizerEnabled(bool enabled);
    void setRandomizerRange(int minimumPercent, int maximumPercent);
    void setPointerBrandFilters(const QStringList &allowed, const QStringList &blocked);
    void setKeyboardBrandFilters(const QStringList &allowed, const QStringList &blocked);
    void deliverAccessConfirmation(bool granted);

protected:
    void run() override;

private:
    static int openRestricted(const char *path, int flags, void *userData);
    static void closeRestricted(int fd, void *userData);

    bool setupUinput();
    void teardownUinput();
    bool setupLibinput();
    void teardownLibinput();

    void applyPendingActivation();
    void processEvent(libinput_event *event);
    void handlePointerMotion(libinput_event_pointer *pointerEvent);
    void handleKeyboardKey(libinput_event_keyboard *keyboardEvent);
    void handleDeviceAdded(libinput_event *event);
    void handleDeviceRemoved(libinput_event *event);

    void pressKey(uint16_t keycode);
    void releaseKey(uint16_t keycode);
    void sendKeyEvent(uint16_t keycode, int value);
    void releaseActiveKey();
    bool shouldApplyMotion();
    bool isPointerDeviceAllowed(libinput_device *device);
    bool isKeyboardDeviceAllowed(libinput_device *device);
    QString describeDevice(libinput_device *device) const;
    void updatePointerDevice(libinput_device *device);
    void updateKeyboardDevice(libinput_device *device);
    void refreshDeviceSignal();
    bool requestDeviceAccess(const QString &devicePath);
    void emitAccessRequest(const QString &path);

    int m_uinputFd{-1};
    libinput *m_libinput{nullptr};
    udev *m_udev{nullptr};

    std::atomic<bool> m_randomizerEnabled{false};
    std::atomic<int> m_randomizerMinimum{70};
    std::atomic<int> m_randomizerMaximum{90};

    QMutex m_activationMutex;
    uint16_t m_pendingActivationKeycode{KEY_LEFTSHIFT};
    bool m_activationDirty{true};

    std::atomic<uint16_t> m_activationKeycode{KEY_LEFTSHIFT};
    bool m_activationPressed{false};

    const uint16_t m_keycodeA{KEY_A};
    const uint16_t m_keycodeD{KEY_D};
    uint16_t m_currentlyPressedKeycode{0};

    std::chrono::steady_clock::time_point m_lastMotion;

    std::mt19937 m_randomEngine;
    std::uniform_real_distribution<double> m_unitDistribution;

    QStringList m_pointerAllowedBrands;
    QStringList m_pointerBlockedBrands;
    QStringList m_keyboardAllowedBrands;
    QStringList m_keyboardBlockedBrands;

    QMutex m_deviceMutex;
    QString m_pointerDeviceName;
    QString m_keyboardDeviceName;
    bool m_pointerDetected{false};
    bool m_keyboardDetected{false};

    QMutex m_accessMutex;
    QWaitCondition m_accessWait;
    bool m_accessDecisionPending{false};
    bool m_accessGranted{false};
    QString m_pendingAccessPath;
};
