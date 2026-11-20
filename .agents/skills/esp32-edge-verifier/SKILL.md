---
name: esp32-edge-verifier
description: >-
  Use this skill when designing, testing, verifying, or debugging local edge automation logic, hysteresis threshold algorithms, security state machines, or offline queuing in ESP32 firmware.
---

# ESP32 Edge Logic & Verification Guide

Tài liệu hướng dẫn thiết kế và kiểm chứng các quy tắc tự động hóa tại biên (Edge Rules) độc lập với kết nối mạng trên ESP32.

---

## 1. Thuật Toán Trễ Hysteresis (Node A – Môi Trường)

Để tránh hiện tượng đóng cắt liên tục (chattering) khi giá trị cảm biến dao động quanh ngưỡng, bộ điều khiển môi trường áp dụng dải trễ (deadband / hysteresis):

### 1.1. Logic Điều Khiển Quạt (Nhiệt độ)
- **Ngưỡng bật quạt:** `T >= 30.0°C`
- **Ngưỡng tắt quạt:** `T <= 28.0°C`
- **Vùng trễ (`28.0°C < T < 30.0°C`):** Giữ nguyên trạng thái trước đó của quạt.

```cpp
bool evaluate_fan_state(float temp_c, bool current_fan_state) {
    if (isnan(temp_c)) return false; // Trạng thái an toàn khi lỗi cảm biến
    if (temp_c >= 30.0f) return true;
    if (temp_c <= 28.0f) return false;
    return current_fan_state; // Giữ nguyên
}
```

### 1.2. Logic Điều Khiển Đèn (Ánh sáng)
- **Ngưỡng bật đèn:** `Lux < 100.0`
- **Ngưỡng tắt đèn:** `Lux > 150.0`
- **Vùng trễ (`100.0 <= Lux <= 150.0`):** Giữ nguyên trạng thái đèn trước đó.

---

## 2. Máy Trạng Thái An Ninh (Node B – An Ninh & Cửa)

Node B hoạt động theo mô hình Máy trạng thái hữu hạn (FSM):

```
[DISARMED] ──(Lệnh ARM / Bật công tắc)──► [ARMING (Chờ rời nhà)] ──(Hết delay)──► [ARMED]
     ▲                                                                               │
     │                                                                               ▼ (Cửa mở / PIR kích hoạt)
     └──────────────────────────(Lệnh DISARM / Tắt còi)───────────────────────── [ALARMING]
```

### 2.1. Các trạng thái cốt lõi
1. **`DISARMED` (Ở nhà / Vô hiệu hóa):** Không hú còi khi mở cửa hoặc có chuyển động.
2. **`ARMING`:** Khoảng thời gian trễ (ví dụ: 10 giây) cho phép người dùng ra khỏi nhà sau khi kích hoạt chế độ Away.
3. **`ARMED` (Bật giám sát an ninh):**
   - Nếu công tắc cửa (Reed switch) chuyển sang `OPEN`: Kích hoạt báo động ngay lập tức (`ALARMING`).
   - Nếu cảm biến PIR phát hiện chuyển động liên tục: Ghi nhận sự kiện hoặc kích hoạt báo động tùy cấu hình.
4. **`ALARMING` (Đang báo động):**
   - Bật còi buzzer ngắt quãng hoặc liên tục.
   - Nhấp nháy LED an ninh đỏ.
   - Trạng thái chốt (latch state): Tiếp tục báo động cho đến khi có lệnh giải trừ (`DISARM` hoặc `clearAlarm`).

---

## 3. Quy Trình Viết & Chạy Unit Test Cho Edge Rules

Tất cả các logic trên được viết độc lập với Arduino framework (trong thư mục `lib/`) để chạy unit test bằng môi trường `native` với PlatformIO:

### 3.1. Cấu trúc một bài test mẫu trong `test/test_automation.cpp`
```cpp
#include <unity.h>
#include "automation_rules.h"

void test_fan_hysteresis() {
    // Ban đầu quạt đang tắt, nhiệt độ lên 29°C -> vẫn tắt
    TEST_ASSERT_FALSE(evaluate_fan_state(29.0f, false));
    
    // Nhiệt độ vượt 30°C -> quạt phải bật
    TEST_ASSERT_TRUE(evaluate_fan_state(30.2f, false));
    
    // Khi đang bật, nhiệt độ hạ xuống 29°C (trong vùng trễ) -> vẫn phải bật
    TEST_ASSERT_TRUE(evaluate_fan_state(29.0f, true));
    
    // Nhiệt độ hạ xuống 27.5°C -> quạt phải tắt
    TEST_ASSERT_FALSE(evaluate_fan_state(27.5f, true));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_fan_hysteresis);
    return UNITY_END();
}
```

### 3.2. Chạy kiểm thử:
```powershell
pio test -d firmware/node-environment -e native
```
