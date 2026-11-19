/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_lang.h"

#include <stddef.h>
#include <string.h>

#include "muse_text.h"
#include "sdkconfig.h"
#if !CONFIG_MUSE_UI_FREEINK
#include "lvgl.h"
#endif

/*
 * A translation is looked up by its English text, so a caption the code
 * changes falls back to English rather than to the wrong words.
 * test_muse_lang.py checks that each English text is still in the code, that
 * a translation keeps its format conversions and status names their upper
 * case, and that every letter is one its fonts have. A language adds its
 * tables, the Montserrat sizes its letters need and a Kconfig choice. The
 * touch boards' Chinese settings text is selected in muse_settings_ui.c.
 */
typedef struct {
    const char *en, *text;
} muse_lang_entry_t;

#if CONFIG_MUSE_UI_LANG_VI
/* Short names, upper case like the English: they head the screen. */
static const muse_lang_entry_t STATUS[] = {
    { "WAKING UP", "ĐANG KHỞI ĐỘNG" },
    { "READY", "SẴN SÀNG" },
    { "LISTENING", "ĐANG NGHE" },
    { "THINKING", "ĐANG NGHĨ" },
    { "SPEAKING", "ĐANG NÓI" },
    { "ERROR", "LỖI" },
    { "GOODBYE", "TẠM BIỆT" },
    { "WI-FI OFF", "WI-FI ĐANG TẮT" },
    { "SET UP WI-FI", "CHƯA CÀI WI-FI" },
    { "NO WI-FI", "KHÔNG CÓ WI-FI" },
    { "RECONNECTING", "ĐANG KẾT NỐI LẠI" },
    { "CONNECTING", "ĐANG KẾT NỐI" },
    { "USB POWER", "NGUỒN USB" },
    { "CHARGING %d%%", "ĐANG SẠC %d%%" },
    { "BATTERY %d%%", "PIN %d%%" },
};

/* Sentence case: a capital with two marks (Ấ, Ễ) is cramped in 16 px. */
static const muse_lang_entry_t MESSAGES[] = {
    /* The voice path */
    { "WAKING UP...", "Đang khởi động..." },
    { "LISTENING...", "Đang nghe..." },
    { "RECORDING...", "Đang ghi âm..." },
    { "LISTENING %.1fs", "Đang nghe %.1fs" },
    { "RECORDING %.1fs", "Đang ghi âm %.1fs" },
    { "SENDING VOICE NOTE", "Đang gửi tin nhắn thoại" },
    { "NOTE SENT - WAITING FOR MUSE", "Đã gửi, đang chờ Muse" },
    { "HOLD LONGER TO TALK", "Giữ nút lâu hơn để nói" },
    { "RECORD A LONGER NOTE", "Hãy ghi âm lâu hơn" },
    { "MICROPHONE ERROR - TRY AGAIN", "Lỗi mic - thử lại" },
    { "SET UP MUSE FIRST", "Hãy cài đặt Muse trước" },
    { "NO WI-FI", "Không có Wi-Fi" },
    { "AUDIO INIT FAILED", "Lỗi khởi động âm thanh" },
    /* Notes saved to send later */
    { "COULDN'T SAVE THE NOTE", "Không lưu được tin nhắn" },
    { "SAVED, WILL TRY AGAIN", "Đã lưu, sẽ thử lại" },
    { "SAVED, SENDS WHEN ONLINE", "Đã lưu, sẽ gửi khi có mạng" },
    { "SENDING SAVED NOTE", "Đang gửi tin nhắn đã lưu" },
    { "SAVED NOTE SENT", "Đã gửi tin nhắn đã lưu" },
    { "COULDN'T SEND A SAVED NOTE", "Không gửi được tin nhắn đã lưu" },
    { "SAVED NOTE: WILL TRY AGAIN", "Tin nhắn đã lưu: sẽ thử lại" },
    { "NOTES STILL WAITING TO SEND", "Còn tin nhắn chờ gửi" },
    /* A turn that failed */
    { "CAN'T REACH MUSE", "Không kết nối được Muse" },
    { "MUSE NOT SET UP", "Chưa cài đặt Muse" },
    { "LOST CONNECTION TO MUSE", "Mất kết nối với Muse" },
    { "NO REPLY FROM MUSE", "Muse chưa trả lời" },
    { "DIDN'T CATCH THAT", "Chưa nghe rõ, nói lại nhé" },
    { "MUSE STOPPED LISTENING", "Muse đã ngừng nghe" },
    { "MUSE COULDN'T LISTEN", "Muse không nghe được" },
    { "MUSE DIDN'T TAKE IT", "Muse không nhận tin nhắn" },
    { "INTERRUPTED", "Đã bị ngắt" },
    { "CANCELLED", "Đã huỷ" },
    { "SETTINGS CHANGED", "Đã đổi cài đặt" },
    { "REPLY TOO LONG", "Câu trả lời quá dài" },
    { "REPLY TOO LONG: SEE MUSE APP", "Trả lời quá dài: xem app Muse" },
    { "REPLY BUFFER LIMIT - TRY AGAIN", "Bộ nhớ đệm đầy, thử lại nhé" },
    { "MUSE REPLY ACCESS DENIED (403)", "Muse từ chối truy cập (403)" },
    { "MUSE REPLY AUTH REQUIRED (401)", "Muse cần đăng nhập lại (401)" },
    { "CAN'T SUBSCRIBE TO MUSE", "Không nhận được tin từ Muse" },
    { "CAN'T KEEP UP", "Xử lý không kịp" },
    { "OUT OF MEMORY", "Hết bộ nhớ" },
    /* Buttons and power */
    { "SPEAKER ON", "Đã bật loa" },
    { "SPEAKER OFF", "Đã tắt loa" },
    { "HOLD TO MUTE", "Giữ để tắt tiếng" },
    { "HOLD TO UNMUTE", "Giữ để bật tiếng" },
    { "HOLD TO POWER OFF", "Giữ để tắt máy" },
    { "GOODBYE!", "Tạm biệt!" },
    { "COULDN'T POWER OFF", "Không tắt được máy" },
    { "PHONE SETUP ON", "Đã bật cài đặt qua điện thoại" },
    { "PHONE SETUP: %s", "Cài qua ĐT: %s" },
    { "PHONE SETUP OFF", "Đã tắt cài đặt qua điện thoại" },
    { "RESETTING...", "Đang đặt lại..." },
};

