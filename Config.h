#pragma once

// ============================================================
// 集中式坐标与触控参数配置
//
// 所有硬编码坐标 / 触控参数统一集中于此，程序一律从这里读取。
// 仅针对 1080x2400 固定分辨率标定，用户不可通过串口修改。
// ============================================================

namespace Config {

// ---- 屏幕分辨率（固定，仅针对此设备标定）----
constexpr int screen_width  = 1080;
constexpr int screen_height = 2400;

// ---- 逻辑坐标上限（与 HID Report Map 保持一致，X 按宽高比折算）----
constexpr int logical_max_y = 10000;
constexpr int logical_max_x = logical_max_y * screen_width / screen_height; // = 4500

// ---- 触控动作参数 ----
constexpr int swipe_distance_px = 150;  // 滑动距离(像素)
constexpr int swipe_steps       = 50;   // 滑动插值步数
constexpr int swipe_step_ms     = 4;    // 滑动每步延迟
constexpr int tap_down_ms       = 14;   // 点击按下到释放间隔

// ---- 悬停缓动参数（头像间普通横向移动）----
constexpr int hover_steps     = 60;   // 悬停移动步数
constexpr int hover_delay_min = 2;    // 起始每步延迟(ms)，快
constexpr int hover_delay_max = 26;   // 结束每步延迟(ms)，慢，形成顿挫减速
constexpr float hover_ease_k  = 8.0f; // easeOutExpo 强度 k（越大越"先快后慢"）

// ---- 跨环跳回缓动参数（从最后一个头像跳回第一个，大跨度横向）----
constexpr int hover_jump_steps     = 96;    // 更多步数，轨迹更细腻
constexpr int hover_jump_delay_min = 1;     // 起始更快
constexpr int hover_jump_delay_max = 30;    // 尾部更慢，丝滑滑入
constexpr float hover_jump_ease_k  = 12.0f; // 更强的"先快后慢"

// ---- 锚点（操作后光标回到的位置）----
constexpr int anchor_x = 993;
constexpr int anchor_y = 1535;

// ---- 评论 ----
constexpr int comment_x       = 1002;  // 评论按钮（打开评论）
constexpr int comment_y       = 1605;
constexpr int comment_close_x = 540;   // 关闭评论区点击点
constexpr int comment_close_y = 726;

// ---- 分享 ----
constexpr int share_x         = 1002;  // 分享按钮（打开分享）
constexpr int share_y         = 1960;
constexpr int share_cancel_x  = 1003;  // 取消/关闭分享面板点击点
constexpr int share_cancel_y  = 1700;
constexpr int avatar_y        = 1902;  // 好友头像纵坐标（固定）
constexpr int avatar_x[5]     = {116, 308, 500, 692, 884};  // 5 个头像横坐标
constexpr int confirm_share_x = 815;   // 确认分享按钮 X
constexpr int confirm_share_y = 2321;  // 确认分享按钮 Y

// ---- 点赞 ----
constexpr int like_x = 1002;  // 点赞位置
constexpr int like_y = 1426;

} // namespace Config
