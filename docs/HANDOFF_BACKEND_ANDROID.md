# Tài liệu bàn giao kỹ thuật: Firmware ESP32 ↔ Backend ↔ Android

> **CẢNH BÁO CHUYỂN ĐỔI KIẾN TRÚC SANG THINGSBOARD (QUAN TRỌNG):**  
> Nhóm đã chuyển hướng từ mô hình **Mosquitto + X.509 mTLS + 12 topic custom `smarthome/v1/...`** sang **ThingsBoard Device Provisioning** (`provisionDeviceKey` / `provisionDeviceSecret`).  
> Vì vậy, các mục về **X.509 client certificates, Mosquitto ACL theo `CN`, và danh sách topic `smarthome/v1/...`** trong Mục 3 và Mục 6 bên dưới **KHÔNG CÒN LÀ MẶC ĐỊNH**, mà được giữ lại làm tham chiếu lịch sử về ngữ nghĩa dữ liệu, an toàn biên và kiểm thử logic firmware.  
> Đặc tả chuyển đổi chính đang ở trạng thái **`DRAFT / PROPOSED`** tại **[contracts/thingsboard-v1.md](../contracts/thingsboard-v1.md)** và tóm tắt triển khai tại **[Mục 10](#10-chuyển-đổi-sang-thingsboard-device-provisioning)** bên dưới.

> **Mục đích:** Tài liệu bàn giao giao diện tích hợp cho **Khương Anh Tài** (Backend, ThingsBoard/Broker, PostgreSQL, Docker) và **Bùi Trung Hoàng** (Android Kotlin, Jetpack Compose, Kiểm thử) dựa trên đặc tả thiết bị do **Phạm Ngọc Hưng** phụ trách.  
> **Nguồn chuẩn hợp đồng hiện hành:** Bản nháp chuyển đổi [contracts/thingsboard-v1.md](../contracts/thingsboard-v1.md) (`DRAFT / PROPOSED`), tài liệu legacy [contracts/mqtt-v1.md](../contracts/mqtt-v1.md) và bộ 20 mẫu JSON ngữ nghĩa trong [contracts/examples/](../contracts/examples/).  
> **Tài liệu gốc đối chiếu:** [README.md](../README.md), [PLAN_PHAN_CUNG_FIRMWARE.md](PLAN_PHAN_CUNG_FIRMWARE.md), [Bao_cao_BT02_IoT.docx](Bao_cao_BT02_IoT.docx).  
> **Trạng thái tích hợp:** Firmware đã có phần logic thuần C++ (`automation`/`security`, `command_guard`, `utc_time`) kèm unit test native; **phần mạng Wi-Fi, ThingsBoard provisioning, JSON codec, MQTT/TLS trên firmware và mã nguồn Backend/Android hiện CHƯA triển khai**. Mọi điểm đánh dấu `[PROPOSED]` / `TB-DEC-*` cần nhóm thống nhất trước khi đóng băng bản tích hợp.

---

## 1. Kiến trúc tổng thể & Nguồn chân lý duy nhất (SSOT)

### 1.1. Hai ESP32 MQTT Client độc lập (Không Gateway)
- **Kiến trúc đã chốt:** Hệ thống gồm **hai nút ESP32 độc lập** kết nối trực tiếp qua Wi-Fi 2,4 GHz tới **Mosquitto MQTT Broker**:
  - **`node-a` (`role: "environment"`):** Giám sát môi trường & điều khiển đèn/quạt.
    - Môi trường `REAL` (mạch thật): **`DHT11`** + `BH1750` + LED + Quạt DC 5V qua MOSFET.
    - Môi trường `SIM` (Wokwi): **`DHT22`** (chỉ thay thế trong mô phỏng) + `LDR` (`LDR_SIM`) + LED + LED/PWM mô phỏng quạt.
  - **`node-b` (`role: "security"`):** Giám sát an ninh & mô hình cửa.
    - Môi trường `REAL`: `PIR_HC_SR501` + `REED_SWITCH` + `HOME_AWAY_SWITCH` + `BUZZER` + `SG90` (+ LED trạng thái).
    - Môi trường `SIM` (Wokwi): `PIR` + `SLIDE_SWITCH_REED_SIM` + `BUZZER` + `SERVO`.
- **Không dùng Gateway trung gian:** `node-a` và `node-b` **không** nối dây trực tiếp, không dùng ESP-NOW và không node nào làm gateway cho node kia. Mọi giao tiếp nghiệp vụ đều đi qua Broker/Backend.

### 1.2. Chính sách SSOT (Single Source of Truth)
| Miền dữ liệu | Nguồn chân lý duy nhất (SSOT) | Ranh giới trách nhiệm |
| --- | --- | --- |
| **Device Registry & Topology** (`device_id`, binding chứng chỉ X.509, trạng thái `active`/`revoked`, gán khu vực) | **Backend + PostgreSQL** | Node **không** được phép tự đăng ký vào hệ thống. Topic `meta` chỉ khai báo năng lực cho node đã được duyệt trước. |
| **Chính sách / Ngưỡng tự động dài hạn** | **Backend** (cấu hình gốc) + **ESP32** (bản sao đang chạy) | Khi mất mạng, ESP32 tiếp tục dùng bộ ngưỡng hợp lệ gần nhất đang giữ cục bộ. |
| **Trạng thái thực tế thiết bị (`reported state`)** | **Chính từng ESP32** (`node-a` / `node-b`) | Chỉ `state` và `command-ack` do ESP32 phát mới phản ánh trạng thái phần cứng thật. Backend/App không tự suy đoán lệnh đã thực thi. |
| **Lịch sử cảm biến & sự kiện (≥ 7 ngày)** | **Backend + PostgreSQL** | ESP32 chỉ đệm ngắn hạn trong RAM khi rớt mạng, không lưu lịch sử dài hạn. |

---

## 2. Phân biệt 3 tầng xác nhận (Không nhầm `PUBACK` với xác nhận nghiệp vụ)

Khi Backend gửi `command` hoặc nhận `telemetry`/`event`, cần phân tách rạch ròi 3 tầng xác nhận theo [contracts/mqtt-v1.md](../contracts/mqtt-v1.md):

| Tầng xác nhận | Cơ chế kỹ thuật | Điều **được** chứng minh | Điều **KHÔNG** được suy ra |
| --- | --- | --- | --- |
| **Tầng 1: Transport (`MQTT PUBACK`, QoS 1)** | Giao thức MQTT giữa Client và Mosquitto Broker. | Gói tin đã đến bên nhận kế tiếp (Broker đã nhận `command` từ Backend, hoặc Broker đã nhận `telemetry`/`event` từ ESP32). | **Không** chứng minh ESP32 đã nhận lệnh (khi Backend gửi), **không** chứng minh đèn/quạt/servo đã tác động, và **không** chứng minh Backend đã ghi vào PostgreSQL. |
| **Tầng 2: Thực thi thiết bị (`command-ack` + `state`)** | ESP32 chạy pipeline 6 bước (parse, envelope, idempotency, expiry, bounds, GPIO) rồi publish `.../command-ack` và `.../state`. | Lệnh `command_id` đã thực sự được ESP32 kiểm tra và áp dụng (hoặc từ chối/quá hạn/trùng lặp) trên phần cứng thật. | **Không** chứng minh dữ liệu đó đã được PostgreSQL lưu bền vững nếu Backend gặp sự cố ghi DB. |
| **Tầng 3: Lưu trữ bền vững (`Backend Persistence` `[PROPOSED]`)** | Backend tiêu thụ bản tin từ Mosquitto, `COMMIT` thành công vào PostgreSQL rồi mới cập nhật trạng thái/history cho Android. | Dữ liệu lịch sử và trạng thái đã được lưu an toàn phục vụ truy vấn 7 ngày và đồng bộ đa thiết bị. | Trách nhiệm triển khai nằm hoàn toàn ở Backend, không thuộc Firmware. |

---

## 3. Yêu cầu dành cho Backend (Khương Anh Tài)

> **Lưu ý chuyển đổi ThingsBoard (Legacy Notice cho 3.1 & 3.2):** Các mục **3.1 (Mosquitto, X.509 mTLS & ACL)** và **3.2 (Registry Gate trên Mosquitto)** dưới đây thuộc mô hình Mosquitto cũ nên **không còn là mặc định triển khai**; phần chuyển đổi sang ThingsBoard xem **[Mục 10](#10-chuyển-đổi-sang-thingsboard-device-provisioning)** và **[contracts/thingsboard-v1.md](../contracts/thingsboard-v1.md)**. Riêng các quy tắc nghiệp vụ ở **3.3** (lưu trữ ≥ 7 ngày, xử lý `time_synced: false`, chống trùng backfill và phân biệt `command-ack`) vẫn cần bảo toàn qua Rule Chain hoặc Backend Adapter.

### 3.1. Mosquitto Broker, X.509 mTLS & ACL theo danh tính *(Legacy — không còn mặc định)*
- **Bảo mật kết nối `REAL`:**
  - Bắt buộc **TLS 1.2+ với X.509 Mutual Authentication (mTLS)** (cổng khuyến nghị `8883`, `require_certificate true` trên Mosquitto).
  - Cấp cặp `(client_cert, private_key)` **riêng biệt** cho `node-a`, `node-b` và dịch vụ `backend-service`. **Tuyệt đối không chèn private key, chứng chỉ thật hoặc mật khẩu vào Git repository.**
  - Firmware môi trường `REAL` kiểm chứng server certificate bằng **CA Root Certificate** pin sẵn và **cấm tuyệt đối `setInsecure()`**.
- **Mapping chứng chỉ → Identity & Đối soát:**
  - Cấu hình Mosquitto `use_identity_as_username true` trích từ trường `Common Name (CN)` (hoặc `SAN`): `CN=node-a`, `CN=node-b`, `CN=backend-service`.
  - Backend khi nhận bản tin phải đối soát 3 lớp: `authenticated_identity` (CN từ Broker/TLS) == `topic.device_id` == `payload.device_id`. Mọi sai lệch đều phải loại bỏ và ghi log cảnh báo bảo mật.
- **ACL tối thiểu quyền (Mosquitto ACL):**
  - **`node-a` (`CN=node-a`):** chỉ `write` trên `smarthome/v1/node-a/{telemetry,command-ack,state,status,meta}` và `read` trên `smarthome/v1/node-a/command`. Cấm đọc/ghi `node-b` hoặc wildcard `#`, `+`.
  - **`node-b` (`CN=node-b`):** chỉ `write` trên `smarthome/v1/node-b/{event,command-ack,state,status,meta}` và `read` trên `smarthome/v1/node-b/command`. Cấm đọc/ghi `node-a` hoặc wildcard `#`, `+`.
  - **`backend-service`:** `read` các topic dữ liệu/trạng thái của các node hợp lệ; `write` vào `smarthome/v1/+/command` (với QoS `1` và **bắt buộc `retain: false`**).

### 3.2. Registry Gate — Chống node lạ tự tham gia topology
- **Quy tắc bất khả xâm phạm:** Một chứng chỉ X.509 hợp lệ (do nội bộ CA ký) **KHÔNG tự động cấp quyền đăng ký thiết bị hay gia nhập topology**.
- Quy trình duyệt bắt buộc ở Backend:
  1. Quản trị viên khai báo trước (`pre-register`) `device_id` (`"node-a"`, `"node-b"`) cùng định danh chứng chỉ ở trạng thái `active` trong Device Registry (PostgreSQL).
  2. Mosquitto ACL chỉ mở quyền cho các `CN` đã duyệt.
  3. Nếu nhận bất kỳ bản tin nào (kể cả `meta`) mang `device_id` chưa có trong Registry hoặc đang ở trạng thái `revoked`, Backend phải **từ chối đưa vào DB/topology và phát cảnh báo an ninh**, không tự tạo device mới.
  4. Khi thu hồi node: đổi trạng thái Registry sang `revoked`, gỡ `CN` khỏi ACL và đưa chứng chỉ vào CRL.

### 3.3. Nhận dữ liệu, xử lý `command-ack`, chống trùng backfill & lưu trữ 7 ngày
- **Tiếp nhận & Lưu trữ 7 ngày (BT02):**
  - Backend tiêu thụ `telemetry` (`node-a`), `event` (`node-b`), `state`, `status`, `meta`, `command-ack`; kiểm tra `schema_version == 1` trên mọi bản tin.
  - Trên `meta`, chỉ đọc các trường khai báo (`role`, `board`, `build_env`, `firmware_version`, `tls_mode`, `sensors`, `actuators`, `capabilities`) cho node đã có trong Registry.
  - **Yêu cầu lưu lịch sử tối thiểu 7 ngày thuộc 100% trách nhiệm Backend + PostgreSQL** (retention policy, index truy vấn theo khoảng thời gian). ESP32 không lưu dữ liệu dài hạn.
- **Chống trùng khi gửi bù (`buffered: true`):**
  - Khi mất mạng, node đệm tạm trong RAM và xả dần với `"buffered": true`. Ngoài ra QoS `1` có thể gây truyền lại gói tin khi đứt mạng ngay trước `PUBACK`.
  - `[PROPOSED]` Backend dùng khóa duy nhất **`(device_id, boot_id, seq)`** (cần thống nhất ở `PROP-04` về phạm vi `seq` toàn node hay theo từng stream) để `INSERT ... ON CONFLICT DO NOTHING`, đảm bảo idempotent ở DB.
- **Xử lý bản tin chưa đồng bộ giờ (`time_synced: false`, `timestamp: null`):**
  - Backend **không** từ chối các bản tin `telemetry`/`event`/`state` có `timestamp: null` khi `time_synced: false`.
  - `[PROPOSED]` Gắn `server_received_at` và cờ `timestamp_source = "server_fallback"`; riêng bản ghi `buffered: true` có thể nội suy mốc tương đối dựa trên chênh lệch `uptime_ms` cùng `boot_id`.
- **Xử lý `command-ack`:**
  - Cập nhật trạng thái lệnh theo `command_id` (`applied`, `rejected`, `expired`, `duplicate`), kèm `applied_mode`, `executed_at` và lý do `reason` (nếu có) để đẩy lên Android; đối chiếu `last_command_id` trong `state`.
  - Chỉ phát lệnh `command` với `retain: false`, luôn kèm `schema_version: 1`, `command_id`, `device_id`, `target`, `action`, `value`, `issued_at`, `expires_at` và `[PROPOSED]` `ttl_ms`, `require_time_sync`.
- **Cập nhật `online` / `offline` / `stale`:**
  - Theo dõi `status` (`online: true`, `rssi_dbm`, `free_heap`, `reconnect_count` khi kết nối xong; `online: false` qua LWT `lwt_unexpected_disconnect` hoặc `graceful_shutdown`).
  - Duy trì `last_seen_at` từ `telemetry`/`event`/`state`/`status` để phát hiện tình trạng `stale` nếu quá nhiều chu kỳ (ví dụ > 15–20 giây đối với `node-a`) mà Broker chưa kịp kích hoạt LWT keep-alive timeout.

---

## 4. Yêu cầu dành cho Android (Bùi Trung Hoàng)

### 4.1. Dữ liệu hiển thị cho hai node
- **`node-a` (Môi trường & Tiện nghi):**
  - Cảm biến: `temperature_c` (°C), `humidity_pct` (%RH), `temp_hum_source` (`"DHT11"` cho mạch thật, `"DHT22"` cho mô phỏng Wokwi), `light_lux` (lux), `light_raw` (ADC `0..4095`), `light_source` (`"BH1750"` hoặc `"LDR_SIM"`).
  - Thiết bị & Chế độ: `lamp_on`, `fan_on`, `device_mode` (`"AUTO"` / `"MANUAL"`), bộ ngưỡng `thresholds` đang hiệu lực (`light_on_lux`, `light_off_lux`, `fan_on_temp_c`, `fan_off_temp_c`).
- **`node-b` (An ninh & Mô hình cửa):**
  - Trạng thái hiện tại: `security_mode` (`"HOME"` / `"AWAY"`), `door_open`, `motion_detected`, `alarm_on`, `servo_angle`.
  - Nhật ký sự kiện (`event_type`): `door_changed`, `motion_detected`, `alarm_triggered`, `alarm_cleared`, `security_mode_changed`, `boot_reset`, kèm `trigger_source`.
  - Lưu ý hiển thị trên UI: `servo_angle` điều khiển **mô hình cửa SG90**, không phải khóa cửa an ninh thật.

### 4.2. Phân biệt `desired` vs `reported state` & Vòng đời lệnh
- **Không cập nhật lạc quan (No fake optimistic toggle):** Khi người dùng bấm bật/tắt đèn, quạt, đổi `AUTO`/`MANUAL`, `HOME`/`AWAY` hoặc góc `servo`, App gửi yêu cầu qua Backend và đưa nút về trạng thái **`pending`** (*đang chờ thiết bị xác nhận*).
- **Chuyển trạng thái UI theo kết quả thật:**
  - **`applied`:** Backend nhận `command-ack` (`status: "applied"`) và `state` mới từ ESP32 → App cập nhật UI khớp `reported state`.
  - **`rejected`:** Ví dụ gửi góc servo ngoài dải an toàn (`servo_angle_out_of_safe_range`) → giữ nguyên trạng thái cũ trên UI và hiện thông báo lý do `reason`.
  - **`expired`:** Lệnh quá hạn hoặc bị từ chối do ESP32 chưa đồng bộ đồng hồ (`clock_unsynced_cannot_verify_expiry`) → báo hết hạn, không hiển thị là đã thực thi.
  - **`duplicate`:** Lệnh trùng `command_id` đã xử lý trước đó → không thay đổi UI sai lệch.

### 4.3. Trạng thái `offline` / `stale`, lỗi cảm biến & cảnh báo
- **`offline` & `stale`:**
  - Khi `online == false`: hiển thị thiết bị đang ngoại tuyến (hiển thị dấu thời gian cập nhật cuối), vô hiệu hóa hoặc cảnh báo rõ khi người dùng cố gửi lệnh điều khiển. Ghi chú trên giao diện: khi mất mạng/Backend, **edge rules cục bộ trên ESP32 vẫn tự chạy**, chỉ thông báo lên điện thoại bị trễ hoặc không đến.
  - Khi `online == true` nhưng `last_seen_at` quá hạn (trạng thái **`stale`**): hiển thị cảnh báo dữ liệu cũ, không coi là số đo thời gian thực.
- **Lỗi cảm biến & trường `null`:**
  - Nếu `valid == false` hoặc `temperature_c` / `humidity_pct` là `null` (có mã trong `sensor_faults`, xem [node-a-telemetry-sensor-fault.json](../contracts/examples/node-a-telemetry-sensor-fault.json)): **tuyệt đối không hiển thị `0°C` hay `0%`**. Phải hiện `"Lỗi cảm biến"` / `"N/A"` theo `sensor_health` (`"fault"` / `"degraded"`).
  - `[PROPOSED]` Khi chạy mô phỏng `SIM` (`light_source == "LDR_SIM"`), trường `light_lux` là `null` và chỉ có `light_raw` (`0..4095`): App hiển thị giá trị `light_raw` (hoặc nhãn `"LDR mô phỏng"`), **không** hiển thị `0 lux`.
- **Cảnh báo an ninh:**
  - Ưu tiên cảnh báo nổi bật khi nhận sự kiện `alarm_triggered` (`door_open: true` trong `security_mode: "AWAY"`), phân biệt với sự kiện `motion_detected` (`trigger_source: "pir"`).

---

## 5. Thời gian chưa đồng bộ & Rủi ro bộ đệm RAM / LittleFS

### 5.1. Thời gian chưa đồng bộ (`time_synced: false`) ảnh hưởng cả lệnh lẫn TLS
1. **Đối với payload & kiểm tra hạn lệnh (`expires_at`):**
   - Khi chưa có NTP/RTC (`time_synced: false`), ESP32 **bắt buộc đặt `timestamp: null`** (cấm gửi thời gian giả như `1970-01-01`).
   - Khi nhận `command` có `expires_at` (ISO-8601 UTC) trong lúc `time_synced: false`, ESP32 **không được mặc nhiên coi lệnh cũ là hợp lệ**. Mặc định an toàn là từ chối với `status: "expired"` / `reason: "clock_unsynced_cannot_verify_expiry"` (xem [node-a-command-ack-expired.json](../contracts/examples/node-a-command-ack-expired.json)), trừ khi nhóm thống nhất cơ chế bổ sung ở `PROP-02`.
2. **Đối với xác thực chứng chỉ X.509 mTLS (`notBefore` / `notAfter`):**
   - Trong `REAL`, thư viện TLS trên ESP32 kiểm tra thời hạn hợp lệ của server/client certificate dựa trên đồng hồ hệ thống. Nếu MCU vừa khởi động trong mạng LAN bị mất Internet (không nối được NTP công cộng), quá trình bắt tay TLS có thể thất bại nếu đồng hồ MCU ở năm 1970.
   - **Khuyến nghị kỹ thuật (`PROP-03`):** Dựng **local NTP server** trên Router hoặc máy chủ Backend trong mạng LAN thử nghiệm để ESP32 luôn đồng bộ được giờ nội bộ (`time_synced: true`) ngay cả khi ngắt Internet.

### 5.2. Rủi ro bộ đệm RAM mất dữ liệu khi reboot & yêu cầu LittleFS giai đoạn sau
- **Giới hạn hiện tại trong v1:** Để giữ vòng lặp nhẹ và tránh ghi flash liên tục, hàng đợi offline backfill đặt trong **RAM** (dự kiến 24–36 mẫu telemetry cho `node-a`, tối đa 20 sự kiện ưu tiên cho `node-b`, ghi đè cũ nhất khi đầy và tăng `dropped_offline_count`).
- **Rủi ro mất dữ liệu khi mất điện/reboot:** Toàn bộ dữ liệu đệm trong RAM **sẽ bị mất sạch** nếu ESP32 bị rút nguồn, brownout (ví dụ sụt áp từ servo SG90/quạt nếu cấp nguồn sai) hoặc watchdog reset trước khi kết nối lại Broker.
- **Yêu cầu thiết kế cho giai đoạn sau:** Nếu nhóm hoặc bài nghiệm thu yêu cầu giữ dữ liệu đệm qua chu kỳ mất điện, phải thiết kế phân vùng **LittleFS** (hoặc ring-log riêng trên flash) ở giai đoạn nâng cấp sau, kèm cơ chế quay vòng tệp giới hạn dung lượng và tính toán giảm số lần ghi để **hạn chế hao mòn bộ nhớ flash (wear-leveling)** — không tự suy diễn rằng v1 đã có lưu trữ bền vững trên flash.

---

## 6. Bảng mapping Topic, Hướng dữ liệu & 20 JSON Examples *(Legacy Custom MQTT Reference)*

> **Lưu ý:** 12 topic `smarthome/v1/...` bên dưới là đặc tả custom MQTT cũ và **không dùng trực tiếp trên ThingsBoard built-in MQTT API**. Bảng ánh xạ từ 12 topic này sang `v1/devices/me/{telemetry,attributes,rpc/request/+,rpc/response/{requestId}}` xem chi tiết tại [contracts/thingsboard-v1.md](../contracts/thingsboard-v1.md#4-ánh-xạ-api-mqtt-chuẩn-thingsboard--so-sánh-với-12-topic-custom-mqtt-v1md). Bộ 20 mẫu JSON vẫn giữ nguyên để tham chiếu cấu trúc dữ liệu nghiệp vụ.

Tiền tố chung: `smarthome/v1/{device_id}/...`. Nguồn chuẩn trường JSON xem chi tiết tại [contracts/mqtt-v1.md](../contracts/mqtt-v1.md).

| Node | Topic | Hướng | QoS | Retain | Mẫu JSON đối chiếu trong `contracts/examples/` |
| --- | --- | --- | :---: | :---: | --- |
| `node-a` | `smarthome/v1/node-a/telemetry` | Node A → Broker/Backend | `1` | `false` | [node-a-telemetry-real.json](../contracts/examples/node-a-telemetry-real.json)<br>[node-a-telemetry-sim-unsynced.json](../contracts/examples/node-a-telemetry-sim-unsynced.json)<br>[node-a-telemetry-sensor-fault.json](../contracts/examples/node-a-telemetry-sensor-fault.json) |
| `node-a` | `smarthome/v1/node-a/command` | Backend → Node A | `1` | `false` | [node-a-command-lamp.json](../contracts/examples/node-a-command-lamp.json)<br>[node-a-command-thresholds.json](../contracts/examples/node-a-command-thresholds.json) `[PROPOSED]` |
| `node-a` | `smarthome/v1/node-a/command-ack` | Node A → Backend | `1` | `false` | [node-a-command-ack-applied.json](../contracts/examples/node-a-command-ack-applied.json)<br>[node-a-command-ack-expired.json](../contracts/examples/node-a-command-ack-expired.json) |
| `node-a` | `smarthome/v1/node-a/state` | Node A → Backend/App | `1` | `true` | [node-a-state.json](../contracts/examples/node-a-state.json) |
| `node-a` | `smarthome/v1/node-a/status` | Node A / LWT → Backend/App | `1` | `true` | [node-a-status-online.json](../contracts/examples/node-a-status-online.json)<br>[node-a-status-offline-lwt.json](../contracts/examples/node-a-status-offline-lwt.json) |
| `node-a` | `smarthome/v1/node-a/meta` | Node A → Backend | `1` | `true` `[PROPOSED]` | [node-a-meta.json](../contracts/examples/node-a-meta.json) |
| `node-b` | `smarthome/v1/node-b/event` | Node B → Broker/Backend | `1` | `false` | [node-b-event-door-away-alarm.json](../contracts/examples/node-b-event-door-away-alarm.json)<br>[node-b-event-motion.json](../contracts/examples/node-b-event-motion.json) |
| `node-b` | `smarthome/v1/node-b/command` | Backend → Node B | `1` | `false` | [node-b-command-security-mode.json](../contracts/examples/node-b-command-security-mode.json)<br>[node-b-command-servo.json](../contracts/examples/node-b-command-servo.json) |
| `node-b` | `smarthome/v1/node-b/command-ack` | Node B → Backend | `1` | `false` | [node-b-command-ack-rejected.json](../contracts/examples/node-b-command-ack-rejected.json) |
| `node-b` | `smarthome/v1/node-b/state` | Node B → Backend/App | `1` | `true` | [node-b-state.json](../contracts/examples/node-b-state.json) |
| `node-b` | `smarthome/v1/node-b/status` | Node B / LWT → Backend/App | `1` | `true` | [node-b-status-online.json](../contracts/examples/node-b-status-online.json)<br>[node-b-status-offline-lwt.json](../contracts/examples/node-b-status-offline-lwt.json) |
| `node-b` | `smarthome/v1/node-b/meta` | Node B → Backend | `1` | `true` `[PROPOSED]` | [node-b-meta.json](../contracts/examples/node-b-meta.json) |

---

## 7. Danh sách đề xuất cần nhóm phê duyệt (`PROP-01` .. `PROP-08`)

> Tất cả 8 mục dưới đây hiện ở trạng thái **`CHƯA PHÊ DUYỆT`**. Cần phản hồi chốt từ các thành viên liên quan trước khi viết code tích hợp MQTT trên firmware, Backend và Android.

| Mã | Nội dung cần chốt | Đề xuất của Firmware | Trạng thái | Người cần quyết định | Ảnh hưởng nếu chưa chốt |
| --- | --- | --- | --- | --- | --- |
| **PROP-01** | Bổ sung 2 topic `command-ack` (`retain: false`) và `meta` (`retain: true`) vào bộ topic chính thức v1 | Giữ đủ 6 topic cho mỗi node như Mục 6; phân biệt `command-ack` với `state` | **CHƯA PHÊ DUYỆT** | Khương Anh Tài (Backend) & Bùi Trung Hoàng (Android) | Backend không subscribe `command-ack`/`meta` nên App không phân biệt được lệnh `rejected`/`expired`/`duplicate` và không thấy trạng thái xác nhận chi tiết |
| **PROP-02** | Cơ chế kiểm tra hạn lệnh khi `time_synced == false` (`ttl_ms`, `require_time_sync`, `target_boot_id`) | Mặc định từ chối lệnh có `expires_at` khi chưa sync giờ; chỉ cho phép nếu có local NTP hoặc xác thực phiên qua `boot_id` + `ttl_ms` sau `status=online` | **CHƯA PHÊ DUYỆT** | Khương Anh Tài (Backend) & Phạm Ngọc Hưng (Firmware) | Nếu test trong LAN không có Internet và chưa có local NTP, mọi lệnh điều khiển từ App có thể bị ESP32 từ chối với lý do `clock_unsynced_cannot_verify_expiry` |
| **PROP-03** | Đồng bộ thời gian nội bộ LAN (local NTP) khi mất Internet | Dựng local NTP trong mạng LAN/Router/Server test để giữ `time_synced: true` | **CHƯA PHÊ DUYỆT** | Khương Anh Tài (Backend/Hạ tầng) & Phạm Ngọc Hưng (Firmware) | Không có giờ chuẩn làm lệch timestamp telemetry khi offline Internet và có thể làm fail bước kiểm tra hạn chứng chỉ X.509 khi handshake TLS |
| **PROP-04** | Khóa chống trùng backfill `(device_id, boot_id, seq)` & cờ `timestamp_source = "server_fallback"` | Backend dùng unique key `(device_id, boot_id, seq)` để `ON CONFLICT DO NOTHING` và gắn `server_received_at` khi `timestamp: null` | **CHƯA PHÊ DUYỆT** | Khương Anh Tài (Backend) | Dễ bị trùng bản ghi trong PostgreSQL khi QoS 1 gửi lại / xả backfill, hoặc lỗi `NOT NULL` nếu DB ép `timestamp` do ESP32 gửi phải khác `null` |
| **PROP-05** | Hỗ trợ lệnh `target: "thresholds"` cấu hình ngưỡng `AUTO` cho `node-a` | Nhận object `light_on_lux`/`light_off_lux`/`fan_on_temp_c`/`fan_off_temp_c` qua `node-a/command` và trả lại trong `node-a/state` | **CHƯA PHÊ DUYỆT** | Khương Anh Tài (Backend) & Bùi Trung Hoàng (Android) | Chức năng "cấu hình ngưỡng tự động" trong BT02 không điều chỉnh được từ xa qua App, chỉ cố định cứng trên firmware |
| **PROP-06** | Quy tắc xung đột công tắc vật lý `HOME/AWAY` trên `node-b` (`REAL`) & vai trò `PIR` khi `AWAY` | Công tắc vật lý hoạt động theo chuyển mạch (*edge transition* / last-writer-wins) và phát `state`; mở cửa kích hoạt còi ngay, `PIR` gửi `event` và thống nhất có hú còi hay không | **CHƯA PHÊ DUYỆT** | Phạm Ngọc Hưng, Khương Anh Tài & Bùi Trung Hoàng (Cả nhóm) | Nếu dùng công tắc giữ mức mà app đổi mode ngược lại thì công tắc sẽ ghi đè app liên tục; hoặc còi kêu sai kỳ vọng khi có chuyển động trong chế độ `AWAY` |
| **PROP-07** | Hiển thị ánh sáng khi chạy `SIM` (`light_source: "LDR_SIM"`, `light_lux: null`, `light_raw: 0..4095`) | Android hiển thị `light_raw` kèm nhãn mô phỏng; **không** ép `null` thành `0 lux` | **CHƯA PHÊ DUYỆT** | Bùi Trung Hoàng (Android) & Khương Anh Tài (Backend) | Giao diện Android và biểu đồ lịch sử có thể báo `0 lux` giả (gây hiểu nhầm tối hoàn toàn) hoặc văng lỗi parse khi `light_lux` là `null` |
| **PROP-08** | Cơ chế kết nối MQTT của môi trường `SIM` (Lab CA mTLS tự ký vs mock loopback `127.0.0.1`) | Ưu tiên Lab CA mTLS riêng cho test; hoặc mock loopback `127.0.0.1` cô lập (vẫn cấm `setInsecure()` ở `REAL`) | **CHƯA PHÊ DUYỆT** | Khương Anh Tài (Backend) & Phạm Ngọc Hưng (Firmware) | Hai bên không dựng được môi trường thử nghiệm thống nhất cho Wokwi / mock broker khi chưa có phần cứng hoặc chưa phát hành bộ test cert |

---

## 8. Nhu cầu giao tiếp Backend ↔ Android `[PROPOSED — CHƯA TRIỂN KHAI]`

> **Lưu ý ranh giới:** Tài liệu thiết bị **không** tự thiết kế đường dẫn REST API, JWT payload hay schema bảng PostgreSQL của Backend. Các hạng mục dưới đây chỉ liệt kê **nhu cầu dữ liệu & hành vi tối thiểu** mà Backend (Khương Anh Tài) và Android (Bùi Trung Hoàng) cần tự thống nhất và triển khai; **hiện toàn bộ ở trạng thái `PROPOSED` — CHƯA TRIỂN KHAI**.

1. **`[PROPOSED]` Xác thực người dùng & Quản lý thiết bị (Backend ↔ Android):**
   - App đăng nhập lấy phiên/token do Backend cấp; chỉ hiển thị danh sách node có trạng thái hợp lệ trong Backend Device Registry (node lạ bị chặn ở lớp MQTT/Registry không xuất hiện trên App).
2. **`[PROPOSED]` Lấy trạng thái tức thời & snapshot ban đầu (`state` + `status` + `meta`):**
   - Cung cấp cho Android snapshot mới nhất của `node-a` và `node-b` gồm `online`, `last_seen_at`, `stale`, `reported state`, `sensor_health` và bộ ngưỡng `thresholds` đang áp dụng.
3. **`[PROPOSED]` Kênh gửi lệnh điều khiển từ Android → Backend → MQTT:**
   - Backend nhận yêu cầu từ Android, sinh `command_id` duy nhất + `issued_at` + `expires_at` (+ `ttl_ms`), publish xuống `smarthome/v1/{device_id}/command` (QoS `1`, `retain: false`), và trả về cho Android trạng thái ban đầu là `pending` kèm `command_id` để đối chiếu kết quả thực thi thực tế.
4. **`[PROPOSED]` Kênh cập nhật thời gian thực (WebSocket / Push từ Backend → Android):**
   - Đẩy các thay đổi mới nhất về `telemetry` (5s), `event` (≤ 1s), `state`, `status` (`online`/`offline`) và kết quả `command-ack` (`applied`/`rejected`/`expired`/`duplicate`) tới Android để UI cập nhật mà không cần polling dày.
5. **`[PROPOSED]` Truy vấn lịch sử cảm biến & sự kiện tối thiểu 7 ngày (Backend → Android):**
   - Hỗ trợ lọc theo `device_id`, khoảng thời gian (`from`–`to` trong tối thiểu 7 ngày gần nhất), phân biệt được nguồn mốc thời gian (`time_synced` thật vs `server_fallback`) và các bản ghi `valid: false` (lỗi cảm biến) để đồ thị Android không vẽ điểm `0` giả.

---

## 9. Trách nhiệm thành viên & Checklist kiểm thử liên nhóm (theo BT02)

### 9.1. Phân chia trách nhiệm chính thức
| Thành viên | Phụ trách | Đầu việc bàn giao cụ thể |
| --- | --- | --- |
| **Phạm Ngọc Hưng** | Thiết bị / Firmware ESP32 & Tích hợp phần cứng | Hoàn thiện driver `DHT11`/`BH1750` (`REAL`), `DHT22`/`LDR` (`SIM`), `PIR`/`Reed`/`Buzzer`/`SG90`/`Home-Away`, MQTT/mTLS client (`REAL` không `setInsecure()`), validation 6 bước, edge rules offline, ring-buffer RAM, kiểm thử unit + Wokwi + mạch thật. |
| **Khương Anh Tài** | Backend, Broker, PostgreSQL & Docker | Dựng Mosquitto (mTLS + ACL theo `CN`), PostgreSQL schema (lưu trữ ≥ 7 ngày + dedup backfill), Backend Node.js/Express + WebSocket, Device Registry SSOT chống node lạ, xử lý `time_synced=false`, cung cấp API/WS `[PROPOSED]` cho Android. |
| **Bùi Trung Hoàng** | Android Kotlin, Test Plan & Tài liệu nghiệm thu | Ứng dụng Kotlin/Jetpack Compose MVVM (Dashboard, Control với `pending` vs `reported`, History 7 ngày, Alarm), xử lý `offline`/`stale`/`null` cảm biến, chủ trì bảng đo kiểm thử BT02, video demo & hoàn thiện báo cáo. |

### 9.2. Checklist kiểm thử liên nhóm đối chiếu chỉ tiêu [Bao_cao_BT02_IoT.docx](Bao_cao_BT02_IoT.docx)
- [ ] **1. Độ trễ điều khiển LAN (`≤ 2 giây`, `≥ 95%` trong 20 lần thử):** Gửi 20 lệnh bật/tắt đèn, quạt, đổi mode hoặc quay servo từ Android; đo tổng thời gian từ lúc bấm gửi tới lúc nhận `command-ack` + `reported state` cập nhật trên App (không đo ở `MQTT PUBACK`).
- [ ] **2. Chu kỳ telemetry & sự kiện (`5 giây/lần` môi trường; `≤ 1 giây` cửa/chuyển động):** Đối chiếu `uptime_ms`/timestamp giữa Serial ESP32, log Broker và bản ghi lưu ở PostgreSQL.
- [ ] **3. Edge rules khi mất Backend / mất mạng:** Ngắt Broker/Internet, thay đổi nhiệt độ/ánh sáng ở `node-a` (`AUTO`) và mở cửa ở `node-b` (`AWAY`); xác minh ESP32 vẫn tự bật/tắt tải và hú còi cục bộ, sau đó kiểm tra đồng bộ lại `state` + backfill khi mạng có lại.
- [ ] **4. Thử nghiệm lỗi cảm biến & dữ liệu `null`:** Rút cảm biến trên mạch thử (`REAL`) hoặc giả lập lỗi đọc; xác nhận `node-a` gửi `valid: false` với `temperature_c: null`/`humidity_pct: null` và Android hiển thị cảnh báo lỗi (không hiển thị `0`).
- [ ] **5. Kiểm thử Lệnh hết hạn / Duplicate / Reject:** Gửi lại cùng `command_id` (xác minh `duplicate` không kích hoạt lại actuator), gửi góc servo vượt biên an toàn (xác minh `rejected`), gửi lệnh hết `expires_at` hoặc khi `time_synced: false` (xác minh không thực thi lệnh cũ).
- [ ] **6. Phục hồi kết nối (`≤ 30 giây`, 5 lần ngắt/mở lại mạng):** Tắt/bật Router/AP hoặc Mosquitto 5 lần liên tiếp; đo thời gian từ lúc mạng bình thường trở lại đến khi `status` chuyển `online: true` và đồng bộ `state` mới nhất.
- [ ] **7. An toàn & Chống node lạ gia nhập topology:** Thử kết nối bằng chứng chỉ sai CA, sai `CN` với `device_id` trong topic/payload, hoặc chứng chỉ hợp lệ nhưng `device_id` chưa có / đã `revoked` trong Backend Registry; xác minh Broker/Backend chặn đứng và không lộ dữ liệu trên Android.
- [ ] **8. Lưu trữ lịch sử 7 ngày & Vận hành liên tục `≥ 24 giờ`:** Chạy hệ thống liên tục ≥ 24 giờ trên phần cứng thật (theo dõi `free_heap`, `uptime_ms`, `reconnect_count`, không treo/reset thủ công) và truy vấn API lịch sử 7 ngày từ Android khớp PostgreSQL.

---

## 10. Chuyển đổi sang ThingsBoard Device Provisioning

> **Trạng thái mục này:** **`DRAFT / PROPOSED`** *(cập nhật bổ sung theo hướng chuyển đổi ThingsBoard; không xóa nội dung lịch sử Mục 1–9 để cả nhóm đối chiếu sự thay đổi).*  
> **Đặc tả giao thức đầy đủ:** [contracts/thingsboard-v1.md](../contracts/thingsboard-v1.md).

### 10.1. Thay đổi cốt lõi & Cảnh báo nội dung cũ không còn mặc định
1. **Bỏ mặc định X.509 client certificates & Mosquitto ACL:** Nhóm sử dụng **ThingsBoard Device Provisioning** với cặp `provisionDeviceKey` và `provisionDeviceSecret` cấp ở mức **Device Profile** (không mặc định tự phát hành X.509 client cert + Mosquitto `use_identity_as_username` như Mục 3.1–3.2).
2. **Tách biệt Provisioning Key/Secret với MQTT Credentials vận hành:**
   - Khi chưa có credential trong NVS, ESP32 kết nối MQTT với `username = "provision"`, subscribe `/provision/response`, publish `/provision/request`, rồi nhận `credentialsType` (`ACCESS_TOKEN` hoặc `MQTT_BASIC`) và `credentialsValue`.
   - **Tuyệt đối không** dùng `provisionDeviceKey`/`provisionDeviceSecret` để publish telemetry hay nhận RPC sau khi đăng ký.
   - Hai nút `node-a` và `node-b` vẫn hoạt động độc lập và **bắt buộc có `deviceName` + runtime credentials riêng biệt** lưu bền vững trong NVS cho các lần reconnect/reboot sau.
3. **Không dùng trực tiếp 12 topic `smarthome/v1/...`:** ThingsBoard sử dụng `v1/devices/me/telemetry`, `v1/devices/me/attributes`, `v1/devices/me/rpc/request/+`, `v1/devices/me/rpc/response/{requestId}`. Các tính năng Mosquitto như LWT (`.../status`) và broker-level `retain: true` (`.../state`) **không** hoạt động nguyên bản giống Mosquitto trên ThingsBoard built-in MQTT transport; cần thay thế bằng **Device State Service (`active`, `inactivityTimeout`)**, client attributes và Rule Chain / Backend adapter.
4. **Bảo vệ bí mật cấu hình:** Mọi tài liệu/code chỉ dùng placeholder giả (`PUT_PROVISION_KEY_HERE`, `PUT_PROVISION_SECRET_HERE`, `REPLACE_WITH_PROVISIONED_ACCESS_TOKEN`). Cấm commit key/secret/token thật vào Git.

### 10.2. Cập nhật trách nhiệm dự kiến của Backend, Android & Firmware

| Thành phần | Trách nhiệm chuyển đổi cần chuẩn bị (`[PROPOSED]`) | Ranh giới & Lưu ý quan trọng |
| --- | --- | --- |
| **Backend / ThingsBoard Admin (Khương Anh Tài)** | - Dựng/chọn hạ tầng ThingsBoard (`CE` Docker hay `Cloud` theo `TB-DEC-01`).<br>- Tạo **Device Profile** có Provisioning; khuyến nghị dùng **`Check pre-provisioned devices`** (`TB-DEC-04`) và tạo trước thực thể cho `node-a`, `node-b` để chặn node lạ tự sinh.<br>- Thiết lập Rule Chain / Backend Adapter: xử lý `Connect`/`Disconnect`/`Inactivity` + `inactivityTimeout` thay LWT; giữ các trường hợp đồng `command_id`, `issued_at`, `expires_at` (chuẩn 20 ký tự `"YYYY-MM-DDTHH:MM:SSZ"`, không chứa `.sssZ`), `ttl_ms`, `require_time_sync` trong `params` của Server-side RPC; chống trùng `(device_id, boot_id, seq)`; xử lý `time_synced=false` + `buffered=true`.<br>- Đảm bảo **lưu lịch sử ≥ 7 ngày** (ThingsBoard TSDB TTL hoặc PostgreSQL BFF riêng theo `TB-DEC-08`). | Nếu chọn mô hình giữ Node.js + PostgreSQL làm BFF trước Android (`TB-DEC-07`), Backend chịu trách nhiệm đồng bộ Telemetry/Attribute/Alarm/RPC giữa ThingsBoard và Android API/WS. |
| **Android (Bùi Trung Hoàng)** | - **Giữ nguyên toàn bộ hành vi UI/UX đã bàn ở Mục 4:** phân biệt `desired` → `pending` → `reported state` (chỉ xác nhận khi có `command-ack`/RPC response + `state` mới); cấm ép `null` thành `0°C` hay `0 lux`; xử lý `offline`/`stale`, `alarm_triggered` vs `motion_detected`, lịch sử 7 ngày.<br>- Cập nhật tầng kết nối (Repository/Retrofit/WebSocket) theo `TB-DEC-07`: kết nối tới Backend BFF **hoặc** trực tiếp ThingsBoard REST (`/api/auth/login`, `/api/plugins/telemetry/...`, `/api/rpc/twoway/...`) & WebSocket (`/api/ws`). | Không thay đổi logic kiểm chứng an toàn của ứng dụng Android; chỉ thay đổi endpoint/schema dữ liệu đầu vào tùy quyết định `TB-DEC-07`. |
| **Firmware ESP32 (Phạm Ngọc Hưng)** | - Hiện tại trong repo đã hoàn thiện & unit-test các module thuần C++ (`automation`/`security`, `command_guard`, `utc_time`); **chưa** triển khai Wi-Fi, ThingsBoard provisioning, JSON codec, MQTT và TLS.<br>- Ở các task tiếp theo sau khi chốt `TB-DEC-01`..`TB-DEC-09`, sẽ viết lớp provisioning + lưu NVS + adapter ánh xạ ThingsBoard MQTT API nối vào `CommandGuard`/`UtcParser` và state machine hiện có. | Giữ nguyên quy tắc an toàn biên (`dht/bh1750` lỗi → `null`, `SG90` trong biên, `pir_triggers_alarm_in_away=false`, `HOME/AWAY` ưu tiên an toàn). |

### 10.3. Danh sách quyết định cần nhóm chốt trước khi code tích hợp (`[PROPOSED / OPEN]`)

> Toàn bộ 9 quyết định dưới đây đang ở trạng thái **`CHƯA ĐƯỢC PHÊ DUYỆT`**, đồng bộ với Mục 6 của [contracts/thingsboard-v1.md](../contracts/thingsboard-v1.md):

| Mã | Quyết định cần thống nhất | Lựa chọn đang mở |
| --- | --- | --- |
| **`TB-DEC-01`** | Nền tảng ThingsBoard | ThingsBoard CE (Docker LAN/VPS) vs ThingsBoard Cloud vs PE |
| **`TB-DEC-02`** | Kênh Provisioning | MQTT (`/provision/request` + `/provision/response`, `username="provision"`) vs HTTP (`POST /api/v1/provision`) |
| **`TB-DEC-03`** | Loại Credential vận hành | `ACCESS_TOKEN` (khuyến nghị gọn nhẹ) vs `MQTT_BASIC` (`clientId`/`userName`/`password`) |
| **`TB-DEC-04`** | Chiến lược chống node lạ & số Profile | `Check pre-provisioned devices` (đề xuất ưu tiên) vs `Allow creating new devices`; 1 hay 2 Device Profile |
| **`TB-DEC-05`** | Quy ước đặt `deviceName` | Tên cố định vs có tiền tố/MAC tránh trùng tenant; mapping về `device_id` (`"node-a"`/`"node-b"`) cho `CommandGuard` |
| **`TB-DEC-06`** | TLS (`8883`) & Đồng bộ đồng hồ (SNTP) | Pin Server Root CA trên `REAL` (cấm `setInsecure()`) vs LAN `SIM`; nguồn SNTP / local NTP cho TLS & `UtcParser` |
| **`TB-DEC-07`** | Kiến trúc Backend & Android | Android gọi qua Node.js BFF (bọc sẵn RPC envelope & lưu DB) vs gọi thẳng ThingsBoard REST/WS + Rule Chain |
| **`TB-DEC-08`** | Chính sách lưu trữ 7 ngày (BT02) | ThingsBoard TSDB (PostgreSQL/TTL hoặc quota Cloud DPSD) vs PostgreSQL riêng của Backend BFF |
| **`TB-DEC-09`** | Chuyển hóa `PROP-01`..`PROP-08` | Chốt mapping `command-ack`, xử lý lệnh khi `time_synced=false`, cập nhật `thresholds` (RPC vs Shared Attributes), `PIR`, `LDR_SIM` |
