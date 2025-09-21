#pragma once

#include <QThread>
#include <QMutex>
#include <QObject>

#include <atomic>
#include <random>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>

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
    void setActivationKeysym(quint32 keysym);
    void setRandomizerEnabled(bool enabled);
    void setRandomizerRange(int minimumPercent, int maximumPercent);

protected:
    void run() override;

private:
    void applyPendingActivation(Display *display);
    void handleRawMotion(Display *display, const XIRawEvent *event);
    void handleRawKey(Display *display, const XIRawEvent *event, bool pressed);
    void pressKey(Display *display, int keycode);
    void releaseKey(Display *display, int keycode);
    void releaseActiveKey(Display *display);
    bool shouldApplyMotion();

    std::atomic<bool> m_randomizerEnabled{false};
    std::atomic<int> m_randomizerMinimum{70};
    std::atomic<int> m_randomizerMaximum{90};

    QMutex m_activationMutex;
    KeySym m_pendingActivationKeysym{XK_Shift_L};
    bool m_activationDirty{true};

    int m_activationKeycode{0};
    int m_currentlyPressedKeycode{0};
    bool m_activationPressed{false};

    int m_keycodeA{0};
    int m_keycodeD{0};

    std::mt19937 m_randomEngine;
    std::uniform_real_distribution<double> m_unitDistribution;
};
