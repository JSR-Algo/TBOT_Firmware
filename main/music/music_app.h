#ifndef MUSIC_APP_H_
#define MUSIC_APP_H_

#include <functional>
#include <string>
#include <vector>

// APP_MUSIC: phat WAV PCM 16-bit mono (uu tien 24kHz) tu /sdcard/tbot_music theo
// kieu STREAMING (doc tung chunk day vao playback queue cua AudioService), khong
// load ca file vao RAM. Khong pha chatbox/audio; dung chung duong output codec.

enum class MusicState { Stopped, Playing, Paused };

// Quet /sdcard/tbot_music, tra ve ten cac file .wav (sap xep). Rong neu khong co.
std::vector<std::string> MusicScanFiles();

// Tao music streaming task (goi 1 lan luc khoi tao app).
void MusicPlayerInit();

// Callback khi bai ket thuc tu nhien (chay tren music task -> nen marshal sang
// main task ben trong callback). Dung de gui DANCE:MUSIC:STOP + cap nhat UI.
void MusicSetOnEnd(std::function<void()> cb);

// Phat file theo ten (trong /sdcard/tbot_music). false neu mo/parse loi.
bool MusicPlay(const std::string& filename);
void MusicPause();
void MusicResume();
void MusicStop();
MusicState MusicGetState();

#endif  // MUSIC_APP_H_
