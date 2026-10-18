/*
 * oneiros_win.c - Oneiros, the NyxOS AI, as a desktop app.
 *
 * A windowed front-end for the from-scratch generative model that ships in NyxOS.
 * It is the same compact char-level RMSNorm transformer the `nyxgen` command runs
 * (github.com/nyxos-dev/oneiros) -- own embeddings, own attention, own backprop,
 * even its own expf/tanhf -- only here it streams live in the compositor: type a
 * seed, press Dream, and watch it dream NyxOS-style C one character at a time.
 * Weights load from the initramfs (/usr/pkg/nyxgen/model.bin, ONEIROS4).
 *
 * Two constraints shape the code. (1) The kernel task stack is 4 KB, so every
 * forward-pass temporary lives on the heap (the Scratch block), never the stack.
 * (2) The kernel is -mno-sse, so the SysV float ABI (args + return in xmm) is
 * unavailable: a float can't cross a function-call boundary. So the math is
 * written as statement-expression MACROS (textually inlined, no call, x87 only),
 * and the temperature is handed to forward() through the Scratch struct, not as a
 * by-value float argument. No libm, no SSE.
 */
#include "../core/theme.h"
#include "../../core/kernel.h"
#include "../core/compositor.h"
#include "oneiros_win.h"
#include "../../drivers/video/font.h"

/* ---- model geometry (must match the ONEIROS4 checkpoint) ------------------- */
#define OG_V   256          /* char-level: a byte is a token */
#define OG_E   32
#define OG_D   OG_E
#define OG_B   32           /* context window */
#define OG_NH  4
#define OG_DH  (OG_D/OG_NH)
#define OG_FF  96
#define OG_HID 64
#define OG_RMS_EPS 1e-5f

typedef struct {
    float C[OG_V*OG_D], P[OG_B*OG_D];
    float ga[OG_D], gf[OG_D], gh[OG_D];
    float Wq[OG_D*OG_D], Wk[OG_D*OG_D], Wv[OG_D*OG_D], Wo[OG_D*OG_D];
    float Wf1[OG_FF*OG_D], bf1[OG_FF];
    float Wf2[OG_D*OG_FF], bf2[OG_D];
    float Wh[OG_HID*OG_D], bh[OG_HID];
    float Wout[OG_V*OG_HID], bout[OG_V];
} Params;

/* forward-pass working set -- HEAP, never the 4 KB kernel stack. `temp` is passed
 * in here (not as a by-value float arg, which -mno-sse can't pass). */
typedef struct {
    float xt[OG_B][OG_D], xn[OG_B][OG_D], k[OG_B][OG_D], v[OG_B][OG_D];
    float att[OG_NH][OG_B], asc[OG_B];
    float q[OG_D], cx[OG_D], attn[OG_D], r1[OG_D], rn[OG_D], r2[OG_D], rh[OG_D];
    float g[OG_FF], hid[OG_HID];
    float probs[OG_V];
    float temp;
} Scratch;

/* ---- own float math as inline macros (no float returns; kernel is -mno-sse) --- */
#define K_FINITE(X) __extension__({ union { float f; uint32_t u; } _v; _v.f=(X); (_v.u&0x7F800000u)!=0x7F800000u; })

#define K_SQRTF(X) __extension__({ float _x=(X); float _g; \
    if (_x<=0.0f) _g=0.0f; else { _g=_x; for (int _i=0;_i<24;_i++) _g=0.5f*(_g+_x/_g); } _g; })

#define K_EXPF(X) __extension__({ float _x=(X); float _e; \
    if (_x!=_x) _e=_x; else if (_x>88.0f) _e=3.0e38f; else if (_x<-88.0f) _e=0.0f; else { \
        int _n=(int)(_x*1.44269504f+(_x>=0?0.5f:-0.5f)); float _r=_x-(float)_n*0.6931471805f; \
        float _p=1.0f+_r*(1.0f+_r*(0.5f+_r*(0.16666667f+_r*(0.041666668f+_r*0.008333334f)))); \
        union { float f; int32_t i; } _u; _u.i=(int32_t)((_n+127)<<23); _e=_p*_u.f; } _e; })

