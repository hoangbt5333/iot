---
name: iot-contract-sync
description: >-
  Use this skill when implementing, checking, or updating MQTT payload schemas, ThingsBoard telemetry, RPC methods, attributes, or JSON serialization/deserialization in ESP32 firmware according to contracts/thingsboard-v1.md or contracts/mqtt-v1.md.
---

# IoT Contract & Message Synchronization Guide

Tài liệu hướng dẫn đồng bộ hóa cấu trúc bản tin MQTT giữa mã nguồn C++ của ESP32 và các tài liệu hợp đồng giao tiếp trong thư mục `contracts/`.

---

## 1. Chuẩn Giao Tiếp ThingsBoard (`contracts/thingsboard-v1.md`)

Đây là chuẩn giao tiếp chính đang được nhóm đề xuất cho Node A và Node B.

### 1.1. Telemetry Upload (`v1/devices/me/telemetry`)
Firmware đóng gói dữ liệu cảm biến và trạng thái thiết bị gửi lên định kỳ (mặc định 5s hoặc khi có biến động trạng thái).

**Cấu trúc JSON Node A (Môi trường):**
```json
{
  "ts": 1732101234000,
  "values": {
    "temperature": 28.5,
    "humidity": 65.0,
    "light_lux": 180.0,
    "lamp_state": true,
    "fan_state": false,
    "mode": "AUTO"
  }
}
```
*Lưu ý:* Khi chạy mô phỏng Wokwi LDR chưa hiệu chuẩn lux, trường độ sáng xuất dạng `"light_raw": 1250` và `"source": "LDR_SIM"`.

**Cấu trúc JSON Node B (An ninh):**
```json
{
  "ts": 1732101234000,
  "values": {
    "motion": false,
    "door_open": false,
    "security_mode": "ARMED",
    "alarm_state": false,
    "servo_angle": 0
  }
}
```

---

### 1.2. Server-Side RPC (`v1/devices/me/rpc/request/{requestId}`)
Lệnh điều khiển từ Cloud / App Android gửi xuống cho ESP32.

ESP32 lắng nghe topic: `v1/devices/me/rpc/request/+`

| Node | Tên Method | Tham số (`params`) | Ý nghĩa |
| :--- | :--- | :--- | :--- |
| **Node A** | `setLamp` | `{"enabled": true/false}` | Bật/tắt đèn (chỉ áp dụng ở mode MANUAL) |
| **Node A** | `setFan` | `{"enabled": true/false}` | Bật/tắt quạt (chỉ áp dụng ở mode MANUAL) |
| **Node A** | `setMode` | `{"mode": "AUTO" / "MANUAL"}` | Chuyển chế độ hoạt động |
| **Node B** | `setSecurityMode` | `{"mode": "HOME" / "AWAY" / "DISARMED"}` | Đổi chế độ an ninh |
| **Node B** | `setServoDoor` | `{"angle": 0..180}` | Điều khiển mở/đóng cửa servo |
| **Node B** | `clearAlarm` | `{}` | Tắt còi báo động khẩn cấp |

**Quy tắc phản hồi RPC:**
ESP32 sau khi thực thi lệnh phải gửi phản hồi về topic `v1/devices/me/rpc/response/{requestId}`:
```json
{
  "success": true,
  "current_state": true,
  "message": "OK"
}
```

---

### 1.3. Shared Attributes (Cập nhật ngưỡng từ xa)
ThingsBoard đẩy cấu hình ngưỡng xuống qua topic `v1/devices/me/attributes`:
- `temp_high_threshold`: Ngưỡng nhiệt độ bật quạt (ví dụ: `30.0`).
- `temp_low_threshold`: Ngưỡng nhiệt độ tắt quạt (ví dụ: `28.0`).
- `light_threshold_on`: Ngưỡng lux bật đèn (ví dụ: `100.0`).
- `light_threshold_off`: Ngưỡng lux tắt đèn (ví dụ: `150.0`).

---

## 2. Checklist Khi Viết Mã Nguồn C++ Với `ArduinoJson`

1. **Kích thước buffer:**
   - Dùng `JsonDocument` kích thước vừa vặn (ví dụ 256–512 bytes) khai báo cục bộ trên stack trong hàm tạo bản tin, không tạo Document tĩnh toàn cục gây lãng phí RAM.
2. **Kiểm tra kiểu dữ liệu:**
   - Trường boolean dùng `doc["is_lamp_on"] = lamp_on;` (sinh ra `true`/`false`).
   - Không xuất chuỗi `"true"` hay số `1` cho các cờ boolean.
3. **Phòng vệ lỗi parse (CommandGuard):**
   - Khi nhận RPC payload từ broker, phải kiểm tra `DeserializationError` trước khi đọc trường.
   - Kiểm tra đầy đủ sự tồn tại của `doc["method"]` và kiểu của `doc["params"]` trước khi áp dụng vào actuator.