/* The button menu (muse_menu.c) and the pairing card (muse_ui.c): sentence
 * case, in Montserrat, but for the titles and pages, drawn in unscii, whose
 * accented letters come from the caption font. */
static const muse_lang_entry_t MENU[] = {
    /* Rows */
    { "Volume", "Âm lượng" },
    { "Speaker", "Loa" },
    { "Brightness", "Độ sáng" },
    { "Mic gain", "Độ nhạy mic" },
    { "Auto-sleep", "Tự ngủ" },
    { "Text size", "Cỡ chữ" },
    { "Phone setup", "Cài qua điện thoại" },
    { "Status", "Trạng thái" },
    { "Battery", "Pin" },
    { "Reset pairing", "Đặt lại ghép nối" },
    { "Screen off", "Tắt màn hình" },
    { "Power off", "Tắt máy" },
    { "Close menu", "Đóng menu" },
    /* What the talk button does */
    { "Change", "Đổi" },
    { "Toggle", "Bật/tắt" },
    { "Open", "Mở" },
    { "Select", "Chọn" },
    { "Close", "Đóng" },
    { "Back", "Quay lại" },
    { "Reset", "Đặt lại" },
    { "Cancel", "Huỷ" },
    { "Esc Back", "Esc Quay lại" },
    { "Esc Cancel", "Esc Huỷ" },
    { LV_SYMBOL_DOWN " Down", LV_SYMBOL_DOWN " Xuống" },
    { "Down " LV_SYMBOL_RIGHT, "Xuống " LV_SYMBOL_RIGHT },
    /* Values */
    { "On", "Bật" },
    { "Off", "Tắt" },
    { "Not set", "Chưa cài" },
    { "Joining", "Đang vào" },
    { "Failed", "Lỗi" },
    { "Not found", "Không thấy" },
    { "Never", "Không bao giờ" },
    { "30 s", "30 giây" },
    { "1 min", "1 phút" },
    { "2 min", "2 phút" },
    { "5 min", "5 phút" },
    { "10 min", "10 phút" },
    { "Small", "Nhỏ" },
    { "Medium", "Vừa" },
    { "Large", "Lớn" },
    { "Custom", "Tuỳ chỉnh" },
    /* Titles, upper case */
    { "MENU", "CÀI ĐẶT" },
    { "MENU  ^v Move  <> Change", "CÀI ĐẶT  ^v Chọn  <> Đổi" },
    { "STATUS", "TRẠNG THÁI" },
    { "BATTERY", "PIN" },
    { "POWER OFF", "TẮT MÁY" },
    { "RESET PAIRING", "ĐẶT LẠI GHÉP NỐI" },
    /* Pages, their columns lined up as in the English */
    { "Wi-Fi %s\nIP    %s\nLink  %s\nMuse  %s\nPhone %s\nPower %s\nVer   %s",
      "Wi-Fi %s\nIP    %s\nLink  %s\nMuse  %s\nĐT    %s\nNguồn %s\nBản   %s" },
    { "off", "tắt" },
    { "offline", "mất mạng" },
    { "Connected", "Đã kết nối" },
    { "Starting", "Đang khởi động" },
    { "Ready to pair", "Chờ ghép nối" },
    { "App connected", "App đã kết nối" },
    { "Confirm pairing", "Xác nhận ghép nối" },
    { "Connecting", "Đang kết nối" },
    { "Online", "Trực tuyến" },
    { "Offline", "Ngoại tuyến" },
    { "Error", "Lỗi" },
    { "Not set up", "Chưa cài" },
    { "Saved", "Đã lưu" },
    { "Can't connect", "Không kết nối được" },
    { "Unplug USB to\nmeasure how\nlong the\nbattery lasts.", "Rút USB để\nđo xem pin\ndùng được\nbao lâu." },
    { "%s %s\nBatt  %d>%d%%\nRate  %s\nFull  %s\nOff   %s\nSleep %s\nWakes %s\nBusy  %s",
      "%s %s\nPin   %d>%d%%\nHao   %s\nĐầy   %s\nTắt   %s\nNgủ   %s\nThức  %s\nBận   %s" },
    { "On batt", "Dùng pin" },
    { "Last run", "Lần trước" },
    { "Turn Muse off?\n\nPress the %s button to turn it back on.", "Tắt Muse?\n\nBấm nút %s để bật lại." },
    { "Forget Wi-Fi and the Muse app pairing, then restart?", "Quên Wi-Fi và ghép nối app Muse, rồi khởi động lại?" },
    /* The pairing card */
    { "Pairing code", "Mã ghép nối" },
    { "Enter on phone", "Nhập trên điện thoại" },
    { "Enter it on your phone", "Nhập mã trên điện thoại" },
    { "Tap screen", "Chạm màn hình" },
    { "Tap to confirm pairing", "Chạm để xác nhận ghép nối" },
    { "Press", "Bấm" },
    { "Press button", "Bấm nút" },
    { "%s button", "Nút %s" },
    { "Press the %s button", "Bấm nút %s" },
    { "Muse app", "App Muse" },
    { "Pair with Muse app", "Ghép nối với app Muse" },
};