#define K_TANHF(X) __extension__({ float _tx=(X); float _t; \
    if (_tx>15.0f) _t=1.0f; else if (_tx<-15.0f) _t=-1.0f; \
    else { float _ee=K_EXPF(2.0f*_tx); _t=(_ee-1.0f)/(_ee+1.0f); } _t; })

static void rms(const float *x, const float *g, float *y){
    float ss=0; for (int i=0;i<OG_D;i++) ss+=x[i]*x[i];
    float irms=1.0f/K_SQRTF(ss/(float)OG_D+OG_RMS_EPS);
    for (int i=0;i<OG_D;i++) y[i]=g[i]*x[i]*irms;
}

/* one forward pass over the B-token context -> s->probs (temperature-softmaxed,
 * temperature read from s->temp). softmax(logits/T) is exactly temperature sampling. */
static int forward(const Params *p, Scratch *s, const unsigned char *tok){
    if (!(s->temp>0.0f) || !K_FINITE(s->temp)) return 0;
    const int L=OG_B-1; const float scale=1.0f/K_SQRTF((float)OG_DH);
    for (int t=0;t<OG_B;t++) for (int d=0;d<OG_D;d++) s->xt[t][d]=p->C[(int)tok[t]*OG_D+d]+p->P[t*OG_D+d];
    for (int t=0;t<OG_B;t++) rms(s->xt[t], p->ga, s->xn[t]);
    for (int a=0;a<OG_D;a++){ float acc=0; for (int b=0;b<OG_D;b++) acc+=p->Wq[a*OG_D+b]*s->xn[L][b]; s->q[a]=acc; }
    for (int t=0;t<OG_B;t++) for (int a=0;a<OG_D;a++){ float ks=0,vs=0;
        for (int b=0;b<OG_D;b++){ ks+=p->Wk[a*OG_D+b]*s->xn[t][b]; vs+=p->Wv[a*OG_D+b]*s->xn[t][b]; }
        s->k[t][a]=ks; s->v[t][a]=vs; }
    for (int h=0;h<OG_NH;h++){ int o=h*OG_DH; float mx=-1e30f;
        for (int t=0;t<OG_B;t++){ float acc=0; for (int i=0;i<OG_DH;i++) acc+=s->q[o+i]*s->k[t][o+i]; acc*=scale; if (!K_FINITE(acc)) return 0; s->asc[t]=acc; if (acc>mx) mx=acc; }
        float sum=0; for (int t=0;t<OG_B;t++){ float e=K_EXPF(s->asc[t]-mx); s->att[h][t]=e; sum+=e; }
        if (!(sum>0.0f) || !K_FINITE(sum)) return 0;
        for (int t=0;t<OG_B;t++) s->att[h][t]/=sum;
        for (int i=0;i<OG_DH;i++){ float c=0; for (int t=0;t<OG_B;t++) c+=s->att[h][t]*s->v[t][o+i]; s->cx[o+i]=c; }
    }
    for (int e=0;e<OG_D;e++){ float acc=0; for (int d=0;d<OG_D;d++) acc+=p->Wo[e*OG_D+d]*s->cx[d]; s->attn[e]=acc; }
    for (int d=0;d<OG_D;d++) s->r1[d]=s->xt[L][d]+s->attn[d];
    rms(s->r1, p->gf, s->rn);
    for (int j=0;j<OG_FF;j++){ float acc=p->bf1[j]; for (int d=0;d<OG_D;d++) acc+=p->Wf1[j*OG_D+d]*s->rn[d]; s->g[j]=acc; }
    for (int d=0;d<OG_D;d++){ float acc=p->bf2[d]; for (int j=0;j<OG_FF;j++){ float r=s->g[j]>0?s->g[j]:0; acc+=p->Wf2[d*OG_FF+j]*r; } s->r2[d]=s->r1[d]+acc; }
    rms(s->r2, p->gh, s->rh);
    for (int m=0;m<OG_HID;m++){ float z=p->bh[m]; for (int d=0;d<OG_D;d++) z+=p->Wh[m*OG_D+d]*s->rh[d]; if (!K_FINITE(z)) return 0; s->hid[m]=K_TANHF(z); }
    float it=1.0f/s->temp, mx=-1e30f;
    for (int c=0;c<OG_V;c++){ float z=p->bout[c]; for (int m=0;m<OG_HID;m++) z+=p->Wout[c*OG_HID+m]*s->hid[m]; z*=it; if (!K_FINITE(z)) return 0; s->probs[c]=z; if (z>mx) mx=z; }
    float sm=0; for (int c=0;c<OG_V;c++){ s->probs[c]=K_EXPF(s->probs[c]-mx); sm+=s->probs[c]; }
    if (!(sm>0.0f) || !K_FINITE(sm)) return 0;
    for (int c=0;c<OG_V;c++) s->probs[c]/=sm;
    return 1;
}

