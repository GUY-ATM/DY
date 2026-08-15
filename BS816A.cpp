#include "BS816A.h"

BS816A_Driver::BS816A_Driver(uint8_t kout1Pin, uint8_t kout2Pin, uint8_t kout3Pin,
                             uint8_t kout4Pin, uint8_t kout5Pin) {
    _pins[0] = kout1Pin;
    _pins[1] = kout2Pin;
    _pins[2] = kout3Pin;
    _pins[3] = kout4Pin;
    _pins[4] = kout5Pin;

    for (int i = 0; i < BS816A_NUM_KEYS; i++) {
        _currentState[i]     = false;
        _lastStableState[i]  = false;
        _lastReading[i]      = false;
        _lastDebounceTime[i] = 0;
        _baselineSet[i]      = false;
    }
}

void BS816A_Driver::begin() {
    for (int i = 0; i < BS816A_NUM_KEYS; i++) {
        pinMode(_pins[i], INPUT);   // OMS=GND，CMOS 直接输出，无需上拉
        Serial.printf("[触摸] 按键 %s -> GPIO%d (输入)\n", _keyNames[i], _pins[i]);
    }
    Serial.println("[触摸] BS816A 驱动已初始化 (5 键, OMS=GND 高电平有效)");
}

bool BS816A_Driver::isKeyPressed(uint8_t keyIndex) {
    if (keyIndex >= BS816A_NUM_KEYS) return false;
    return _currentState[keyIndex];
}

const char* BS816A_Driver::getKeyName(uint8_t keyIndex) {
    if (keyIndex >= BS816A_NUM_KEYS) return "???";
    return _keyNames[keyIndex];
}

TouchEvent BS816A_Driver::getKeyEvent(uint8_t keyIndex) {
    if (keyIndex >= BS816A_NUM_KEYS) return TOUCH_NONE;

    // 读取 GPIO（OMS=GND，CMOS 直接输出，高电平 = 触摸按下）
    bool reading = digitalRead(_pins[keyIndex]);
    unsigned long now = millis();

    // 首次读取：仅建立基准状态，避免上电/连接时芯片尚未稳定导致的误触发
    if (!_baselineSet[keyIndex]) {
        _baselineSet[keyIndex]      = true;
        _lastReading[keyIndex]      = reading;
        _lastStableState[keyIndex]  = reading;
        _currentState[keyIndex]     = reading;
        _lastDebounceTime[keyIndex] = now;
        return TOUCH_NONE;
    }

    // 如果读数发生变化，重置消抖计时
    if (reading != _lastReading[keyIndex]) {
        _lastDebounceTime[keyIndex] = now;
        Serial.printf("[触摸][原始] %s -> %s (开始消抖)\n",
                      _keyNames[keyIndex],
                      reading ? "按下" : "释放");
    }
    _lastReading[keyIndex] = reading;

    // 消抖检查：读数必须在 DEBOUNCE_DELAY_MS 内保持稳定
    if ((now - _lastDebounceTime[keyIndex]) < DEBOUNCE_DELAY_MS) {
        return TOUCH_NONE;
    }

    // 状态变化检测（只判断短按，无长按逻辑）
    if (reading != _lastStableState[keyIndex]) {
        _lastStableState[keyIndex] = reading;
        _currentState[keyIndex]    = reading;

        if (reading) {
            Serial.printf("[触摸] %s 按下\n", _keyNames[keyIndex]);
            return TOUCH_PRESS;
        } else {
            Serial.printf("[触摸] %s 释放\n", _keyNames[keyIndex]);
            return TOUCH_RELEASE;
        }
    }

    return TOUCH_NONE;
}
