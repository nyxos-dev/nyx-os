/*
 * nyxgen.c - Oneiros, running inside NyxOS.
 *
 * A compact char-level RMSNorm transformer that generates NyxOS-style C, entirely
 * inside the OS. Self-contained: its own expf/logf/tanhf/sqrtf (NyxOS libm lacks
 * exp/log/tanh, kernel is -mno-sse), no -lm. Reads its weights from the package
 * dir the initramfs ships it in: /usr/pkg/nyxgen/model.bin.
 *
 * The full 1.78-bits/byte champion (token-level, 2.9 MB) lives host-side in the
 * public repo; this is the small model that fits in the OS image.
 *
 *   nyxgen [n] [temp] [seed]     e.g.  nyxgen 200 0.7 "static void "
 */
/*
 * Two build paths from ONE source:
 *   - Host (gcc):   the C standard library.
 *   - In NyxOS (cc = TinyCC, __TINYC__): the OS's own libc subset. The in-OS
 *     toolchain ships no <stdint.h>/<stdlib.h>/<time.h>, so the fixed-width
 *     types are spelled directly (LP64 x86_64) and the RNG is seeded from the
 *     RTC via NyxOS's own time(nyx_tm*). This is why `xbm install nyxgen` works.
 */
#ifdef __TINYC__
#include "libc.h"                       /* malloc/fopen/fread/fputs/printf/getenv/atoi/atof/memmove... + time(nyx_tm*) */
typedef unsigned char  u8;
typedef int            i32;
typedef unsigned int   u32;
typedef unsigned long  u64;
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
typedef uint8_t  u8;
typedef int32_t  i32;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define V   256          /* char-level: a byte is a token          */
#define E   32
#define D   E
#define B   32
#define NH  4
#define DH  (D/NH)
#define FF  96
#define HID 64
#define RMS_EPS 1e-5f

typedef struct {
    float C[V*D], P[B*D];
    float ga[D], gf[D], gh[D];
    float Wq[D*D], Wk[D*D], Wv[D*D], Wo[D*D];
    float Wf1[FF*D], bf1[FF];
    float Wf2[D*FF], bf2[D];
    float Wh[HID*D], bh[HID];
    float Wout[V*HID], bout[V];
} Params;

/* own float math (no libm) */
static float k_expf(float x){
    if (x!=x) return x;
    if (x>88.0f) return 3.0e38f;
    if (x<-88.0f) return 0.0f;
    const float LOG2E=1.44269504f, LN2=0.6931471805f;
    int n=(int)(x*LOG2E+(x>=0?0.5f:-0.5f)); float r=x-(float)n*LN2;
    float p=1.0f+r*(1.0f+r*(0.5f+r*(0.16666667f+r*(0.041666668f+r*0.008333334f))));
    union{float f;i32 i;}u; u.i=(i32)((n+127)<<23); return p*u.f;
}
static float k_logf(float x){
    if (x<=0.0f) return -88.0f;
    union{float f;i32 i;}u; u.f=x; int e=((u.i>>23)&0xFF)-127;
    u.i=(u.i&0x807FFFFF)|0x3F800000; float m=u.f,t=(m-1.0f)/(m+1.0f),t2=t*t;
    float s=t*(2.0f+t2*(0.6666667f+t2*(0.4f+t2*0.2857143f))); return (float)e*0.6931471805f+s;
}
static float k_tanhf(float x){
    if (x>15.0f) return 1.0f;
    if (x<-15.0f) return -1.0f;
    float e=k_expf(2.0f*x); return (e-1.0f)/(e+1.0f);
}
static float k_sqrtf(float x){ if(x<=0)return 0; float g=x; for(int i=0;i<24;i++) g=0.5f*(g+x/g); return g; }

static u64 rng=0x9E3779B97F4A7C15ULL;
static u32 rnd(void){ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return (u32)(rng>>32); }

static int text_token(int c){ return c=='\n' || c=='\t' || (c>=32 && c<127); }
static int text_byte(int c){ return text_token(c)?c:' '; }
static int finite_float(float value){
    union { float value; u32 bits; } v; v.value=value;
    return (v.bits&0x7F800000u)!=0x7F800000u;
}

