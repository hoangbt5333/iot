---
name: platformio-workflow
description: >-
  Use this skill when compiling, building, cleaning, testing, or diagnosing PlatformIO firmware for ESP32 in node-environment or node-security, including env:sim (Wokwi simulation), env:real (physical hardware), and env:native (Unity unit testing).
---

# PlatformIO Workflow & Build Guide

Tài liệu hướng dẫn quy trình biên dịch, kiểm thử đơn vị và xử lý sự cố mã nguồn firmware ESP32 bằng PlatformIO CLI trong dự án.

---

## 1. Môi Trường Thực Thi PlatformIO

Dự án có hai node firmware độc lập:
- `firmware/node-environment` (Node A)
- `firmware/node-security` (Node B)

Mỗi node hỗ trợ 3 môi trường (`env`):
1. **`sim`**: Biên dịch mã nhị phân ESP32 phục vụ mô phỏng Wokwi (`-D NODE_ENV_SIM=1`). Output tạo ra `.pio/build/sim/firmware.bin` và `firmware.elf`.
2. **`real`**: Biên dịch cho bo mạch ESP32 thật (`-D NODE_ENV_REAL=1`) với driver phần cứng thật (DHT11, BH1750, PIR, servo...).
3. **`native`**: Biên dịch mã chạy trực tiếp trên CPU máy tính chủ (x86_64) để chạy nhanh các bài Unit Test với framework Unity mà không cần nạp vào bo mạch.

---

## 2. Cách Gọi Lệnh PlatformIO Trên Windows

Nếu lệnh `pio` chưa được thêm vào biến môi trường hệ thống (`PATH`), có thể gọi thông qua Python hoặc đường dẫn mặc định của PlatformIO Core:

```powershell
# Cách 1 (Nếu đã có pio trong PATH):
pio run -d firmware/node-environment -e sim

# Cách 2 (Nếu dùng PlatformIO Core cài qua Python):
python -m platformio run -d firmware/node-environment -e sim

# Cách 3 (Dùng trực tiếp binary trong virtualenv của PlatformIO):
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -d firmware/node-environment -e sim
```

---

## 3. Các Lệnh Thường Dùng Cho Từng Tác Vụ

### 3.1. Biên dịch phục vụ mô phỏng Wokwi (`env:sim`)
Sau khi sửa code, chạy lệnh sau để cập nhật file binary cho Wokwi:

```powershell
# Cho Node A:
pio run -d firmware/node-environment -e sim

# Cho Node B:
pio run -d firmware/node-security -e sim
```
*Lưu ý:* Wokwi sẽ đọc trực tiếp từ `firmware/<node>/.pio/build/sim/firmware.bin` theo cấu hình trong `wokwi.toml`.

### 3.2. Chạy Unit Test trên máy tính (`env:native`)
Chạy test mà không cần nạp mạch hay mở Wokwi:

```powershell
# Chạy toàn bộ test của Node A:
pio test -d firmware/node-environment -e native

# Chạy toàn bộ test của Node B:
pio test -d firmware/node-security -e native
```

### 3.3. Dọn dẹp cache khi gặp lỗi thư viện
Khi thêm sửa `lib_deps` hoặc đổi cờ compiler:

```powershell
# Xóa thư mục build:
pio run -d firmware/node-environment -t clean -e sim
```

---

## 4. Xử Lý Các Lỗi Biên Dịch Phổ Biến

1. **Lỗi `fatal error: ... no such file or directory`:**
   - Kiểm tra xem thư viện đã được khai báo trong `lib_deps` của `platformio.ini` chưa.
   - Với thư viện trong thư mục `lib/`, kiểm tra xem đã include đúng file header trong `src/` hoặc `include/` chưa.
2. **Lỗi `undefined reference to ...` (Linker Error):**
   - Đảm bảo các hàm triển khai trong `.cpp` khớp chữ ký với khai báo trong `.h`.
   - Với môi trường `native`, đảm bảo không gọi các hàm đặc thù của Arduino Core (như `digitalRead`, `Serial`, `WiFi`) trong các file logic thuần kiểm thử.
3. **Tràn RAM / Flash:**
   - Kiểm tra kích thước RAM và Flash sau khi build:
     ```text
     RAM:   [==        ]  18.5% (used 60620 bytes from 327680 bytes)
     Flash: [======    ]  62.1% (used 814080 bytes from 1310720 bytes)
     ```
   - Nếu RAM tăng đột biến, kiểm tra các mảng toàn cục, kích thước queue trong `offline_queue` và chuỗi JSON tĩnh.
