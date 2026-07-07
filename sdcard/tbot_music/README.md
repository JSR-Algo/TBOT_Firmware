# TBOT Music

Đặt file nhạc .wav vào thư mục này. Trên thẻ SD, đường dẫn là:
/sdcard/tbot_music/<tên bài>.wav

Định dạng hỗ trợ (Phase 1):
- WAV PCM signed 16-bit
- Mono (stereo sẽ tự lấy kênh trái)
- Sample rate 24000 Hz (khác 24000 vẫn phát nhưng sai cao độ)

Chuyển đổi bằng ffmpeg:
  ffmpeg -i input.mp3 -ac 1 -ar 24000 -sample_fmt s16 output.wav