/* Same text alphabet as the desktop app. Temperature weights are in [0,1];
 * sample their allowed mass only, so NUL/escape/control bytes never reach stdout. */
static int sample_text(const float *probs, u32 random){
    float total=0;
    int last=' ';
    for (int c=0;c<V;c++) if (text_token(c) && probs[c]>0.0f && probs[c]<=1.0f){
        total+=probs[c]; last=c;
    }
    if (!(total>0.0f)) return ' ';
    float target=(float)(random>>8)*(1.0f/16777216.0f)*total, cumulative=0;
    for (int c=0;c<V;c++) if (text_token(c) && probs[c]>0.0f && probs[c]<=1.0f){
        cumulative+=probs[c];
        if (target<cumulative) return c;
    }
    return last;
}

static void rms(const float *x, const float *g, float *y){
    float ss=0; for(int i=0;i<D;i++) ss+=x[i]*x[i];
    float irms=1.0f/k_sqrtf(ss/(float)D+RMS_EPS);
    for(int i=0;i<D;i++) y[i]=g[i]*x[i]*irms;
}

static int forward(const Params *p, const unsigned char *tok, float *probs){
    const int L=B-1; const float scale=1.0f/k_sqrtf((float)DH);
    float xt[B][D], xn[B][D], q[D], k[B][D], v[B][D], att[NH][B];
    float ctx[D], attn[D], r1[D], rn[D], g[FF], r2[D], rh[D], hid[HID];
    for (int t=0;t<B;t++) for (int d=0;d<D;d++) xt[t][d]=p->C[(int)tok[t]*D+d]+p->P[t*D+d];
    for (int t=0;t<B;t++) rms(xt[t], p->ga, xn[t]);
    for (int a=0;a<D;a++){ float s=0; for(int b=0;b<D;b++) s+=p->Wq[a*D+b]*xn[L][b]; q[a]=s; }
    for (int t=0;t<B;t++) for (int a=0;a<D;a++){ float ks=0,vs=0;
        for (int b=0;b<D;b++){ ks+=p->Wk[a*D+b]*xn[t][b]; vs+=p->Wv[a*D+b]*xn[t][b]; }
        k[t][a]=ks; v[t][a]=vs; }
    for (int h=0;h<NH;h++){ int o=h*DH; float sc[B],mx=-1e30f;
        for (int t=0;t<B;t++){ float s=0; for(int i=0;i<DH;i++) s+=q[o+i]*k[t][o+i]; s*=scale; if (!finite_float(s)) return 0; sc[t]=s; if(s>mx)mx=s; }
        float sum=0; for(int t=0;t<B;t++){ float e=k_expf(sc[t]-mx); att[h][t]=e; sum+=e; }
        if (!(sum>0.0f) || !finite_float(sum)) return 0;
        for (int t=0;t<B;t++) att[h][t]/=sum;
        for (int i=0;i<DH;i++){ float c=0; for(int t=0;t<B;t++) c+=att[h][t]*v[t][o+i]; ctx[o+i]=c; }
    }
    for (int e=0;e<D;e++){ float s=0; for(int d=0;d<D;d++) s+=p->Wo[e*D+d]*ctx[d]; attn[e]=s; }
    for (int d=0;d<D;d++) r1[d]=xt[L][d]+attn[d];
    rms(r1, p->gf, rn);
    for (int j=0;j<FF;j++){ float s=p->bf1[j]; for(int d=0;d<D;d++) s+=p->Wf1[j*D+d]*rn[d]; g[j]=s; }
    for (int d=0;d<D;d++){ float s=p->bf2[d]; for(int j=0;j<FF;j++){ float r=g[j]>0?g[j]:0; s+=p->Wf2[d*FF+j]*r; } r2[d]=r1[d]+s; }
    rms(r2, p->gh, rh);
    for (int m=0;m<HID;m++){ float z=p->bh[m]; for(int d=0;d<D;d++) z+=p->Wh[m*D+d]*rh[d]; if (!finite_float(z)) return 0; hid[m]=k_tanhf(z); }
    float mx=-1e30f;
    for (int c=0;c<V;c++){ float z=p->bout[c]; for(int m=0;m<HID;m++) z+=p->Wout[c*HID+m]*hid[m]; if (!finite_float(z)) return 0; probs[c]=z; if(z>mx)mx=z; }
    float s=0; for(int c=0;c<V;c++){ probs[c]=k_expf(probs[c]-mx); s+=probs[c]; }
    if (!(s>0.0f) || !finite_float(s)) return 0;
    for (int c=0;c<V;c++) probs[c]/=s;
    return 1;
}

