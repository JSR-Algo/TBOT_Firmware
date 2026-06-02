# Tài liệu tích hợp Tbot Robot (Tiếng Việt)

File chính: `xiaozhi-robot-vi.tex`

## Cách compile ra PDF

LaTeX **chưa được cài** trên máy này. Có 3 cách:

### ⚡ Cách 1 — Overleaf (online, không cài gì)
1. Vào https://www.overleaf.com → đăng ký (free)
2. Click **New Project → Blank Project**
3. Mở project, xóa file mặc định, upload `xiaozhi-robot-vi.tex`
4. Chọn compiler **XeLaTeX** (góc trên trái → Menu → Compiler)
5. Click **Recompile** → tải PDF

### 💻 Cách 2 — MiKTeX (offline, lần đầu cài ~200MB)
1. Tải MiKTeX: https://miktex.org/download
2. Cài đặt (chọn "Install missing packages on the fly: Yes")
3. Mở PowerShell ở folder này:
   ```powershell
   xelatex xiaozhi-robot-vi.tex
   xelatex xiaozhi-robot-vi.tex   # chạy lần 2 để TOC chuẩn
   ```
4. Output: `xiaozhi-robot-vi.pdf`

### 📦 Cách 3 — TeX Live (offline, cài đầy đủ ~5GB)
1. Tải: https://tug.org/texlive/acquire-netinstall.html
2. Cài full
3. Tương tự cách 2

## Yêu cầu font
File dùng:
- **Times New Roman** (mainfont)
- **Calibri** (sansfont)  
- **Consolas** (monofont)

Cả 3 đều có sẵn trên Windows. Nếu thiếu, sửa trong file:
```latex
\setmainfont{Times New Roman}   % đổi sang font có sẵn
```

## Cập nhật tài liệu
Khi project thay đổi, ping Claude với yêu cầu cụ thể (vd. "thêm phần X vào tài liệu LaTeX") — Claude sẽ edit `xiaozhi-robot-vi.tex` rồi bạn re-compile.

## Nội dung
- Khái niệm nền tảng (ESP32-S3, ESP-IDF, build pipeline, WebSocket, MCP, AFE...)
- Pin GPIO chi tiết của board ES3C28P (trích từ file xlsx chính thức của kit)
- Map source code xiaozhi-esp32 (mỗi folder làm gì)
- Kế hoạch tích hợp servo + WebSocket custom server
- Workflow build/flash/test
- Glossary thuật ngữ

~50 trang PDF.
