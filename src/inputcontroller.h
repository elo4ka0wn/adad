#pragma once

#include <QMutex>
#include <QObject>
#include <QThread>

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

public slots:
    void setActivationKeycode(quint32 keycode);
    void setRandomizerEnabled(bool enabled);
    void setRandomizerRange(int minimumPercent, int maximumPercent);

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

    void pressKey(uint16_t keycode);
    void releaseKey(uint16_t keycode);
    void sendKeyEvent(uint16_t keycode, int value);
    void releaseActiveKey();
    bool shouldApplyMotion();

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
};
