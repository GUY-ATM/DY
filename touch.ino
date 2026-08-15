/*
 * ESP32-C3 BLE 刷抖音神器 v4 (NimBLE Digitizer)
 *
 * 功能：通过 NimBLE HID Digitizer 绝对坐标触控控制手机短视频APP
 *   KOUT1: 上键 -> 下滑(上一个视频)
 *   KOUT2: 下键 -> 上滑(下一个视频)
 *   KOUT3: 分享按钮
 *   KOUT4: 评论按钮
 *   KOUT5: 点赞（点击后回到锚点）
 *
 * 所有坐标/触控参数统一从 Config.h 读取，仅针对 1080x2400 固定分辨率标定，
 * 不支持串口修改。LED: 慢闪=等待连接  常亮=已连接
 */

#include <math.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include "BS816A.h"
#include "Config.h"

// ==================== GPIO 引脚 ====================
#define KOUT1_PIN       0
#define KOUT2_PIN       1
#define KOUT3_PIN       2
#define KOUT4_PIN       3
#define KOUT5_PIN       4
#define ONBOARD_LED_PIN 8

// ==================== Digitizer Report Map ====================
// X和Y拆成独立Input item, 各自Logical Max匹配宽高比。
// Logical Max 直接由 Config::logical_max_x / logical_max_y 生成，保证与坐标映射一致。
#define REPORT_ID_TOUCH   0x01

static const uint8_t REPORT_MAP[] = {
  0x05, 0x0D,                    // USAGE_PAGE (Digitizer)
  0x09, 0x04,                    // USAGE (Touch Screen)
  0xA1, 0x01,                    // COLLECTION (Application)
  0x85, REPORT_ID_TOUCH,         //   REPORT_ID
  // Tip Switch
  0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
  // In Range
  0x09, 0x32, 0x81, 0x02,
  // Padding 6 bits
  0x75, 0x01, 0x95, 0x06, 0x81, 0x03,
  // X  (逻辑 0~logical_max_x, 对应屏幕宽)
  0x05, 0x01, 0x09, 0x30,
  0x15, 0x00, 0x26,
  (uint8_t)(Config::logical_max_x & 0xFF), (uint8_t)((Config::logical_max_x >> 8) & 0xFF),
  0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
  // Y  (逻辑 0~logical_max_y, 对应屏幕高)
  0x09, 0x31,
  0x15, 0x00, 0x26,
  (uint8_t)(Config::logical_max_y & 0xFF), (uint8_t)((Config::logical_max_y >> 8) & 0xFF),
  0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
  0xC0
};

// ==================== 连接回调 ====================
class ConnCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* p, NimBLEConnInfo& i) override {
    Serial.println(F(">> [BLE] 已连接"));
  }
  void onDisconnect(NimBLEServer* p, NimBLEConnInfo& i, int r) override {
    Serial.printf("<< [BLE] 断开 (%d)\n", r);
    NimBLEDevice::startAdvertising();
  }
};

// ==================== 全局对象 ====================
NimBLEServer*         bleServer = nullptr;
NimBLECharacteristic* bleReport = nullptr;
BS816A_Driver         touch(KOUT1_PIN, KOUT2_PIN, KOUT3_PIN, KOUT4_PIN, KOUT5_PIN);

// ==================== 状态变量 ====================
bool  shareMode    = false;   // 是否处于分享选择状态
int   avatarIndex  = 0;       // 当前头像序号(0~4)
bool  avatarTapped = false;   // 是否按下过 love 键（点击过头像）
bool  commentMode  = false;   // 是否处于评论状态
int   cursorX      = Config::anchor_x;   // 当前悬停光标位置 X
int   cursorY      = Config::anchor_y;   // 当前悬停光标位置 Y

// ==================== 坐标转换 ====================
// X和Y各自独立的Logical Max, 匹配屏幕宽高比避免X被压缩
inline uint16_t px2log(int px, int screenPx, uint16_t logicalMax) {
  int32_t v = (int32_t)px * logicalMax / screenPx;
  return (uint16_t)constrain(v, 0, logicalMax);
}

