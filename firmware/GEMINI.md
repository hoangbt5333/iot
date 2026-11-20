# Quy Tắc Phát Triển Firmware ESP32 (Arduino C++ / PlatformIO)

> Áp dụng cho: Mọi mã nguồn trong thư mục `firmware/` (`node-environment` và `node-security`).

---

## 1. Nguyên Tắc Lập Trình Nhúng & Quản Lý Thời Gian

1. **Tuyệt đối không dùng hàm `delay()` gây chặn (blocking):**
   - Vòng lặp `loop()` chính phải luôn được giữ ở trạng thái non-blocking để xử lý liên tục mạng Wi-Fi, MQTT client, đọc cảm biến và giám sát an ninh.
   - Quản lý khoảng thời gian (sampling rate, heartbeat, LED blink) bắt buộc dùng cơ chế kiểm tra `millis()` không chặn hoặc FreeRTOS Software Timer / FreeRTOS Tasks.
   - Ví dụ:
     ```cpp
     uint32_t now = millis();
     if (now - last_sample_ms >= SAMPLE_INTERVAL_MS) {
         last_sample_ms = now;
         // thực hiện công việc
     }
     ```

2. **Chống phân mảnh bộ nhớ Heap (Memory Safety):**
   - Hạn chế tối đa việc sử dụng class `String` của Arduino trong vòng lặp liên tục hoặc khi xây dựng payload JSON lớn.
   - Ưu tiên sử dụng chuỗi tĩnh cố định kích thước (`char buffer[N]`, `snprintf`) hoặc thư viện `ArduinoJson` với bộ nhớ được cấu hình hợp lý (`StaticJsonDocument` hoặc `JsonDocument` trên heap có kiểm soát).
   - Mọi hàng đợi tin nhắn (`offline_queue`) bắt buộc phải có dung lượng giới hạn trên (`BOUNDED_CAPACITY`) và có chính sách loại bỏ tin cũ nhất khi đầy (drop oldest) để tránh cạn kiệt RAM gây tràn heap (Heap Exhaustion / Kernel Panic).

---

## 2. Quy Tắc Phần Cứng & An Toàn GPIO

1. **Cảnh báo mức điện áp logic:**
   - ESP32 hoạt động ở mức logic **3.3V**. Tuyệt đối không nối trực tiếp tín hiệu 5V từ cảm biến hoặc mạch ngoài vào chân GPIO hay ADC của ESP32.
   - Module DHT11, BH1750, PIR, Reed Switch phải được cấp nguồn và mức logic tương thích 3.3V.

2. **Phân bổ chân GPIO an toàn:**
   - **Tuyệt đối tránh GPIO 6 – 11:** Đây là các chân kết nối trực tiếp với chip nhớ SPI Flash tích hợp của ESP32.
   - **Cẩn trọng với Strapping Pins (GPIO 0, 2, 12, 15):** Có thể làm vi điều khiển không khởi động được nếu bị kéo mức logic sai khi cấp nguồn.
   - **Quy tắc ADC:** Khi bật Wi-Fi, bộ chuyển đổi **ADC2 bị vô hiệu hóa**. Tất cả chân đọc Analog (như LDR trên Wokwi) bắt buộc phải dùng thuộc kênh **ADC1** (GPIO 32, 33, 34, 35, 36, 39).
   - **Chân chỉ nhận tín hiệu vào (Input-Only):** GPIO 34, 35, 36, 39 là chân input-only, **không có điện trở pull-up/pull-down nội**, cần gắn điện trở ngoài nếu dùng digital input.

3. **Chân GPIO hiện tại của dự án (Đối chiếu với `docs/PLAN_PHAN_CUNG_FIRMWARE.md`):**
   - **Node A:**
     - DHT DATA: `GPIO 16`
     - BH1750 (Real I2C): `SDA = GPIO 21`, `SCL = GPIO 22`
     - Wokwi LDR (Sim): `GPIO 34` (ADC1)
     - LED Đèn: `GPIO 26` (qua điện trở hạn dòng)
     - Quạt MOSFET: `GPIO 25`
   - **Node B:**
     - Cảm biến PIR: `GPIO 27`
     - Reed Switch (Cửa): `GPIO 32` (kèm pull-up)
     - Còi Buzzer: `GPIO 18`
     - Servo SG90: `GPIO 19` (PWM tín hiệu, nguồn 5V ngoài chung GND)
     - LED Trạng thái: `GPIO 26`
     - Nút gạt vật lý Home/Away: `GPIO 33` (kèm pull-up)

---

## 3. Tách Biệt Mô Phỏng (`sim`) và Phần Cứng Thật (`real`)

Mã nguồn được cấu hình qua preprocessor flags trong `platformio.ini`:
- **`env:sim` (`-D NODE_ENV_SIM=1`):** Chạy trên giả lập Wokwi.
  - Node A dùng: `Wokwi DHT22` (thay thế chức năng) và `LDR Photoresistor` (qua ADC1).
  - Wi-Fi kết nối: SSID `Wokwi-GUEST`, không mật khẩu.
- **`env:real` (`-D NODE_ENV_REAL=1`):** Nạp vào mạch thật ESP32 DevKit.
  - Node A dùng: `DHT11 thật` và `BH1750 thật` (I2C).
- **Tầng Driver trừu tượng (Abstraction Layer):**
  - Tầng logic kinh doanh (`automation/`, `security/`, `command_guard/`) **không được phép phụ thuộc trực tiếp** vào thư viện phần cứng cụ thể. Mọi giao tiếp với phần cứng phải đi qua interface/adapter (ví dụ `IDhtDriver`, `ILightDriver`, `ISecurityInputs`).

---

## 4. Kiểm Thử Đơn Vị (Unit Testing)

1. Mọi thuật toán hoặc logic thuần túy (tính toán trễ Hysteresis, máy trạng thái State Machine, Command Guard, giải mã thời gian UTC) phải có unit test tương ứng trong thư mục `test/`.
2. Unit test phải chạy độc lập trên máy chủ thông qua môi trường `[env:native]` với framework `Unity` (`pio test -e native`) mà không cần nạp lên phần cứng thật.
