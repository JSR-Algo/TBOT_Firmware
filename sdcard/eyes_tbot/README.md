# eyes_tbot — biểu cảm mắt TBOT (chế độ trò chuyện)

Hoạt ảnh mắt phát sáng, phát từ thẻ SD, hiển thị khi robot ở chế độ trò chuyện.

## happy (vui)
- `happy.gif` — 320×180, **24 frame thật**, ~12 fps (delay 80 ms/frame → 12.5 fps, mức GIF gần 12 fps nhất), lặp vô hạn (~2 giây/vòng).
  - Là 24 frame do hoạ sĩ vẽ, cắt trực tiếp — không nội suy, nên nét và mượt.
  - Encode: crush đuôi tối về đen + palette chung, KHÔNG dither → khử banding "viền xanh" quanh mắt, nền đen sạch tuyệt đối (mép = maxblue 0).
- `happy_frames/frame_01.png … frame_24.png` — 24 frame gốc đã cắt (320×180), giữ lại để tái dùng / đổi fps / xuất raw.

## Nguồn
3 sprite sheet trong `~/Downloads/eyes/`:
- sheet (1): HAPPY – FRAMES 1–10  (lưới 5×2)
- sheet (2): HAPPY – FRAMES 11–20 (lưới 5×2)
- sheet (3): HAPPY – FRAMES 21–24 (lưới 2×2)

Pipeline: tự dò lưới theo vùng phát sáng xanh → cô lập từng tile (không dính ô kế) →
chuẩn hoá theo bề rộng cặp mắt để 24 frame cùng cỡ → neo tâm mắt cố định (không giật) →
resize LANCZOS về 320×180 → GIF palette chung để đỡ nhấp nháy.

## Ghi chú thiết bị
- LCD board hiện tại: 480×320. GIF để 320×180 theo yêu cầu (vùng hiển thị mắt); scale khi vẽ.
- GIF chỉ biểu diễn được bội số 10 ms nên không đạt 60 fps thật; muốn mượt hơn/không tốn CPU giải mã
  thì xuất chuỗi frame thô (BMP/RGB565) từ `happy_frames/` rồi tự blit.