// ==================== 触控发送 ====================
void sendTouch(uint16_t x, uint16_t y, bool tip, bool inRange) {
  if (bleServer->getConnectedCount() == 0) return;
  uint8_t r[5] = {
    (uint8_t)((tip ? 0x01 : 0x00) | (inRange ? 0x02 : 0x00)),
    (uint8_t)(x & 0xFF), (uint8_t)((x >> 8) & 0xFF),
    (uint8_t)(y & 0xFF), (uint8_t)((y >> 8) & 0xFF)
  };
  bleReport->setValue(r, 5);
  bleReport->notify();
}

// ==================== 无阻塞动作执行器 ====================
// 每个触控动作被拆成若干"帧"，由 actionTick() 在 loop 中按 millis() 非阻塞执行
struct ActionFrame {
  uint16_t px, py;    // 物理像素坐标
  bool tip;           // 是否按下
  bool inRange;       // 是否悬停
  uint16_t waitMs;    // 本帧发送后等待的毫秒数
};

#define ACTION_MAX_FRAMES 220
ActionFrame actionFrames[ACTION_MAX_FRAMES];
uint16_t actionFrameCount = 0;
uint16_t actionFrameIndex = 0;
bool         actionBusy   = false;
unsigned long actionNextMs = 0;

void actionBegin() { actionFrameCount = 0; }

void actionAdd(uint16_t px, uint16_t py, bool tip, bool inRange, uint16_t waitMs) {
  if (actionFrameCount >= ACTION_MAX_FRAMES) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      Serial.println(F("[动作] 警告：帧队列已满，动作被截断"));
    }
    return;
  }
  ActionFrame &f = actionFrames[actionFrameCount++];
  f.px = px; f.py = py;
  f.tip = tip; f.inRange = inRange;
  f.waitMs = waitMs;
}

void actionStart() {
  actionFrameIndex = 0;
  actionBusy = (actionFrameCount > 0);
  if (actionBusy) actionNextMs = millis();
}

// 主循环调用：非阻塞地逐帧发送触控数据
void actionTick() {
  if (!actionBusy) return;
  unsigned long now = millis();
  if ((int32_t)(actionNextMs - now) > 0) return;   // 未到发送时刻（防 millis 回绕）
  ActionFrame &f = actionFrames[actionFrameIndex];
  sendTouch(px2log(f.px, Config::screen_width,  Config::logical_max_x),
            px2log(f.py, Config::screen_height, Config::logical_max_y),
            f.tip, f.inRange);
  actionNextMs = now + f.waitMs;
  actionFrameIndex++;
  if (actionFrameIndex >= actionFrameCount) {
    actionBusy = false;
    actionFrameCount = 0;
  }
}

// ==================== 待处理按键队列 ====================
// 动作执行期间仍持续轮询触摸芯片，捕获到的按键先入队，
// 待动作结束后依次执行，避免"动作中按下的键被丢弃"。
#define KEY_QUEUE_SIZE 8
uint8_t keyQueue[KEY_QUEUE_SIZE];
uint8_t keyQueueHead  = 0;
uint8_t keyQueueTail  = 0;
uint8_t keyQueueCount = 0;

bool keyQueuePush(uint8_t idx) {
  if (keyQueueCount >= KEY_QUEUE_SIZE) {
    Serial.println(F("[按键] 队列已满，丢弃一次按下"));
    return false;
  }
  keyQueue[keyQueueTail] = idx;
  keyQueueTail = (uint8_t)((keyQueueTail + 1) % KEY_QUEUE_SIZE);
  keyQueueCount++;
  return true;
}

uint8_t keyQueuePop() {
  uint8_t idx = keyQueue[keyQueueHead];
  keyQueueHead = (uint8_t)((keyQueueHead + 1) % KEY_QUEUE_SIZE);
  keyQueueCount--;
  return idx;
}

// ---- 追加：线性悬停移动（tip=false, inRange=true）----
void addHoverLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  for (int i = 1; i <= Config::swipe_steps; i++) {
    int x = x1 + (int)((x2 - x1) * (long)i / Config::swipe_steps);
    int y = y1 + (int)((y2 - y1) * (long)i / Config::swipe_steps);
    actionAdd(x, y, false, true, Config::swipe_step_ms);
  }
}

// ---- 指数缓出 easing：e = 1 - 2^(-k*t)，先极快后极慢，丝滑滑入 ----
// 仅在"动作构建"时一次性计算中间点（非逐帧热路径），浮点开销可忽略。
static inline float easeOutExpo(float t, float k) {
  if (t >= 1.0f) return 1.0f;
  if (t <= 0.0f) return 0.0f;
  return 1.0f - powf(2.0f, -k * t);
}