static int load_ckpt(const char *path, Params *p){
    FILE *f=fopen(path,"rb"); if(!f) return 0; char m[8]; i32 dims[6];
    if (fread(m,1,8,f)!=8||memcmp(m,"ONEIROS4",8)){ fclose(f); return 0; }
    if (fread(dims,sizeof(dims),1,f)!=1){ fclose(f); return 0; }
    if (dims[0]!=V||dims[1]!=E||dims[2]!=B||dims[3]!=HID||dims[4]!=NH||dims[5]!=FF){
        fprintf(stderr,"model dims mismatch\n"); fclose(f); return 0; }
    int ok=fread(p,sizeof(Params),1,f)==1;
    if (ok) ok=fgetc(f)==EOF && !ferror(f);   /* exact ONEIROS4 payload */
    fclose(f);
    if (!ok) return 0;
    for (unsigned long offset=0;offset<sizeof(Params);offset+=sizeof(float)){
        float value; memmove(&value,(const u8*)p+offset,sizeof(value));
        if (!finite_float(value)) return 0;
    }
    return 1;
}

int main(int argc, char **argv){
    int nout   = (argc>1)? atoi(argv[1]) : 200;
    float temp = (argc>2)? (float)atof(argv[2]) : 0.7f;
    const char *seed = (argc>3)? argv[3] : "static void ";
    const char *path = getenv("ONEIROS_MODEL");
    if (!path) path = "/usr/pkg/nyxgen/model.bin";

    if (!(temp>0.0f) || !finite_float(temp)){
        fprintf(stderr,"nyxgen: temperature must be positive and finite\n"); return 1;
    }

    Params *p=malloc(sizeof(Params));
    if (!p || !load_ckpt(path,p)){ fprintf(stderr,"nyxgen: cannot load model %s\n",path); free(p); return 1; }
    float *probs=malloc(V*sizeof(float));
    if (!probs){ fprintf(stderr,"nyxgen: out of memory\n"); free(p); return 1; }

    unsigned char ctx[B];
    int sl=(int)strlen(seed);
    for (int t=0;t<B;t++){ int idx=sl-B+t; ctx[t]=(idx>=0)?(unsigned char)text_byte((u8)seed[idx]):(unsigned char)' '; }
    for (int i=0;i<sl;i++) putchar(text_byte((u8)seed[i]));

#ifdef __TINYC__
    { nyx_tm tm; time(&tm);                          /* in-OS: seed from the RTC (SYS_TIME) */
      u64 s = (u64)tm.sec + 61u*tm.min + 3661u*tm.hour
            + 100003u*(u64)tm.mday + 1300021u*(u64)tm.mon + 79800011u*(u64)tm.year;
      rng ^= s*0x2545F4914F6CDD1DULL; }
#else
    rng ^= (u64)time(NULL)*0x2545F4914F6CDD1DULL;    /* host: wall-clock seed */
#endif
    for (int i=0;i<nout;i++){
        if (!forward(p,ctx,probs)){
            fprintf(stderr,"nyxgen: numerical inference failure\n");
            free(probs); free(p); return 1;
        }
        float mx=-1e30f; for(int c=0;c<V;c++){ float lp=k_logf(probs[c]+1e-12f); probs[c]=lp; if(lp>mx)mx=lp; }
        /* Subtract before dividing: tiny positive temperatures may underflow a
         * non-max weight, but cannot turn the maximum into infinity-infinity. */
        for(int c=0;c<V;c++) probs[c]=k_expf((probs[c]-mx)/temp);
        int nx=sample_text(probs,rnd());
        putchar(nx);
        memmove(ctx,ctx+1,B-1); ctx[B-1]=(unsigned char)nx;
    }
    putchar('\n');
    free(probs); free(p);
    return 0;
}