#elif CONFIG_MUSE_UI_LANG_ZH
static const muse_lang_entry_t ZH_STATUS[] = {
    { "WAKING UP", "正在唤醒" },
    { "READY", "待机" },
    { "LISTENING", "我在听" },
    { "THINKING", "正在思考" },
    { "SPEAKING", "正在回答" },
    { "ERROR", "出错了" },
    { "GOODBYE", "再见" },
    { "WI-FI OFF", "无线网络已关闭" },
    { "SET UP WI-FI", "请设置无线网络" },
    { "NO WI-FI", "无线网络未连接" },
    { "RECONNECTING", "正在重连" },
    { "CONNECTING", "正在连接" },
    { "USB POWER", "USB 供电" },
    { "CHARGING %d%%", "正在充电 %d%%" },
    { "BATTERY %d%%", "电量 %d%%" },
};

static const muse_lang_entry_t ZH_MESSAGES[] = {
    { "WAKING UP...", "正在唤醒" },
    { "LISTENING...", "我在听" },
    { "RECORDING...", "正在录音" },
    { "LISTENING %.1fs", "已听 %.1f 秒" },
    { "RECORDING %.1fs", "已录 %.1f 秒" },
    { "SENDING VOICE NOTE", "正在发送语音留言" },
    { "NOTE SENT - WAITING FOR MUSE", "留言已发，等待 Muse 回复" },
    { "HOLD LONGER TO TALK", "请按住黄键说话" },
    { "RECORD A LONGER NOTE", "请多录一会儿" },
    { "SET UP MUSE FIRST", "请先设置 Muse" },
    { "NO WI-FI", "无线网络未连接" },
    { "AUDIO INIT FAILED", "音频启动失败" },
    { "COULDN'T SAVE THE NOTE", "语音留言保存失败" },
    { "SAVED, WILL TRY AGAIN", "已保存，稍后重试" },
    { "SAVED, SENDS WHEN ONLINE", "已保存，联网后发送" },
    { "SENDING SAVED NOTE", "正在发送已保存的留言" },
    { "SAVED NOTE SENT", "已发送保存的留言" },
    { "COULDN'T SEND A SAVED NOTE", "语音留言发送失败" },
    { "SAVED NOTE: WILL TRY AGAIN", "留言稍后重试" },
    { "NOTES STILL WAITING TO SEND", "还有留言等待发送" },
    { "CAN'T REACH MUSE", "无法连接 Muse" },
    { "MUSE NOT SET UP", "Muse 尚未设置" },
    { "LOST CONNECTION TO MUSE", "Muse 连接已断开" },
    { "NO REPLY FROM MUSE", "Muse 尚未回复" },
    { "DIDN'T CATCH THAT", "没听清，请再说一次" },
    { "MUSE STOPPED LISTENING", "Muse 已停止收听" },
    { "MUSE COULDN'T LISTEN", "Muse 无法收听" },
    { "MUSE DIDN'T TAKE IT", "Muse 没收到留言" },
    { "INTERRUPTED", "已打断" },
    { "CANCELLED", "已取消" },
    { "SETTINGS CHANGED", "设置已修改" },
    { "REPLY TOO LONG", "回复太长" },
    { "REPLY TOO LONG: SEE MUSE APP", "回复太长，请在 Muse 应用查看" },
    { "REPLY BUFFER LIMIT - TRY AGAIN", "回复缓冲已满，请重试" },
    { "MUSE REPLY ACCESS DENIED (403)", "Muse 拒绝访问（403）" },
    { "MUSE REPLY AUTH REQUIRED (401)", "Muse 需要重新登录（401）" },
    { "CAN'T SUBSCRIBE TO MUSE", "无法接收 Muse 消息" },
    { "CAN'T KEEP UP", "处理速度跟不上" },
    { "OUT OF MEMORY", "内存不足" },
    { "SPEAKER ON", "扬声器已开" },
    { "SPEAKER OFF", "扬声器已关" },
    { "HOLD TO MUTE", "长按静音" },
    { "HOLD TO UNMUTE", "长按恢复声音" },
    { "HOLD TO POWER OFF", "长按关机" },
    { "GOODBYE!", "再见" },
    { "COULDN'T POWER OFF", "关机失败" },
    { "PHONE SETUP ON", "手机配对已开启" },
    { "PHONE SETUP: %s", "手机配对：%s" },
    { "PHONE SETUP OFF", "手机配对已关闭" },
    { "RESETTING...", "正在重置" },
    { "MICROPHONE ERROR - TRY AGAIN", "麦克风出错，请重试" },
    { "RECONNECTING - KEEP HOLDING", "正在重连，请继续按住" },
    { "HOLD YELLOW TO TALK", "长按黄键说话" },
    { "ONE MOMENT...", "请稍等" },
    { "NO REPLY YET", "还没有回复" },
    { "REPLY MISSED - HOLD TO TRY AGAIN", "回复中断，长按重试" },
    { "MIC BUFFER FULL - RELEASE AND RETRY", "录音缓冲已满，松开重试" },
    { "HANGING UP...", "正在挂断" },
    { "CONNECTING TO MUSE...", "正在连接 Muse" },
};