// ---- 悬停缓动移动（参数化：步数 / 延时范围 / 缓动强度 k）----
void addHoverEaseParam(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                       int steps, int delayMin, int delayMax, float k) {
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / (float)steps;
    float e = easeOutExpo(t, k);
    int px = x1 + (int)lroundf((float)(x2 - x1) * e);
    int py = y1 + (int)lroundf((float)(y2 - y1) * e);
    int d  = delayMin + (int)(t * (float)(delayMax - delayMin));
    actionAdd(px, py, false, true, d);
  }
}

// ---- 头像间普通横向移动 ----
void addHoverEase(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  addHoverEaseParam(x1, y1, x2, y2,
                    Config::hover_steps, Config::hover_delay_min, Config::hover_delay_max,
                    Config::hover_ease_k);
}

// ---- 跨环跳回（从最后一个头像跳回第一个，大跨度，更明显的先快后慢）----
void addHoverJump(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  addHoverEaseParam(x1, y1, x2, y2,
                    Config::hover_jump_steps, Config::hover_jump_delay_min, Config::hover_jump_delay_max,
                    Config::hover_jump_ease_k);
}

// ---- 单点：按下 -> 离开 ----
void doTap(uint16_t px, uint16_t py) {
  actionBegin();
  actionAdd(px, py, true, true, Config::tap_down_ms);
  actionAdd(0, 0, false, false, 0);
  actionStart();
}

// ---- 点击后悬停原地（分享状态选中/取消）----
void doTapStay(uint16_t px, uint16_t py) {
  actionBegin();
  actionAdd(px, py, true, true, Config::tap_down_ms);
  actionAdd(px, py, false, true, 0);
  actionStart();
  cursorX = px; cursorY = py;
}

// ---- 点击后悬停返回目标点，保持光标位置连续 ----
void doTapReturn(uint16_t px, uint16_t py, uint16_t rx, uint16_t ry) {
  actionBegin();
  actionAdd(px, py, true, true, Config::tap_down_ms);
  actionAdd(px, py, false, true, 0);
  addHoverLine(px, py, rx, ry);
  actionAdd(0, 0, false, false, 0);
  actionStart();
  cursorX = rx; cursorY = ry;
}

// ---- 滑动：按住 -> 滑动 -> 悬停终点 -> 悬停回起点 -> 离开 ----
void doSwipe(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  actionBegin();
  actionAdd(x1, y1, true, true, 60);       // 初始按住 60ms 确保手机识别
  for (int i = 1; i <= Config::swipe_steps; i++) {
    int x = x1 + (int)((x2 - x1) * (long)i / Config::swipe_steps);
    int y = y1 + (int)((y2 - y1) * (long)i / Config::swipe_steps);
    actionAdd(x, y, true, true, Config::swipe_step_ms);
  }
  actionAdd(x2, y2, false, true, 0);       // 终点抬起（保持悬停）
  addHoverLine(x2, y2, x1, y1);            // 悬停回起点
  actionAdd(0, 0, false, false, 0);        // 离开
  actionStart();
  cursorX = x1; cursorY = y1;
}

// ---- 打开分享：点击分享按钮 -> 等待面板 -> 缓动悬停到第一个头像 ----
void doOpenShare(uint16_t sx, uint16_t sy) {
  int x0 = cursorX, y0 = cursorY;
  actionBegin();
  actionAdd(sx, sy, true, true, Config::tap_down_ms);   // 点击分享按钮
  actionAdd(0, 0, false, false, 400);                   // 等待分享面板弹出
  addHoverEase(x0, y0, Config::avatar_x[0], Config::avatar_y);
  actionStart();
  cursorX = Config::avatar_x[0]; cursorY = Config::avatar_y;
}

// ---- 打开评论：点击评论按钮 -> 悬停回锚点 -> 等待面板 ----
void doOpenComment(uint16_t cx, uint16_t cy) {
  actionBegin();
  actionAdd(cx, cy, true, true, Config::tap_down_ms);
  actionAdd(cx, cy, false, true, 0);
  addHoverLine(cx, cy, Config::anchor_x, Config::anchor_y);
  actionAdd(0, 0, false, false, 400);                   // 等待评论区弹出
  actionStart();
  cursorX = Config::anchor_x; cursorY = Config::anchor_y;
}