int oneiros_inference_selftest(void){
    Params *p=(Params*)kmalloc(sizeof(Params));
    Scratch *s=(Scratch*)kmalloc(sizeof(Scratch));
    if (!p || !s){ kfree(p); kfree(s); return 1; }
    unsigned char tok[OG_B];
    memset_asm(tok, 0, sizeof(tok));
    memset_asm(p, 0, sizeof(*p));
    s->temp=0.7f;
    int result=0;
    if (!forward(p,s,tok)){ result=2; goto done; }
    for (int i=0;i<OG_V;i++) if (s->probs[i]!=1.0f/OG_V){ result=3; goto done; }
    const uint32_t bits=0x7F7FFFFFu;
    for (size_t i=0;i<sizeof(*p);i+=sizeof(bits)) memmove((uint8_t*)p+i,&bits,sizeof(bits));
    if (forward(p,s,tok)){ result=4; goto done; }
    memset_asm(p, 0, sizeof(*p));
    if (!forward(p,s,tok)){ result=5; goto done; }
    s->temp=0;
    if (forward(p,s,tok)) result=6;
done:
    kfree(s); kfree(p); return result;
}

/* ---- model load from the initramfs ----------------------------------------- */
/* Validate the same exact ONEIROS4 payload accepted by nyxgen. Read float
 * encodings as integers: this needs neither aligned loads nor the float ABI,
 * and NaN/Inf weights must never reach the compositor's forward pass. */
static int ow_model_valid(const uint8_t *data, uint32_t size){
    if (!data || size != 8+24+sizeof(Params)) return 0;
    static const char magic[8]={'O','N','E','I','R','O','S','4'};
    for (int i=0;i<8;i++) if (data[i]!=(uint8_t)magic[i]) return 0;
    int32_t dims[6]; memmove(dims, data+8, sizeof(dims));
    if (dims[0]!=OG_V||dims[1]!=OG_E||dims[2]!=OG_B||dims[3]!=OG_HID||dims[4]!=OG_NH||dims[5]!=OG_FF)
        return 0;
    for (uint32_t offset=32;offset<size;offset+=sizeof(uint32_t)){
        uint32_t bits; memmove(&bits, data+offset, sizeof(bits));
        if ((bits&0x7F800000u)==0x7F800000u) return 0;
    }
    return 1;
}