static const muse_lang_entry_t ZH_MENU[] = {
    { "Volume", "音量" },
    { "Speaker", "扬声器" },
    { "Brightness", "亮度" },
    { "Mic gain", "麦克风增益" },
    { "Auto-sleep", "自动息屏" },
    { "Text size", "字号" },
    { "Phone setup", "手机配对" },
    { "Status", "状态" },
    { "Battery", "电池" },
    { "Reset pairing", "重置配对" },
    { "Screen off", "息屏" },
    { "Power off", "关机" },
    { "Close menu", "关闭菜单" },
    { "Change", "修改" },
    { "Toggle", "开关" },
    { "Open", "打开" },
    { "Select", "选择" },
    { "Close", "关闭" },
    { "Back", "返回" },
    { "Reset", "重置" },
    { "Cancel", "取消" },
    { "Esc Back", "Esc 返回" },
    { "Esc Cancel", "Esc 取消" },
    { "On", "开" },
    { "Off", "关" },
    { "Not set", "未设置" },
    { "Joining", "连接中" },
    { "Failed", "失败" },
    { "Not found", "未找到" },
    { "Never", "从不" },
    { "30 s", "30 秒" },
    { "1 min", "1 分钟" },
    { "2 min", "2 分钟" },
    { "5 min", "5 分钟" },
    { "10 min", "10 分钟" },
    { "Small", "小" },
    { "Medium", "中" },
    { "Large", "大" },
    { "Custom", "自定义" },
    { "MENU", "设置" },
    { "MENU  ^v Move  <> Change", "设置  ^v 选择  <> 修改" },
    { "STATUS", "状态" },
    { "BATTERY", "电池" },
    { "POWER OFF", "关机" },
    { "RESET PAIRING", "重置配对" },
    { "Wi-Fi %s\nIP    %s\nLink  %s\nMuse  %s\nPhone %s\nPower %s\nVer   %s", "网络 %s\nIP   %s\n链路 %s\nMuse %s\n手机 %s\n电源 %s\n版本 %s" },
    { "off", "已关闭" },
    { "offline", "离线" },
    { "Connected", "已连接" },
    { "Starting", "正在启动" },
    { "Ready to pair", "等待配对" },
    { "App connected", "应用已连接" },
    { "Confirm pairing", "确认配对" },
    { "Connecting", "正在连接" },
    { "Online", "在线" },
    { "Offline", "离线" },
    { "Error", "出错" },
    { "Not set up", "尚未设置" },
    { "Saved", "已保存" },
    { "Can't connect", "无法连接" },
    { "Unplug USB to\nmeasure how\nlong the\nbattery lasts.", "拔掉 USB 后\n开始测量\n待机耗电。" },
    { "%s %s\nBatt  %d>%d%%\nRate  %s\nFull  %s\nOff   %s\nSleep %s\nWakes %s\nBusy  %s", "%s %s\n电量 %d>%d%%\n耗电 %s\n满电 %s\n息屏 %s\n休眠 %s\n唤醒 %s\n忙碌 %s" },
    { "On batt", "电池供电" },
    { "Last run", "上次记录" },
    { "Turn Muse off?\n\nPress the %s button to turn it back on.", "要关闭手表吗？\n\n按%s键开机。" },
    { "Forget Wi-Fi and the Muse app pairing, then restart?", "忘记无线网络和 Muse 配对后重启？" },
    { "Pairing code", "配对码" },
    { "Enter on phone", "在手机输入" },
    { "Enter it on your phone", "请在手机上输入" },
    { "Tap screen", "触摸屏幕" },
    { "Tap to confirm pairing", "轻触确认配对" },
    { "Press", "按下" },
    { "Press button", "按键" },
    { "%s button", "%s键" },
    { "Press the %s button", "按%s键" },
    { "Muse app", "Muse 应用" },
    { "Pair with Muse app", "与 Muse 应用配对" },
    { "No saved network nearby", "附近没有已保存的网络" },
    { "Can't join; retrying", "连接失败，正在重试" },
    { "Joining...", "正在连接" },
    { "Wi-Fi off", "无线网络已关闭" },
    { "Starting...", "正在启动" },
    { "red", "红" },
    { "red bottom-left", "左下红" },
    { "top-left", "左上" },
    { "top-right", "右上" },
    { "yellow", "黄" },
    { "blue", "蓝" },
    { "screen", "屏幕" },
};
#define STATUS ZH_STATUS
#define MESSAGES ZH_MESSAGES
#define MENU ZH_MENU
#endif