// ---- 分享状态：缓动悬停到指定头像（jump=true 表示跨环跳回）----
void doHoverAvatar(int idx, bool jump) {
  int x0 = cursorX, y0 = cursorY;
  actionBegin();
  if (jump) addHoverJump(x0, y0, Config::avatar_x[idx], Config::avatar_y);
  else      addHoverEase(x0, y0, Config::avatar_x[idx], Config::avatar_y);
  actionStart();
  cursorX = Config::avatar_x[idx]; cursorY = Config::avatar_y;
}

// ==================== 按键处理 ====================
void handleKey(uint8_t idx) {
  // 分享选择状态：按键功能改变
  if (shareMode) {
    switch (idx) {
      case 0: {  // KOUT1: 上一个头像（悬停移动，不点击）
        bool jump = (avatarIndex == 0);      // 0 -> 4 跨环跳回
        avatarIndex = (avatarIndex + 4) % 5;
        Serial.printf("[分享] 移动到第%d个头像%s\n", avatarIndex + 1, jump ? " (跳回)" : "");
        doHoverAvatar(avatarIndex, jump);
        break;
      }
      case 1: {  // KOUT2: 下一个头像（悬停移动，不点击）
        bool jump = (avatarIndex == 4);      // 4 -> 0 跨环跳回
        avatarIndex = (avatarIndex + 1) % 5;
        Serial.printf("[分享] 移动到第%d个头像%s\n", avatarIndex + 1, jump ? " (跳回)" : "");
        doHoverAvatar(avatarIndex, jump);
        break;
      }
      case 2:  // KOUT3: 未选头像则中断分享，已选头像则确认分享
        if (avatarTapped) {
          Serial.println(F("[分享] 确认分享"));
          doTapReturn(Config::confirm_share_x, Config::confirm_share_y,
                      Config::anchor_x, Config::anchor_y);
        } else {
          Serial.println(F("[分享] 未选择头像，中断分享"));
          doTapReturn(Config::share_cancel_x, Config::share_cancel_y,
                      Config::anchor_x, Config::anchor_y);
        }
        shareMode = false;
        break;
      case 3:  // KOUT4: 关闭分享面板，中断分享 -> 回到锚点
        Serial.println(F("[分享] 取消分享"));
        doTapReturn(Config::share_cancel_x, Config::share_cancel_y,
                    Config::anchor_x, Config::anchor_y);
        shareMode = false;
        break;
      case 4:  // KOUT5: 点击当前头像（选中/取消选中）
        Serial.printf("[分享] 点击第%d个头像\n", avatarIndex + 1);
        doTapStay(Config::avatar_x[avatarIndex], Config::avatar_y);
        avatarTapped = true;
        break;
    }
    return;
  }

  // 评论状态：可上下滑动，分享/点赞无效，评论键关闭评论区
  if (commentMode) {
    int cx = Config::anchor_x, cy = Config::anchor_y;  // 锚点
    switch (idx) {
      case 0:  // KOUT1: 上键 -> 下滑(上一个)
        Serial.println(F("[评论] 下滑(上一个)"));
        doSwipe(cx, cy, cx, cy + Config::swipe_distance_px);
        break;
      case 1:  // KOUT2: 下键 -> 上滑(下一个)
        Serial.println(F("[评论] 上滑(下一个)"));
        doSwipe(cx, cy, cx, cy - Config::swipe_distance_px);
        break;
      case 2:  // KOUT3: 分享按钮无效
        Serial.println(F("[评论] 分享键无效"));
        break;
      case 3:  // KOUT4: 再次点击评论按钮 -> 关闭评论区并回到锚点
        Serial.println(F("[评论] 关闭评论区"));
        doTapReturn(Config::comment_close_x, Config::comment_close_y,
                    Config::anchor_x, Config::anchor_y);
        commentMode = false;
        break;
      case 4:  // KOUT5: 点赞键无效
        Serial.println(F("[评论] 点赞键无效"));
        break;
    }
    return;
  }

  int cx = Config::anchor_x, cy = Config::anchor_y;  // 锚点（固定位置，操作后光标回到此处）
  switch (idx) {
    case 0:  // KOUT1: 上键 -> 下滑(切换上一个视频)
      Serial.println(F("[动作] 下滑(上一个)"));
      doSwipe(cx, cy, cx, cy + Config::swipe_distance_px);
      break;
    case 1:  // KOUT2: 下键 -> 上滑(切换下一个视频)
      Serial.println(F("[动作] 上滑(下一个)"));
      doSwipe(cx, cy, cx, cy - Config::swipe_distance_px);
      break;
    case 2:  // KOUT3: 分享 -> 打开面板并进入分享选择状态
      Serial.println(F("[动作] 分享"));
      doOpenShare(Config::share_x, Config::share_y);
      shareMode = true;
      avatarIndex = 0;
      avatarTapped = false;
      break;
    case 3:  // KOUT4: 评论 -> 打开评论区后回到锚点，进入评论状态
      Serial.println(F("[动作] 评论"));
      doOpenComment(Config::comment_x, Config::comment_y);
      commentMode = true;
      break;
    case 4:  // KOUT5: 点赞 -> 点击后回到锚点
      Serial.println(F("[动作] 点赞"));
      doTapReturn(Config::like_x, Config::like_y, cx, cy);
      break;
  }
}

