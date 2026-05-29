// Diagnostic probe (NOT a ctest) for the NTSC artifact-colour phase across
// the colour pipelines. ColorNTSC is the known-correct reference (its colours
// are pinned by hgr_render_smoke_test against MAME apple2video.cpp). We render
// the same canonical patterns in the other pipelines and print the resulting
// colours so we can see which modes invert the artifact hue (purple↔green,
// blue↔orange = 180° subcarrier-phase error) and by how much.
//
//   build/artifact_probe
//
// Patterns (from hgr_render_smoke_test, MAME-verified for ColorNTSC):
//   $01 @ col0 → pixel 0 = PURPLE (lo-res 3, rgb e6,28,ff)
//   $01 @ col1 → pixel 7 = GREEN  (lo-res 12, rgb 19,d7,00)
//   $81 @ col1 → pixel 7 = ORANGE (R>G>B, R high)

#include "Apple2Display.h"
#include "Memory.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
uint8_t R(uint32_t c){return c&0xFF;} uint8_t G(uint32_t c){return (c>>8)&0xFF;} uint8_t B(uint32_t c){return (c>>16)&0xFF;}

void writeHgr(Memory& m, int y, int col, uint8_t v){
    uint16_t a = 0x2000 + 0x400*(y&7) + 0x80*((y>>3)&7) + 0x28*(y>>6);
    m.memWrite(uint16_t(a+col), v);
}
const char* hue(uint32_t c){
    int r=R(c),g=G(c),b=B(c);
    if(r<24&&g<24&&b<24) return "black";
    if(r>220&&g>220&&b>220) return "white";
    if(b>r&&b>g&&r>g) return "PURPLE/VIOLET";
    if(g>r&&g>b) return "GREEN";
    if(r>g&&g>b&&r>120) return "ORANGE";
    if(b>r&&b>g&&g>r) return "BLUE";
    return "other";
}
void show(const char* tag, Apple2Display& d, int x, int y){
    const uint32_t* p = d.pixels();
    uint32_t c = p[y*d.width()+x];
    std::printf("    %-22s px(%d,%d) = rgb(%3d,%3d,%3d)  %s\n",
                tag, x, y, R(c),G(c),B(c), hue(c));
}

// CPU port of the OE demod (matches NtscPostProcessor's fragment shader,
// neutral params) — demodulate one output sample at signal (sx, line).
uint32_t demodOE(const uint8_t* sig, int sw, int sh, float sx, int line, float phaseShift){
    const float PI=3.14159265358979f;
    auto S=[&](int x){ return (x<0||x>=sw)?0.0f:sig[line*sw+x]/255.0f; };
    float sigmaY=0.8f;
    float sigC = 2.5f + (1.0f-2.5f)*0.5f;   // mix(2.5,1.0,sharpness=0.5)=1.75
    int N=8; float Y=0,I=0,Q=0,wY=0,wC=0;
    for(int i=-N;i<=N;++i){
        float fx=sx+i; float s=S(int(fx));
        float dy=float(i);
        float gy=std::exp(-0.5f*dy*dy/(sigmaY*sigmaY));
        float gc=std::exp(-0.5f*dy*dy/(sigC*sigC));
        float ph=PI*0.5f*fx + phaseShift;
        Y+=s*gy; I+=s*std::sin(ph)*gc*2.0f; Q+=s*std::cos(ph)*gc*2.0f; wY+=gy; wC+=gc;
    }
    Y/=wY; I/=wC; Q/=wC;
    float r=Y+0.956f*I+0.621f*Q, g=Y-0.272f*I-0.647f*Q, b=Y-1.106f*I+1.703f*Q;
    auto cl=[](float v){return v<0?0:(v>1?1:v);};
    return 0xFF000000u | (uint32_t(cl(b)*255)<<16)|(uint32_t(cl(g)*255)<<8)|uint32_t(cl(r)*255);
}
}

int main(){
    Memory mem; Apple2Display disp;
    mem.memRead(0xC050); mem.memRead(0xC057); mem.memRead(0xC054); // graphics, hires, page1
    // SOLID colour fields (full rows) — LUT and square-filter must agree on
    // hue here (isolated dots are a sharper decode and legitimately differ).
    //   $55 (even dots, MSB0) → VIOLET   $2A (odd dots, MSB0) → GREEN
    //   $D5 (even dots, MSB1) → BLUE     $AA (odd dots, MSB1) → ORANGE
    for(int c=0;c<40;++c){
        writeHgr(mem,0,c,0x55);
        writeHgr(mem,1,c,0x2A);
        writeHgr(mem,2,c,0xD5);
        writeHgr(mem,3,c,0xAA);
    }

    struct M{Apple2Display::HiResMode m; const char* n;};
    M modes[]={
        {Apple2Display::HiResMode::ColorNTSC,      "NTSC (reference)"},
        {Apple2Display::HiResMode::ColorCompMedium,"NTSC medium"},
        {Apple2Display::HiResMode::ColorComp4Bit,  "4-bit square"},
    };
    std::printf("Solid fields — expected: row0=VIOLET row1=GREEN row2=BLUE row3=ORANGE\n");
    for(auto& e:modes){
        disp.setHiResMode(e.m); disp.render(mem);
        std::printf("  %s:\n", e.n);
        show("$55 violet",disp,140,0);
        show("$2A green ",disp,140,1);
        show("$D5 blue  ",disp,140,2);
        show("$AA orange",disp,140,3);
    }

    // OE: demodulate the composite signal (CPU port) at several phase shifts
    // to find which makes the artifact colours match the reference.
    disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
    disp.render(mem);
    if(disp.signalProduced()){
        const uint8_t* sig=disp.signal(); int sw=disp.signalWidth(), sh=disp.signalHeight();
        const float PI=3.14159265358979f;
        float shifts[]={0.0f, PI*0.5f, PI, PI*1.5f};
        const char* sn[]={"+0","+90","+180","+270"};
        for(int k=0;k<4;++k){
            // Solid rows 0..3 (violet/green/blue/orange), mid-line, signal x=280.
            uint32_t v=demodOE(sig,sw,sh,280.0f,0,shifts[k]);
            uint32_t g=demodOE(sig,sw,sh,280.0f,1,shifts[k]);
            uint32_t b=demodOE(sig,sw,sh,280.0f,2,shifts[k]);
            uint32_t o=demodOE(sig,sw,sh,280.0f,3,shifts[k]);
            std::printf("  OE demod (phase %4s): violet=%s green=%s blue=%s orange=%s\n",
                sn[k], hue(v), hue(g), hue(b), hue(o));
        }
    }
    return 0;
}