int oneiros_model_selftest(void){
    const uint32_t size=32+sizeof(Params);
    if (ow_model_valid(NULL, size)) return 1;
    uint8_t *data=(uint8_t*)kmalloc(size);
    if (!data) return 2;
    memset_asm(data, 0, size);
    memmove(data, "ONEIROS4", 8);
    const int32_t dims[6]={OG_V,OG_E,OG_B,OG_HID,OG_NH,OG_FF};
    memmove(data+8, dims, sizeof(dims));
    int result=0;
    if (!ow_model_valid(data, size)) { result=3; goto done; }
    if (ow_model_valid(data, size-1) || ow_model_valid(data, size+1)) { result=4; goto done; }
    data[0]='X';
    if (ow_model_valid(data, size)) { result=5; goto done; }
    data[0]='O'; data[8]=0xFF;
    if (ow_model_valid(data, size)) { result=6; goto done; }
    memmove(data+8, dims, sizeof(dims));
    const uint32_t offsets[3]={32,32+sizeof(Params)/2,size-4};
    const uint32_t invalid[3]={0x7FC00000u,0x7F800000u,0xFF800000u};
    const uint32_t zero=0;
    for (int i=0;i<3;i++){
        memmove(data+offsets[i], &invalid[i], sizeof(uint32_t));
        if (ow_model_valid(data, size)) { result=7+i; goto done; }
        memmove(data+offsets[i], &zero, sizeof(zero));
    }
    if (!ow_model_valid(data, size)) result=10;
done:
    kfree(data);
    return result;
}

/* Read via vfs_fdata (a direct pointer to the memory-backed initramfs file), the
 * way the other in-kernel apps read files -- NOT sequential vfs_read, whose offset
 * doesn't advance for these fds. `data` is valid only until vfs_close, so the
 * Params are copied out before closing. Layout: "ONEIROS4"(8) + int32 dims[6] + Params. */
static Params* load_model(void){
    int fd=vfs_open("/usr/pkg/nyxgen/model.bin", 0, 0);
    if (fd<0) return NULL;
    uint32_t size=vfs_fsize(fd);
    uint8_t *data=vfs_fdata(fd);
    if (!ow_model_valid(data, size)){ vfs_close(fd); return NULL; }
    Params *p=(Params*)kmalloc(sizeof(Params));
    if (!p){ vfs_close(fd); return NULL; }
    memmove(p, data+8+24, sizeof(Params));
    vfs_close(fd); return p;
}

/* ---- the app --------------------------------------------------------------- */
#define OW_SEED_MAX 96
#define OW_OUT_MAX  4096
#define OW_GEN_N    240          /* chars per Dream */
#define OW_STEPS    3            /* forward passes per ~30 Hz tick (streaming) */
#define OW_PAD      12
#define OW_LH       18           /* output line height */
#define OW_FCW      8            /* font cell width */

struct oneiros_win {
    Params  *model;
    Scratch *sc;
    int      loaded;
    char     seed[OW_SEED_MAX];
    int      seed_len;
    char     out[OW_OUT_MAX];
    int      out_len;
    int      copied_len;
    unsigned char ctx[OG_B];
    int      generating, want, done, numeric_error;
    float    temp;
    uint64_t rng;
};

static uint32_t ow_rnd(oneiros_win_t *o){ o->rng^=o->rng<<13; o->rng^=o->rng>>7; o->rng^=o->rng<<17; return (uint32_t)(o->rng>>32); }

static int ow_text_token(int c){ return c=='\n' || c=='\t' || (c>=32 && c<127); }

/* Sample only displayable text, renormalizing the allowed probability mass.
 * forward() provides normalized probabilities; ignore invalid values as well as
 * control bytes. Integer RNG input avoids the kernel's unavailable float ABI. */
static int ow_sample_text(const float *pr, uint32_t random){
    float total=0;
    int last=' ';
    for (int c=0;c<OG_V;c++) if (ow_text_token(c) && pr[c]>0.0f && pr[c]<=1.0f){
        total+=pr[c]; last=c;
    }
    if (!(total>0.0f)) return ' ';
    /* Use 24 bits so converting the largest RNG value never rounds to 1.0. */
    float target=(float)(random>>8)*(1.0f/16777216.0f)*total, cumulative=0;
    for (int c=0;c<OG_V;c++) if (ow_text_token(c) && pr[c]>0.0f && pr[c]<=1.0f){
        cumulative+=pr[c];
        if (target<cumulative) return c;
    }
    return last;   /* rounding at the CDF end must also return an allowed token */
}

