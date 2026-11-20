---
name: wokwi-simulation
description: >-
  Use this skill when creating, modifying, wiring, or troubleshooting Wokwi simulation circuits (diagram.json), simulation config (wokwi.toml), virtual sensors/actuators, or virtual networking (Wokwi-GUEST) for ESP32 Node A and Node B.
---

# Wokwi Simulation & Circuit Wiring Guide

Tài liệu hướng dẫn cấu hình mạch mô phỏng, đấu nối linh kiện ảo và chạy mô phỏng firmware ESP32 trên extension Wokwi.

---

## 1. Cấu Trúc File Cần Thiết Cho Wokwi

Mỗi node (`node-environment`, `node-security`) chứa 2 file điều khiển mô phỏng:

1. **`wokwi.toml`**: Khai báo đường dẫn file binary và file ELF để Wokwi nạp mã:
   ```toml
   [wokwi]
   version = 1
   firmware = ".pio/build/sim/firmware.bin"
   elf = ".pio/build/sim/firmware.elf"
   ```
   *Lưu ý quan trọng:* Trước khi ấn chạy mô phỏng, bắt buộc phải biên dịch môi trường `sim` thành công bằng lệnh:
   `pio run -d firmware/<node> -e sim`

2. **`diagram.json`**: Mô tả sơ đồ chân, linh kiện và dây nối trên canvas đồ họa của Wokwi.

---

## 2. Danh Mục Linh Kiện Chuẩn & Sơ Đồ Chân (Pinout)

### 2.1. Node A (Môi trường & Tiện nghi)
*Đường dẫn:* `firmware/node-environment/diagram.json`

| Linh kiện ảo Wokwi | Type ID Wokwi | Chân kết nối ESP32 | Ghi chú |
| :--- | :--- | :--- | :--- |
| **Bo mạch ESP32** | `board-esp32-devkit-c-v4` | - | Bo chính 38 chân |
| **Cảm biến nhiệt/ẩm** | `wokwi-dht22` | `VCC` -> 3V3, `GND` -> GND, `SDA` -> `GPIO 16` | Mô hình thay thế cho DHT11 (Wokwi không có DHT11 chuẩn) |
| **Cảm biến ánh sáng** | `wokwi-photoresistor-sensor` | `VCC` -> 3V3, `GND` -> GND, `AO` -> `GPIO 34` | Đọc qua ADC1 (GPIO 34) |
| **Đèn LED Đèn** | `wokwi-led` (color: yellow) | `A` -> Điện trở 220Ω -> `GPIO 26`, `C` -> GND | Đèn chiếu sáng |
| **Đèn LED Quạt** | `wokwi-led` (color: blue) | `A` -> Điện trở 220Ω -> `GPIO 25`, `C` -> GND | Đại diện cho quạt 5V / MOSFET |
| **Điện trở hạn dòng** | `wokwi-resistor` | 220Ω | Hạn dòng cho các LED |

### 2.2. Node B (An ninh & Cửa)
*Đường dẫn:* `firmware/node-security/diagram.json`

| Linh kiện ảo Wokwi | Type ID Wokwi | Chân kết nối ESP32 | Ghi chú |
| :--- | :--- | :--- | :--- |
| **Bo mạch ESP32** | `board-esp32-devkit-c-v4` | - | Bo chính |
| **Cảm biến chuyển động** | `wokwi-pir-motion-sensor` | `VCC` -> 3V3, `GND` -> GND, `OUT` -> `GPIO 27` | Giả lập phát hiện người |
| **Cửa (Reed switch)** | `wokwi-slide-switch` | `1` -> `GPIO 32`, `2` -> GND | Gạt công tắc giả lập đóng/mở cửa |
| **Nút Home/Away** | `wokwi-slide-switch` | `1` -> `GPIO 33`, `2` -> GND | Chuyển chế độ tại chỗ |
| **Còi báo động** | `wokwi-buzzer` | `1` -> GND, `2` -> `GPIO 18` | Báo động khi đột nhập |
| **Động cơ Servo** | `wokwi-servo` | `GND` -> GND, `V+` -> 5V, `PWM` -> `GPIO 19` | Giả lập cơ cấu mở cửa |
| **LED Trạng thái** | `wokwi-led` (color: red) | `A` -> Điện trở 220Ω -> `GPIO 26`, `C` -> GND | Chỉ báo trạng thái an ninh |

---

## 3. Cú Pháp Nối Dây Trong `diagram.json`

Trong `diagram.json`, mảng `connections` định nghĩa các đường dây nối theo format:
```json
"connections": [
  [ "esp:3V3", "dht22:VCC", "red", [ "v0" ] ],
  [ "esp:GND.1", "dht22:GND", "black", [ "v0" ] ],
  [ "esp:16", "dht22:SDA", "green", [ "v0" ] ],
  [ "esp:26", "r_lamp:1", "gold", [ "h0" ] ],
  [ "r_lamp:2", "led_lamp:A", "gold", [ "v0" ] ],
  [ "led_lamp:C", "esp:GND.2", "black", [ "v0" ] ]
]
```

**Quy tắc nối dây:**
- Màu dây nên đặt theo chuẩn: `red` (Nguồn VCC 3.3V/5V), `black` (GND), `green`/`blue`/`gold` (Tín hiệu GPIO).
- Khi kết nối LED, luôn nối qua điện trở `wokwi-resistor` để tuân thủ logic phần cứng.

---

## 4. Mô Phỏng Mạng Wi-Fi & MQTT Trong Wokwi

1. **Thông số Wi-Fi ảo của Wokwi:**
   - **SSID:** `Wokwi-GUEST`
   - **Mật khẩu:** `""` (chuỗi rỗng, không có mật khẩu)
   - *Cơ chế:* Wokwi cung cấp một mạng NAT ảo cho phép ESP32 gửi/nhận gói tin TCP/IP ra ngoài Internet hoặc broker công cộng (như `test.mosquitto.org` hoặc `broker.hivemq.com`).

2. **Kết nối về Mosquitto Local trên máy tính:**
   - Để kết nối từ Wokwi ảo về broker Mosquitto chạy trên máy tính host (`localhost:1883`), cần sử dụng tiện ích **Wokwi IoT Gateway** (chạy trên máy tính để tạo đường hầm mạng).
   - Nếu chưa cài Wokwi IoT Gateway, nên trỏ broker trong firmware `sim` sang một broker cloud công cộng để test.

---

## 5. Quy Trình Khởi Động & Debug Mô Phỏng

1. Mở file `firmware/node-environment/src/main.cpp` hoặc `diagram.json`.
2. Mở Command Palette trong IDE (`Ctrl+Shift+P` / `F1`).
3. Gõ `Wokwi: Start Simulator` để bật mô phỏng.
4. Mở cửa sổ **Serial Monitor** tích hợp của Wokwi để quan sát log in ra từ `Serial.println()` ở baudrate `115200`.
