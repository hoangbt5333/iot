# KẾ HOẠCH PHẦN CỨNG VÀ FIRMWARE ESP32 – SMART HOME IoT

> **Đề tài:** Hệ thống nhà thông minh IoT dùng ESP32 và ứng dụng Android Kotlin – Giám sát môi trường, điều khiển thiết bị và tự động hóa cục bộ.  
> **Nhóm:** 9 – Học phần IT4735 IoT và Ứng dụng.  
> **Người phụ trách mảng này:** Phạm Ngọc Hưng (20235342) – Thiết bị / Firmware / Tích hợp hệ thống.  
> **Trạng thái tài liệu:** Phương án kỹ thuật để trao đổi và triển khai; **chưa phải nhật ký đã lắp ráp hay kết quả nghiệm thu**. **Cập nhật linh kiện:** Node A sử dụng **DHT11 đã có sẵn**, thay cho phương án SHT31/DHT22 nêu trong BT02.  
> **Tài liệu gốc của nhóm:** [Bao_cao_BT02_IoT.docx](Bao_cao_BT02_IoT.docx).  
> **Phạm vi:** Chỉ mảng phần cứng, ESP32, firmware, mô phỏng và giao tiếp từ thiết bị tới broker; Node.js/PostgreSQL/Android do các thành viên khác phụ trách.
> **Cảnh báo chuyển đổi giao tiếp (`LEGACY` vs `DRAFT`):** Mô hình Mosquitto + `smarthome/v1/...` bên dưới (Mục 2 và Mục 7) hiện là tài liệu **`LEGACY / REFERENCE`**. Hướng đề xuất ThingsBoard nằm tại [contracts/thingsboard-v1.md](../contracts/thingsboard-v1.md) (`DRAFT / PROPOSED`, **`TB-DEC-01`..`TB-DEC-09` đang `OPEN`**); xem ghi chú chi tiết trước [Mục 7](#7-hợp-đồng-mqtt-sơ-bộ-cho-backend-chưa-chốt) và [HANDOFF_BACKEND_ANDROID.md](HANDOFF_BACKEND_ANDROID.md).

## 1. Mục tiêu và ranh giới trách nhiệm

Hình thành hai nút ESP32 kết nối Wi-Fi 2,4 GHz, gửi telemetry/sự kiện bằng MQTT, nhận lệnh từ hệ thống và **phản ứng cục bộ (edge rules) ngay cả khi broker/backend không hoạt động**. Tài liệu này cụ thể hóa phần thiết bị của BT02, **không thay thế hoặc sửa yêu cầu đã thống nhất của nhóm**.

| Thành phần | Phụ trách | Ranh giới cần bàn giao |
| --- | --- | --- |
| ESP32, cảm biến, mạch chấp hành, firmware, kiểm thử thiết bị | Phạm Ngọc Hưng | GPIO, đặc tả thiết bị, MQTT topics/payload, quy tắc trạng thái |
| Mosquitto, Node.js/Express, PostgreSQL, API/WebSocket, Docker | Khương Anh Tài | Địa chỉ broker, xác thực, lệnh từ backend, lưu và xử lý dữ liệu |
| Android Kotlin/Jetpack Compose, dashboard/control/history/alarm, báo cáo | Bùi Trung Hoàng | Mô hình thiết bị, tên chỉ số/đơn vị, thao tác điều khiển và trạng thái xác nhận |

**Ưu tiên tích hợp:** Chốt sớm bản hợp đồng MQTT ở mục 7 để backend và Android có thể phát triển song song; **không cần đợi lắp xong toàn bộ mạch thật**.

## 2. Quyết định kỹ thuật và giới hạn

- **Nền tảng phần cứng đích:** 2 × ESP32 DevKit sử dụng module ESP-WROOM-32 (kiểm tra phiên bản chân trước khi mua/nối).
- **Firmware:** C++/Arduino framework dưới PlatformIO; tổ chức module độc lập, không viết tất cả logic trong một vòng loop.
- **Mô phỏng:** Wokwi + PlatformIO; Antigravity IDE là môi trường agent viết code. Nếu extension không hoạt động trong Antigravity, dùng VS Code cho phần mô phỏng trên cùng thư mục source.
- **Mạch thật:** Lắp từng phần bằng breadboard, dùng nguồn DC 5V an toàn cho tải thích hợp; cảm biến DHT11/ESP32 cần mức logic 3,3V; không nối điện lưới vào breadboard.
- **Mạng:** Mosquitto làm broker; MQTT/TCP cho môi trường LAN, khi đưa Internet vào phạm vi triển khai phải xem xét xác thực broker, ACL và TLS.
- **Hai node Wokwi:** Dùng **hai project/sơ đồ riêng** (Wokwi không chạy hai MCU trong cùng sơ đồ); kiểm thử liên thông qua broker bằng mạng mô phỏng/gateway phù hợp. Phần mềm có thể build local, nhưng không suy ra toàn bộ mô phỏng chạy offline.
- **Không dùng Proteus làm nền tảng chính** vì dự án tập trung ESP32 Arduino C++/MQTT; chỉ cân nhắc Proteus nếu cần phân tích đặc thù mạch điện.
- **Bảo toàn hành vi khi thay sensor:** Phân tách **DHT11 phần cứng thật / DHT22 mô phỏng Wokwi** và BH1750/LDR ở tầng driver. Business/edge rules không phụ thuộc cứng vào thư viện hay định dạng tín hiệu của cảm biến.

## 3. Thiết kế hai node

### 3.1. Node A – Môi trường và tiện nghi

**Đầu vào thực tế:** **DHT11 đã có sẵn** (nhiệt độ/độ ẩm) và BH1750 (độ sáng lux). **Wokwi:** dùng DHT22 làm mô hình *thay thế* cho chức năng nhiệt độ/độ ẩm và LDR thay cảm biến BH1750; đây không phải mô phỏng chính xác phần cứng DHT11.  
**Đầu ra:** LED tượng trưng đèn; quạt DC 5V dùng MOSFET driver để bật/tắt hoặc PWM khi phần cứng cho phép.  
**Đầu ra logic:** temperature_c, humidity_pct, light_lux/light_raw, lamp_on, fan_on, device_mode (AUTO/MANUAL), online.

**Edge rules đề xuất, cần nhóm thống nhất ngưỡng sau khi thử:**
1. Ở AUTO, đèn bật khi ánh sáng nhỏ hơn **100 lux**, tắt khi lớn hơn **150 lux** (hysteresis).
2. Ở AUTO, quạt bật khi nhiệt độ từ **30°C**, tắt khi xuống dưới **28°C** (hysteresis). **DHT11 có sai số nhiệt độ thường khoảng ±2°C**, nên chỉ coi đây là ngưỡng demo; cần kiểm nghiệm thực tế, cân nhắc lọc dữ liệu/thời gian giữ trạng thái tối thiểu để hạn chế đóng cắt liên tục.
3. Ở MANUAL, lệnh hợp lệ từ ứng dụng có quyền ưu tiên theo phiên điều khiển; không để vòng AUTO ghi đè ngay lập tức. Khi mất mạng, giữ chính sách cục bộ được cấu hình gần nhất và không thực thi lại lệnh đã hết hạn.
4. Khi cảm biến trả về giá trị không hợp lệ/mất kết nối, ghi trạng thái lỗi thay vì xuất 0°C/0 lux như số đo thật; cơ cấu chấp hành chuyển sang trạng thái an toàn đã định nghĩa.

**Cần lưu ý:** LDR mô phỏng không đo chính xác như BH1750. Nếu chưa hiệu chuẩn, gửi light_raw và source=LDR_SIM; không khẳng định số lux LDR là phép đo vật lý chuẩn.

### 3.2. Node B – An ninh và cửa

**Đầu vào:** PIR HC-SR501, reed switch + nam châm, công tắc Home/Away vật lý (đề xuất bổ sung).  
**Đầu ra:** active buzzer, LED báo trạng thái, servo SG90 đại diện **mô hình cơ cấu cửa**, không phải khóa an ninh thật.  
**Đầu ra logic:** motion_detected, door_open, security_mode (HOME/AWAY), alarm_on, servo_angle, online.

**Edge rules đề xuất:**
1. Khi chế độ **AWAY** và phát hiện **cửa mở**, kích hoạt cảnh báo cục bộ (buzzer/LED) ngay cả khi mất MQTT.
2. PIR có thể là điều kiện cảnh báo bổ sung trong AWAY, nhưng phải thống nhất rõ mức ưu tiên, độ trễ giữ tín hiệu và xử lý kích hoạt lặp; không coi mức HIGH kéo dài là nhiều sự kiện mới.
3. Công tắc vật lý cho phép thay đổi HOME/AWAY tại chỗ khi mất mạng. Quy tắc giải quyết xung đột với lệnh từ app (ưu tiên và thời điểm áp dụng) phải được định nghĩa trước khi triển khai.
4. Servo chỉ quay theo lệnh đã được kiểm tra quyền và giới hạn góc; **không tự động mở khóa cửa thật** khi nhận được một bản tin MQTT bất kỳ.

## 4. BOM – Danh mục vật tư cần chuẩn bị

Bảng phân biệt **linh kiện trong đề cương BT02** và **phụ kiện/điều chỉnh kỹ thuật**. Giá trong BT02 chỉ là dự toán của nhóm, không phải báo giá tại ngày mua.

| Vật tư | Số lượng | Nguồn yêu cầu | Vai trò / lưu ý mua |
| --- | ---: | --- | --- |
| ESP32 DevKit, ESP-WROOM-32 | 2 | BT02 | Hai node độc lập; ưu tiên bo phổ biến có USB-UART |
| **DHT11 có sẵn** (module 3 chân hoặc cảm biến 4 chân, cần kiểm tra) | 1 | **Điều chỉnh từ BT02**, vốn nêu SHT31 hoặc DHT22 | Đọc nhiệt độ/độ ẩm thực tế; không cần mua DHT22; kiểm tra thứ tự chân và điện trở pull-up của loại đang có |
| BH1750 GY-302 | 1 | BT02 | Đọc độ sáng bằng I2C, nguồn logic 3,3V |
| PIR HC-SR501 | 1 | BT02 | Phát hiện chuyển động, kiểm tra nguồn nuôi và điện áp chân OUT theo module thực |
| Reed switch + nam châm | 1 bộ | BT02 | Tín hiệu cửa đóng/mở; debounce |
| LED đỏ/xanh/vàng | 5–10 | Bộ actuator BT02 | Mô phỏng đèn và chỉ báo |
| Điện trở 220–330Ω và 10kΩ | 1 bộ nhỏ | Bổ sung | Hạn dòng LED, pull-up/down, hỗ trợ DHT |
| Buzzer chủ động có driver phù hợp mức 3,3V | 1 | Bộ actuator BT02 | Cảnh báo an ninh; xác minh mức dòng và cực tính |
| Servo SG90 | 1 | Bộ actuator BT02 | Mô hình cửa; cấp nguồn riêng đủ dòng |
| Quạt mini DC 5V | 1 | Bộ actuator BT02 | Tải thực tế, không nối thẳng GPIO |
| MOSFET logic-level/module công suất tương thích điều khiển 3,3V | 1 | BT02 cho phép relay/MOSFET | Ưu tiên cho quạt, có mạch bảo vệ phù hợp; tránh chọn chỉ theo tên MOSFET |
| Relay 1 kênh tương thích logic 3,3V | 1, tùy chọn | BT02 cho phép relay/MOSFET | Chỉ cần nếu muốn thử relay; **không bắt buộc mua cùng MOSFET** |
| Breadboard 830 điểm | 2 | BT02, cụ thể hóa | Mỗi node một bảng thử |
| Dây jumper đủ ba kiểu đầu | 1 bộ mỗi kiểu | BT02, cụ thể hóa | Đực–đực, đực–cái, cái–cái |
| Cáp USB **có truyền dữ liệu** | 2 | Bổ sung | Phù hợp loại cổng thực tế của bo ESP32 |
| Nguồn DC 5V đủ dòng (gợi ý dự phòng ≥2A) | 1 | BT02, cụ thể hóa | Cấp tải servo/quạt; tính theo tổng dòng thực, chung GND với ESP32 |
| Công tắc Home/Away | 1 | Bổ sung đề xuất | Chuyển chế độ tại thiết bị trong tình huống offline |
| Đồng hồ vạn năng | 1 | Dụng cụ khuyến nghị | Đo điện áp, xác định chân, kiểm tra chập dây |
| Linh kiện dự phòng | 10–15% | BT02 | Dây, điện trở, vài LED, cảm biến/board tùy kinh phí |

**Chi phí:** BT02 ghi **khoảng 555.000–850.000 VNĐ** (không tính laptop, router, điện thoại) theo cấu hình gốc **SHT31/DHT22**. Hiện **DHT11 đã có sẵn** nên không tính mua DHT22; các vật tư/dụng cụ bổ sung vẫn có thể làm tổng thực mua khác dự toán. **Chưa có báo giá thực tế mới**; không coi bảng này là xác nhận đã mua các linh kiện còn lại.

### 4.1. Tương ứng Wokwi ↔ phần cứng thật

| Chức năng | Wokwi | Phần cứng thật | Điều chỉnh code |
| --- | --- | --- | --- |
| Nhiệt độ/độ ẩm | **DHT22 – mô hình thay thế** | **DHT11 – cảm biến thật** | **Hai kiểu giao thức/định dạng đọc khác nhau**: chọn kiểu cảm biến phù hợp từng build, không dùng DHT11 driver để mặc định đọc mô hình DHT22 |
| Độ sáng | LDR photoresistor (AO) | BH1750 GY-302 | Driver ADC1 vs I2C; không coi raw ADC là lux |
| Chuyển động | PIR mô phỏng | HC-SR501 | Cùng digital input, kiểm tra mức logic/độ trễ |
| Cửa đóng/mở | Slide switch | Reed switch + nam châm | Cùng digital input, khác cơ cấu vật lý |
| Bật/tắt đèn | LED | LED + điện trở | Tương tự |
| Quạt 5V | LED báo quạt / tín hiệu PWM | Motor + MOSFET/driver | Mô phỏng logic, **không** mô phỏng dòng khởi động |
| Còi/servo | Buzzer, servo | Active buzzer, SG90 | Kiểm tra nguồn, driver và giới hạn góc |

> **Các khác biệt quan trọng so với phần cứng thật:** Danh mục cảm biến chuẩn của Wokwi có **DHT22, chưa thấy DHT11**; BH1750/reed switch cũng không được liệt kê trực tiếp. Có thể dùng custom chip nhưng không cần thiết ở giai đoạn đầu. **Không được ghi trong báo cáo rằng đã mô phỏng và xác nhận driver DHT11 bằng Wokwi-DHT22.** Đọc DHT11 thật phải được kiểm chứng trên ESP32 vật lý.

## 5. Bảng GPIO dự kiến – phải đối chiếu bo thực tế trước khi nối

### 5.1. Node A

| Tín hiệu | GPIO ESP32 | Loại | Chú ý |
| --- | --- | --- | --- |
| **DHT11 DATA** | 16 | Digital in/out | 3,3V logic; xác minh module 3 chân hay cảm biến trần 4 chân và thứ tự chân trước khi nối |
| BH1750 SDA | 21 | I2C SDA | 3,3V |
| BH1750 SCL | 22 | I2C SCL | 3,3V |
| Wokwi LDR AO | 34 | ADC1 input-only | **Chỉ mô phỏng**; giữ điện áp đầu vào trong ngưỡng ESP32 thật |
| Đèn LED | 26 | Digital output | LED qua điện trở hạn dòng |
| Quạt MOSFET gate/driver | 25 | Digital/PWM output | Không cấp nguồn motor từ GPIO |

### 5.2. Node B

| Tín hiệu | GPIO ESP32 | Loại | Chú ý |
| --- | --- | --- | --- |
| PIR OUT | 27 | Digital input | Kiểm tra mức logic theo module thực tế |
| Reed switch | 32 | Digital input + pull-up | Định nghĩa rõ HIGH/LOW ↔ OPEN/CLOSED; debounce |
| Buzzer driver | 18 | Digital output | Không quá tải dòng GPIO |
| Servo signal | 19 | PWM output | Servo cấp 5V riêng, chung GND |
| LED trạng thái | 26 | Digital output | Qua điện trở |
| Công tắc Home/Away | 33 | Digital input + pull-up | Debounce và quy tắc ưu tiên |

**Nguyên tắc GPIO:** Tránh GPIO6–11 (flash), chân strapping dễ gây lỗi boot, GPIO1/3 khi cần UART; GPIO34–39 **chỉ là input và không có pull-up/down nội**. ADC analog khi dùng Wi-Fi nên ở **ADC1** (GPIO32–39); không tùy tiện chọn ADC2. Pinout cuối cùng phải khớp chính xác module ESP32 đã mua.

### 5.3. Nguồn, chống hỏng và an toàn

- ESP32 làm việc ở **logic 3,3V**. Không đưa tín hiệu 5V trực tiếp vào GPIO/ADC; **5V trên nguồn tải không đồng nghĩa tín hiệu GPIO chịu được 5V**.
- **DHT11 đang có:** xác định trước đây là module **3 chân** hay cảm biến trần **4 chân**; thứ tự chân có thể khác, tuyệt đối không đoán theo màu vỏ. Ưu tiên cấp VCC 3,3V và kéo DATA lên **3,3V**, không kéo lên 5V. Loại trần thường cần điện trở pull-up 4,7–10kΩ; module có thể đã tích hợp.
- Quạt DC cần mạch driver/MOSFET logic-level, điện trở gate, pull-down, diode dập xung phù hợp nếu là motor chổi than và cấp nguồn đúng dòng; không nối quạt trực tiếp vào GPIO.
- SG90 cấp nguồn 5V riêng đủ dòng; phải **nối GND chung** giữa nguồn actuator và ESP32; kiểm tra dòng đỉnh khi servo kẹt.
- Nếu dùng relay: kiểm tra active-low/active-high, dòng cuộn relay, nguồn và mức kích hoạt để đặt giá trị khởi động **an toàn**.
- Chỉ dùng tải DC điện áp thấp trong mô hình thử. **Không đấu 220V trên breadboard**.
- Ngắt nguồn trước khi đổi dây; đo điện áp/tiếp xúc bằng đồng hồ vạn năng trước khi cấp nguồn cho mô-đun mới.

## 6. Kiến trúc firmware và môi trường phát triển

### 6.1. Cấu trúc mã nguồn ĐỀ XUẤT (chưa tồn tại)

~~~text
IoT/
├── README.md
├── docs/
│   └── PLAN_PHAN_CUNG_FIRMWARE.md
├── contracts/                   # Giao thức chung, tạo khi nhóm chốt
│   ├── mqtt.md
│   └── examples/
└── firmware/
    ├── node-environment/
    │   ├── platformio.ini
    │   ├── wokwi.toml
    │   ├── diagram.json
    │   ├── src/main.cpp
    │   ├── include/
    │   ├── lib/
    │   │   ├── sensors/
    │   │   ├── actuators/
    │   │   ├── connectivity/
    │   │   └── automation/
    │   └── test/
    └── node-security/
        ├── platformio.ini
        ├── wokwi.toml
        ├── diagram.json
        ├── src/main.cpp
        ├── include/
        ├── lib/
        └── test/
~~~

Tách module thành:
- **Sensor drivers** (DHT11 thật, DHT22 thay thế trên Wokwi, BH1750, LDR, PIR, Reed) → kết quả đo, cờ hợp lệ, đơn vị. Phần business logic chỉ dùng giá trị chuẩn hóa, không phụ thuộc kiểu DHT.
- **Actuator drivers** (lamp, fan, buzzer, servo) → write + đọc lại trạng thái điều khiển.
- **Automation service** → Auto/Manual, hysteresis, Away/Home, trạng thái an toàn.
- **Connectivity/MQTT** → Wi-Fi, MQTT subscribe/publish, LWT, reconnect/backoff.
- **Data/commands** → serialize JSON, kiểm tra schema/version/TTL, chống xử lý trùng.
- **Diagnostics** → Serial log, uptime, heap, RSSI, mã lỗi, số lần reconnect.

**Không khóa firmware vào simulation:** Một build flag hoặc dependency injection chọn nguồn sáng LDR_SIM/BH1750 thật; phần automation nhận cùng một kiểu dữ liệu chuẩn hóa.

### 6.2. Cấu hình DHT11 thật và DHT22 mô phỏng

**Hai cấu hình build phải khác nhau ở tầng driver:**

- **HARDWARE:** khởi tạo DHT sensor với loại **DHT11** trên GPIO16, đọc ít nhất cách nhau **2 giây** để tránh lặp dữ liệu cũ. DHT11 có dải đo tham khảo **0–50°C (sai số khoảng ±2°C)** và **20–80% RH (sai số khoảng ±5% RH)**, đủ cho demo trong phòng nhưng không dùng như thiết bị đo chính xác cao.
- **SIMULATION:** Wokwi có sẵn part **DHT22**, vì vậy driver của build mô phỏng phải chọn **DHT22** hoặc sử dụng một mock sensor thay thế rõ ràng. Đây chỉ là cách kiểm thử luồng telemetry/logic; không xác nhận tính đúng đắn của thời gian đọc hay độ chính xác của DHT11 thật.
- Cả hai build vẫn xuất **temperature_c** và **humidity_pct**; bổ sung **temp_humidity_sensor** (`DHT11` hoặc `DHT22_SIM`) và **simulation** để phân biệt nguồn dữ liệu ở log/telemetry. Giữ **chu kỳ đọc cảm biến tối thiểu 2 giây**, chu kỳ publish MQTT có thể đặt riêng (ví dụ 5 giây).
- Khi firmware đọc lỗi (NaN, timeout, giá trị ngoài dải hợp lệ), không xuất giá trị 0 làm số đo hợp lệ; cập nhật cờ `valid=false`, ghi lỗi và áp dụng hành vi actuator an toàn.

### 6.3. PlatformIO và Wokwi

Cấu hình khởi tạo định hướng (tên môi trường phải khớp đường dẫn firmware thực tế):

~~~ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
~~~

~~~toml
[wokwi]
version = 1
firmware = ".pio/build/esp32dev/firmware.bin"
elf = ".pio/build/esp32dev/firmware.elf"
~~~

Mỗi node là một **project PlatformIO riêng**, có diagram.json cùng cấp với wokwi.toml. Node A cần cấu hình môi trường build riêng cho **DHT11 thật** và **DHT22_SIM** (ví dụ build_flags/env và pin mapping). PlatformIO build trước, Wokwi chỉ nạp kết quả build; **không tuyên bố đã build/thử thành công chỉ vì có file cấu hình**.

Đối với Wokwi web, Wi-Fi ảo mặc định là **Wokwi-GUEST**. Khi chạy ESP32 thật, cấu hình SSID/mật khẩu riêng và địa chỉ broker trong LAN; tuyệt đối không hardcode mật khẩu/token thật trong commit public.

**Để ESP32 mô phỏng nói chuyện với broker chạy trên PC:** Wokwi Public Gateway không truy cập localhost của cậu; phải xác minh Private IoT Gateway / Wokwi VS Code gateway và dùng địa chỉ đích phù hợp (ví dụ host.wokwi.internal khi chạy theo cách Wokwi tài liệu). Trước khi giao các thành viên khác, cần chứng minh được kết nối bằng một MQTT publish/subscribe thử nghiệm. Nếu điều kiện license/gateway không đáp ứng, dùng broker từ xa dành riêng môi trường test hoặc hai ESP32 thật – không mở broker công khai không xác thực.

> **CẢNH BÁO CHUYỂN ĐỔI KIẾN TRÚC GIAO TIẾP (`LEGACY` vs ThingsBoard `DRAFT`):**
> - **Mô hình cũ là `LEGACY / REFERENCE`:** Các mô tả Mosquitto + Node.js/Express + 12 custom topic `smarthome/v1/{device_id}/...` trong Mục 7 bên dưới và [contracts/mqtt-v1.md](../contracts/mqtt-v1.md) chỉ giữ lại làm baseline lịch sử về ngữ nghĩa dữ liệu và biên an toàn, **không còn là mặc định đã chốt để tích hợp mới với ThingsBoard**.
> - **Hướng chuyển đổi đang đề xuất (`DRAFT / PROPOSED`):** Nhóm dự kiến chuyển sang **ThingsBoard Device Provisioning** và **ThingsBoard built-in MQTT API** theo [contracts/thingsboard-v1.md](../contracts/thingsboard-v1.md) (và [HANDOFF_BACKEND_ANDROID.md](HANDOFF_BACKEND_ANDROID.md)). ThingsBoard dùng namespace riêng (`v1/devices/me/telemetry`, `v1/devices/me/attributes`, `v1/devices/me/rpc/request/+`) nên **không tương thích trực tiếp trên wire với 12 topic `smarthome/v1/...`**. Tài liệu chuyển đổi vẫn ở trạng thái **`DRAFT / PROPOSED`** và toàn bộ **`TB-DEC-01`..`TB-DEC-09`** hiện **`OPEN`** (chưa được nhóm phê duyệt).
> - **Hiện trạng firmware thực tế:** Source Node A/Node B hiện mới có cảm biến, actuators, edge rules cục bộ, `CommandGuard`, `UtcParser`, `WiFiConnectionManager` (non-blocking STA) và bounded offline RAM queues; **chưa triển khai hoặc runtime-verify** MQTT client, ThingsBoard provisioning, TLS, SNTP, JSON transport, live telemetry/RPC và lưu trữ credential NVS. Cả hai bản `REAL` đang **`UNCONFIGURED` về Wi-Fi** (không chứa credential thật trong repository). Edge rules của Node A/Node B tiếp tục chạy độc lập offline ở mức source/unit-test/build, còn **runtime Wokwi và ESP32 thật hiện `NOT_VERIFIED`**.

## 7. Hợp đồng MQTT sơ bộ cho backend (CHƯA CHỐT)

**Tiền tố:** smarthome/v1. Một topic cho một loại dữ liệu/ý nghĩa, dùng **device_id** cố định để backend phân biệt node.

| Topic | Bên gửi → nhận | Mục đích | Retained? |
| --- | --- | --- | --- |
| smarthome/v1/node-a/telemetry | A → broker/backend | Nhiệt độ, độ ẩm, ánh sáng | Không |
| smarthome/v1/node-a/command | Backend → A | Điều khiển đèn/quạt, cấu hình | **Không** |
| smarthome/v1/node-a/state | A → backend/app | Trạng thái thực tế, AUTO/MANUAL | Có thể retained (trạng thái hiện tại) |
| smarthome/v1/node-a/status | A/LWT → backend | online/offline | Có |
| smarthome/v1/node-b/event | B → backend | Cửa/chuyển động/cảnh báo | Không |
| smarthome/v1/node-b/command | Backend → B | HOME/AWAY, servo, cấu hình | **Không** |
| smarthome/v1/node-b/state | B → backend/app | Trạng thái thực tế | Có thể retained |
| smarthome/v1/node-b/status | B/LWT → backend | online/offline | Có |

### 7.1. Bản tin ví dụ

~~~json
{
  "schema_version": 1,
  "device_id": "node-a",
  "boot_id": "example-boot-01",
  "seq": 42,
  "timestamp": "2026-11-14T12:00:00Z",
  "temperature_c": 29.0,
  "humidity_pct": 61.0,
  "temp_humidity_sensor": "DHT11",
  "simulation": false,
  "light_lux": 130.0,
  "light_source": "BH1750",
  "valid": true
}
~~~

~~~json
{
  "schema_version": 1,
  "command_id": "cmd-example-001",
  "device_id": "node-a",
  "target": "lamp",
  "action": "set",
  "value": true,
  "mode": "MANUAL",
  "expires_at": "2026-11-14T12:00:30Z"
}
~~~

~~~json
{
  "schema_version": 1,
  "device_id": "node-a",
  "command_id": "cmd-example-001",
  "result": "applied",
  "lamp_on": true,
  "device_mode": "MANUAL"
}
~~~

**Đây chỉ là schema minh họa để thống nhất**, thời gian trong ví dụ không phải telemetry thực. Khi chưa có NTP, **không gửi timestamp giả**: sử dụng null/uptime và cờ time_synced=false, phía backend tiếp nhận và gắn thời gian nhận. Với LDR mô phỏng chưa hiệu chuẩn, có thể để light_lux=null, gửi light_raw và light_source=LDR_SIM.

### 7.2. Quy tắc truyền tin bắt buộc thống nhất

- MQTT **QoS 1** cho lệnh/state/event nếu yêu cầu xác nhận truyền; **QoS không đảm bảo xử lý nghiệp vụ đúng một lần**.
- Khi kết nối, thiết lập **LWT** offline retained; sau khi subscribe thành công, publish online retained và state ban đầu.
- Bản tin command **không retained**; dùng command_id + expires_at + dedup để chống thực thi lại khi reconnect.
- Phân biệt **desired command** (điều muốn làm) và **reported state** (thiết bị thật đang ở trạng thái nào). Backend/app không được đánh dấu bật đèn thành công chỉ vì đã publish.
- Thiết bị khởi động với trạng thái output an toàn; không nhận cấu hình không xác thực; không commit Wi-Fi password/broker credentials.
- Node B khi offline vẫn chạy edge rules từ cảm biến của chính node; backend ngừng hoạt động có thể làm **thông báo lên điện thoại đến chậm hoặc không đến**, không được gọi là cảnh báo điện thoại offline.

## 8. Kế hoạch triển khai theo giai đoạn (KHÔNG phải checklist tiến độ)

| Giai đoạn | Việc thực hiện | Bằng chứng nghiệm thu |
| --- | --- | --- |
| A. Nạp và debug board thật | Cáp USB → PlatformIO → Blink LED → Serial Monitor | Build log, nạp thành công, LED đổi trạng thái |
| B. Node A cảm biến | **DHT11 thật** riêng → BH1750 riêng → kết hợp; thử Wokwi-DHT22/LDR để kiểm chứng logic | Log giá trị thực, kiểm tra biến thiên, ảnh/sơ đồ đấu nối; **tách riêng kết quả mô phỏng và kết quả DHT11 thật** |
| C. MQTT Node A | Publish dữ liệu và status; thử subscribe command | Log broker/subscriber, telemetry đúng JSON |
| D. Actuator và edge Node A | LED, MOSFET/quạt; AUTO/MANUAL, mất broker | Đo trạng thái thực tế, video khi ngắt mạng |
| E. Node B | PIR + reed + buzzer + servo; HOME/AWAY cục bộ | Mô phỏng/đóng mở cửa, cảnh báo khi AWAY |
| F. Tích hợp hệ thống | Hai ESP32 thật / hai mô phỏng → một broker → backend/app | End-to-end log, trạng thái đúng, history đúng |
| G. Nghiệm thu và báo cáo | Kiểm tra ổn định, mất mạng, lỗi cảm biến, tài liệu hoàn chỉnh | Ảnh đầy đủ, video/demo, bảng test có số đo thật |

**Không tự nhận đã hoàn thành các giai đoạn trên.** Các ảnh và số liệu sẽ chỉ được thêm sau khi có thử nghiệm.

## 9. Tiêu chí kiểm thử đề xuất và chỗ BT02 chưa chốt

Theo BT02 nhóm thống nhất **20 lần điều khiển**, **5 lần ngắt/mở lại mạng**, kiểm thử Away + cửa mở, kiểm thử edge khi mất backend; các ô định lượng độ trễ/chu kỳ cập nhật/reconnect/vận hành/lưu trữ **đang để trống**.

| Chỉ tiêu | Mức **đề xuất để thảo luận** (chưa phải cam kết) | Cách đo |
| --- | --- | --- |
| Telemetry môi trường | Mỗi 5 giây; **DHT11 đọc không dày hơn 2 giây/lần** | Đối chiếu timestamp giữa ESP32, broker, backend; phân biệt dữ liệu thật và mô phỏng |
| Phản ứng cửa/Away cục bộ | Trong 1 giây từ khi sự kiện đã được debounced | Đo log/videotime, phân biệt độ trễ PIR phần cứng |
| Điều khiển LAN | ≥19/20 lệnh hoàn tất trong 2 giây | Đo từ lệnh đến **reported state**, không chỉ broker ACK |
| Reconnect | Mục tiêu trong 30 giây sau khi mạng/broker trở lại | Ngắt rồi phục hồi 5 lần, đo từng lần |
| Vận hành liên tục | 24 giờ trên phần cứng thật | Uptime, free heap, reconnect, watchdog/reset |
| Lưu trữ lịch sử | Mục tiêu 7 ngày, do backend quản lý | Truy vấn lịch sử 7 ngày |
| Mất broker | Edge rules vẫn chạy, state trở về đồng bộ khi reconnect | Ngắt broker, thay đổi cảm biến, kiểm tra actuator |

**Không thể bảo đảm “không mất một mẫu telemetry nào” khi mất nguồn/mạng vô hạn chỉ nhờ RAM.** Nếu yêu cầu lưu đệm, nhóm phải chốt dung lượng, thời gian giữ dữ liệu, cách phân biệt duplicate và chính sách dữ liệu cũ.

## 10. Quy ước phát triển cùng Gemini / GitHub

- Gemini trong Antigravity code theo **phạm vi và tiêu chí nghiệm thu của từng giai đoạn**; không được tự đánh dấu code là đã test trên phần cứng nếu chưa có log.
- Mọi thay đổi pin mapping, điện áp nguồn, bản tin MQTT, quy tắc ưu tiên AUTO/MANUAL, HOME/AWAY phải được cập nhật ở tài liệu trước hoặc cùng commit với code.
- Phát triển trên branch tính năng, review diff trước khi merge main; build/test thực tế bằng PlatformIO; không commit thư mục .pio, secret, Wi-Fi password, token.
- Mỗi module cần README phụ hoặc chú thích chỉ ra **chân GPIO, nguồn cấp, thư viện, kết quả và giới hạn mô phỏng**.
- Tài liệu kế hoạch là quyết định kỹ thuật **của mảng Phạm Ngọc Hưng phụ trách**, dựa trên BT02; thông tin giao tiếp dùng chung cần xác nhận với hai thành viên khác trước khi coi là chính thức.

## 11. Nguồn tham khảo, cơ sở đối chiếu

**Tài liệu nội bộ:**
- [BT02 – Đề cương đề tài của Nhóm 9](Bao_cao_BT02_IoT.docx) (nguồn gốc yêu cầu và BOM).
- Tài liệu học phần IT4735, **Chương 2 – Công nghệ IoT** (cảm biến, telemetry/state/command), **Chương 4 – Bảo mật IoT**, **Chương 5 – Thiết kế và xây dựng** (BOM, pinout, firmware, 24h, kiểm thử).

**Tài liệu kỹ thuật chính thức:**
- [Wokwi – ESP32 hỗ trợ](https://docs.wokwi.com/guides/esp32).
- [Wokwi – phần cứng mô phỏng](https://docs.wokwi.com/getting-started/supported-hardware).
- [Wokwi – hai MCU riêng project](https://docs.wokwi.com/faq).
- [Wokwi + PlatformIO](https://docs.wokwi.com/vscode/platformio).
- [Wokwi – Wi-Fi và IoT Gateway](https://docs.wokwi.com/guides/esp32-wifi).
- [Wokwi – DHT22 (thay thế mô phỏng)](https://docs.wokwi.com/parts/wokwi-dht22), [LDR](https://docs.wokwi.com/parts/wokwi-photoresistor-sensor), [PIR](https://docs.wokwi.com/parts/wokwi-pir-motion-sensor), [Servo](https://docs.wokwi.com/parts/wokwi-servo).
- [Wokwi – danh mục cảm biến hỗ trợ](https://docs.wokwi.com/getting-started/supported-hardware) (không liệt kê DHT11 trong danh mục chính thức được kiểm tra).
- [Adafruit – DHT11 và DHT22: thông số, sơ đồ, cách đọc](https://learn.adafruit.com/dht/).
- [PlatformIO – ESP32 Dev Module](https://docs.platformio.org/en/stable/boards/espressif32/esp32dev.html).
- [Espressif – GPIO/ADC hạn chế](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html).