#if CONFIG_MUSE_UI_LANG_VI || CONFIG_MUSE_UI_LANG_ZH
static const char *lookup(const muse_lang_entry_t *t, size_t n, const char *en)
{
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(t[i].en, en)) {
            return t[i].text;
        }
    }
    return en;
}

const char *muse_lang_status(const char *en)
{
    return lookup(STATUS, sizeof(STATUS) / sizeof(STATUS[0]), en);
}

const char *muse_lang_message(const char *en)
{
    return lookup(MESSAGES, sizeof(MESSAGES) / sizeof(MESSAGES[0]), en);
}

const char *muse_lang_menu(const char *en)
{
    return lookup(MENU, sizeof(MENU) / sizeof(MENU[0]), en);
}

#endif

#if CONFIG_MUSE_UI_LANG_VI
/* LVGL's Montserrat has ASCII and its symbols; fonts/muse_font_vi_<px>.c has
 * Montserrat's Vietnamese letters at the same size and baseline. */
#if LV_FONT_MONTSERRAT_12
LV_FONT_DECLARE(muse_font_vi_12)
#endif
LV_FONT_DECLARE(muse_font_vi_14)
LV_FONT_DECLARE(muse_font_vi_20)
LV_FONT_DECLARE(muse_font_vi_28)

