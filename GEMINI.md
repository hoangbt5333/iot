# Smart Home IoT System – Workspace Rules & Agent Guidelines

> Dự án: **Hệ thống nhà thông minh IoT dùng ESP32 và ứng dụng Android Kotlin** (Nhóm 9 - IT4735).  
> Thành viên: **Phạm Ngọc Hưng** (Firmware/Hardware), **Khương Anh Tài** (Backend/DB/MQTT), **Bùi Trung Hoàng** (Android App).

---

## 1. Nguyên Tắc Kiến Trúc & Ranh Giới Dự Án

1. **Phân định 2 Node độc lập (Dual-Node Architecture):**
   - **Node A (Environment & Comfort):** Giám sát nhiệt độ, độ ẩm, ánh sáng; điều khiển đèn, quạt (tự động theo ngưỡng có trễ *hysteresis* hoặc thủ công).
   - **Node B (Security & Access):** Giám sát chuyển động (PIR), cửa đóng/mở (reed switch), nút Home/Away vật lý; điều khiển còi báo động (buzzer), servo mở cửa, LED trạng thái.
   - Hai node trao đổi dữ liệu qua MQTT broker, **không nối dây trực tiếp với nhau**.

2. **Triết lý Offline-First (Local Edge Automation):**
   - Mọi quy tắc tự động hóa cốt lõi (edge rules) như tự bật quạt khi nóng, tự hú còi khi cửa mở trong chế độ AWAY **phải chạy cục bộ trên ESP32**.
   - Thiết bị phải hoạt động bình thường ngay cả khi mất kết nối Wi-Fi, mất Internet hoặc MQTT Broker sập.
   - Hàng đợi tin nhắn ngoại tuyến (`offline_queue`) có dung lượng giới hạn (bounded RAM queue) để lưu trữ telemetry và gửi bù khi có mạng.

3. **Chuyển đổi giao tiếp (Hợp đồng dữ liệu):**
   - Tài liệu `contracts/mqtt-v1.md` (12 custom topics `smarthome/v1/...`) là kiến trúc **`LEGACY / REFERENCE`**.
   - Hướng tiếp cận hiện tại đang hướng tới **ThingsBoard Device Provisioning & ThingsBoard MQTT API** (`v1/devices/me/telemetry`, `v1/devices/me/attributes`, `v1/devices/me/rpc/request/+`) tại `contracts/thingsboard-v1.md` (`DRAFT`).
   - Mọi thay đổi về định dạng dữ liệu trong firmware C++ phải đối chiếu chặt chẽ với tài liệu trong thư mục [contracts/](file:///c:/Users/hoang/Desktop/New%20folder/IoT/contracts).

---

## 2. Bảo Mật & Quản Lý Thông Tin Nhạy Cảm (Secrets)

1. **Tuyệt đối không commit bí mật:**
   - Không commit SSID Wi-Fi, mật khẩu Wi-Fi thật của nhà/phòng lab vào git.
   - Không commit token, private key, JWT secret, mật khẩu cơ sở dữ liệu PostgreSQL.
2. **Cấu hình môi trường:**
   - Sử dụng file mẫu (ví dụ `secrets.h.example` hoặc biến môi trường build flag) thay vì hardcode trong source.
   - Giữ trạng thái code trong repo ở chế độ `UNCONFIGURED` hoặc dùng credential mặc định của môi trường mô phỏng Wokwi (`Wokwi-GUEST` không có password).

---

## 3. Quy Chuẩn Làm Việc Của Agent

1. **Tôn trọng ranh giới file:**
   - Không tự ý sửa đổi các file hợp đồng đã thống nhất trong `contracts/` hoặc tài liệu báo cáo `docs/` trừ khi có yêu cầu rõ ràng từ người dùng.
   - Khi chỉnh sửa firmware, luôn bảo toàn tính tương thích giữa môi trường mô phỏng `sim` và phần cứng thật `real`.
2. **Kiểm tra trước khi kết luận:**
   - Khi sửa đổi logic điều khiển hoặc cấu trúc dữ liệu, phải đảm bảo code biên dịch sạch (`pio run`) và các bài unit test (`pio test`) đều vượt qua.
3. **Ngôn ngữ giao tiếp:**
   - Trả lời và giải thích bằng Tiếng Việt kỹ thuật, rõ ràng, chính xác.
   - Tên biến, hàm, comment trong mã nguồn C++ sử dụng Tiếng Anh kỹ thuật thống nhất.