// ==================== 初始化 ====================
void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println(F("==================================="));
  Serial.println(F(" ESP32-C3 BLE 刷抖音神器 v4"));
  Serial.println(F(" NimBLE Digitizer 触控"));
  Serial.println(F("==================================="));
  Serial.printf("[系统] %s rev.%d  内存:%lu\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getFreeHeap());

  // LED
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, HIGH);

  // 触摸驱动
  touch.begin();
  delay(500);

  // 坐标配置摘要（固定，不可串口修改）
  Serial.printf("[配置] 屏幕=%dx%d 逻辑上限=(%d,%d)\n",
                Config::screen_width, Config::screen_height,
                Config::logical_max_x, Config::logical_max_y);
  Serial.printf("[配置] 锚点=(%d,%d) 点赞=(%d,%d) 评论=(%d,%d) 分享=(%d,%d)\n",
                Config::anchor_x, Config::anchor_y,
                Config::like_x, Config::like_y,
                Config::comment_x, Config::comment_y,
                Config::share_x, Config::share_y);

  // NimBLE Digitizer
  Serial.println(F("[BLE] 初始化 NimBLE..."));
  NimBLEDevice::init("DY");
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ConnCallbacks());

  NimBLEHIDDevice* hid = new NimBLEHIDDevice(bleServer);
  bleReport = hid->getInputReport(REPORT_ID_TOUCH);
  hid->setManufacturer("ESP32-C3");
  hid->setHidInfo(0x00, 0x02);
  hid->setReportMap((uint8_t*)REPORT_MAP, sizeof(REPORT_MAP));

  NimBLEAdvertising* adv = bleServer->getAdvertising();
  adv->setAppearance(0x03C2);
  adv->addServiceUUID(hid->getHidService()->getUUID());
  adv->start();

  Serial.printf("[BLE] 广播中: 'DY'\n");
  Serial.printf("[系统] 可用内存: %lu\n", ESP.getFreeHeap());
  Serial.println(F("[系统] 就绪！等待蓝牙连接..."));
  Serial.println();
}

// ==================== 主循环 ====================
void loop() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  unsigned long now = millis();
  bool connected = (bleServer->getConnectedCount() > 0);

  // LED
  if (!connected) {
    if (now - lastBlink >= 500) {
      lastBlink = now;
      ledState = !ledState;
      digitalWrite(ONBOARD_LED_PIN, ledState ? LOW : HIGH);
    }
  } else {
    digitalWrite(ONBOARD_LED_PIN, LOW);
  }

  // 未连接时不处理触摸，直接返回
  if (!connected) return;

  // 持续轮询触摸按键（动作执行中也要轮询，保证按下边沿不被错过）
  for (uint8_t i = 0; i < 5; i++) {
    if (touch.getKeyEvent(i) == TOUCH_PRESS) {
      if (actionBusy) {
        keyQueuePush(i);   // 动作中：入队，动作结束后执行
      } else {
        handleKey(i);      // 空闲：立即执行
      }
    }
  }

  // 非阻塞执行触控动作帧
  actionTick();

  // 动作结束后消费队列，保证多次快速点击依次执行
  if (!actionBusy && keyQueueCount > 0) {
    handleKey(keyQueuePop());
  }
}
