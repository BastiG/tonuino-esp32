#ifndef __CONTROLLER_H__
#define __CONTROLLER_H__

#include <Arduino.h>

#define CTRL_PIN_VOLUME_UP      2
#define CTRL_PIN_VOLUME_DOWN    4
#define CTRL_PIN_PAUSE          27
#define CTRL_PIN_NEXT           17
#define CTRL_PIN_PREVIOUS       21

#define COMMAND_DEBOUNCE        200
#define COMMAND_REPEAT          1000


class Controller {
public:
    typedef void (*CallbackFunc)(void);
    struct CallbackData {
        CallbackData() :
            lastTrigger(0),
            lastCommand(0),
            callback(nullptr) {
        };
        u_long lastTrigger;
        u_long lastCommand;
        CallbackFunc callback;
    };

    Controller();
    ~Controller();
    bool begin();

    void loop();

    void setVolumeUpCallback(CallbackFunc callback) { dataVolumeUp.callback = callback; }
    void setVolumeDownCallback(CallbackFunc callback) { dataVolumeDown.callback = callback; }
    void setPauseCallback(CallbackFunc callback) { dataPause.callback = callback; }
    void setNextCallback(CallbackFunc callback) { dataNext.callback = callback; }
    void setPreviousCallback(CallbackFunc callback) { dataPrevious.callback = callback; }
protected:
    static void IRAM_ATTR trigger(void* arg);
private:
    void _commandRepeat(CallbackData* data, u_long timeout);
    CallbackData dataVolumeUp;
    CallbackData dataVolumeDown;
    CallbackData dataPause;
    CallbackData dataNext;
    CallbackData dataPrevious;
};

#endif