int oneiros_text_selftest(void){
    float pr[OG_V];
    memset_asm(pr, 0, sizeof(pr));
    if (ow_sample_text(pr, 0)!=' ' || ow_sample_text(pr, 0xFFFFFFFFu)!=' ') return 1;
    for (int c=0;c<OG_V;c++){
        pr[c]=1.0f;
        int expected=(c==9 || c==10 || (c>31 && c<127))?c:' ';
        if (ow_sample_text(pr, 0)!=expected || ow_sample_text(pr, 0xFFFFFFFFu)!=expected) return 2;
        pr[c]=0.0f;
    }
    pr[0]=1.0f; pr[27]=1.0f; pr[127]=1.0f; pr[255]=1.0f;
    pr['A']=0.25f; pr['B']=0.75f;
    if (ow_sample_text(pr, 0)!='A' || ow_sample_text(pr, 0x3FFFFF00u)!='A' ||
        ow_sample_text(pr, 0x40000000u)!='B' || ow_sample_text(pr, 0xFFFFFFFFu)!='B') return 3;
    memset_asm(pr, 0, sizeof(pr));
    union { uint32_t bits; float value; } bad;
    bad.bits=0x7FC00000u; pr['A']=bad.value;   /* NaN */
    bad.bits=0x7F800000u; pr['B']=bad.value;   /* infinity */
    pr['C']=-0.5f; pr['D']=2.0f;
    if (ow_sample_text(pr, 0xFFFFFFFFu)!=' ') return 4;
    pr['\t']=0.25f; pr['\n']=0.75f;
    if (ow_sample_text(pr, 0)!='\t' || ow_sample_text(pr, 0x40000000u)!='\n' ||
        ow_sample_text(pr, 0xFFFFFFFFu)!='\n') return 5;
    return 0;
}

oneiros_win_t* oneiros_create_ctx(void){
    oneiros_win_t *o=(oneiros_win_t*)kmalloc(sizeof(oneiros_win_t));
    if (!o) return NULL;
    memset_asm(o, 0, sizeof(*o));
    o->temp=0.7f;
    o->rng=0x9E3779B97F4A7C15ULL;
    const char *def="static void ";
    int i=0; for (; def[i] && i<OW_SEED_MAX-1; i++) o->seed[i]=def[i];
    o->seed[i]=0; o->seed_len=i;
    o->model=load_model();
    o->loaded=(o->model!=NULL);
    if (o->loaded){ o->sc=(Scratch*)kmalloc(sizeof(Scratch)); if (!o->sc) o->loaded=0; }
    return o;
}

void oneiros_win_close(window_t *win){
    oneiros_win_t *o=(oneiros_win_t*)win->reserved;
    if (!o) return;
    if (o->model) kfree(o->model);
    if (o->sc)    kfree(o->sc);
    /* the compositor frees `reserved` (o) itself after this returns */
}

static void ow_start(oneiros_win_t *o){
    if (!o->loaded) return;
    int n=o->seed_len; if (n>OW_OUT_MAX-1) n=OW_OUT_MAX-1;
    memmove(o->out, o->seed, n); o->out[n]=0; o->out_len=n;
    o->copied_len=0;
    for (int t=0;t<OG_B;t++){ int idx=o->seed_len-OG_B+t; o->ctx[t]=(idx>=0)?(unsigned char)o->seed[idx]:(unsigned char)' '; }
    o->want=OW_GEN_N; o->done=0; o->generating=1; o->numeric_error=0;
    o->rng ^= (uint64_t)get_ticks()*0x2545F4914F6CDD1DULL + 0x9E3779B97F4A7C15ULL;
}