const lv_font_t *muse_lang_montserrat(const lv_font_t *font)
{
    static const struct {
        const lv_font_t *base, *letters;
    } SIZES[] = {
#if LV_FONT_MONTSERRAT_12
        { &lv_font_montserrat_12, &muse_font_vi_12 },
#endif
        { &lv_font_montserrat_14, &muse_font_vi_14 },
        { &lv_font_montserrat_20, &muse_font_vi_20 },
        { &lv_font_montserrat_28, &muse_font_vi_28 },
    };
    static lv_font_t copies[sizeof(SIZES) / sizeof(SIZES[0])];
    for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
        if (font == SIZES[i].base) {
            if (!copies[i].get_glyph_dsc) {
                copies[i] = *font;
                copies[i].fallback = SIZES[i].letters;
            }
            return &copies[i];
        }
    }
    return font;
}
#elif CONFIG_MUSE_UI_LANG_ZH
LV_FONT_DECLARE(muse_font_cjk_noto_16)

const lv_font_t *muse_lang_montserrat(const lv_font_t *font)
{
    const lv_font_t *const base[] = {
#if LV_FONT_MONTSERRAT_12
        &lv_font_montserrat_12,
#endif
        &lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_20,
        &lv_font_montserrat_28, &lv_font_unscii_16,
    };
    static lv_font_t copies[sizeof(base) / sizeof(base[0])];
    for (size_t i = 0; i < sizeof(base) / sizeof(base[0]); i++) {
        if (base[i] == font) {
            if (!copies[i].get_glyph_dsc) {
                copies[i] = *font;
                copies[i].fallback = &muse_font_cjk_noto_16;
            }
            return &copies[i];
        }
    }
    return font;
}
#endif

#if !CONFIG_MUSE_UI_LANG_VI && !CONFIG_MUSE_UI_LANG_ZH
const char *muse_lang_status(const char *en)
{
    return en;
}

const char *muse_lang_message(const char *en)
{
    return en;
}

const char *muse_lang_menu(const char *en)
{
    return en;
}

#if !CONFIG_MUSE_UI_FREEINK
const lv_font_t *muse_lang_montserrat(const lv_font_t *font)
{
    return font;
}
#endif
#endif

#if !CONFIG_MUSE_UI_FREEINK
#if CONFIG_MUSE_LATIN_FONT
LV_FONT_DECLARE(muse_font_latin_16)
#endif

const lv_font_t *muse_lang_label_font(const char *text, const lv_font_t *plain)
{
#if CONFIG_MUSE_UI_LANG_ZH
    if (muse_text_has_cjk(text)) {
        return muse_lang_montserrat(plain == &lv_font_unscii_8 ? &lv_font_unscii_16 : plain);
    }
#endif
#if CONFIG_MUSE_LATIN_FONT
    if (muse_text_has_latin(text)) {
        return &muse_font_latin_16;
    }
#endif
    (void)text;
    return plain;
}
#endif
