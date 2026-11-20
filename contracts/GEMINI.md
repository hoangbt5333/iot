# Quy Tắc Quản Lý Hợp Đồng Dữ Liệu (Contracts)

> Áp dụng cho: Mọi đặc tả giao tiếp trong thư mục `contracts/` giữa Firmware ESP32, Broker, Backend Node.js và Android Kotlin.

---

## 1. Trạng Thái Hợp Đồng & Quy Ước Kiến Trúc

1. **`contracts/thingsboard-v1.md` (DRAFT / PROPOSED):**
   - Hướng kiến trúc chuẩn đang đề xuất của dự án sử dụng giao thức chuẩn ThingsBoard:
     - **Telemetry:** Topic `v1/devices/me/telemetry` (dữ liệu chuỗi thời gian như nhiệt độ, độ ẩm, trạng thái cửa, độ sáng).
     - **Client Attributes:** Topic `v1/devices/me/attributes` (thông tin cấu hình, firmware version, IP, MAC).
     - **Server-side RPC:** Topic `v1/devices/me/rpc/request/+` (lệnh điều khiển từ Android/Cloud xuống ESP32 như bật đèn, đổi chế độ, gạt servo).
     - **Shared Attributes:** Thay đổi ngưỡng tự động (Hysteresis threshold) từ xa.
   - Các quyết định `TB-DEC-01` đến `TB-DEC-09` nếu được chỉnh sửa phải cập nhật trạng thái rõ ràng (`OPEN` -> `APPROVED` / `REJECTED`).

2. **`contracts/mqtt-v1.md` (LEGACY / REFERENCE):**
   - Bộ 12 custom topic (`smarthome/v1/{device_id}/...`) dùng đối chiếu lịch sử ngữ nghĩa dữ liệu.
   - Giữ làm phương án dự phòng (fallback) nếu nhóm không triển khai ThingsBoard Server mà tự dựng Mosquitto độc lập.

---

## 2. Quy Chuẩn Kiểu Dữ Liệu & Payload JSON

1. **Thời gian (Timestamp):**
   - Mọi mốc thời gian trong payload (nếu có trường `ts`) bắt buộc dùng chuẩn **UTC Epoch Milliseconds** (số nguyên 64-bit, ví dụ: `1732101234000`).
   - Nếu thiết bị chưa đồng bộ được SNTP khi đang offline, phải đánh dấu flag offline hoặc dùng local monotonic tick cho đến khi có mạng.

2. **Kiểu dữ liệu trường (Types Consistency):**
   - **Số thực (Float):** Giới hạn tối đa 1–2 chữ số thập phân cho cảm biến (ví dụ `temperature: 28.5`, `humidity: 65.0`).
   - **Boolean:** Bắt buộc dùng `true`/`false` chuẩn JSON, tuyệt đối không dùng số nguyên `0`/`1` hay chuỗi `"true"`/`"false"`.
   - **Đơn vị chuẩn:**
     - Nhiệt độ: Độ C (°C).
     - Độ ẩm: Phần trăm tương đối (%RH).
     - Ánh sáng: Lux (`light_lux`), nếu là cảm biến mô phỏng LDR chưa hiệu chuẩn thì ghi rõ trường `light_raw` và `source: "LDR_SIM"`.
     - Góc quay servo: Số nguyên từ 0 đến 180 độ.

3. **RPC Request / Response Format:**
   - Mọi bản tin RPC gửi xuống ESP32 phải có `method` và `params`.
   - ESP32 khi thực thi xong phải gửi response về topic `v1/devices/me/rpc/response/{requestId}` phản hồi trạng thái thực tế sau khi điều khiển (`success: true/false`, giá trị actuator hiện thời).