static void ow_gen_one(oneiros_win_t *o){
    o->sc->temp=o->temp;
    if (!forward(o->model, o->sc, o->ctx)){
        o->numeric_error=1; o->generating=0; return;
    }
    int nx=ow_sample_text(o->sc->probs, ow_rnd(o));
    if (o->out_len<OW_OUT_MAX-1){ o->out[o->out_len++]=(char)nx; o->out[o->out_len]=0; }
    memmove(o->ctx, o->ctx+1, OG_B-1); o->ctx[OG_B-1]=(unsigned char)nx;
    o->done++;
}

int oneiros_win_tick(window_t *win){
    oneiros_win_t *o=(oneiros_win_t*)win->reserved;
    if (!o || !o->generating) return 0;
    for (int i=0;i<OW_STEPS && o->generating && o->done<o->want;i++) ow_gen_one(o);
    if (o->done>=o->want) o->generating=0;
    return 1;
}

/* ---- layout (shared by draw + click; ox,oy = client origin) ---------------- */
typedef struct { int x,y,w,h; } Rect;
typedef struct { Rect seed, dream, stop, clear, copy, tminus, tplus, out; } Layout;
static int ow_hit(Rect r,int mx,int my){ return mx>=r.x && mx<r.x+r.w && my>=r.y && my<r.y+r.h; }

static void ow_layout(int ox,int oy,int cw,int ch,Layout *L){
    L->seed=(Rect){ ox+OW_PAD+40, oy+44, cw-2*OW_PAD-40, 22 };
    L->dream=(Rect){ ox+OW_PAD,        oy+76, 76, 26 };
    L->stop =(Rect){ L->dream.x+76+8,  oy+76, 58, 26 };
    L->clear=(Rect){ L->stop.x+58+8,   oy+76, 62, 26 };
    L->copy =(Rect){ L->clear.x+62+8,  oy+76, 62, 26 };
    L->tplus =(Rect){ ox+cw-OW_PAD-24,       oy+76, 24, 26 };
    L->tminus=(Rect){ ox+cw-OW_PAD-24-44-24, oy+76, 24, 26 };
    L->out=(Rect){ ox+OW_PAD, oy+112, cw-2*OW_PAD, ch-112-22 };
}

static void ow_btn(Rect r,const char *label,int hot,int enabled){
    uint32_t fill = !enabled ? THEME_PANEL : (hot?THEME_ACCENT:THEME_BUTTON);
    fb_fill_rect(r.x, r.y, r.w, r.h, fill);
    fb_fill_rect(r.x, r.y, r.w, 1, THEME_BORDER);
    fb_fill_rect(r.x, r.y+r.h-1, r.w, 1, THEME_BORDER);
    fb_fill_rect(r.x, r.y, 1, r.h, THEME_BORDER);
    fb_fill_rect(r.x+r.w-1, r.y, 1, r.h, THEME_BORDER);
    int tw=(int)strlen(label)*OW_FCW;
    uint32_t fg = !enabled ? THEME_TEXT_DIM : (hot?THEME_ON_ACCENT:THEME_TEXT);
    font_draw_string_trans(r.x+(r.w-tw)/2, r.y+(r.h-16)/2, label, fg);
}

/* render `s` into `box`, wrapped, scrolled so the tail is visible */
static void ow_draw_text(Rect box,const char *s,int len,int caret){
    int cols=(box.w-10)/OW_FCW; if (cols<1) cols=1; if (cols>127) cols=127;
    int rows=(box.h-6)/OW_LH;   if (rows<1) rows=1;
    int col=0,total=1;
    for (int i=0;i<len;i++){ char c=s[i];
        if (c=='\n'){ total++; col=0; }
        else { int cells=c=='\t'?4-(col%4):1;
            while (cells--){ if (col>=cols){ total++; col=0; } col++; } } }
    int start=total-rows; if (start<0) start=0;
    char line[128]; int ll=0, row=0; int tx=box.x+5, ty=box.y+4;
    col=0;
    for (int i=0;i<len;i++){ char c=s[i];
        if (c=='\n'){
            if (row>=start){ line[ll]=0; font_draw_string_trans(tx, ty+(row-start)*OW_LH, line, THEME_TEXT); }
            ll=0; row++; col=0;
        } else {
            int cells=c=='\t'?4-(col%4):1;
            while (cells--){
                if (col>=cols){ if (row>=start){ line[ll]=0; font_draw_string_trans(tx, ty+(row-start)*OW_LH, line, THEME_TEXT); } ll=0; row++; col=0; }
                if (ll<127){ line[ll++]=c=='\t'?' ':c; }
                col++;
            }
        } }
    if (row>=start){ line[ll]=0; font_draw_string_trans(tx, ty+(row-start)*OW_LH, line, THEME_TEXT); }
    if (caret){ int cax=tx+col*OW_FCW, cay=ty+(row-start)*OW_LH;
        if (row>=start && ((get_ticks()/8)&1)) fb_fill_rect(cax, cay, OW_FCW, 15, THEME_ACCENT); }
}

