#include "android_texture.h"
#include <string.h>
#define FCC(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
static uint16_t word(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static void rgb565(uint16_t color, uint8_t *out) {
    unsigned r=(color>>11)&31,g=(color>>5)&63,b=color&31;
    out[0]=(r<<3)|(r>>2);out[1]=(g<<2)|(g>>4);out[2]=(b<<3)|(b>>2);out[3]=255;
}
int android_texture_rgba(uint32_t format,unsigned width,unsigned height,unsigned pitch,const uint8_t *source,uint8_t *rgba) {
    int bc=format==FCC('D','X','T','1')?1:format==FCC('D','X','T','3')?2:format==FCC('D','X','T','5')?3:0;
    if(bc) {
        unsigned bytes=bc==1?8:16;
        if(pitch<((width+3)/4)*bytes)return 0;
        for(unsigned by=0;by<(height+3)/4;++by)for(unsigned bx=0;bx<(width+3)/4;++bx) {
            const uint8_t *block=source+by*pitch+bx*bytes,*colors=block+(bc==1?0:8);
            uint16_t a=word(colors),b=word(colors+2);uint8_t palette[4][4];
            rgb565(a,palette[0]);rgb565(b,palette[1]);
            if(a>b||bc!=1) {
                for(int ch=0;ch<3;++ch){palette[2][ch]=(2*palette[0][ch]+palette[1][ch])/3;palette[3][ch]=(palette[0][ch]+2*palette[1][ch])/3;}
                palette[2][3]=palette[3][3]=255;
            } else {
                for(int ch=0;ch<3;++ch)palette[2][ch]=(palette[0][ch]+palette[1][ch])/2;
                palette[2][3]=255;memset(palette[3],0,4);
            }
            uint8_t alpha[8];uint64_t alpha_bits=0;
            if(bc==3) {
                alpha[0]=block[0];alpha[1]=block[1];
                int end=alpha[0]>alpha[1]?8:6,denominator=end-1;
                for(int i=2;i<end;++i)alpha[i]=((end-i)*alpha[0]+(i-1)*alpha[1])/denominator;
                if(end==6){alpha[6]=0;alpha[7]=255;}
                for(int i=0;i<6;++i)alpha_bits|=(uint64_t)block[2+i]<<(8*i);
            }
            for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
                if(bx*4+x>=width||by*4+y>=height)continue;
                unsigned index=y*4+x,choice=(colors[4+index/4]>>(2*(index%4)))&3;
                uint8_t *pixel=rgba+((by*4+y)*width+bx*4+x)*4;memcpy(pixel,palette[choice],4);
                if(bc==2)pixel[3]=((block[index/2]>>(4*(index%2)))&15)*17;
                if(bc==3)pixel[3]=alpha[(alpha_bits>>(index*3))&7];
            }
        }
        return 1;
    }
    unsigned bytes=(format==21||format==22||format==32||format==33)?4:format==20?3:
        (format==23||format==25||format==26||format==51)?2:(format==28||format==50)?1:0;
    if(!bytes||pitch<width*bytes)return 0;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const uint8_t *in=source+y*pitch+x*bytes;uint8_t *out=rgba+(y*width+x)*4;
        out[3]=255;
        switch(format) {
        case 20:case 21:case 22:out[0]=in[2];out[1]=in[1];out[2]=in[0];if(format==21)out[3]=in[3];break;
        case 32:case 33:memcpy(out,in,3);if(format==32)out[3]=in[3];break;
        case 23:rgb565(word(in),out);break;
        case 25:{unsigned v=word(in);out[0]=((v>>10)&31)*255/31;out[1]=((v>>5)&31)*255/31;out[2]=(v&31)*255/31;out[3]=(v&32768)?255:0;break;}
        case 26:{unsigned v=word(in);out[0]=((v>>8)&15)*17;out[1]=((v>>4)&15)*17;out[2]=(v&15)*17;out[3]=(v>>12)*17;break;}
        case 28:out[0]=out[1]=out[2]=255;out[3]=in[0];break;
        case 50:case 51:out[0]=out[1]=out[2]=in[0];if(format==51)out[3]=in[1];break;
        }
    }
    return 1;
}
int android_texture_selftest(void) {
    uint8_t bc1[8]={0,0xf8,0xe0,7,0xe4,0xe4,0xe4,0xe4},out[68];memset(out,0xcc,sizeof out);
    if(!android_texture_rgba(FCC('D','X','T','1'),3,2,8,bc1,out))return 1;
    const uint8_t expected[]={255,0,0,255,0,255,0,255,170,85,0,255};
    if(memcmp(out,expected,12)||memcmp(out+12,expected,12)||out[24]!=0xcc)return 2;
    uint8_t bc3[16]={0,255,0xff,0xff,0xff,0xff,0xff,0xff,0,0xf8,0xe0,7,0,0,0,0};
    if(!android_texture_rgba(FCC('D','X','T','5'),4,4,16,bc3,out)||out[3]!=255)return 3;
    bc3[2]=6; // low alpha index 6 -> exactly transparent in the six-value mode.
    if(!android_texture_rgba(FCC('D','X','T','5'),1,1,16,bc3,out)||out[3])return 4;
    const uint8_t bgra[]={3,2,1,4};
    if(!android_texture_rgba(21,1,1,4,bgra,out)||memcmp(out,(uint8_t[]){1,2,3,4},4))return 5;
    return 0;
}
