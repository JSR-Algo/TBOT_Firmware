#include "main/display/lvgl_display/gif/gif_rgb565.h"
#include <cassert>
int main() {
    const unsigned char source[] = {0,0,255,255, 0,255,0,255, 255,0,0,255, 255,255,255,0};
    unsigned short output[16] = {};
    UpscaleOpaqueGifRgb565(source, output, 2, 2);
    const unsigned short expected[] = {0xf800,0xf800,0x07e0,0x07e0,0xf800,0xf800,0x07e0,0x07e0,0x001f,0x001f,0,0,0x001f,0x001f,0,0};
    for (int i=0;i<16;++i) assert(output[i]==expected[i]);
}