void oneiros_win_draw(window_t *win, int cx, int cy, uint32_t cw, uint32_t ch){
    oneiros_win_t *o=(oneiros_win_t*)win->reserved;
    if (!o) return;
    Layout L; ow_layout(cx, cy, (int)cw, (int)ch, &L);

    fb_fill_rect(cx, cy, cw, ch, THEME_WINDOW_BG);
    font_draw_string_trans(cx+OW_PAD, cy+8, "Oneiros", THEME_ACCENT);
    font_draw_string_trans(cx+OW_PAD+72, cy+10, "a model that dreams NyxOS code", THEME_TEXT_DIM);

    /* seed field */
    font_draw_string_trans(cx+OW_PAD, L.seed.y+3, "seed", THEME_TEXT_DIM);
    fb_fill_rect(L.seed.x, L.seed.y, L.seed.w, L.seed.h, THEME_PANEL);
    fb_fill_rect(L.seed.x, L.seed.y, L.seed.w, 1, THEME_BORDER);
    fb_fill_rect(L.seed.x, L.seed.y+L.seed.h-1, L.seed.w, 1, THEME_BORDER);
    fb_fill_rect(L.seed.x, L.seed.y, 1, L.seed.h, THEME_BORDER);
    fb_fill_rect(L.seed.x+L.seed.w-1, L.seed.y, 1, L.seed.h, THEME_BORDER);
    {
        int maxc=(L.seed.w-10)/OW_FCW; if (maxc<1) maxc=1;
        const char *sv=o->seed; int sl=o->seed_len;
        if (sl>maxc){ sv=o->seed+(sl-maxc); sl=maxc; }
        char tmp[OW_SEED_MAX]; int j=0; for (; j<sl; j++) tmp[j]=sv[j]; tmp[j]=0;
        font_draw_string_trans(L.seed.x+5, L.seed.y+3, tmp, THEME_TEXT);
        if (!o->generating && ((get_ticks()/8)&1))
            fb_fill_rect(L.seed.x+5+sl*OW_FCW, L.seed.y+3, OW_FCW, 15, THEME_ACCENT);
    }

    /* controls */
    ow_btn(L.dream, "Dream", 1, o->loaded && !o->generating);
    ow_btn(L.stop,  "Stop",  0, o->generating);
    ow_btn(L.clear, "Clear", 0, 1);
    ow_btn(L.copy,  "Copy",  0, o->out_len>0);
    ow_btn(L.tminus, "-", 0, 1);
    ow_btn(L.tplus,  "+", 0, 1);
    {
        int t10=(int)(o->temp*10.0f+0.5f);       /* round, so 0.7f shows "0.7" not "0.6" */
        char tb[8]; snprintf(tb, sizeof(tb), "%d.%d", t10/10, t10%10);
        font_draw_string_trans(L.tminus.x+24+(44-3*OW_FCW)/2, L.tminus.y+5, tb, THEME_TEXT);
        font_draw_string_trans(L.tminus.x-2-4*OW_FCW, L.tminus.y+5, "temp", THEME_TEXT_DIM);
    }

    /* output */
    fb_fill_rect(L.out.x, L.out.y, L.out.w, L.out.h, THEME_PANEL);
    fb_fill_rect(L.out.x, L.out.y, L.out.w, 1, THEME_BORDER);
    fb_fill_rect(L.out.x, L.out.y+L.out.h-1, L.out.w, 1, THEME_BORDER);
    fb_fill_rect(L.out.x, L.out.y, 1, L.out.h, THEME_BORDER);
    fb_fill_rect(L.out.x+L.out.w-1, L.out.y, 1, L.out.h, THEME_BORDER);
    if (!o->loaded){
        font_draw_string_trans(L.out.x+8, L.out.y+8, "Oneiros couldn't load its model:", THEME_TEXT_DIM);
        font_draw_string_trans(L.out.x+8, L.out.y+8+OW_LH, "/usr/pkg/nyxgen/model.bin", THEME_TEXT_DIM);
        font_draw_string_trans(L.out.x+8, L.out.y+8+3*OW_LH, "(install it with: xbm install nyxgen)", THEME_TEXT_DIM);
    } else {
        Rect inner=(Rect){ L.out.x+1, L.out.y+1, L.out.w-2, L.out.h-2 };
        ow_draw_text(inner, o->out, o->out_len, o->generating);
    }

    /* status line */
    {
        char st[64];
        if (!o->loaded)          snprintf(st, sizeof(st), "no model");
        else if (o->numeric_error) snprintf(st, sizeof(st), "numeric error; try another model");
        else if (o->generating && o->copied_len)
                                snprintf(st, sizeof(st), "dreaming %d/%d; copied %d chars", o->done, o->want, o->copied_len);
        else if (o->generating)  snprintf(st, sizeof(st), "dreaming  %d/%d", o->done, o->want);
        else if (o->copied_len)  snprintf(st, sizeof(st), "copied %d chars", o->copied_len);
        else if (o->out_len>0)   snprintf(st, sizeof(st), "done  %d chars", o->out_len);
        else                     snprintf(st, sizeof(st), "type a seed, press Dream (Enter)");
        font_draw_string_trans(cx+OW_PAD, cy+(int)ch-16, st, THEME_TEXT_DIM);
    }
}

