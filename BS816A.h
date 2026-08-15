#pragma once

#include <Arduino.h>

#define BS816A_NUM_KEYS 5

// 触摸事件类型（仅短按，无长按）
enum TouchEvent {
    TOUCH_NONE = 0,    // 无事件
    TOUCH_PRESS,       // 按下（下降沿）
    TOUCH_RELEASE      // 释放（上升沿）
};

class BS816A_Driver {
public:
    BS816A_Driver(uint8_t kout1Pin, uint8_t kout2Pin, uint8_t kout3Pin,
                  uint8_t kout4Pin, uint8_t kout5Pin);

    void begin();

    // 主循环调用，返回指定按键的事件
    TouchEvent getKeyEvent(uint8_t keyIndex);

    // 获取按键当前是否按下
    bool isKeyPressed(uint8_t keyIndex);

    // 获取按键名称
    const char* getKeyName(uint8_t keyIndex);

private:
    uint8_t _pins[BS816A_NUM_KEYS];
    bool _currentState[BS816A_NUM_KEYS];       // 当前稳定状态
    bool _lastStableState[BS816A_NUM_KEYS];    // 上一次稳定状态
    bool _lastReading[BS816A_NUM_KEYS];        // 上一次原始读数
    unsigned long _lastDebounceTime[BS816A_NUM_KEYS]; // 消抖计时
    bool _baselineSet[BS816A_NUM_KEYS];               // 是否已建立初始基准状态

    static const unsigned long DEBOUNCE_DELAY_MS = 20; // 消抖延时 20ms

    const char* _keyNames[BS816A_NUM_KEYS] = {
        "KOUT1", "KOUT2", "KOUT3", "KOUT4", "KOUT5"
    };
};