void oneiros_win_click(window_t *win, int mx, int my, int btn){
    if (btn!=1) return;
    oneiros_win_t *o=(oneiros_win_t*)win->reserved;
    if (!o) return;
    Layout L; ow_layout(win->x, WIN_CLIENT_Y(win), (int)win->w, (int)win->h, &L);
    if (ow_hit(L.dream,mx,my))  { ow_start(o); return; }
    if (ow_hit(L.stop,mx,my))   { o->generating=0; return; }
    if (ow_hit(L.clear,mx,my))  { o->out_len=0; o->out[0]=0; o->generating=0; o->done=0; o->copied_len=0; o->numeric_error=0; return; }
    if (ow_hit(L.copy,mx,my))   {
        if (o->out_len>0){ clipboard_set(o->out, (uint32_t)o->out_len); o->copied_len=o->out_len; }
        return;
    }
    if (ow_hit(L.tminus,mx,my)) { o->temp-=0.1f; if (o->temp<0.1f) o->temp=0.1f; return; }
    if (ow_hit(L.tplus,mx,my))  { o->temp+=0.1f; if (o->temp>1.5f) o->temp=1.5f; return; }
}

void oneiros_win_key(window_t *win, int key){
    oneiros_win_t *o=(oneiros_win_t*)win->reserved;
    if (!o) return;
    if (key=='\r' || key=='\n'){ ow_start(o); return; }
    if (o->generating){ if (key==27) o->generating=0; return; }   /* Esc stops */
    if (key=='\b' || key==0x7F){ if (o->seed_len>0){ o->seed[--o->seed_len]=0; } return; }
    if (key>=32 && key<127){ if (o->seed_len<OW_SEED_MAX-1){ o->seed[o->seed_len++]=(char)key; o->seed[o->seed_len]=0; } }
}
