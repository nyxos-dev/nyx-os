// ============================================================
// selene_win.c - Selene, the NyxOS web browser (v5.9.44, links + history v5.9.45)
// ============================================================
// A GUI window: a URL bar on top, a rendered page below, a status line at the bottom.
// Pressing Enter fetches the URL with http_get() (which drives the network itself),
// running DHCP first if we have no IP. The HTML is stripped to text (tags removed,
// <script>/<style> dropped, a few entities decoded, <title> pulled out) and word-
// wrapped into lines the view scrolls through.
//
// v5.9.45: <a href> links are parsed and rendered as coloured, underlined text.
// Tab / Shift-Tab move a selection between links, Enter follows the selected one (or
// loads the URL bar when nothing is selected), a click follows the link under the
// cursor, and Backspace goes Back through a per-window history stack. Relative and
// root-relative hrefs are resolved against the current page. (Back then HTTP only; HTTPS
// over TLS 1.2 landed at v5.9.56, so Selene now fetches https:// pages too.)
#include "../../core/kernel.h"
#include "../../core/urlparse.h"
#include "../../core/urlcodec.h"
#include "../core/compositor.h"
#include "selene_win.h"
#include "../../net/http.h"
#include "../../crypto/tls/tls.h"
#include "../../image/png.h"
#include "../../image/bmp.h"
#include "../../image/gif.h"
#include "../../image/jpeg.h"
#include "../../drivers/video/font.h"

#define SEL_BAR       34        // top toolbar (back button + URL box) height
#define SEL_STATUS    20        // bottom status strip height
#define SEL_PAD       8
#define SEL_LINE_H    18        // px per rendered text row
#define SEL_WRAP      86        // wrap width in chars (SEL_WRAP*8 < content width)
#define SEL_LINE_COLS 96
#define SEL_MAX_LINES 1200
#define SEL_LIST_MAXDEPTH 8     // <ul>/<ol> nesting tracked for indent + <ol> numbering (deeper = clamped)
#define SEL_LIST_INDENT   2     // spaces of indent added per list nesting level
#define SEL_QUOTE_MAXDEPTH 8    // <blockquote> nesting tracked for the left margin (deeper = clamped)
#define SEL_QUOTE_INDENT  4     // spaces of left margin added per <blockquote> nesting level
#define SEL_BOX_MAXDEPTH 8      // bordered <div> nesting tracked for the drawn box outline
#define SEL_BOX_INSET    5      // px each nested box is inset on both sides
#define SEL_BOX_TOP    200      // line_rule sentinel: this marker line is a box TOP edge (> hr's 1..100)
#define SEL_BOX_BOT    201      // line_rule sentinel: this marker line is a box BOTTOM edge
#define SEL_MAX_CELL_BOXES 32   // bordered-<div>-in-a-table-cell outlines drawn per page
#define SEL_PRE_TAB       4     // a tab in <pre> text expands to this many spaces
#define SEL_MAX_LINKS 240       // per page; link id stored as a uint8 (index+1)
#define SEL_HIST      32        // Back history depth
#define SEL_MAX_FORMS  16       // <form>s per page
#define SEL_MAX_FIELDS 64       // form controls per page; field id stored as a uint8 (index+1)
#define SEL_MAX_IMGS   128      // <img>s per page; image id stored as a uint8 (index+1)
#define SEL_IMG_BOX_W     24    // image display box width, in characters
#define SEL_IMG_BOX_LINES 5     // image display box height, in text rows
#define SEL_IMG_FETCH_CAP (512 * 1024)   // per-image download buffer (bytes)
#define SEL_FIELD_W    22       // rendered width (chars) of a text-input box
#define SEL_MAX_TABS   6        // browser tabs per Selene window
#define SEL_TABS_H     24       // tab-strip height (drawn below the toolbar)
#define SEL_TABS_NEWW  26       // width of the [+] new-tab button
#define SELENE_TICK_MS 33       // compositor tick period (~30fps) — the unit the GIF animator counts in

// Form control kinds.
#define SEL_FLD_TEXT   0        // text/search/email/url/password/tel/number -> editable box
#define SEL_FLD_SUBMIT 1        // submit button (<input type=submit> or <button>)
#define SEL_FLD_HIDDEN 2        // hidden field: carried into the submission, not shown

typedef struct { char url[192]; } sel_link_t;
typedef struct { char action[192]; uint8_t method; } sel_form_t;   // method 0 = GET (only GET so far)
typedef struct { int form; uint8_t kind; char name[64]; char value[160]; } sel_field_t;
// <img>: alt/src + decoded RGBA (px=NULL until fetched; tried=1 once attempted). Animated GIFs also
// carry the composited frames: `px` then ALIASES frames[cur_frame].pixels (freed via frames[], not px).
typedef struct {
    char alt[64]; char src[160];
    uint8_t* px; uint16_t iw, ih; uint8_t tried;
    gif_frame_t* frames;                 // animated GIF frames, else NULL (static image)
    int nframes, cur_frame;              // frame count + the one px points at
    uint32_t anim_ms;                    // ms accumulated toward the current frame's delay
    int loop_count, loops_done;          // NETSCAPE loop count (0 = infinite) + loops completed so far
} sel_img_t;

typedef struct {
    char url[256];
    int  url_len;
    char cur_url[256];           // the URL of the page currently shown (for history)
    char title[96];
    char status[96];
    int  scroll;                 // top visible line index
    int  num_lines;
    char    (*lines)[SEL_LINE_COLS];   // kmalloc'd SEL_MAX_LINES rows of text
    uint8_t (*link_of)[SEL_LINE_COLS]; // per-char link id (0 = none, else link index+1)
    sel_link_t* links;                 // kmalloc'd SEL_MAX_LINKS
    int  num_links;
    int  sel_link;               // -1 = editing the URL bar; else the selected link index
    // HTML forms: a per-char field-id grid (like link_of), the fields, and their owning forms
    uint8_t (*field_of)[SEL_LINE_COLS]; // per-char field id (0 = none, else field index+1)
    sel_field_t* fields;               // kmalloc'd SEL_MAX_FIELDS
    int  num_fields;
    sel_form_t*  forms;                // kmalloc'd SEL_MAX_FORMS
    int  num_forms;
    int  sel_field;              // focused form field index (-1 = none)
    // Images: a per-char image-id grid (like field_of) and the parsed <img> alt/src. Same-origin
    // images are fetched, decoded (PNG/BMP/GIF/JPEG) and drawn inline; a framed "[img: alt]" box
    // is the fallback when the fetch/decode fails or the format is unsupported.
    uint8_t (*img_of)[SEL_LINE_COLS];  // per-char image id (0 = none, else image index+1)
    sel_img_t* images;                 // kmalloc'd SEL_MAX_IMGS
    int  num_imgs;
    // Inline-CSS colours: per-char palette indices (0 = default; else palette[idx-1]). color_of is the
    // foreground (style="color:.." / <font color=>), bgcolor_of the background (background[-color]:..).
    // Both index the same per-page, deduped palette.
    uint8_t (*color_of)[SEL_LINE_COLS];
    uint8_t (*bgcolor_of)[SEL_LINE_COLS];
    uint8_t (*bold_of)[SEL_LINE_COLS]; // per-char text-style flags: bit0=bold, bit1=underline, bit2=line-through, bits4-5=sub/sup, bit6=overline (<b>/<u>/<s>/<del>/style)
    uint32_t palette[255];             // framebuffer pixel values, index 0 => palette id 1
    int  npalette;
    uint8_t page_bg, page_fg;          // <body bgcolor>/<body text> (+ CSS body background/color): page background & default text colour (palette idx, 0 = default)
    uint8_t line_align[SEL_MAX_LINES]; // per-line text alignment: 0=left (default), 1=center, 2=right
    uint8_t line_rule[SEL_MAX_LINES];  // per-line <hr> flag: 1 = draw a real horizontal rule (not text)
    // Bordered <div> INSIDE a table cell -> a drawn outline rectangle. render_table records each in txt-index
    // space (txt0/txt1) + the cell's column span; after wrap_text the txt indices are resolved to line0/line1.
    struct { uint32_t txt0, txt1; uint16_t line0, line1; uint8_t col0, col1, col, used; } cell_boxes[SEL_MAX_CELL_BOXES];
    int num_cell_boxes;
    // base for resolving relative links (from the loaded URL)
    char     base_host[128];
    uint16_t base_port;
    int      base_https;         // the loaded page's scheme (for resolving relative links)
    char     base_path[256];
    // Back history (stack of URLs left behind) + Forward stack (pages left by going Back).
    char hist[SEL_HIST][256];
    int  hist_len;
    char fwd[SEL_HIST][256];
    int  fwd_len;
    // Find-in-page (Ctrl+F): a query, whether the find bar is capturing input, and match tracking.
    char find_q[64];
    int  find_len;
    int  find_active;            // 1 = the find bar is open and taking keystrokes
    int  find_matches;           // total matches of find_q on the page
    int  find_cur;               // index of the current (accented) match, 0-based
    // A load spans network parsing and HTML rendering. Keep its buffers off the
    // kernel stack rather than retaining another kilobyte across those calls.
    struct {
        http_response_t response;
        char host[128], path[256], final_host[128], redirect[256];
    } load;
} selene_ctx_t;

// A Selene window holds several tabs, each a full page context; one is the active view.
typedef struct { selene_ctx_t* tab[SEL_MAX_TABS]; int ntabs; int active; } selene_tabs_t;

// ---- small string helpers (name buffers are already lowercased) ----

static int sel_streq(const char* a, const char* b) { return strcmp(a, b) == 0; }

static int is_block_tag(const char* n) {
    static const char* B[] = { "p","br","div","h1","h2","h3","h4","h5","h6","li","tr",
        "hr","ul","ol","table","section","article","header","footer","nav","form",
        "blockquote","pre","dd","dt","dl","center","main","aside","address",0 };   // <figure>/<figcaption> have dedicated arms (indent + own line); main/aside/address are HTML5 block elements too
    for (int i = 0; B[i]; i++) if (sel_streq(n, B[i])) return 1;
    return 0;
}

// The named/numeric HTML entities Selene decodes, each to a short display string. The font is a
// single-byte 256-glyph set, so multi-char symbols map to readable ASCII approximations (— -> "--",
// … -> "...", © -> "(c)"), while ° uses the CP437 degree glyph (0xF8). `cp` is the Unicode code point,
// used to also resolve the numeric forms (&#176; / &#xB0;).
typedef struct { const char* name; uint32_t cp; const char* str; } sel_entity_t;
static const sel_entity_t SEL_ENTITIES[] = {
    {"amp",38,"&"},    {"lt",60,"<"},     {"gt",62,">"},      {"quot",34,"\""}, {"apos",39,"'"},
    {"nbsp",160,"\xff"},{"copy",169,"(c)"},{"reg",174,"(r)"}, {"trade",8482,"(tm)"},   // 0xFF = a blank-rendering, non-breaking space (wrap_text won't split on it)
    {"mdash",8212,"--"},{"ndash",8211,"-"},{"hellip",8230,"..."},
    {"lsquo",8216,"'"},{"rsquo",8217,"'"},{"ldquo",8220,"\""},{"rdquo",8221,"\""},
    {"laquo",171,"<<"},{"raquo",187,">>"},{"middot",183,"*"}, {"bull",8226,"*"},
    {"times",215,"x"}, {"divide",247,"/"},{"plusmn",177,"+/-"},
    {"frac12",189,"1/2"},{"frac14",188,"1/4"},{"frac34",190,"3/4"},
    {"deg",176,"\xf8"},{"sect",167,"S"},  {"para",182,"P"},   {"euro",8364,"EUR"},
    {"pound",163,"GBP"},{"cent",162,"c"},   {"yen",165,"\x9d"}, {"micro",181,"\xe6"},   // CP437: yen 0x9D, micro (mu) 0xE6
    {"iexcl",161,"\xad"},{"iquest",191,"\xa8"},{"ordf",170,"\xa6"},{"ordm",186,"\xa7"},{"not",172,"\xaa"},{"szlig",223,"\xe1"},  // Latin-1 letters/marks as CP437 glyphs (iexcl/iquest = Spanish, szlig = ss-ligature)
    {"sup2",178,"\xfd"},{"sup3",179,"3"},    // superscripts (CP437 has a glyph only for squared)
    {"larr",8592,"\x1b"},{"uarr",8593,"\x18"},{"rarr",8594,"\x1a"},{"darr",8595,"\x19"},{"harr",8596,"\x1d"},   // arrows (CP437 control-range glyphs 0x18-0x1D)
    {"hearts",9829,"\x03"},{"diams",9830,"\x04"},{"clubs",9827,"\x05"},{"spades",9824,"\x06"},   // card suits (CP437 0x03-0x06)
    {"equiv",8801,"\xf0"},{"ge",8805,"\xf2"},{"le",8804,"\xf3"},{"ne",8800,"!="},{"minus",8722,"-"},   // math relations (CP437: equiv 0xF0, ge 0xF2, le 0xF3)
    {"radic",8730,"\xfb"},{"infin",8734,"\xec"},{"cap",8745,"\xef"},   // math symbols (CP437: sqrt 0xFB, infinity 0xEC, intersection 0xEF)
    {"dagger",8224,"+"},{"Dagger",8225,"++"},{"prime",8242,"'"},{"Prime",8243,"\""},   // typographic marks (ASCII fallbacks; no CP437 glyph)
    {"alpha",945,"\xe0"},{"beta",946,"\xe1"},{"Gamma",915,"\xe2"},{"pi",960,"\xe3"},{"Pi",928,"\xe3"},   // Greek letters (CP437 0xE0-0xEE)
    {"Sigma",931,"\xe4"},{"sigma",963,"\xe5"},{"mu",956,"\xe6"},{"tau",964,"\xe7"},{"Phi",934,"\xe8"},
    {"Theta",920,"\xe9"},{"Omega",937,"\xea"},{"delta",948,"\xeb"},{"phi",966,"\xed"},{"epsilon",949,"\xee"},
    {"aacute",225,"\xa0"},{"eacute",233,"\x82"},{"iacute",237,"\xa1"},{"oacute",243,"\xa2"},{"uacute",250,"\xa3"},   // accented Latin-1 letters (CP437)
    {"ntilde",241,"\xa4"},{"Ntilde",209,"\xa5"},{"agrave",224,"\x85"},{"egrave",232,"\x8a"},{"ccedil",231,"\x87"},{"Ccedil",199,"\x80"},
    {"uuml",252,"\x81"},{"ouml",246,"\x94"},{"auml",228,"\x84"},{"Uuml",220,"\x9a"},{"Ouml",214,"\x99"},{"Auml",196,"\x8e"},
};
#define SEL_NENT (int)(sizeof(SEL_ENTITIES)/sizeof(SEL_ENTITIES[0]))

// Decode an HTML entity at p (len bytes available). On success writes up to `cap` bytes of the decoded
// text to `out`, sets *outlen + *adv (bytes consumed, incl. the trailing ';'), and returns 1. Handles
// the SEL_ENTITIES table (named + by code point) and numeric &#nn; / &#xhh;.
static int decode_entity(const uint8_t* p, uint32_t len, char* out, uint32_t cap, uint32_t* outlen, uint32_t* adv) {
    if (len < 3 || p[0] != '&') return 0;
    uint32_t semi = 0;
    for (uint32_t i = 1; i < len && i < 10; i++) if (p[i] == ';') { semi = i; break; }
    if (!semi) return 0;
    const char* s = 0; char tmp[2];
    if (p[1] == '#') {
        int hex = (p[2] == 'x' || p[2] == 'X');
        uint32_t v = 0;
        for (uint32_t i = hex ? 3 : 2; i < semi; i++) {
            char c = (char)p[i];
            if (hex) {
                if (c >= '0' && c <= '9') v = v*16 + (c - '0');
                else if (c >= 'a' && c <= 'f') v = v*16 + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v = v*16 + (c - 'A' + 10);
                else return 0;
            } else {
                if (c >= '0' && c <= '9') v = v*10 + (c - '0'); else return 0;
            }
        }
        for (int k = 0; k < SEL_NENT && !s; k++) if (SEL_ENTITIES[k].cp == v) s = SEL_ENTITIES[k].str;
        if (!s) { tmp[0] = (v >= 0x20 && v < 0x7F) ? (char)v : (v == 0xA0 ? (char)0xFF : '?'); tmp[1] = '\0'; s = tmp; }   // U+00A0 -> non-breaking space (0xFF)
    } else {
        char name[8]; uint32_t nl = 0;
        for (uint32_t i = 1; i < semi && nl < 7; i++) name[nl++] = (char)p[i];
        name[nl] = '\0';
        for (int k = 0; k < SEL_NENT && !s; k++) if (sel_streq(name, SEL_ENTITIES[k].name)) s = SEL_ENTITIES[k].str;
        if (!s) return 0;
    }
    uint32_t n = 0; while (s[n] && n < cap) { out[n] = s[n]; n++; }
    *outlen = n; *adv = semi + 1;
    return 1;
}

// Case-insensitive: does p start with the (lowercase) literal lit?
static int ci_starts(const uint8_t* p, uint32_t len, const char* lit) {
    for (uint32_t i = 0; lit[i]; i++) {
        if (i >= len) return 0;
        char c = (char)p[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != lit[i]) return 0;
    }
    return 1;
}

// Resolve an href against the loaded page's base into an absolute http:// URL in out
// (192 bytes). Empty out means "not navigable" (fragment, mailto:, javascript:).
static void selene_resolve(selene_ctx_t* s, const char* href, char* out) {
    while (*href == ' ') href++;
    out[0] = '\0';
    const char* scheme = s->base_https ? "https" : "http";
    if (strncmp(href, "http://", 7) == 0)  { strncpy(out, href, 191); out[191] = '\0'; return; }
    if (strncmp(href, "https://", 8) == 0) { strncpy(out, href, 191); out[191] = '\0'; return; }   // keep https — Selene speaks TLS now
    if (href[0] == '#') {                                  // in-page anchor (href="#" / "#section"): a real link to the CURRENT page (browsers render it blue), so resolve to this page instead of dropping it
        char hp[160]; uint16_t dp = s->base_https ? 443 : 80;
        if (s->base_port != dp) snprintf(hp, sizeof(hp), "%s:%u", s->base_host, s->base_port);
        else                    snprintf(hp, sizeof(hp), "%s", s->base_host);
        snprintf(out, 192, "%s://%s%s", scheme, hp, s->base_path);
        return;
    }
    if (href[0] == '\0') return;                           // <a> with no href (e.g. a named anchor <a name=..>) is not a link
    if (strncmp(href, "mailto:", 7) == 0 || strncmp(href, "javascript:", 11) == 0) return;
    if (href[0] == '/' && href[1] == '/')  { snprintf(out, 192, "%s:%s", scheme, href); return; }

    char hostport[160];
    uint16_t defport = s->base_https ? 443 : 80;
    if (s->base_port != defport) snprintf(hostport, sizeof(hostport), "%s:%u", s->base_host, s->base_port);
    else                         snprintf(hostport, sizeof(hostport), "%s", s->base_host);

    if (href[0] == '/') { snprintf(out, 192, "%s://%s%s", scheme, hostport, href); return; }

    // relative: keep the base path's directory (everything up to the last '/')
    char dir[200]; int last = -1;
    for (int k = 0; s->base_path[k] && k < 198; k++) if (s->base_path[k] == '/') last = k;
    if (last < 0) { dir[0] = '/'; dir[1] = '\0'; }
    else { int dl = last + 1; __builtin_memcpy(dir, s->base_path, dl); dir[dl] = '\0'; }
    snprintf(out, 192, "%s://%s%s%s", scheme, hostport, dir, href);
}

// ---- word-wrap a stripped-text buffer into ctx->lines, carrying per-char link ids ----
// tindent[i] = the <blockquote> nesting level in effect at char i; a fresh line (hard '\n' OR soft
// word-wrap) whose content sits at level L is prefixed with L*SEL_QUOTE_INDENT spaces, so a quoted
// paragraph stays indented even where it wraps. Level 0 (the common case) is byte-for-byte unchanged.
// line_start (optional, may be NULL) is a pure side table: line_start[li] = the txt index of the first
// char placed on final line li. It is only WRITTEN here (never read, never alters wrapping), so s->lines
// and every per-char array are byte-identical with or without it; render_table uses it after wrap to map
// a cell's line-start txt index to its final line number for the in-cell box outline.
static void wrap_text(selene_ctx_t* s, const char* txt, const uint8_t* tlink, const uint8_t* tfield,
                      const uint8_t* timg, const uint8_t* tcolor, const uint8_t* tbgcol,
                      const uint8_t* tbold, const uint8_t* talign, const uint8_t* trule,
                      const uint8_t* tindent, uint32_t* line_start, uint32_t ti) {
    int li = 0, col = 0, bol = 1;   // bol: at a HARD line start (after '\n') — preserve intentional leading indent
    s->lines[0][0] = '\0';
    if (line_start) line_start[0] = 0;
    uint32_t i = 0;
    while (i < ti && li < SEL_MAX_LINES) {
        char c = txt[i];
        if (c == '\n') {
            s->lines[li][col] = '\0';
            li++; col = 0; bol = 1;
            if (li < SEL_MAX_LINES) s->lines[li][0] = '\0';
            if (line_start && li < SEL_MAX_LINES) line_start[li] = i + 1;   // next line's first char
            i++;
            continue;
        }
        if (c == ' ') {
            // Leading spaces are dropped after a soft word-wrap, but kept after a hard '\n' (list indents).
            if ((col > 0 || bol) && col < SEL_LINE_COLS - 1) { s->link_of[li][col] = tlink[i]; s->field_of[li][col] = tfield[i]; s->img_of[li][col] = timg[i]; s->color_of[li][col] = tcolor[i]; s->bgcolor_of[li][col] = tbgcol[i]; s->bold_of[li][col] = tbold[i]; s->line_align[li] = talign[i]; s->line_rule[li] = trule[i]; s->lines[li][col++] = ' '; }
            i++;
            continue;
        }
        uint32_t st = i;
        while (i < ti && txt[i] != ' ' && txt[i] != '\n') i++;
        int wlen = (int)(i - st);
        int indent = (int)tindent[st] * SEL_QUOTE_INDENT;     // <blockquote> left margin for this word's line
        int off = 0;
        while (wlen > 0 && li < SEL_MAX_LINES) {
            if (col > indent && col + wlen > SEL_WRAP) {       // word won't fit: next line
                if (col > 0 && s->lines[li][col-1] == ' ') col--;
                s->lines[li][col] = '\0';
                li++; col = 0; bol = 0;                        // soft-wrapped continuation (indent re-applied below)
                if (li >= SEL_MAX_LINES) break;
                s->lines[li][0] = '\0';
                if (line_start) line_start[li] = st + off;      // this line's first (word) char
            }
            if (col == 0 && indent > 0)                        // fresh line inside a <blockquote>: left margin, with a "|" bar (CP437 0xB3) at each nesting level
                for (int q = 0; q < indent && col < SEL_LINE_COLS - 1; q++) {
                    s->color_of[li][col] = 0; s->bgcolor_of[li][col] = 0; s->bold_of[li][col] = 0;   // clear the synthesized margin cells so the bar draws in the default colour
                    s->lines[li][col++] = (q % SEL_QUOTE_INDENT == 0) ? (char)0xB3 : ' ';
                }
            int take = wlen;
            if (take > SEL_WRAP - col) take = SEL_WRAP - col; // hard-split an over-long word
            if (take <= 0) { s->lines[li][col] = '\0'; li++; col = 0; bol = 0; if (li < SEL_MAX_LINES) s->lines[li][0]='\0'; if (line_start && li < SEL_MAX_LINES) line_start[li] = st + off; continue; }
            for (int k = 0; k < take && col < SEL_LINE_COLS - 1; k++) {
                s->link_of[li][col] = tlink[st + off + k];
                s->field_of[li][col] = tfield[st + off + k];
                s->img_of[li][col] = timg[st + off + k];
                s->color_of[li][col] = tcolor[st + off + k];
                s->bgcolor_of[li][col] = tbgcol[st + off + k];
                s->bold_of[li][col] = tbold[st + off + k];
                s->line_align[li] = talign[st + off + k];
                s->line_rule[li] = trule[st + off + k];
                s->lines[li][col++] = txt[st + off + k];
            }
            bol = 0;                                           // wrote content — no longer at a hard line start
            off += take; wlen -= take;
        }
    }
    if (li < SEL_MAX_LINES) { s->lines[li][col] = '\0'; s->num_lines = li + 1; }
    else s->num_lines = SEL_MAX_LINES;
}

// Extract the href value from an <a ...> tag spanning body[j..te) into out (192).
static void extract_href(const uint8_t* body, uint32_t j, uint32_t te, char* out) {
    out[0] = '\0';
    for (uint32_t k = j; k + 4 < te; k++) {
        if (ci_starts(body + k, te - k, "href")) {
            uint32_t m = k + 4;
            while (m < te && (body[m] == ' ' || body[m] == '=')) m++;
            char q = 0;
            if (m < te && (body[m] == '"' || body[m] == '\'')) { q = (char)body[m]; m++; }
            uint32_t h = 0;
            while (m < te && h < 191) {
                char ch = (char)body[m];
                if (q ? (ch == q) : (ch == ' ' || ch == '>')) break;
                out[h++] = ch; m++;
            }
            out[h] = '\0';
            return;
        }
    }
}

// Extract a named attribute value from a tag spanning body[j..te) into out (outcap bytes).
// Case-insensitive attribute name, honours single/double quotes; empty out if absent.
static void extract_attr(const uint8_t* body, uint32_t j, uint32_t te, const char* attr,
                         char* out, uint32_t outcap) {
    out[0] = '\0';
    uint32_t al = 0; while (attr[al]) al++;
    for (uint32_t k = j; k + al < te; k++) {
        if (k > j) { char pc = (char)body[k-1];          // require a word boundary before the name
            if (!(pc==' '||pc=='\t'||pc=='\n'||pc=='\r'||pc=='/')) continue; }
        if (!ci_starts(body + k, te - k, attr)) continue;
        uint32_t m = k + al;
        while (m < te && (body[m]==' '||body[m]=='\t'||body[m]=='\n'||body[m]=='\r')) m++;
        if (m >= te || body[m] != '=') continue;         // must be name=value
        m++;
        while (m < te && (body[m]==' '||body[m]=='\t'||body[m]=='\n'||body[m]=='\r')) m++;
        char q = 0;
        if (m < te && (body[m]=='"' || body[m]=='\'')) { q = (char)body[m]; m++; }
        uint32_t h = 0;
        while (m < te && h + 1 < outcap) {
            char ch = (char)body[m];
            if (q ? (ch == q) : (ch==' '||ch=='>'||ch=='\t'||ch=='\n'||ch=='\r')) break;
            out[h++] = ch; m++;
        }
        out[h] = '\0';
        return;
    }
}

// Ensure the text buffer ends with `want` newlines (1 = line break, 2 = a blank line
// between paragraphs), collapsing so runs of block tags don't pile up blank lines.
static uint32_t sel_ensure_nl(char* txt, uint8_t* tlink, uint32_t ti, uint32_t cap, int want) {
    int have = 0;
    while ((int)ti - 1 - have >= 0 && txt[ti - 1 - have] == '\n') have++;
    while (have < want && ti < cap) { txt[ti] = '\n'; tlink[ti] = 0; ti++; have++; }
    return ti;
}

// Emit a run of literal characters into the text stream, tagged with a form-field id (0 = none).
static void sel_emit(char* txt, uint8_t* tlink, uint8_t* tfield, uint32_t* ti, uint32_t cap,
                     const char* str, int fid) {
    for (const char* p = str; *p && *ti < cap; p++) {
        txt[*ti] = *p; tlink[*ti] = 0; tfield[*ti] = (uint8_t)fid; (*ti)++;
    }
}

// ===== <table> column layout: parse rows/cells, size columns, emit a bordered ASCII table =====
#define SEL_TBL_MAXCOLS 12       // columns beyond this are dropped
#define SEL_TBL_MAXROWS 80       // rows beyond this are dropped
#define SEL_TBL_MAXCELLS 480     // total cells captured (kmalloc'd)
#define SEL_TBL_COLCAP   22      // max display width of one column (chars)
#define SEL_TBL_ROWCAP   84      // max total row width (< SEL_WRAP, so rows never word-wrap)
#define SEL_CELL_CAP     320     // per-cell chars extracted for multi-line wrapping (>= one line; bounds a cell's height)
#define SEL_CELL_MAXLINES 12     // max wrapped lines a single table cell may occupy (bounds a row's height)

typedef struct { uint32_t s, e; uint16_t row, col; uint8_t th, cspan, rspan, align, bg; } sel_tcell_t;  // align: 0 left, 1 center, 2 right; bg: cell background palette idx (0 = none)

// Read an integer tag attribute (e.g. colspan/rowspan) from the tag in body[s..e); 0 if absent.
static int sel_span_attr(const uint8_t* body, uint32_t s, uint32_t e, const char* name) {
    char b[8]; extract_attr(body, s, e, name, b, sizeof(b));
    int v = 0; const char* q = b; while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
    return v;
}

// Parse a decimal string like "70" or "0.7" into thousandths (70 -> 70000, 0.7 -> 700); -1 if no digits.
// Used for <progress>/<meter> value/max/min, which may be fractional, without needing floating point.
static int sel_parse_milli(const char* s) {
    while (*s == ' ') s++;
    long v = 0; int any = 0;
    for (; *s >= '0' && *s <= '9'; s++) { v = v * 10 + (*s - '0'); any = 1; }
    v *= 1000;
    if (*s == '.') { s++; int frac = 0, fd = 0;
        for (; *s >= '0' && *s <= '9' && fd < 3; s++) { frac = frac * 10 + (*s - '0'); fd++; any = 1; }
        while (fd < 3) { frac *= 10; fd++; }
        v += frac; }
    return any ? (int)v : -1;
}

// If body[i] opens/closes exactly tag `nm`, return 1, set *close (1=closing) and *past (just after '>').
static int sel_tag_match(const uint8_t* body, uint32_t i, uint32_t e, const char* nm, int* close, uint32_t* past) {
    if (i >= e || body[i] != '<') return 0;
    uint32_t j = i + 1; *close = 0;
    if (j < e && body[j] == '/') { *close = 1; j++; }
    char name[10]; int n = 0;
    while (j < e && n < 9) { char t = (char)body[j];
        if ((t>='a'&&t<='z')||(t>='A'&&t<='Z')||(t>='0'&&t<='9')) { name[n++] = (t>='A'&&t<='Z')?t+32:t; j++; }
        else break; }
    name[n] = '\0';
    if (!sel_streq(name, nm)) return 0;
    while (j < e && body[j] != '>') j++;
    if (j < e) j++;
    *past = j;
    return 1;
}

// Flatten a nested <table> (body[i] at its '<table' open) into a compact inline "[ a b / c d ]"
// so a table inside a cell reads in place instead of corrupting the grid. Declared here, used by
// sel_cell_text below; defined after it (it calls back into sel_cell_text for each inner cell).
static uint32_t sel_flatten_nested(selene_ctx_t* sx, const uint8_t* body, uint32_t i, uint32_t e, char* out,
                                   uint8_t* ocol, uint8_t* obg, uint8_t* obold, uint8_t* olink, int* n, int cap, int break_rows);
// Styled cell extractor (defined below) — forward-declared so sel_flatten_nested can style each nested cell.
static int sel_cell_text_styled(selene_ctx_t* sx, const uint8_t* body, uint32_t s, uint32_t e,
                                char* out, uint8_t* ocol, uint8_t* obg, uint8_t* obold, uint8_t* olink, int cap, int upper);

// Plain text of a table cell body[s..e): strip inner tags, decode entities, collapse whitespace, trim.
// A nested <table> is replaced by sel_flatten_nested's compact "[ ... ]" form (kept in place, not merged).
static int sel_cell_text(const uint8_t* body, uint32_t s, uint32_t e, char* out, int cap, int upper) {
    int n = 0, last_space = 1;
    for (uint32_t i = s; i < e && n < cap - 1; ) {
        char c = (char)body[i];
        if (c == '<') {
            int tc; uint32_t tp;
            if (sel_tag_match(body, i, e, "table", &tc, &tp) && !tc) {    // nested table -> inline "[ ... ]"
                if (!last_space && n < cap - 1) out[n++] = ' ';
                i = sel_flatten_nested(0, body, i, e, out, 0, 0, 0, 0, &n, cap, 0);   // break_rows=0, NULL attrs: the plain "[ a b / c d ]" one-line form (column-width measurement + captions — no 0x01, no styling)
                if (n < cap - 1) { out[n++] = ' '; }
                last_space = 1; continue;
            }
            while (i < e && body[i] != '>') i++;
            if (i < e) i++;
            continue;
        }
        if (c == '&') { char eb[8]; uint32_t el, adv;
            if (decode_entity(body + i, e - i, eb, sizeof(eb), &el, &adv)) {
                for (uint32_t k = 0; k < el && n < cap - 1; k++) {
                    char d = eb[k];
                    if (d == ' ') { if (!last_space) { out[n++] = ' '; last_space = 1; } }
                    else { if (upper && d>='a'&&d<='z') d -= 32; out[n++] = d; last_space = 0; }
                }
                i += adv; continue; }
            out[n++] = '&'; last_space = 0; i++; continue; }
        if (c==' '||c=='\t'||c=='\n'||c=='\r') { if (!last_space) { out[n++] = ' '; last_space = 1; } i++; continue; }
        if (upper && c>='a'&&c<='z') c -= 32;
        out[n++] = c; last_space = 0; i++;
    }
    while (n > 0 && out[n-1] == ' ') n--;   // trim trailing space
    out[n] = '\0';
    return n;
}

// Flatten the nested <table> whose '<table' open is at body[i] into a compact one-line "[ a b / c d ]"
// (cells space-separated, rows separated by " / "), appended into out[*n..cap). A table nested inside
// this one collapses to "[..]" (no unbounded recursion). Returns the index just past the matching
// </table>, so the caller resumes after the whole nested table.
//
// STYLED variant (break_rows=1, from sel_cell_text_styled): if the per-char attr arrays are non-NULL
// (ocol/obg/obold/olink, same base as out), each nested cell is extracted with its inline styling +
// links via sel_cell_text_styled (so a nested file-list's names render blue+clickable and coloured
// spans render) and the synthesized glyphs/separators carry 0 attrs. The PLAIN path passes sx + all
// four attr pointers as NULL: no attr writes, no sx use, byte-identical "[ a b / c d ]" output (kept
// exact for column-width measurement + captions).
static uint32_t sel_flatten_nested(selene_ctx_t* sx, const uint8_t* body, uint32_t i, uint32_t e, char* out,
                                   uint8_t* ocol, uint8_t* obg, uint8_t* obold, uint8_t* olink, int* n, int cap, int break_rows) {
    int c0; uint32_t past;
    if (!sel_tag_match(body, i, e, "table", &c0, &past)) return i + 1;   // shouldn't happen; skip one byte
    i = past;
    #define SFN_PUT(ch) do { if (*n < cap - 1) { out[*n] = (char)(ch); if (olink) { ocol[*n]=0; obg[*n]=0; obold[*n]=0; olink[*n]=0; } (*n)++; } } while (0)   // styled path: glyphs/separators carry no style
    if (!break_rows) { SFN_PUT('['); SFN_PUT(' '); }   // break_rows: no "[ ]" wrapper — rows will stack, one per line
    int firstrow = 1, firstcell = 1, depth = 1;
    while (i < e && depth > 0) {
        if (body[i] != '<') { i++; continue; }
        int cl; uint32_t p;
        if (sel_tag_match(body, i, e, "table", &cl, &p)) {              // deeper nesting
            if (cl) { depth--; if (depth == 0) { i = p; break; } i = p; continue; }
            if (!firstcell) SFN_PUT(' ');                               // a table-in-a-cell: collapse to "[..]"
            SFN_PUT('['); SFN_PUT('.'); SFN_PUT('.'); SFN_PUT(']'); firstcell = 0;
            int d = 1; uint32_t m = p;                                  // skip the whole deeper table
            while (m < e && d > 0) {
                if (body[m] == '<') { int c2; uint32_t p2;
                    if (sel_tag_match(body, m, e, "table", &c2, &p2)) { if (c2) d--; else d++; m = p2; continue; } }
                m++;
            }
            i = m; continue;
        }
        if (sel_tag_match(body, i, e, "tr", &cl, &p)) {                 // new row
            if (!cl) { if (!firstrow) { if (break_rows) SFN_PUT(0x01); else { SFN_PUT(' '); SFN_PUT('/'); SFN_PUT(' '); } } firstrow = 0; firstcell = 1; }   // break_rows: a hard line break (0x01) between rows so each renders on its own line
            i = p; continue;
        }
        if ((sel_tag_match(body, i, e, "td", &cl, &p) || sel_tag_match(body, i, e, "th", &cl, &p)) && !cl) {
            uint32_t k = p;                                            // this cell's end (breaks at any table tag)
            while (k < e) {
                if (body[k] == '<') { int c3; uint32_t p3;
                    if (sel_tag_match(body,k,e,"td",&c3,&p3) || sel_tag_match(body,k,e,"th",&c3,&p3) ||
                        sel_tag_match(body,k,e,"tr",&c3,&p3) || sel_tag_match(body,k,e,"table",&c3,&p3)) break; }
                k++;
            }
            if (!firstcell) SFN_PUT(' ');
            if (olink) {                                              // styled path: extract inline styling + links straight into the attr arrays at offset *n (range [p,k) is table-free -> no re-entry)
                int w = sel_cell_text_styled(sx, body, p, k, out + *n, ocol + *n, obg + *n, obold + *n, olink + *n, cap - *n, 0);
                *n += w;
            } else {                                                  // plain path: unchanged plain-text copy via temp buffer
                char cb[SEL_TBL_COLCAP + 2];
                int w = sel_cell_text(body, p, k, cb, sizeof(cb), 0);  // table-free range -> no re-entry here
                for (int z = 0; z < w; z++) SFN_PUT(cb[z]);
            }
            firstcell = 0;
            i = k; continue;
        }
        { uint32_t k = i + 1; while (k < e && body[k] != '>') k++; if (k < e) k++; i = k; }   // skip other tag
    }
    if (!break_rows) { SFN_PUT(' '); SFN_PUT(']'); }
    #undef SFN_PUT
    return i;
}

// Inline-CSS helpers (defined below, after the table code) — forward-declared so render_table can read
// a <caption>'s caption-side property.
static int sel_css_get(const char* style, const char* prop, char* out, int cap);
static int sel_ci_streq(const char* a, const char* b);
static int sel_parse_css_color(const char* v, uint32_t* rgb);
static uint8_t sel_intern_color(selene_ctx_t* s, uint32_t rgb);

// Styled variant of sel_cell_text: identical plain-text extraction, but ALSO records the inline styling
// of each output char -- colour (ocol), background (obg), bold (obold) and hyperlink id (olink) -- from the
// cell's own tags: <b>/<strong> (bold), style="color:"/"background[-color]:", <font color/bgcolor>, <mark>,
// the monospace family <code>/<kbd>/<samp>/<tt> (grey background), and <a href> (registers the link in the
// page and marks its chars). Colours are interned into the page palette via sx, exactly like the main parse
// loop, so a coloured word, a background "pill" or a link inside a <td>/<th> renders instead of being
// flattened to plain text. A nested <table> still collapses to "[ ... ]" and its synthesized chars carry no
// styling. ocol/obg/obold/olink must each hold at least `cap` bytes.
static int sel_cell_text_styled(selene_ctx_t* sx, const uint8_t* body, uint32_t s, uint32_t e,
                                char* out, uint8_t* ocol, uint8_t* obg, uint8_t* obold, uint8_t* olink, int cap, int upper) {
    int n = 0, last_space = 1, cur_link = 0;
    struct { char tag[12]; uint8_t col, bg, bold; } stk[10]; int sd = 0;   // inline style stack within the cell
    uint8_t cc = 0, cbgv = 0, cbold = 0;                                   // current colour / background / bold
    for (uint32_t i = s; i < e && n < cap - 1; ) {
        char c = (char)body[i];
        if (c == '<') {
            int tc; uint32_t tp;
            if (sel_tag_match(body, i, e, "table", &tc, &tp) && !tc) {      // nested table -> flatten (no styling)
                if (!last_space && n < cap - 1) { out[n]=' '; ocol[n]=cc; obg[n]=cbgv; obold[n]=cbold; olink[n]=(uint8_t)cur_link; n++; }
                i = sel_flatten_nested(sx, body, i, e, out, ocol, obg, obold, olink, &n, cap, 1);   // break_rows=1 + attr arrays: the nested table renders one row per line, each nested cell keeping its OWN inline styling + links
                if (n < cap - 1) { out[n]=' '; ocol[n]=0; obg[n]=0; obold[n]=0; olink[n]=0; n++; }
                last_space = 1; continue;
            }
            uint32_t m = i + 1; int close = 0;
            if (m < e && body[m] == '/') { close = 1; m++; }
            char nm[12]; int nl = 0;
            while (m < e && nl < 11) { char t = (char)body[m];
                if ((t>='a'&&t<='z')||(t>='A'&&t<='Z')||(t>='0'&&t<='9')) { nm[nl++]=(t>='A'&&t<='Z')?t+32:t; m++; } else break; }
            nm[nl] = '\0';
            uint32_t te2 = m; while (te2 < e && body[te2] != '>') te2++;
            if (sel_streq(nm, "a")) {                                       // <a href>/</a>: register + track a hyperlink so cell links are clickable + blue
                if (close) cur_link = 0;
                else if (sx->num_links < SEL_MAX_LINKS) {
                    char href[192]; extract_href(body, m, te2, href);
                    char abs[192]; selene_resolve(sx, href, abs);
                    if (abs[0]) { strncpy(sx->links[sx->num_links].url, abs, 191); sx->links[sx->num_links].url[191] = '\0';
                        cur_link = sx->num_links + 1; sx->num_links++; }
                }
            }
            if (is_block_tag(nm)) {                                         // <br> or a block boundary (open OR close) -> a hard line break in the multi-line cell, marked by a non-rendering sentinel (0x01)
                if (n > 0 && out[n-1] != 0x01 && n < cap - 1) { out[n]=0x01; ocol[n]=0; obg[n]=0; obold[n]=0; olink[n]=0; n++; last_space = 1; }   // suppress a leading break + collapse runs
            }
            if (close) {
                if (sd > 0 && sel_streq(stk[sd-1].tag, nm)) { sd--; cc = sd?stk[sd-1].col:0; cbgv = sd?stk[sd-1].bg:0; cbold = sd?stk[sd-1].bold:0; }
            } else if (nl > 0) {
                uint8_t ncol = cc, nbg = cbgv, nbold = cbold; int set = 0;
                char stylev[128]; extract_attr(body, m, te2, "style", stylev, sizeof(stylev));
                char cval[40] = {0}, bval[40] = {0}; uint32_t rgb;
                if (stylev[0]) { sel_css_get(stylev, "color", cval, sizeof(cval));
                    if (!sel_css_get(stylev, "background-color", bval, sizeof(bval))) sel_css_get(stylev, "background", bval, sizeof(bval)); }
                if (!cval[0] && sel_streq(nm,"font")) extract_attr(body, m, te2, "color",   cval, sizeof(cval));
                if (!bval[0] && sel_streq(nm,"font")) extract_attr(body, m, te2, "bgcolor", bval, sizeof(bval));
                if (cval[0] && sel_parse_css_color(cval, &rgb)) { uint8_t x = sel_intern_color(sx, rgb); if (x) { ncol = x; set = 1; } }
                if (bval[0] && sel_parse_css_color(bval, &rgb)) { uint8_t x = sel_intern_color(sx, rgb); if (x) { nbg  = x; set = 1; } }
                if (sel_streq(nm,"b") || sel_streq(nm,"strong")) { nbold = 1; set = 1; }
                if (sel_streq(nm,"mark")) { uint8_t hy = sel_intern_color(sx, 0xFFFF00), bk = sel_intern_color(sx, 0x000000); if (hy) { nbg = hy; set = 1; } if (bk) { ncol = bk; set = 1; } }
                if (sel_streq(nm,"code")||sel_streq(nm,"kbd")||sel_streq(nm,"samp")||sel_streq(nm,"tt")) { uint8_t g = sel_intern_color(sx, 0xE6E6E6); if (g) { nbg = g; set = 1; } }
                int voidtag = sel_streq(nm,"br")||sel_streq(nm,"img")||sel_streq(nm,"input")||sel_streq(nm,"hr")||sel_streq(nm,"meta")||sel_streq(nm,"wbr");
                if (set && !voidtag && sd < 10) {
                    strncpy(stk[sd].tag, nm, 11); stk[sd].tag[11]='\0';
                    stk[sd].col = ncol; stk[sd].bg = nbg; stk[sd].bold = nbold; sd++;
                    cc = ncol; cbgv = nbg; cbold = nbold;
                }
            }
            i = (te2 < e) ? te2 + 1 : e;
            continue;
        }
        if (c == '&') { char eb[8]; uint32_t el, adv;
            if (decode_entity(body + i, e - i, eb, sizeof(eb), &el, &adv)) {
                for (uint32_t k = 0; k < el && n < cap - 1; k++) { char d = eb[k];
                    if (d == ' ') { if (!last_space) { out[n]=' '; ocol[n]=cc; obg[n]=cbgv; obold[n]=cbold; olink[n]=(uint8_t)cur_link; n++; last_space = 1; } }
                    else { if (upper && d>='a'&&d<='z') d -= 32; out[n]=d; ocol[n]=cc; obg[n]=cbgv; obold[n]=cbold; olink[n]=(uint8_t)cur_link; n++; last_space = 0; } }
                i += adv; continue; }
            out[n]='&'; ocol[n]=cc; obg[n]=cbgv; obold[n]=cbold; olink[n]=(uint8_t)cur_link; n++; last_space = 0; i++; continue; }
        if (c==' '||c=='\t'||c=='\n'||c=='\r') { if (!last_space) { out[n]=' '; ocol[n]=cc; obg[n]=cbgv; obold[n]=cbold; olink[n]=(uint8_t)cur_link; n++; last_space = 1; } i++; continue; }
        if (upper && c>='a'&&c<='z') c -= 32;
        out[n]=c; ocol[n]=cc; obg[n]=cbgv; obold[n]=cbold; olink[n]=(uint8_t)cur_link; n++; last_space = 0; i++;
    }
    while (n > 0 && (out[n-1] == ' ' || out[n-1] == 0x01)) n--;   // trim trailing spaces + a dangling hard-break sentinel
    out[n] = '\0';
    return n;
}

// Does the cell body[s..e) contain a bordered <div> (a <div style="border..."> whose border is not
// none / 0)? If so return 1 and set *colout to its border-colour palette index (0 = grey default),
// resolved exactly like the top-level box in the main loop (explicit border-color, else the colour token
// in the border shorthand). Used to draw an outline around a panel that lives inside a table cell.
static int sel_cell_bordered_div(selene_ctx_t* sx, const uint8_t* body, uint32_t s, uint32_t e, uint8_t* colout) {
    *colout = 0;
    for (uint32_t i = s; i + 4 < e; i++) {
        if (body[i] != '<') continue;
        char a = (char)body[i+1], b = (char)body[i+2], c = (char)body[i+3];
        if (!((a=='d'||a=='D') && (b=='i'||b=='I') && (c=='v'||c=='V'))) continue;   // "<div"
        uint32_t te2 = i + 4; while (te2 < e && body[te2] != '>') te2++;
        char st[160] = {0}; extract_attr(body, i + 4, te2, "style", st, sizeof(st));
        if (!st[0]) continue;
        char bv[48] = {0};
        if (!(sel_css_get(st, "border", bv, sizeof(bv)) || sel_css_get(st, "border-width", bv, sizeof(bv)) ||
              sel_css_get(st, "border-top", bv, sizeof(bv)) || sel_css_get(st, "border-style", bv, sizeof(bv)))) continue;
        if (sel_ci_streq(bv, "none") || bv[0] == '0') continue;                       // border explicitly off
        char bc[40] = {0}; uint32_t brgb;                                              // resolve the stroke colour
        if (sel_css_get(st, "border-color", bc, sizeof(bc))) { if (sel_parse_css_color(bc, &brgb)) *colout = sel_intern_color(sx, brgb); }
        else for (int z = 0; bv[z]; ) { while (bv[z] == ' ') z++; int e3 = z; while (bv[e3] && bv[e3] != ' ') e3++;
            char tok[40]; int tl = 0; for (int q = z; q < e3 && tl < 39; q++) tok[tl++] = bv[q]; tok[tl] = '\0';
            if (tl && sel_parse_css_color(tok, &brgb)) { *colout = sel_intern_color(sx, brgb); break; }
            z = e3; if (!bv[z]) break; }
        return 1;
    }
    return 0;
}

// A subtle "panel" background tint derived from the page background: a touch lighter on a dark page, a
// touch darker on a light one (so a bordered card reads as a filled panel like a real repo page, without
// hurting text contrast). Interned into the page palette; the 24 low bits of a palette entry are RGB
// regardless of framebuffer bpp (fb_rgb packs r<<16|g<<8|b), so the page bg's channels read back directly.
// The panel-tint PIXEL for a given page-background pixel: a touch lighter on a dark page, a touch darker on
// a light one (luminance test). Pure (no palette mutation), so the draw loop can call it directly on the
// resolved page background to fill a top-level box, matching the interned index render_table uses for the
// in-cell band (both start from the same page-bg pixel).
static uint32_t sel_tint_px(uint32_t bgpx) {
    int r = (bgpx >> 16) & 0xFF, g = (bgpx >> 8) & 0xFF, b = bgpx & 0xFF;
    int lum = (r * 30 + g * 59 + b * 11) / 100;
    int d = lum < 128 ? 12 : -10;   // dark page -> panel a touch lighter; light page -> a touch darker
    r += d; g += d; b += d;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}
static uint8_t sel_panel_tint(selene_ctx_t* sx) {
    uint32_t bgpx = (sx->page_bg && sx->page_bg <= sx->npalette) ? sx->palette[sx->page_bg - 1] : fb_rgb(248, 248, 250);
    return sel_intern_color(sx, sel_tint_px(bgpx) & 0xFFFFFF);   // low 24 bits = 0xRRGGBB for sel_intern_color
}

// Parse the table in body[ts..te) and emit it, aligned, into the text stream at *pti. has_border=1
// draws the boxed +--+ rules and | separators; has_border=0 (border="0"/style border:none) lays the
// same aligned columns out spaced apart, with no rules or pipes. cellpad is the HTML cellpadding
// (spaces of horizontal breathing room inside each cell, per side; default 1 keeps the old layout).
typedef struct {
    int used, mcell, cs, spanw, nlines;
    int lbrk[SEL_CELL_MAXLINES], llen[SEL_CELL_MAXLINES];
    uint8_t align, th, bg, boxed, boxcol;
    int boxidx;
} sel_table_wrap_t;
typedef struct { int a, b; uint8_t bg; } sel_table_segment_t;
typedef struct { int c, a, b; } sel_table_box_t;
typedef struct {
    int colw[SEL_TBL_MAXCOLS];
    uint8_t col_align[SEL_TBL_MAXCOLS], col_bg[SEL_TBL_MAXCOLS];
    char rule[SEL_LINE_COLS], capline[SEL_LINE_COLS], line[SEL_LINE_COLS];
    uint8_t lcol[SEL_LINE_COLS], lbg[SEL_LINE_COLS], lbld[SEL_LINE_COLS], llnk[SEL_LINE_COLS];
    sel_table_wrap_t wr[SEL_TBL_MAXCOLS];
    sel_table_segment_t seg[SEL_TBL_MAXCOLS + 1];
    sel_table_box_t lboxes[SEL_TBL_MAXCOLS];
} sel_table_scratch_t;

static void render_table(selene_ctx_t* sx, const uint8_t* body, uint32_t ts, uint32_t te,
                         char* txt, uint8_t* tlink, uint8_t* tfield, uint8_t* tbgcol, uint8_t* tcolor, uint8_t* tbold, uint32_t* pti, uint32_t cap,
                         int has_border, int cellpad) {
    int pad = cellpad < 0 ? 0 : (cellpad > 8 ? 8 : cellpad);   // cellpadding: spaces INSIDE each cell, per side (default 1 == unchanged)
    sel_tcell_t* cells = (sel_tcell_t*)kmalloc(SEL_TBL_MAXCELLS * sizeof(sel_tcell_t));
    if (!cells) return;
    uint8_t* occ = (uint8_t*)kmalloc(SEL_TBL_MAXROWS * SEL_TBL_MAXCOLS);   // grid: 1 = covered by a span
    if (!occ) { kfree(cells); return; }
    sel_table_scratch_t* scratch = (sel_table_scratch_t*)kmalloc(sizeof(*scratch));
    if (!scratch) { kfree(cells); kfree(occ); return; }
    for (int z = 0; z < SEL_TBL_MAXROWS * SEL_TBL_MAXCOLS; z++) occ[z] = 0;
    int* colw = scratch->colw; for (int c = 0; c < SEL_TBL_MAXCOLS; c++) colw[c] = 0;
    uint8_t* col_align = scratch->col_align; for (int c = 0; c < SEL_TBL_MAXCOLS; c++) col_align[c] = 0;  // <col>/<colgroup align> per-column default
    uint8_t* col_bg = scratch->col_bg; for (int c = 0; c < SEL_TBL_MAXCOLS; c++) col_bg[c] = 0;  // <col>/<colgroup bgcolor> per-column default background
    int ncols = 0, nrows = 0, ncells = 0, has_header = 0, row = -1, curcol = 0, cur_row_align = 0, cur_row_bg = 0, colidx = 0;

    // Pass 1 — collect cells (row, col, col/row span, th?, byte range). colspan/rowspan cells reserve
    // their footprint in the occupancy grid so later cells skip past covered columns and stay aligned.
    for (uint32_t i = ts; i < te && ncells < SEL_TBL_MAXCELLS; ) {
        if (body[i] != '<') { i++; continue; }
        int close; uint32_t past;
        if (sel_tag_match(body, i, te, "tr", &close, &past)) {
            if (!close && row < SEL_TBL_MAXROWS - 1) { row++; if (row + 1 > nrows) nrows = row + 1; curcol = 0;
                cur_row_align = 0;                                        // <tr align> / style text-align: a row-wide default
                char rav[16] = {0}; extract_attr(body, i, past, "align", rav, sizeof(rav));
                if (!rav[0]) { char rst[64] = {0}; extract_attr(body, i, past, "style", rst, sizeof(rst));
                    if (rst[0]) sel_css_get(rst, "text-align", rav, sizeof(rav)); }
                if      (sel_ci_streq(rav, "center")) cur_row_align = 1;
                else if (sel_ci_streq(rav, "right"))  cur_row_align = 2;
                cur_row_bg = 0;                                           // <tr bgcolor> / style background: a row-wide cell background
                { char rbg[40] = {0}; extract_attr(body, i, past, "bgcolor", rbg, sizeof(rbg));
                  char rst2[80] = {0}; extract_attr(body, i, past, "style", rst2, sizeof(rst2));
                  if (!rbg[0] && rst2[0]) { if (!sel_css_get(rst2, "background-color", rbg, sizeof(rbg))) sel_css_get(rst2, "background", rbg, sizeof(rbg)); }
                  uint32_t rr = 0; if (rbg[0] && sel_parse_css_color(rbg, &rr)) cur_row_bg = sel_intern_color(sx, rr); }
            }
            i = past; continue;
        }
        int isth = 0, isopen = 0; uint32_t cpast = i;
        if (sel_tag_match(body, i, te, "td", &close, &past) && !close) { isopen = 1; isth = 0; cpast = past; }
        else if (sel_tag_match(body, i, te, "th", &close, &past) && !close) { isopen = 1; isth = 1; cpast = past; }
        if (isopen) {
            if (row < 0) { row = 0; nrows = 1; curcol = 0; }         // cells before an explicit <tr>
            int cspan = sel_span_attr(body, i, cpast, "colspan"); if (cspan < 1) cspan = 1; if (cspan > SEL_TBL_MAXCOLS) cspan = SEL_TBL_MAXCOLS;
            int rspan = sel_span_attr(body, i, cpast, "rowspan"); if (rspan < 1) rspan = 1; if (rspan > SEL_TBL_MAXROWS) rspan = SEL_TBL_MAXROWS;
            uint32_t k = cpast;                                       // find the cell's end (next cell/row tag)
            while (k < te) {
                if (body[k] == '<') { int c3; uint32_t p3;
                    if (sel_tag_match(body,k,te,"table",&c3,&p3) && !c3) {   // nested table: skip it whole,
                        int d = 1; uint32_t m = p3;                          // stay inside this cell (its
                        while (m < te && d > 0) {                            // rows must not enter the grid)
                            if (body[m] == '<') { int c5; uint32_t p5;
                                if (sel_tag_match(body,m,te,"table",&c5,&p5)) { if (c5) d--; else d++; m = p5; continue; } }
                            m++;
                        }
                        k = m; continue;
                    }
                    if (sel_tag_match(body,k,te,"td",&c3,&p3) || sel_tag_match(body,k,te,"th",&c3,&p3) ||
                        sel_tag_match(body,k,te,"tr",&c3,&p3)) break; }
                k++;
            }
            while (curcol < SEL_TBL_MAXCOLS && occ[row * SEL_TBL_MAXCOLS + curcol]) curcol++;   // skip rowspan-covered cols
            if (curcol < SEL_TBL_MAXCOLS) {
                if (curcol + cspan > SEL_TBL_MAXCOLS) cspan = SEL_TBL_MAXCOLS - curcol;
                for (int rr = 0; rr < rspan && row + rr < SEL_TBL_MAXROWS; rr++)     // reserve the footprint
                    for (int cc = 0; cc < cspan; cc++) occ[(row + rr) * SEL_TBL_MAXCOLS + (curcol + cc)] = 1;
                char cb[SEL_TBL_ROWCAP + 2];
                int w = sel_cell_text(body, cpast, k, cb, sizeof(cb), 0);
                if (cspan == 1) { if (w > SEL_TBL_COLCAP) w = SEL_TBL_COLCAP; if (w > colw[curcol]) colw[curcol] = w; }
                uint8_t calign = col_align[curcol];                       // column default (<col>/<colgroup align>)
                if (cur_row_align) calign = (uint8_t)cur_row_align;        // an explicit <tr align> overrides the column; a cell align= overrides both
                { char av[16] = {0}; extract_attr(body, i, cpast, "align", av, sizeof(av));
                  if (!av[0]) { char cst[64] = {0}; extract_attr(body, i, cpast, "style", cst, sizeof(cst));
                      if (cst[0]) sel_css_get(cst, "text-align", av, sizeof(av)); }
                  if      (sel_ci_streq(av, "center")) calign = 1;
                  else if (sel_ci_streq(av, "right"))  calign = 2;
                  else if (sel_ci_streq(av, "left"))   calign = 0; }
                uint8_t cbg = col_bg[curcol];                            // column default (<col>/<colgroup bgcolor>)
                if (cur_row_bg) cbg = (uint8_t)cur_row_bg;                // an explicit <tr bgcolor> overrides the column; a cell bgcolor= / style overrides both
                { char cbv[40] = {0}; extract_attr(body, i, cpast, "bgcolor", cbv, sizeof(cbv));
                  char cbs[80] = {0}; extract_attr(body, i, cpast, "style", cbs, sizeof(cbs));
                  if (!cbv[0] && cbs[0]) { if (!sel_css_get(cbs, "background-color", cbv, sizeof(cbv))) sel_css_get(cbs, "background", cbv, sizeof(cbv)); }
                  uint32_t cr = 0; if (cbv[0] && sel_parse_css_color(cbv, &cr)) { uint8_t x = sel_intern_color(sx, cr); if (x) cbg = x; } }
                cells[ncells].s = cpast; cells[ncells].e = k;
                cells[ncells].row = (uint16_t)row; cells[ncells].col = (uint16_t)curcol;
                cells[ncells].th = (uint8_t)isth; cells[ncells].cspan = (uint8_t)cspan; cells[ncells].rspan = (uint8_t)rspan;
                cells[ncells].align = calign; cells[ncells].bg = cbg;
                ncells++;
                if (isth) has_header = 1;
                if (curcol + cspan > ncols) ncols = curcol + cspan;
                if (row + rspan > nrows) nrows = row + rspan;
                curcol += cspan;
            }
            i = k; continue;
        }
        {   // <col align span> (positional) / <colgroup align span>: a per-column default alignment cells inherit
            int is_cg = 0, is_col = 0;
            if (sel_tag_match(body, i, te, "colgroup", &close, &past) && !close) is_cg = 1;   // colgroup first (col is its prefix)
            else if (sel_tag_match(body, i, te, "col", &close, &past) && !close) is_col = 1;
            if (is_cg || is_col) {
                char cav[16] = {0}; extract_attr(body, i, past, "align", cav, sizeof(cav));
                char cbgv[40] = {0}; extract_attr(body, i, past, "bgcolor", cbgv, sizeof(cbgv));
                { char cstyl[80] = {0}; extract_attr(body, i, past, "style", cstyl, sizeof(cstyl));
                  if (cstyl[0]) { if (!cav[0]) sel_css_get(cstyl, "text-align", cav, sizeof(cav));
                      if (!cbgv[0]) { if (!sel_css_get(cstyl, "background-color", cbgv, sizeof(cbgv))) sel_css_get(cstyl, "background", cbgv, sizeof(cbgv)); } } }
                if (is_cg && !cav[0] && !cbgv[0]) { i = past; continue; }        // a plain <colgroup> container: its <col> children fill
                uint8_t ca = sel_ci_streq(cav, "center") ? 1 : (sel_ci_streq(cav, "right") ? 2 : 0);
                uint8_t cbg = 0; uint32_t crgb = 0; if (cbgv[0] && sel_parse_css_color(cbgv, &crgb)) cbg = sel_intern_color(sx, crgb);   // <col bgcolor> column default
                int sp = sel_span_attr(body, i, past, "span"); if (sp < 1) sp = 1; if (sp > SEL_TBL_MAXCOLS) sp = SEL_TBL_MAXCOLS;
                for (int q = 0; q < sp && colidx < SEL_TBL_MAXCOLS; q++) { col_align[colidx] = ca; col_bg[colidx] = cbg; colidx++; }
                i = past; continue;
            }
        }
        { uint32_t k = i + 1; while (k < te && body[k] != '>') k++; if (k < te) k++; i = k; }   // skip other tag
    }
    if (ncols == 0 || nrows == 0) { kfree(cells); kfree(occ); kfree(scratch); return; }
    if (ncols > SEL_TBL_MAXCOLS) ncols = SEL_TBL_MAXCOLS;
    if (nrows > SEL_TBL_MAXROWS) nrows = SEL_TBL_MAXROWS;
    for (int c = 0; c < ncols; c++) if (colw[c] < 1) colw[c] = 1;      // empty columns still get a slot

    // Grow the spanned columns of each colspan cell until its content fits across them.
    for (int m = 0; m < ncells; m++) {
        int cs = cells[m].cspan; if (cs <= 1) continue;
        int c0 = cells[m].col; if (c0 + cs > ncols) cs = ncols - c0; if (cs <= 1) continue;
        char cb[SEL_TBL_ROWCAP + 2];
        int need = sel_cell_text(body, cells[m].s, cells[m].e, cb, sizeof(cb), cells[m].th);
        if (need > SEL_TBL_ROWCAP) need = SEL_TBL_ROWCAP;
        int span = (2 * pad + 1) * (cs - 1); for (int k = 0; k < cs; k++) span += colw[c0 + k];
        while (span < need) {
            int bumped = 0;
            for (int k = 0; k < cs && span < need; k++) if (colw[c0 + k] < SEL_TBL_COLCAP) { colw[c0 + k]++; span++; bumped = 1; }
            if (!bumped) break;
        }
    }

    int total = 1; for (int c = 0; c < ncols; c++) total += colw[c] + 2 * pad + 1;   // "|" + per col (pad + x + pad + "|")
    while (total > SEL_TBL_ROWCAP) {                                    // shrink widest column until it fits
        int mx = -1, mi = 0; for (int c = 0; c < ncols; c++) if (colw[c] > mx) { mx = colw[c]; mi = c; }
        if (mx <= 3) break;
        colw[mi]--; total--;
    }

    // Build a horizontal rule "+----+---+" once (reused for top / header sep / bottom).
    char* rule = scratch->rule; { int p = 0; rule[p++] = '+';
        for (int c = 0; c < ncols && p < SEL_LINE_COLS - 2; c++) {
            for (int z = 0; z < colw[c] + 2 * pad && p < SEL_LINE_COLS - 2; z++) rule[p++] = '-';
            rule[p++] = '+'; } rule[p] = '\0'; }

    *pti = sel_ensure_nl(txt, tlink, *pti, cap, 2);
    // <caption>: render its text centred over the table. By default it sits above the top rule (HTML
    // caption-side:top); style="caption-side:bottom" on the <caption> moves it below the bottom rule.
    // It is scanned here rather than consumed as a cell, so a data table can carry a real title. A
    // caption belonging to a nested table is skipped by tracking <table> nesting depth; only THIS
    // table's top-level caption is used. The centred line is built once into capline and emitted at
    // whichever end caption-side selects.
    char* capline = scratch->capline; capline[0] = 0; int cap_ready = 0, cap_bottom = 0;
    {
        uint32_t ctag = 0, cs = 0, ce = 0; int have_cap = 0, tdepth = 0;
        for (uint32_t i = ts; i < te && !have_cap; ) {
            if (body[i] != '<') { i++; continue; }
            int cl; uint32_t p;
            if (sel_tag_match(body, i, te, "table", &cl, &p)) { if (cl) { if (tdepth > 0) tdepth--; } else tdepth++; i = p; continue; }
            if (tdepth == 0 && sel_tag_match(body, i, te, "caption", &cl, &p) && !cl) {
                ctag = i; cs = p; uint32_t k = p;                        // tag is [ctag,cs); caption text starts at cs
                while (k < te) { int c2; uint32_t p2;
                    if (body[k] == '<' && sel_tag_match(body, k, te, "caption", &c2, &p2) && c2) break;   // </caption>
                    k++; }
                ce = k; have_cap = 1; break;
            }
            { uint32_t k = i + 1; while (k < te && body[k] != '>') k++; if (k < te) k++; i = k; }   // skip other tag
        }
        if (have_cap) {
            char cstyle[80] = {0}, csv[24] = {0}, calign[16] = {0};      // caption-side + text-align live on the <caption> tag
            extract_attr(body, ctag, cs, "style", cstyle, sizeof(cstyle));
            if (cstyle[0]) {
                if (sel_css_get(cstyle, "caption-side", csv, sizeof(csv)) && sel_ci_streq(csv, "bottom")) cap_bottom = 1;
                sel_css_get(cstyle, "text-align", calign, sizeof(calign));   // left/right override the centred default
            }
            char capbuf[SEL_LINE_COLS];
            int w = sel_cell_text(body, cs, ce, capbuf, SEL_LINE_COLS, 0);
            if (w > 0) {
                int pad2;                                                 // horizontal placement of the caption over the table width
                if      (sel_ci_streq(calign, "left"))  pad2 = 0;         // flush left
                else if (sel_ci_streq(calign, "right")) pad2 = total - w; // flush right
                else                                    pad2 = (total - w) / 2;   // centre (default / "center")
                if (pad2 < 0) pad2 = 0;
                int q = 0; for (; q < pad2 && q < SEL_LINE_COLS - 1; q++) capline[q] = ' ';
                for (int z = 0; capbuf[z] && q < SEL_LINE_COLS - 1; z++) capline[q++] = capbuf[z];
                capline[q] = '\0'; cap_ready = 1;
            }
        }
    }
    if (cap_ready && !cap_bottom) {                                       // caption-side:top (default) -- above the box
        sel_emit(txt, tlink, tfield, pti, cap, capline, 0); sel_emit(txt, tlink, tfield, pti, cap, "\n", 0);
    }
    char bch = has_border ? '|' : ' ';                                   // column separator: pipe when boxed, space when borderless
    if (has_border) { sel_emit(txt, tlink, tfield, pti, cap, rule, 0); sel_emit(txt, tlink, tfield, pti, cap, "\n", 0); }
    // Multi-line cells: extract each cell's FULL styled text, word-wrap it to the column width, and emit as
    // many stacked text lines as the tallest cell in the row needs (H). Per-column pools hold each cell's
    // text + attrs (a cell starting at column c uses slice c); wr[] holds the wrap result. On OOM, degrade
    // like the cells/occ allocation above (render nothing).
    char*    pt = (char*)   kmalloc(SEL_TBL_MAXCOLS * SEL_CELL_CAP);
    uint8_t* pc = (uint8_t*)kmalloc(SEL_TBL_MAXCOLS * SEL_CELL_CAP);
    uint8_t* pb = (uint8_t*)kmalloc(SEL_TBL_MAXCOLS * SEL_CELL_CAP);
    uint8_t* pd = (uint8_t*)kmalloc(SEL_TBL_MAXCOLS * SEL_CELL_CAP);
    uint8_t* pl = (uint8_t*)kmalloc(SEL_TBL_MAXCOLS * SEL_CELL_CAP);
    if (!pt || !pc || !pb || !pd || !pl) { if (pt) kfree(pt); if (pc) kfree(pc); if (pb) kfree(pb); if (pd) kfree(pd); if (pl) kfree(pl); kfree(cells); kfree(occ); kfree(scratch); return; }
    sel_table_wrap_t* wr = scratch->wr;
    int tint = -1;                                            // subtle panel-fill tint, interned lazily only if a boxed cell without its own bg needs it (so a table with none stays byte-identical)
    for (int r = 0; r < nrows; r++) {
        // ---- Phase A: extract + word-wrap each cell of this row; H = tallest cell's line count ----
        int H = 1;
        for (int cc = 0; cc < ncols; cc++) wr[cc].used = 0;
        { int c = 0;
          while (c < ncols) {
            int found = -1;
            for (int m = 0; m < ncells; m++) if (cells[m].row == r && cells[m].col == c) { found = m; break; }
            if (found < 0) { c += 1; continue; }                        // empty / rowspan-covered: handled per line in Phase B
            int cs = cells[found].cspan; if (c + cs > ncols) cs = ncols - c;
            int spanw = (2 * pad + 1) * (cs - 1); for (int k = 0; k < cs; k++) spanw += colw[c + k];
            char* ct = pt + c * SEL_CELL_CAP; uint8_t* xc = pc + c * SEL_CELL_CAP, *xb = pb + c * SEL_CELL_CAP, *xd = pd + c * SEL_CELL_CAP, *xl = pl + c * SEL_CELL_CAP;
            int w = sel_cell_text_styled(sx, body, cells[found].s, cells[found].e, ct, xc, xb, xd, xl, SEL_CELL_CAP, cells[found].th);
            /* A <th> header cell renders BOLD by default, like a browser (in addition to the
               existing upper-casing) — xd is the per-char bold array (bit0). Only <th> cells set
               it, so <td> cells and non-table text are unaffected. A cell whose own inline style
               already bolds a span just keeps bit0 set (idempotent). */
            if (cells[found].th) for (int b = 0; b < w; b++) xd[b] |= 1;
            int nl = 0, i = 0;
            while (i < w && nl < SEL_CELL_MAXLINES) {                    // word-wrap ct[0..w) to width spanw, breaking HARD at a 0x01 sentinel (<br>/block boundary)
                int start = i, lastsp = -1, j = i;
                while (j < w && ct[j] != 0x01 && j - start < spanw) { if (ct[j] == ' ') lastsp = j; j++; }
                int end;
                if (j < w && ct[j] == 0x01)  end = j;                    // forced break at the sentinel
                else if (j >= w)             end = w;                    // rest fits
                else                         end = (lastsp > start) ? lastsp : j;   // soft word-wrap (last space, else hard-break a long word)
                wr[c].lbrk[nl] = start; wr[c].llen[nl] = end - start; nl++;
                i = end;
                if (i < w && ct[i] == 0x01) i++;                         // consume the forced-break sentinel
                else while (i < w && ct[i] == ' ') i++;                  // else swallow the soft-break space(s)
            }
            if (nl == 0) { wr[c].lbrk[0] = 0; wr[c].llen[0] = 0; nl = 1; }   // empty cell = one blank line
            wr[c].used = 1; wr[c].mcell = found; wr[c].cs = cs; wr[c].spanw = spanw; wr[c].nlines = nl;
            wr[c].align = cells[found].align; wr[c].th = cells[found].th; wr[c].bg = cells[found].bg;
            wr[c].boxed = (uint8_t)sel_cell_bordered_div(sx, body, cells[found].s, cells[found].e, &wr[c].boxcol);   // cell content is a bordered <div> -> draw an outline around this cell block
            wr[c].boxidx = -1;
            if (nl > H) H = nl;
            c += cs;
          }
        }
        // ---- Phase B: emit H stacked text lines; a cell shows its line h (or blanks past its last line) ----
        for (int h = 0; h < H; h++) {
            char* line = scratch->line; int p = 0; line[p++] = bch;
            uint8_t* lcol = scratch->lcol, *lbg = scratch->lbg, *lbld = scratch->lbld, *llnk = scratch->llnk;
            for (int z = 0; z < SEL_LINE_COLS; z++) { lcol[z] = 0; lbg[z] = 0; lbld[z] = 0; llnk[z] = 0; }
            sel_table_segment_t* seg = scratch->seg; int nseg = 0;
            sel_table_box_t* lboxes = scratch->lboxes; int nlbox = 0;   // boxed cells emitted on this line (col span), for the in-cell outline
            int c = 0;
            while (c < ncols && p < SEL_LINE_COLS - 2) {
                if (wr[c].used) {                                        // a cell starts here — draw its line h across its cspan
                    int seg_a = p, spanw = wr[c].spanw;
                    int off = 0, tw = 0;
                    if (h < wr[c].nlines) { off = wr[c].lbrk[h]; tw = wr[c].llen[h]; }   // else the cell has run out of lines -> blank band
                    if (tw > spanw) tw = spanw;
                    int fill = spanw - tw;                                              // padding to distribute for alignment
                    int lead = wr[c].align == 2 ? fill : (wr[c].align == 1 ? fill / 2 : 0);  // right=all before, center=half
                    char* ct = pt + c * SEL_CELL_CAP; uint8_t* xc = pc + c * SEL_CELL_CAP, *xb = pb + c * SEL_CELL_CAP, *xd = pd + c * SEL_CELL_CAP, *xl = pl + c * SEL_CELL_CAP;
                    for (int pp = 0; pp < pad && p < SEL_LINE_COLS - 2; pp++) line[p++] = ' ';   // left cellpadding
                    for (int q = 0; q < lead && p < SEL_LINE_COLS - 2; q++) line[p++] = ' ';    // leading pad (right / center)
                    for (int z = 0; z < tw && p < SEL_LINE_COLS - 2; z++) { lcol[p] = xc[off + z]; lbg[p] = xb[off + z]; lbld[p] = xd[off + z]; llnk[p] = xl[off + z]; line[p++] = ct[off + z]; }   // this line's cell text + styling
                    for (int q = 0; q < fill - lead && p < SEL_LINE_COLS - 2; q++) line[p++] = ' ';  // trailing pad
                    for (int pp = 0; pp < pad && p < SEL_LINE_COLS - 2; pp++) line[p++] = ' ';   // right cellpadding
                    uint8_t cellbg = wr[c].bg;               // a boxed cell (a bordered-<div> panel) with no explicit bg gets the subtle panel tint so it reads as a filled card
                    if (!cellbg && wr[c].boxed) { if (tint < 0) tint = sel_panel_tint(sx); cellbg = (uint8_t)tint; }
                    if (cellbg && nseg <= SEL_TBL_MAXCOLS) { seg[nseg].a = seg_a; seg[nseg].b = p; seg[nseg].bg = cellbg; nseg++; }
                    if (wr[c].boxed && nlbox < SEL_TBL_MAXCOLS) { lboxes[nlbox].c = c; lboxes[nlbox].a = seg_a; lboxes[nlbox].b = p; nlbox++; }   // this boxed cell's column span on this line
                    line[p++] = bch;
                    c += wr[c].cs;
                } else {                                                // empty, or covered by a span — blank column
                    for (int pp = 0; pp < pad && p < SEL_LINE_COLS - 2; pp++) line[p++] = ' ';   // left cellpadding
                    for (int z = 0; z < colw[c] && p < SEL_LINE_COLS - 2; z++) line[p++] = ' ';
                    for (int pp = 0; pp < pad && p < SEL_LINE_COLS - 2; pp++) line[p++] = ' ';   // right cellpadding
                    line[p++] = bch;
                    c += 1;
                }
            }
            line[p] = '\0';
            uint32_t rbase = *pti;                                          // txt[] index where this line starts (sel_emit writes 1:1)
            sel_emit(txt, tlink, tfield, pti, cap, line, 0);
            for (int b = 0; b < nlbox; b++) {                               // in-cell box outline: open at this cell's first line (h==0), close at its last (h==H-1). txt = line start (matches wrap's line_start[])
                int cc2 = lboxes[b].c;
                if (h == 0 && wr[cc2].boxidx < 0 && sx->num_cell_boxes < SEL_MAX_CELL_BOXES) {
                    int bi = sx->num_cell_boxes++;
                    sx->cell_boxes[bi].txt0 = rbase; sx->cell_boxes[bi].txt1 = rbase;   // txt1 provisional; updated at h==H-1
                    sx->cell_boxes[bi].col0 = (uint8_t)lboxes[b].a; sx->cell_boxes[bi].col1 = (uint8_t)(lboxes[b].b - 1);
                    sx->cell_boxes[bi].col = wr[cc2].boxcol; sx->cell_boxes[bi].used = 1;
                    wr[cc2].boxidx = bi;
                }
                if (h == H - 1 && wr[cc2].boxidx >= 0) sx->cell_boxes[wr[cc2].boxidx].txt1 = rbase;
            }
            for (int m2 = 0; m2 < nseg; m2++)                               // paint each coloured cell band into tbgcol
                for (int q = seg[m2].a; q < seg[m2].b && rbase + (uint32_t)q < cap; q++) tbgcol[rbase + (uint32_t)q] = seg[m2].bg;
            for (int q = 0; q < p && rbase + (uint32_t)q < cap; q++) {       // paint the cell text's own inline styling (overrides the cell-level band where present)
                if (lcol[q]) tcolor[rbase + (uint32_t)q] = lcol[q];
                if (lbg[q])  tbgcol[rbase + (uint32_t)q] = lbg[q];
                if (lbld[q]) tbold[rbase + (uint32_t)q] |= lbld[q];
                if (llnk[q]) tlink[rbase + (uint32_t)q]  = llnk[q];          // a link inside the cell -> clickable + blue via the draw link overlay
            }
            sel_emit(txt, tlink, tfield, pti, cap, "\n", 0);
            if (r == 0 && has_header && has_border && h == H - 1) { sel_emit(txt, tlink, tfield, pti, cap, rule, 0); sel_emit(txt, tlink, tfield, pti, cap, "\n", 0); }   // header rule after the header row's last line
        }
    }
    kfree(pt); kfree(pc); kfree(pb); kfree(pd); kfree(pl);
    if (has_border) sel_emit(txt, tlink, tfield, pti, cap, rule, 0);
    if (cap_ready && cap_bottom) {                                        // caption-side:bottom -- below the box
        sel_emit(txt, tlink, tfield, pti, cap, "\n", 0);
        sel_emit(txt, tlink, tfield, pti, cap, capline, 0);
    }
    *pti = sel_ensure_nl(txt, tlink, *pti, cap, 2);
    kfree(cells); kfree(occ); kfree(scratch);
}

// Free a decoded <img>'s pixels: an animated GIF owns its frames array (px only ALIASES the current
// frame, so it is NOT freed separately); a static image owns its single RGBA buffer via px. Resets
// the image to the un-fetched state so it can be reused.
static void selene_img_free(sel_img_t* im) {
    if (im->frames) {
        for (int f = 0; f < im->nframes; f++) if (im->frames[f].pixels) kfree(im->frames[f].pixels);
        kfree(im->frames);
        im->frames = 0; im->nframes = 0; im->cur_frame = 0; im->anim_ms = 0;
    } else if (im->px) {
        kfree(im->px);
    }
    im->px = 0; im->iw = 0; im->ih = 0;
}

// ---- strip HTML in `body` to text (capturing <a href> links + form controls), then wrap it ----
// ---- inline CSS: parse a style="color:.." value and intern it into the page palette ----

static int sel_ci_eqn(const char* a, const char* b, int n) {           // case-insensitive, n chars
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}
static int sel_ci_streq(const char* a, const char* b) {                 // case-insensitive, full string
    int i = 0;
    for (; a[i] && b[i]; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return a[i] == 0 && b[i] == 0;
}

// Find a declaration `prop: value` among the `;`-separated declarations of an inline style string,
// matching the property name exactly (so "color" does not also match "background-color"). 1 on success.
static int sel_css_get(const char* style, const char* prop, char* out, int cap) {
    int plen = (int)strlen(prop);
    const char* p = style;
    out[0] = '\0';
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        const char* ds = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }       // no ':' in this decl
        const char* pe = p; while (pe > ds && pe[-1] == ' ') pe--;      // trim property name
        p++;
        while (*p == ' ') p++;
        const char* vs = p;
        while (*p && *p != ';') p++;
        const char* ve = p; while (ve > vs && ve[-1] == ' ') ve--;      // trim value
        if ((int)(pe - ds) == plen && sel_ci_eqn(ds, prop, plen)) {
            int vl = (int)(ve - vs); if (vl > cap - 1) vl = cap - 1; if (vl < 0) vl = 0;
            for (int k = 0; k < vl; k++) out[k] = vs[k];
            out[vl] = '\0';
            return out[0] ? 1 : 0;
        }
    }
    return 0;
}

static int sel_hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a CSS text-indent length into whole character cells (one cell = FONT_WIDTH = 8px). Supports
// "Npx" (or a unitless number, treated as px) and "Nem"/"Nrem" (1em ~= the 16px font = 2 cells). A
// fractional part is ignored and a negative indent clamps to 0 (Selene never hangs text left of the margin).
static int sel_parse_indent(const char* v) {
    while (*v == ' ') v++;
    if (*v == '-') return 0;
    int n = 0; while (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; }
    if (*v == '.') { v++; while (*v >= '0' && *v <= '9') v++; }   // ignore any fractional part
    while (*v == ' ') v++;
    int cells;
    if      (v[0] == 'e' && v[1] == 'm')                 cells = n * 2;   // em  -> 2 cells (16px)
    else if (v[0] == 'r' && v[1] == 'e' && v[2] == 'm')  cells = n * 2;   // rem -> 2 cells
    else                                                 cells = (n + 4) / 8;   // px (or unitless): round to nearest cell
    if (cells > 40) cells = 40;                          // never exceed the wrap width
    return cells;
}

// Parse a CSS colour value (#rgb, #rrggbb, or a common named colour) into 0xRRGGBB. 1 on success.
static int sel_parse_css_color(const char* v, uint32_t* rgb) {
    while (*v == ' ') v++;
    if (v[0] == '#') {
        const char* h = v + 1;
        int n = 0; while (n < 8 && sel_hexv(h[n]) >= 0) n++;
        if (n == 6) {
            *rgb = ((uint32_t)sel_hexv(h[0])<<20)|((uint32_t)sel_hexv(h[1])<<16)|
                   ((uint32_t)sel_hexv(h[2])<<12)|((uint32_t)sel_hexv(h[3])<<8) |
                   ((uint32_t)sel_hexv(h[4])<<4) | (uint32_t)sel_hexv(h[5]);
            return 1;
        }
        if (n == 3) {                                                   // #abc -> #aabbcc
            int r = sel_hexv(h[0]), g = sel_hexv(h[1]), b = sel_hexv(h[2]);
            *rgb = ((uint32_t)((r<<4)|r)<<16)|((uint32_t)((g<<4)|g)<<8)|(uint32_t)((b<<4)|b);
            return 1;
        }
        return 0;
    }
    static const struct { const char* n; uint32_t rgb; } NAMED[] = {
        {"black",0x000000},{"white",0xFFFFFF},{"red",0xFF0000},{"green",0x008000},
        {"lime",0x00FF00},{"blue",0x0000FF},{"yellow",0xFFFF00},{"cyan",0x00FFFF},
        {"aqua",0x00FFFF},{"magenta",0xFF00FF},{"fuchsia",0xFF00FF},{"gray",0x808080},
        {"grey",0x808080},{"silver",0xC0C0C0},{"maroon",0x800000},{"olive",0x808000},
        {"navy",0x000080},{"teal",0x008080},{"purple",0x800080},{"orange",0xFFA500},
        {"pink",0xFFC0CB},{"brown",0xA52A2A},{"gold",0xFFD700},{"indigo",0x4B0082},
        {"darkred",0x8B0000},{"darkgreen",0x006400},{"darkblue",0x00008B},{"crimson",0xDC143C},
        // greys / neutrals
        {"lightgray",0xD3D3D3},{"lightgrey",0xD3D3D3},{"darkgray",0xA9A9A9},{"darkgrey",0xA9A9A9},
        {"dimgray",0x696969},{"dimgrey",0x696969},{"slategray",0x708090},{"slategrey",0x708090},
        {"lightslategray",0x778899},{"gainsboro",0xDCDCDC},{"whitesmoke",0xF5F5F5},{"snow",0xFFFAFA},
        // blues
        {"lightblue",0xADD8E6},{"skyblue",0x87CEEB},{"lightskyblue",0x87CEFA},{"deepskyblue",0x00BFFF},
        {"dodgerblue",0x1E90FF},{"cornflowerblue",0x6495ED},{"steelblue",0x4682B4},{"royalblue",0x4169E1},
        {"midnightblue",0x191970},{"mediumblue",0x0000CD},{"powderblue",0xB0E0E6},{"cadetblue",0x5F9EA0},
        // greens
        {"lightgreen",0x90EE90},{"palegreen",0x98FB98},{"springgreen",0x00FF7F},{"seagreen",0x2E8B57},
        {"mediumseagreen",0x3CB371},{"forestgreen",0x228B22},{"limegreen",0x32CD32},{"yellowgreen",0x9ACD32},
        {"greenyellow",0xADFF2F},{"chartreuse",0x7FFF00},{"olivedrab",0x6B8E23},{"darkseagreen",0x8FBC8F},
        // reds / oranges / pinks
        {"tomato",0xFF6347},{"orangered",0xFF4500},{"darkorange",0xFF8C00},{"coral",0xFF7F50},
        {"salmon",0xFA8072},{"lightsalmon",0xFFA07A},{"indianred",0xCD5C5C},{"firebrick",0xB22222},
        {"hotpink",0xFF69B4},{"deeppink",0xFF1493},{"lightpink",0xFFB6C1},{"palevioletred",0xDB7093},
        // yellows / browns / tans
        {"khaki",0xF0E68C},{"darkkhaki",0xBDB76B},{"tan",0xD2B48C},{"wheat",0xF5DEB3},
        {"beige",0xF5F5DC},{"ivory",0xFFFFF0},{"chocolate",0xD2691E},{"sienna",0xA0522D},
        {"peru",0xCD853F},{"sandybrown",0xF4A460},{"goldenrod",0xDAA520},{"saddlebrown",0x8B4513},
        // purples / violets
        {"violet",0xEE82EE},{"orchid",0xDA70D6},{"plum",0xDDA0DD},{"thistle",0xD8BFD8},
        {"darkviolet",0x9400D3},{"darkorchid",0x9932CC},{"darkmagenta",0x8B008B},{"mediumpurple",0x9370DB},
        {"blueviolet",0x8A2BE2},{"slateblue",0x6A5ACD},{"mediumslateblue",0x7B68EE},{"lavender",0xE6E6FA},
        {"rebeccapurple",0x663399},
        // cyans / teals
        {"turquoise",0x40E0D0},{"mediumturquoise",0x48D1CC},{"darkturquoise",0x00CED1},{"lightcyan",0xE0FFFF},
        {"aquamarine",0x7FFFD4},{"darkcyan",0x008B8B},{"lightseagreen",0x20B2AA},{"paleturquoise",0xAFEEEE},
        {0,0}
    };
    for (int i = 0; NAMED[i].n; i++) if (sel_ci_streq(v, NAMED[i].n)) { *rgb = NAMED[i].rgb; return 1; }
    return 0;
}

// Intern an 0xRRGGBB colour into the page palette (stored as fb pixels, deduped). Returns a 1-based
// index (0 = no room); index 0 is reserved for "the default text colour".
static uint8_t sel_intern_color(selene_ctx_t* s, uint32_t rgb) {
    uint32_t px = fb_rgb((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    for (int i = 0; i < s->npalette; i++) if (s->palette[i] == px) return (uint8_t)(i + 1);
    if (s->npalette >= 255) return 0;
    s->palette[s->npalette] = px;
    return (uint8_t)(++s->npalette);
}

// Format an <ol> counter as base-26 letters (1->a, 26->z, 27->aa). out must hold >=8 bytes.
static int sel_fmt_alpha(uint16_t n, char* out, int upper) {
    char tmp[8]; int t = 0;
    if (n == 0) { out[0] = '?'; out[1] = '\0'; return 1; }
    while (n > 0 && t < 6) { uint16_t r = (uint16_t)((n - 1) % 26); tmp[t++] = (char)((upper ? 'A' : 'a') + r); n = (uint16_t)((n - 1) / 26); }
    int o = 0; while (t > 0) out[o++] = tmp[--t];   // reverse into most-significant-first order
    out[o] = '\0'; return o;
}
// Format an <ol> counter as a Roman numeral (1..3999). out must hold >=16 bytes.
static int sel_fmt_roman(uint16_t n, char* out, int upper) {
    static const uint16_t val[13] = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
    static const char* const sym[13] = {"m","cm","d","cd","c","xc","l","xl","x","ix","v","iv","i"};
    int o = 0;
    if (n == 0 || n > 3999) { out[0] = '?'; out[1] = '\0'; return 1; }
    for (int k = 0; k < 13 && n > 0; k++)
        while (n >= val[k] && o < 15) { for (const char* sp = sym[k]; *sp && o < 15; sp++) out[o++] = (char)(upper ? *sp - 32 : *sp); n = (uint16_t)(n - val[k]); }
    out[o] = '\0'; return o;
}
// Is a boolean (valueless) attribute `name` present in the tag span body[s..e)? Word-bounded and
// case-insensitive, so <ol reversed> matches but class="reversed-x" does not. Used for <ol reversed>.
static int sel_attr_present(const uint8_t* body, uint32_t s, uint32_t e, const char* name) {
    uint32_t nl = 0; while (name[nl]) nl++;
    for (uint32_t i = s; i + nl <= e; i++) {
        if (i > s) { char pv = (char)body[i - 1]; if (!(pv==' '||pv=='\t'||pv=='\n'||pv=='\r')) continue; }
        uint32_t k = 0;
        for (; k < nl; k++) { char a = (char)body[i + k]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32); if (a != name[k]) break; }
        if (k != nl) continue;
        char nx = (i + nl < e) ? (char)body[i + nl] : ' ';
        if (nx==' '||nx=='\t'||nx=='\n'||nx=='\r'||nx=='>'||nx=='/'||nx=='=') return 1;
    }
    return 0;
}
// Count the top-level <li> of the list being entered: scan body[from..len) tracking <ol>/<ul>
// nesting (depth 1 = this list) until its matching close, counting <li> only at depth 1. For
// <ol reversed> the first item's number equals this count. Capped at 9999.
static int sel_count_li(const uint8_t* body, uint32_t from, uint32_t len) {
    int depth = 1, count = 0;
    for (uint32_t i = from; i < len && depth > 0; ) {
        if (body[i] != '<') { i++; continue; }
        uint32_t k = i + 1; int close = 0;
        if (k < len && body[k] == '/') { close = 1; k++; }
        char nm[4]; int nl = 0;
        while (k < len && nl < 3) { char ch = (char)body[k];
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) break;
            nm[nl++] = ch; k++; }
        nm[nl] = '\0';
        if ((nm[0]=='o'||nm[0]=='u') && nm[1]=='l' && nm[2]=='\0') { if (close) depth--; else depth++; }
        else if (!close && nm[0]=='l' && nm[1]=='i' && nm[2]=='\0' && depth == 1 && count < 9999) count++;
        i = k;
    }
    return count;
}
// Void (self-closing) HTML elements: they have no end tag, so display:none must NOT skip-to-close
// on them (there is nothing to skip to -- it would swallow the rest of the page).
static int sel_is_void_tag(const char* n) {
    static const char* const V[] = {"img","input","br","hr","meta","link","area","base",
                                     "col","embed","source","track","wbr","param",0};
    for (int i = 0; V[i]; i++) if (sel_streq(n, V[i])) return 1;
    return 0;
}

static void render_html(selene_ctx_t* s, const uint8_t* body, uint32_t len) {
    s->num_lines = 0; s->scroll = 0; s->title[0] = '\0';
    s->num_links = 0; s->sel_link = -1;
    s->num_fields = 0; s->num_forms = 0; s->sel_field = -1;
    s->npalette = 0;                                          // reset the per-page inline-CSS colour palette
    s->page_bg = 0; s->page_fg = 0;                           // reset <body> page background / text colour
    for (int i = 0; i < s->num_imgs; i++) selene_img_free(&s->images[i]);   // free decoded pixels/frames on nav
    s->num_imgs = 0;
    __builtin_memset(s->link_of, 0, SEL_MAX_LINES * SEL_LINE_COLS);
    __builtin_memset(s->field_of, 0, SEL_MAX_LINES * SEL_LINE_COLS);
    __builtin_memset(s->img_of, 0, SEL_MAX_LINES * SEL_LINE_COLS);
    __builtin_memset(s->color_of, 0, SEL_MAX_LINES * SEL_LINE_COLS);
    __builtin_memset(s->bgcolor_of, 0, SEL_MAX_LINES * SEL_LINE_COLS);
    __builtin_memset(s->bold_of, 0, SEL_MAX_LINES * SEL_LINE_COLS);
    __builtin_memset(s->line_align, 0, SEL_MAX_LINES);
    __builtin_memset(s->line_rule, 0, SEL_MAX_LINES);
    s->num_cell_boxes = 0;                                   // in-cell bordered-<div> outlines, collected by render_table this render
    if (!body || !len) { s->num_lines = 0; return; }
    char*    txt    = (char*)kmalloc(len + 1);
    uint8_t* tlink  = (uint8_t*)kmalloc(len + 1);
    uint8_t* tfield = (uint8_t*)kmalloc(len + 1);
    uint8_t* timg   = (uint8_t*)kmalloc(len + 1);
    uint8_t* tcolor = (uint8_t*)kmalloc(len + 1);            // per-char inline-CSS colour index (for wrap_text)
    uint8_t* tbgcol = (uint8_t*)kmalloc(len + 1);            // per-char inline-CSS background index (for wrap_text)
    uint8_t* tbold  = (uint8_t*)kmalloc(len + 1);            // per-char bold flag (for wrap_text)
    uint8_t* talign = (uint8_t*)kmalloc(len + 1);            // per-char text-align (for wrap_text -> line_align)
    uint8_t* trule  = (uint8_t*)kmalloc(len + 1);            // per-char <hr> flag (for wrap_text -> line_rule)
    uint8_t* tindent= (uint8_t*)kmalloc(len + 1);            // per-char <blockquote> nesting level (for wrap_text)
    struct { char tag[16]; uint8_t color, bg, bold, ul, st, al, du, vo, tt, ol, nw; } *colstk = kmalloc(16 * sizeof(*colstk));
    if (!txt || !tlink || !tfield || !timg || !tcolor || !tbgcol || !tbold || !talign || !trule || !tindent || !colstk) {
        if (txt) kfree(txt);
        if (tlink) kfree(tlink);
        if (tfield) kfree(tfield);
        if (timg) kfree(timg);
        if (tcolor) kfree(tcolor);
        if (tbgcol) kfree(tbgcol);
        if (tbold) kfree(tbold);
        if (talign) kfree(talign);
        if (trule) kfree(trule);
        if (tindent) kfree(tindent);
        if (colstk) kfree(colstk);
        return;
    }
    __builtin_memset(tfield,  0, len + 1);
    __builtin_memset(timg,    0, len + 1);
    __builtin_memset(tcolor,  0, len + 1);
    __builtin_memset(tbgcol,  0, len + 1);
    __builtin_memset(tbold,   0, len + 1);
    __builtin_memset(talign,  0, len + 1);
    __builtin_memset(trule,   0, len + 1);
    __builtin_memset(tindent, 0, len + 1);
    uint32_t* line_start = (uint32_t*)kmalloc(SEL_MAX_LINES * sizeof(uint32_t));   // optional txt-idx -> line map for in-cell box resolution (NULL = skip boxes)
    if (line_start) __builtin_memset(line_start, 0xFF, SEL_MAX_LINES * sizeof(uint32_t));
    uint32_t ti = 0;
    int last_space = 1;
    int cur_link = 0;                                         // link id in progress (0 = none)
    int cur_field = 0;                                        // field id in progress (<button> label text)
    int btn_open = 0;                                         // inside a <button>: guards the closing pill padding cap
    int cur_form = -1;                                        // the <form> currently open (-1 = none)
    int cur_hd = 0;                                           // inside an <h1>/<h2> (upper-case its text)
    int pre_mode = 0;                                         // inside <pre> (preserve whitespace literally)
    int pre_skip_nl = 0;                                      // swallow one newline right after <pre> (like browsers)
    int quote_depth = 0;                                      // <blockquote> nesting (left margin level)
    int qmark_depth = 0;                                      // <q> nesting: level 0 uses ", level 1 uses ', alternating
    struct { uint8_t ordered; uint16_t counter; uint8_t type; uint8_t rev; } liststk[SEL_LIST_MAXDEPTH];  // <ul>/<ol> nesting (+ list-style-type, +<ol reversed>)
    int listdepth = 0;                                        // 0 = not in a list
    struct { uint8_t bordered, col; } boxstk[SEL_BOX_MAXDEPTH]; int boxsp = 0;   // bordered-<div> stack: each open <div> pushes {has a CSS border, its border-colour palette idx} so the matching </div> closes + colours the box
    int cur_color = 0, cur_bg = 0, cur_bold = 0, cur_ul = 0, cur_st = 0, cur_align = 0, cur_du = 0, cur_vo = 0, cur_tt = 0, cur_ol = 0, cur_nowrap = 0;  // +text-align, +dotted-underline (<abbr>), +vert-offset (<sub>/<sup>), +text-transform, +overline, +white-space:nowrap
    int coldepth = 0;
    for (uint32_t i = 0; i < len && ti < len; ) {
        char c = (char)body[i];
        if (c == '<') {
            uint32_t j = i + 1;
            int close = 0;
            if (j < len && body[j] == '/') { close = 1; j++; }
            char name[16]; int nl = 0;                        // 16 so 10-char tags (blockquote/figcaption) fit
            while (j < len && nl < 15) {
                char t = (char)body[j];
                if ((t>='a'&&t<='z')||(t>='A'&&t<='Z')||(t>='0'&&t<='9')) { name[nl++] = (t>='A'&&t<='Z')?t+32:t; j++; }
                else break;
            }
            name[nl] = '\0';
            if (!close && (sel_streq(name,"script") || sel_streq(name,"style"))) {
                const char* end = sel_streq(name,"script") ? "</script" : "</style";
                uint32_t k = j;
                while (k < len && !(body[k]=='<' && ci_starts(body+k, len-k, end))) k++;
                i = k;
                while (i < len && body[i] != '>') i++;
                if (i < len) i++;
                continue;
            }
            if (!close && sel_streq(name,"title")) {
                uint32_t k = j; while (k < len && body[k] != '>') k++; if (k < len) k++;
                int tl = 0;
                while (k < len && body[k] != '<' && tl < 95) {
                    if (body[k] == '&') {                          // decode HTML entities in the title, like the body flow
                        char eb[8]; uint32_t el = 0, adv = 0;      // (was copied raw, so a title like "Q&amp;A" showed "&amp;" in the tab + status bar)
                        if (decode_entity(body + k, len - k, eb, sizeof(eb), &el, &adv)) {
                            for (uint32_t z = 0; z < el && tl < 95; z++) {
                                char t = eb[z]; if (t=='\n'||t=='\r'||t=='\t') t = ' ';
                                s->title[tl++] = t;
                            }
                            k += adv; continue;
                        }
                    }
                    char t = (char)body[k]; if (t=='\n'||t=='\r'||t=='\t') t = ' ';
                    s->title[tl++] = t; k++;
                }
                s->title[tl] = '\0';
                i = k;
                continue;
            }
            if (!close && sel_streq(name,"body")) {            // <body bgcolor=/text=> or style background/color -> page background & default text colour
                uint32_t bce = j; while (bce < len && body[bce] != '>') bce++;
                char bb[40] = {0}, bt[40] = {0}, bsty[120] = {0}; uint32_t brgb;
                extract_attr(body, j, bce, "bgcolor", bb, sizeof(bb));
                extract_attr(body, j, bce, "text",    bt, sizeof(bt));
                extract_attr(body, j, bce, "style",   bsty, sizeof(bsty));
                if (!bb[0] && bsty[0]) { if (!sel_css_get(bsty, "background-color", bb, sizeof(bb))) sel_css_get(bsty, "background", bb, sizeof(bb)); }
                if (!bt[0] && bsty[0]) sel_css_get(bsty, "color", bt, sizeof(bt));
                if (bb[0] && sel_parse_css_color(bb, &brgb)) { uint8_t x = sel_intern_color(s, brgb); if (x) s->page_bg = x; }
                if (bt[0] && sel_parse_css_color(bt, &brgb)) { uint8_t x = sel_intern_color(s, brgb); if (x) s->page_fg = x; }
                if (bce < len) bce++;
                i = bce;
                continue;
            }
            // display:none / hidden / visibility:hidden -> skip the element's entire subtree, like
            // <script>/<style>. Only for a non-void container with a real close tag: a void element
            // (img/input/br/hr/...) has none, so scanning to a missing close would swallow the page.
            if (!close && nl > 0 && !sel_is_void_tag(name)) {
                uint32_t hte = j; while (hte < len && body[hte] != '>') hte++;
                int self_close = (hte > j && body[hte - 1] == '/');
                int hide = sel_attr_present(body, j, hte, "hidden");
                if (!hide) {
                    char hsty[96] = {0}, dv[24] = {0};
                    extract_attr(body, j, hte, "style", hsty, sizeof(hsty));
                    if (hsty[0]) {
                        if (sel_css_get(hsty, "display", dv, sizeof(dv)) && sel_ci_streq(dv, "none")) hide = 1;
                        else { dv[0] = '\0'; if (sel_css_get(hsty, "visibility", dv, sizeof(dv)) && sel_ci_streq(dv, "hidden")) hide = 1; }
                    }
                }
                if (hide && !self_close) {
                    uint32_t k = (hte < len) ? hte + 1 : len;    // scan from just after this open tag's '>'
                    int nest = 1;
                    while (k < len && nest > 0) {
                        if (body[k] != '<') { k++; continue; }
                        uint32_t m = k + 1; int kclose = 0;
                        if (m < len && body[m] == '/') { kclose = 1; m++; }
                        char kn[16]; int knl = 0;
                        while (m < len && knl < 15) { char t = (char)body[m];
                            if ((t>='a'&&t<='z')||(t>='A'&&t<='Z')||(t>='0'&&t<='9')) { kn[knl++] = (t>='A'&&t<='Z')?t+32:t; m++; } else break; }
                        kn[knl] = '\0';
                        if (sel_streq(kn, name)) {
                            if (kclose) nest--;
                            else { uint32_t e2 = m; while (e2 < len && body[e2] != '>') e2++; if (!(e2 > m && body[e2-1] == '/')) nest++; }
                        }
                        k = m;
                    }
                    while (k < len && body[k] != '>') k++;        // step past the matching close tag's '>'
                    i = (k < len) ? k + 1 : len;
                    continue;
                }
            }
            // <textarea ...>...</textarea>: render as a multi-line editable field box (like a text
            // <input>, but `rows` tall) and DROP the element's default text content, which otherwise
            // has no <textarea> arm and dumps straight into the page flow as plain text with no field
            // affordance. Handled here, ahead of the inline-CSS emphasis stack, so a styled
            // <textarea style=..> cannot leave an unbalanced push when we skip past its close tag.
            if (!close && sel_streq(name, "textarea")) {
                uint32_t te = j; while (te < len && body[te] != '>') te++;   // end of the open tag
                char rowsv[8] = {0}, nm[64] = {0};
                extract_attr(body, j, te, "rows", rowsv, sizeof(rowsv));
                extract_attr(body, j, te, "name", nm,   sizeof(nm));
                int trows = 2;                                     // box height in rows; default 2, clamp [1,5]
                if (rowsv[0]) { int v = 0; for (const char* q = rowsv; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
                                trows = v < 1 ? 1 : (v > 5 ? 5 : v); }
                int fid = 0;
                if (s->num_fields < SEL_MAX_FIELDS) {              // register a text field: draws as a box, focusable
                    sel_field_t* f = &s->fields[s->num_fields];
                    f->form = cur_form; f->kind = SEL_FLD_TEXT;
                    strncpy(f->name, nm, sizeof(f->name) - 1); f->name[sizeof(f->name) - 1] = '\0';
                    f->value[0] = '\0';                            // starts empty (default content not editable-tracked)
                    fid = s->num_fields + 1; s->num_fields++;
                }
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);        // a block element: start on its own line
                for (int r = 0; r < trows; r++) {                  // `trows` stacked box strips sharing one field id
                    for (int d = 0; d < SEL_FIELD_W && ti < len; d++) { txt[ti] = '_'; tlink[ti] = 0; tfield[ti] = (uint8_t)fid; ti++; }
                    if (ti < len) { txt[ti] = '\n'; tlink[ti] = 0; tfield[ti] = 0; ti++; }
                }
                uint32_t k = (te < len) ? te + 1 : len;            // skip the content up to and past </textarea>
                while (k < len) {
                    if (body[k] == '<') { int c5; uint32_t p5;
                        if (sel_tag_match(body, k, len, "textarea", &c5, &p5)) { k = p5; if (c5) break; continue; } }
                    k++;
                }
                last_space = 1;
                i = k; continue;
            }
            // Inline CSS: a styled open tag (style="color:..") — or <font color=..> — pushes its
            // foreground colour; the matching close pops it. Void elements never push (no close tag).
            // This runs for EVERY tag before the specific handlers below, so any element can be coloured.
            {
                uint32_t cte = j; while (cte < len && body[cte] != '>') cte++;
                if (close) {
                    if (coldepth > 0 && sel_streq(colstk[coldepth - 1].tag, name)) {
                        coldepth--;
                        cur_color = coldepth > 0 ? colstk[coldepth - 1].color : 0;
                        cur_bg    = coldepth > 0 ? colstk[coldepth - 1].bg    : 0;
                        cur_bold  = coldepth > 0 ? colstk[coldepth - 1].bold  : 0;
                        cur_ul    = coldepth > 0 ? colstk[coldepth - 1].ul    : 0;
                        cur_st    = coldepth > 0 ? colstk[coldepth - 1].st    : 0;
                        cur_ol    = coldepth > 0 ? colstk[coldepth - 1].ol    : 0;
                        cur_align = coldepth > 0 ? colstk[coldepth - 1].al    : 0;
                        cur_du    = coldepth > 0 ? colstk[coldepth - 1].du    : 0;
                        cur_vo    = coldepth > 0 ? colstk[coldepth - 1].vo    : 0;
                        cur_tt    = coldepth > 0 ? colstk[coldepth - 1].tt    : 0;
                        cur_nowrap= coldepth > 0 ? colstk[coldepth - 1].nw    : 0;
                    }
                } else if (!(sel_streq(name,"br")||sel_streq(name,"hr")||sel_streq(name,"img")||
                             sel_streq(name,"input")||sel_streq(name,"meta")||sel_streq(name,"link"))) {
                    char stylev[160]; extract_attr(body, j, cte, "style", stylev, sizeof(stylev));
                    char cval[40] = {0}, bval[40] = {0}, wval[24] = {0}, dval[24] = {0}, aval[16] = {0}, tval[20] = {0}, wsval[16] = {0}, sval[20] = {0}, vaval[16] = {0};
                    if (stylev[0]) {
                        sel_css_get(stylev, "color", cval, sizeof(cval));
                        if (!sel_css_get(stylev, "background-color", bval, sizeof(bval)))
                            sel_css_get(stylev, "background", bval, sizeof(bval));   // shorthand: read its colour token
                        sel_css_get(stylev, "font-weight", wval, sizeof(wval));
                        sel_css_get(stylev, "text-decoration", dval, sizeof(dval));
                        sel_css_get(stylev, "text-align", aval, sizeof(aval));
                        sel_css_get(stylev, "text-transform", tval, sizeof(tval));
                        sel_css_get(stylev, "white-space", wsval, sizeof(wsval));
                        sel_css_get(stylev, "font-style", sval, sizeof(sval));
                        sel_css_get(stylev, "vertical-align", vaval, sizeof(vaval));
                    }
                    if (!cval[0] && sel_streq(name, "font")) extract_attr(body, j, cte, "color",   cval, sizeof(cval));
                    if (!bval[0] && sel_streq(name, "font")) extract_attr(body, j, cte, "bgcolor", bval, sizeof(bval));
                    if (!aval[0]) extract_attr(body, j, cte, "align", aval, sizeof(aval));   // legacy presentational align= on a block (<p>/<div>/<h1..h6>); CSS text-align above takes precedence
                    uint32_t rgb;
                    uint8_t nfg = (uint8_t)cur_color, nbg = (uint8_t)cur_bg, nbold = (uint8_t)cur_bold, nul = (uint8_t)cur_ul, nst = (uint8_t)cur_st, nal = (uint8_t)cur_align, ndu = (uint8_t)cur_du, nvo = (uint8_t)cur_vo, ntt = (uint8_t)cur_tt, nol = (uint8_t)cur_ol, nnw = (uint8_t)cur_nowrap;  // inherit unless overridden
                    int set = 0;
                    if (cval[0] && sel_parse_css_color(cval, &rgb)) { uint8_t x = sel_intern_color(s, rgb); if (x) { nfg = x; set = 1; } }
                    if (bval[0] && sel_parse_css_color(bval, &rgb)) { uint8_t x = sel_intern_color(s, rgb); if (x) { nbg = x; set = 1; } }
                    if (sel_streq(name,"b") || sel_streq(name,"strong")) { nbold = 1; set = 1; }   // <b>/<strong> = bold
                    if (sel_streq(name,"legend")) { nbold = 1; set = 1; }   // <fieldset>'s <legend> caption = bold (its group title), via the same style stack (pops on </legend>)
                    if (wval[0]) {                                                                // font-weight: bold-ish vs normal
                        if (sel_ci_streq(wval,"bold")||sel_ci_streq(wval,"bolder")||sel_ci_streq(wval,"600")||
                            sel_ci_streq(wval,"700")||sel_ci_streq(wval,"800")||sel_ci_streq(wval,"900")) { nbold = 1; set = 1; }
                        else if (sel_ci_streq(wval,"normal")||sel_ci_streq(wval,"lighter")||sel_ci_streq(wval,"100")||
                                 sel_ci_streq(wval,"200")||sel_ci_streq(wval,"300")||sel_ci_streq(wval,"400")||
                                 sel_ci_streq(wval,"500")) { nbold = 0; set = 1; }
                    }
                    if (sel_streq(name,"u")) { nul = 1; set = 1; }                                   // <u> = underline
                    if (sel_streq(name,"s") || sel_streq(name,"strike") || sel_streq(name,"del")) { nst = 1; set = 1; }  // = line-through
                    if (dval[0]) {                                                                    // text-decoration
                        if (sel_ci_streq(dval,"underline"))         { nul = 1; set = 1; }
                        else if (sel_ci_streq(dval,"line-through")) { nst = 1; set = 1; }
                        else if (sel_ci_streq(dval,"overline"))     { nol = 1; set = 1; }
                        else if (sel_ci_streq(dval,"none"))         { nul = 0; nst = 0; nol = 0; set = 1; }
                    }
                    // Semantic inline tags mapped onto the existing colour/decoration machinery.
                    if (sel_streq(name,"mark")) {                          // <mark> = black text on a yellow highlight
                        uint8_t hy = sel_intern_color(s, 0xFFFF00), bk = sel_intern_color(s, 0x000000);
                        if (hy) { nbg = hy; set = 1; }
                        if (bk) { nfg = bk; set = 1; }
                    }
                    if (sel_streq(name,"ins")) { nul = 1; set = 1; }      // <ins> (inserted text) = underlined, the <del> counterpart
                    if (sel_streq(name,"abbr")) { ndu = 1; set = 1; }     // <abbr> = dotted underline (the title tooltip is not shown headless)
                    if (sel_streq(name,"sub")) { nvo = 1; set = 1; }      // <sub> = subscript (glyph shifted down)
                    if (sel_streq(name,"sup")) { nvo = 2; set = 1; }      // <sup> = superscript (glyph shifted up)
                    if (vaval[0]) {                                       // CSS vertical-align: the sub/super keywords map 1:1 onto the <sub>/<sup> nvo offset. Other values (baseline/middle/top/bottom/text-*/length) have no glyph-shift equivalent in a fixed-cell renderer -> left inherited.
                        if      (sel_ci_streq(vaval,"sub"))   { nvo = 1; set = 1; }
                        else if (sel_ci_streq(vaval,"super")) { nvo = 2; set = 1; }
                    }
                    if (sel_streq(name,"i") || sel_streq(name,"em") || sel_streq(name,"cite") ||
                        sel_streq(name,"var") || sel_streq(name,"dfn") || sel_streq(name,"address")) { nul = 1; set = 1; }   // italic family: no italic glyphs, so underline it (the classic text-mode italic fallback); <address> is italic too
                    if (sval[0] && (sel_ci_streq(sval,"italic") || sel_ci_streq(sval,"oblique"))) { nul = 1; set = 1; }   // CSS font-style: italic/oblique -> the SAME underline-as-italic fallback the <i>/<em> family uses. No `normal`->nul=0 branch: nul doubles as the underline flag, so clearing it could strip an inherited <u>/text-decoration:underline.
                    if (sel_streq(name,"code") || sel_streq(name,"kbd") ||
                        sel_streq(name,"samp") || sel_streq(name,"tt")) { // monospace-ish tags: a subtle grey code background
                        uint8_t cg = sel_intern_color(s, 0xE6E6E6);
                        if (cg) { nbg = cg; set = 1; }
                    }
                    if (sel_streq(name,"center")) { nal = 1; set = 1; }   // <center> = centred text
                    if (aval[0]) {                                        // text-align: center / right / left|justify
                        if (sel_ci_streq(aval,"center"))     { nal = 1; set = 1; }
                        else if (sel_ci_streq(aval,"right")) { nal = 2; set = 1; }
                        else if (sel_ci_streq(aval,"left") || sel_ci_streq(aval,"justify")) { nal = 0; set = 1; }
                    }
                    if (tval[0]) {                                        // text-transform: uppercase / lowercase / capitalize / none
                        if (sel_ci_streq(tval,"uppercase"))       { ntt = 1; set = 1; }
                        else if (sel_ci_streq(tval,"lowercase"))  { ntt = 2; set = 1; }
                        else if (sel_ci_streq(tval,"capitalize")) { ntt = 3; set = 1; }
                        else if (sel_ci_streq(tval,"none"))       { ntt = 0; set = 1; }
                    }
                    if (wsval[0]) {                                       // white-space: nowrap keeps a run on one line (spaces -> 0xFF non-breaking sentinel)
                        if (sel_ci_streq(wsval,"nowrap"))      { nnw = 1; set = 1; }
                        else if (sel_ci_streq(wsval,"normal")) { nnw = 0; set = 1; }
                    }
                    if (sel_streq(name,"nobr")) { nnw = 1; set = 1; }     // <nobr>: legacy no-wrap element (== white-space:nowrap)
                    if (set && coldepth < 16) {
                        strncpy(colstk[coldepth].tag, name, 15); colstk[coldepth].tag[15] = '\0';
                        colstk[coldepth].color = nfg; colstk[coldepth].bg = nbg; colstk[coldepth].bold = nbold;
                        colstk[coldepth].ul = nul; colstk[coldepth].st = nst; colstk[coldepth].al = nal; colstk[coldepth].du = ndu; colstk[coldepth].vo = nvo; colstk[coldepth].tt = ntt; colstk[coldepth].ol = nol; colstk[coldepth].nw = nnw; coldepth++;
                        cur_color = nfg; cur_bg = nbg; cur_bold = nbold; cur_ul = nul; cur_st = nst; cur_align = nal; cur_du = ndu; cur_vo = nvo; cur_tt = ntt; cur_ol = nol; cur_nowrap = nnw;
                    }
                }
            }
            if (sel_streq(name, "q")) {                        // <q>..</q>: inline quotation marks (nested q alternates " and ')
                char qc;
                if (!close) { qc = (qmark_depth % 2 == 0) ? '"' : '\''; qmark_depth++; }
                else        { if (qmark_depth > 0) qmark_depth--; qc = (qmark_depth % 2 == 0) ? '"' : '\''; }
                if (ti < len) { txt[ti]=qc; tlink[ti]=(uint8_t)cur_link; tfield[ti]=(uint8_t)cur_field;
                    tcolor[ti]=(uint8_t)cur_color; tbgcol[ti]=(uint8_t)cur_bg;
                    tbold[ti]=(uint8_t)(cur_bold|(cur_ul<<1)|(cur_st<<2)|(cur_du<<3)|(cur_vo<<4)|(cur_ol<<6)); talign[ti]=(uint8_t)cur_align;
                    tindent[ti]=(uint8_t)quote_depth; ti++; }
                last_space = 0;
                uint32_t te = j; while (te < len && body[te] != '>') te++;
                i = te; if (i < len) i++;
                continue;
            }
            if (sel_streq(name, "kbd")) {                      // <kbd>..</kbd>: bracket the (grey-background) key text as a keycap "[Key]"
                char kc = close ? ']' : '[';                   // the grey background is applied to the content by the style block above; the brackets stay plain (tbgcol 0)
                if (ti < len) { txt[ti]=kc; tlink[ti]=(uint8_t)cur_link; tfield[ti]=(uint8_t)cur_field;
                    tcolor[ti]=(uint8_t)cur_color; tbgcol[ti]=0;
                    tbold[ti]=(uint8_t)(cur_bold|(cur_ul<<1)|(cur_st<<2)|(cur_du<<3)|(cur_vo<<4)|(cur_ol<<6)); talign[ti]=(uint8_t)cur_align;
                    tindent[ti]=(uint8_t)quote_depth; ti++; }
                last_space = 0;
                uint32_t te = j; while (te < len && body[te] != '>') te++;
                i = te; if (i < len) i++;
                continue;
            }
            if (sel_streq(name, "a")) {                        // hyperlink open/close
                uint32_t te = j; while (te < len && body[te] != '>') te++;
                if (close) {
                    cur_link = 0;
                } else if (s->num_links < SEL_MAX_LINKS) {
                    char href[192]; extract_href(body, j, te, href);
                    char abs[192]; selene_resolve(s, href, abs);
                    if (abs[0]) {
                        strncpy(s->links[s->num_links].url, abs, 191);
                        s->links[s->num_links].url[191] = '\0';
                        cur_link = s->num_links + 1;
                        s->num_links++;
                    }
                }
                i = te; if (i < len) i++;
                continue;
            }
            if (sel_streq(name, "form")) {                     // <form> / </form>
                uint32_t te = j; while (te < len && body[te] != '>') te++;
                if (close) { cur_field = 0; cur_form = -1; }
                else if (s->num_forms < SEL_MAX_FORMS) {
                    char act[192], meth[8], abs[192];
                    extract_attr(body, j, te, "action", act, sizeof(act));
                    extract_attr(body, j, te, "method", meth, sizeof(meth));
                    selene_resolve(s, act[0] ? act : s->base_path, abs);
                    if (!abs[0]) snprintf(abs, sizeof(abs), "%s://%s%s",         // empty action = this page
                        s->base_https ? "https" : "http", s->base_host, s->base_path);
                    strncpy(s->forms[s->num_forms].action, abs, 191); s->forms[s->num_forms].action[191] = '\0';
                    s->forms[s->num_forms].method = (meth[0]=='p'||meth[0]=='P') ? 1 : 0;   // POST=1 (not sent yet)
                    cur_form = s->num_forms; s->num_forms++;
                }
                ti = sel_ensure_nl(txt, tlink, ti, len, 1); last_space = 1;
                i = te; if (i < len) i++;
                continue;
            }
            if (!close && sel_streq(name, "input") && s->num_fields < SEL_MAX_FIELDS) {   // <input ...>
                uint32_t te = j; while (te < len && body[te] != '>') te++;
                char type[16], nm[64], val[160];
                extract_attr(body, j, te, "type",  type, sizeof(type));
                extract_attr(body, j, te, "name",  nm,   sizeof(nm));
                extract_attr(body, j, te, "value", val,  sizeof(val));
                for (int z = 0; type[z]; z++) if (type[z] >= 'A' && type[z] <= 'Z') type[z] += 32;
                if (sel_streq(type,"checkbox") || sel_streq(type,"radio")) {   // render a state glyph; the `checked` attribute = filled
                    int checked = sel_attr_present(body, j, te, "checked");
                    const char* g = sel_streq(type,"radio") ? (checked ? "(*)" : "( )") : (checked ? "[x]" : "[ ]");
                    sel_emit(txt, tlink, tfield, &ti, len, " ", 0);
                    sel_emit(txt, tlink, tfield, &ti, len, g, 0);
                    sel_emit(txt, tlink, tfield, &ti, len, " ", 0);
                    last_space = 1; i = te; if (i < len) i++; continue;
                }
                int kind = SEL_FLD_TEXT;                        // default (text) if no/unknown type
                if (sel_streq(type, "submit")) kind = SEL_FLD_SUBMIT;
                else if (sel_streq(type, "hidden")) kind = SEL_FLD_HIDDEN;
                else if (sel_streq(type,"checkbox")||sel_streq(type,"radio")||sel_streq(type,"file")||
                         sel_streq(type,"image")||sel_streq(type,"button")||sel_streq(type,"reset")) kind = -1;
                if (kind >= 0) {
                    sel_field_t* f = &s->fields[s->num_fields];
                    f->form = cur_form; f->kind = (uint8_t)kind;
                    strncpy(f->name,  nm,  sizeof(f->name)-1);  f->name[sizeof(f->name)-1]  = '\0';
                    strncpy(f->value, val, sizeof(f->value)-1); f->value[sizeof(f->value)-1] = '\0';
                    int fid = s->num_fields + 1; s->num_fields++;
                    if (kind == SEL_FLD_TEXT) {
                        sel_emit(txt, tlink, tfield, &ti, len, " ", 0);
                        for (int d = 0; d < SEL_FIELD_W && ti < len; d++) { txt[ti]='_'; tlink[ti]=0; tfield[ti]=(uint8_t)fid; ti++; }
                        sel_emit(txt, tlink, tfield, &ti, len, " ", 0);
                        last_space = 1;
                    } else if (kind == SEL_FLD_SUBMIT) {
                        char lbl[44]; int b = 0; lbl[b++]='['; lbl[b++]=' ';
                        const char* t = val[0] ? val : "Submit";
                        for (int z = 0; t[z] && b < 40; z++) lbl[b++] = t[z];
                        lbl[b++]=' '; lbl[b++]=']'; lbl[b]='\0';
                        sel_emit(txt, tlink, tfield, &ti, len, " ", 0);
                        sel_emit(txt, tlink, tfield, &ti, len, lbl, fid);
                        sel_emit(txt, tlink, tfield, &ti, len, " ", 0);
                        last_space = 0;
                    }
                }
                i = te; if (i < len) i++;
                continue;
            }
            if (sel_streq(name, "button")) {                   // <button>..</button>: a filled "pill" button
                uint32_t te = j; while (te < len && body[te] != '>') te++;
                if (close) {
                    if (btn_open) {                            // right pill cap: a padding cell carrying the button's field + colours
                        if (ti < len) { txt[ti]=' '; tlink[ti]=(uint8_t)cur_link; tfield[ti]=(uint8_t)cur_field;
                            tcolor[ti]=(uint8_t)cur_color; tbgcol[ti]=(uint8_t)cur_bg; tbold[ti]=0;
                            talign[ti]=(uint8_t)cur_align; tindent[ti]=(uint8_t)quote_depth; ti++; }
                        btn_open = 0; cur_field = 0; last_space = 0;
                    }
                } else {
                    // A styled <button style="background:..;color:.."> has already had cur_bg/cur_color set by
                    // the inline-CSS block above; the submit-field draw pass reads those (via bgcolor_of/color_of
                    // at the pill's first cell) so the pill takes the button's own colours, else a default.
                    char type[16]; extract_attr(body, j, te, "type", type, sizeof(type));
                    for (int z = 0; type[z]; z++) if (type[z] >= 'A' && type[z] <= 'Z') type[z] += 32;
                    if (!sel_streq(type, "button") && !sel_streq(type, "reset") && s->num_fields < SEL_MAX_FIELDS) {   // submit-style: clickable form field
                        sel_field_t* f = &s->fields[s->num_fields];
                        f->form = cur_form; f->kind = SEL_FLD_SUBMIT; f->name[0] = '\0'; f->value[0] = '\0';
                        cur_field = s->num_fields + 1; s->num_fields++;
                    }
                    if (ti < len) { txt[ti]=' '; tlink[ti]=(uint8_t)cur_link; tfield[ti]=0; tcolor[ti]=0; tbgcol[ti]=0; tbold[ti]=0; talign[ti]=(uint8_t)cur_align; tindent[ti]=(uint8_t)quote_depth; ti++; }   // separator (no pill) before the pill
                    if (ti < len) { txt[ti]=' '; tlink[ti]=(uint8_t)cur_link; tfield[ti]=(uint8_t)cur_field; tcolor[ti]=(uint8_t)cur_color; tbgcol[ti]=(uint8_t)cur_bg; tbold[ti]=0; talign[ti]=(uint8_t)cur_align; tindent[ti]=(uint8_t)quote_depth; ti++; }   // left pill cap (padding cell, carries the button's colours)
                    btn_open = 1; last_space = 0;
                }
                i = te; if (i < len) i++;
                continue;
            }
            if (!close && sel_streq(name, "table")) {          // <table>...</table>: aligned column layout
                uint32_t inner = j; while (inner < len && body[inner] != '>') inner++; if (inner < len) inner++;
                int has_border = 1;                            // default boxed; border="0" or style="border:none|0" -> borderless
                { char tb[16] = {0}, tsty[96] = {0}, bv[24] = {0};
                  extract_attr(body, j, inner, "border", tb, sizeof(tb));
                  if (tb[0]) { int allz = 1; for (const char* q = tb; *q; q++) if (*q != '0') { allz = 0; break; } if (allz) has_border = 0; }
                  extract_attr(body, j, inner, "style", tsty, sizeof(tsty));
                  if (tsty[0] && sel_css_get(tsty, "border", bv, sizeof(bv)) && (sel_ci_streq(bv, "none") || bv[0] == '0')) has_border = 0; }
                int cellpad = 1;                               // HTML cellpadding: spaces inside each cell per side (default 1)
                { char cp[8] = {0}; extract_attr(body, j, inner, "cellpadding", cp, sizeof(cp));
                  if (cp[0]) { int v = 0; for (const char* q = cp; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0'); if (v >= 0 && v <= 8) cellpad = v; } }
                int depth = 1; uint32_t k = inner, innerEnd = len;  // match the closing </table> (nesting-aware)
                while (k < len) {
                    if (body[k] == '<') { int c4; uint32_t p4;
                        if (sel_tag_match(body, k, len, "table", &c4, &p4)) {
                            if (c4) { depth--; if (depth == 0) { innerEnd = k; k = p4; break; } }
                            else depth++;
                            k = p4; continue; } }
                    k++;
                }
                render_table(s, body, inner, innerEnd, txt, tlink, tfield, tbgcol, tcolor, tbold, &ti, len, has_border, cellpad);
                last_space = 1;
                i = k; continue;
            }
            if (!close && sel_streq(name, "img") && s->num_imgs < SEL_MAX_IMGS) {   // <img ...> placeholder
                uint32_t te = j; while (te < len && body[te] != '>') te++;
                char alt[64], src[160];
                extract_attr(body, j, te, "alt", alt, sizeof(alt));
                extract_attr(body, j, te, "src", src, sizeof(src));
                sel_img_t* im = &s->images[s->num_imgs];
                strncpy(im->alt, alt, sizeof(im->alt)-1); im->alt[sizeof(im->alt)-1] = '\0';
                strncpy(im->src, src, sizeof(im->src)-1); im->src[sizeof(im->src)-1] = '\0';
                im->px = 0; im->iw = 0; im->ih = 0; im->tried = 0;   // fetched lazily by selene_win_tick
                im->frames = 0; im->nframes = 0; im->cur_frame = 0; im->anim_ms = 0;   // static until a GIF sets these
                im->loop_count = 0; im->loops_done = 0;
                int imgid = s->num_imgs + 1; s->num_imgs++;
                // Caption text: prefer alt, else the src filename, else "image" (kept short so the box fits a line).
                const char* capsrc = alt[0] ? alt : 0;
                if (!capsrc) { int last = -1; for (int z = 0; src[z]; z++) if (src[z]=='/') last = z;
                               capsrc = src[0] ? src + last + 1 : "image"; }
                char lbl[64]; int b = 0;
                lbl[b++]='['; lbl[b++]='i'; lbl[b++]='m'; lbl[b++]='g'; lbl[b++]=':'; lbl[b++]=' ';
                int cl = 0;
                /* Cap the caption so the whole "[img: ...]" label fits within the placeholder
                   box (SEL_IMG_BOX_W cols): "[img: " (6) + "]" (1) plus a 1-col inset each side.
                   A longer alt/filename is cut with a trailing "..." so the label no longer
                   overflows the box border, and its flow-text copy no longer leaks past the
                   box's right edge (which used to show as a stray fragment above the box). */
                int capmax = SEL_IMG_BOX_W - 9; if (capmax < 3) capmax = 3;
                for (int z = 0; capsrc[z] && cl < capmax && b < (int)sizeof(lbl)-2; z++, cl++) {
                    char ch = capsrc[z]; if (ch=='\n'||ch=='\r'||ch=='\t') ch = ' '; lbl[b++] = ch;
                }
                if (capsrc[cl]) for (int e = 0; e < 3 && cl - e > 0; e++) lbl[b-1-e] = '.';   // source longer than the box: mark the cut
                if (cl == 0) { lbl[b++]='i'; lbl[b++]='m'; lbl[b++]='g'; }   // truly empty: "[img:img]"
                lbl[b++]=']'; lbl[b]='\0';
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);       // block-level: the image starts a fresh line at col 0
                for (int z = 0; lbl[z] && ti < len; z++) { txt[ti]=lbl[z]; tlink[ti]=0; tfield[ti]=0; timg[ti]=(uint8_t)imgid; ti++; }
                for (int z = 0; z < SEL_IMG_BOX_LINES && ti < len; z++) { txt[ti]='\n'; tlink[ti]=0; tfield[ti]=0; timg[ti]=0; ti++; }   // reserve the box's height
                last_space = 1;
                i = te; if (i < len) i++;
                continue;
            }
            // Block-level layout so pages read as structure, not one wall of text:
            // headings (h1/h2 also upper-cased for emphasis), list bullets, rules, and
            // paragraph breaks; br/tr/dd/dt are single line breaks.
            int hlevel = (name[0]=='h' && name[1]>='1' && name[1]<='6' && name[2]=='\0') ? name[1]-'0' : 0;
            if (hlevel) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 2);
                cur_hd = (!close && hlevel <= 2);
                if (close) ti = sel_ensure_nl(txt, tlink, ti, len, 2);
                last_space = 1;
            } else if (sel_streq(name, "hr")) {
                // Parse width (style="width:N%" or the legacy width="N"/"N%") and alignment (align= or text-align).
                uint32_t hte = j; while (hte < len && body[hte] != '>') hte++;
                char hw[24] = {0}, ha[16] = {0}, hstyle[120] = {0};
                extract_attr(body, j, hte, "width", hw, sizeof(hw));
                extract_attr(body, j, hte, "align", ha, sizeof(ha));
                extract_attr(body, j, hte, "style", hstyle, sizeof(hstyle));
                if (!hw[0] && hstyle[0]) sel_css_get(hstyle, "width", hw, sizeof(hw));
                if (!ha[0] && hstyle[0]) sel_css_get(hstyle, "text-align", ha, sizeof(ha));
                uint8_t hcol = 0;                                // rule colour index (0 = the default grey)
                if (hstyle[0]) {
                    char hc[40] = {0}; uint32_t hrgb;
                    if (!sel_css_get(hstyle, "color", hc, sizeof(hc))) sel_css_get(hstyle, "border-color", hc, sizeof(hc));
                    if (hc[0] && sel_parse_css_color(hc, &hrgb)) hcol = sel_intern_color(s, hrgb);
                }
                int hpct = 100;                                  // default = full content width
                if (hw[0]) {
                    const char* p = hw; while (*p == ' ') p++;
                    int n = 0; for (; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
                    if (*p == '%') { if (n > 0 && n <= 100) hpct = n; }                       // "N%" = percentage
                    else if (n > 0) { int av = SELENE_W - 2 * SEL_PAD; hpct = n >= av ? 100 : (n * 100 / av); if (hpct < 1) hpct = 1; }  // "N" = px -> %
                }
                int hal = 0;                                     // 0 = left (default), 1 = center, 2 = right
                if (sel_ci_streq(ha, "center")) hal = 1; else if (sel_ci_streq(ha, "right")) hal = 2;
                int hsz = 2;                                     // rule thickness in px (legacy size= or CSS height); default 2, clamped 1..8
                { char hs[16] = {0}; extract_attr(body, j, hte, "size", hs, sizeof(hs));
                  if (!hs[0] && hstyle[0]) sel_css_get(hstyle, "height", hs, sizeof(hs));
                  if (hs[0]) { const char* p = hs; while (*p == ' ') p++; int n = 0;
                    for (; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
                    if (n >= 1) hsz = n > 8 ? 8 : n; } }
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);
                if (ti < len) { txt[ti] = ' '; tlink[ti] = 0; trule[ti] = (uint8_t)hpct; talign[ti] = (uint8_t)hal; tcolor[ti] = hcol; tbgcol[ti] = (uint8_t)hsz; ti++; }  // marker: trule = width%, talign = alignment, tcolor = rule colour, tbgcol = thickness px
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);
                last_space = 1;
            } else if (sel_streq(name,"ul") || sel_streq(name,"ol")) {
                // Track list nesting for per-level indent + <ol> numbering. A top-level list gets a
                // paragraph break around it; a nested list just starts on the line under its parent <li>.
                if (!close) {
                    ti = sel_ensure_nl(txt, tlink, ti, len, listdepth == 0 ? 2 : 1);
                    if (listdepth < SEL_LIST_MAXDEPTH) {
                        int ord = sel_streq(name,"ol") ? 1 : 0;
                        // list-style-type: prefer style="list-style-type:X", else the legacy type= attribute
                        uint32_t lte = j; while (lte < len && body[lte] != '>') lte++;
                        char lstyle[80] = {0}, lst[24] = {0};
                        extract_attr(body, j, lte, "style", lstyle, sizeof(lstyle));
                        if (!(lstyle[0] && sel_css_get(lstyle, "list-style-type", lst, sizeof(lst))))
                            extract_attr(body, j, lte, "type", lst, sizeof(lst));
                        uint8_t lt = 0;
                        if (ord) {                                    // <ol>: 0 decimal, 1 lower-alpha, 2 upper-alpha, 3 lower-roman, 4 upper-roman
                            if      (sel_ci_streq(lst,"lower-alpha")||sel_ci_streq(lst,"lower-latin")||sel_streq(lst,"a")) lt = 1;
                            else if (sel_ci_streq(lst,"upper-alpha")||sel_ci_streq(lst,"upper-latin")||sel_streq(lst,"A")) lt = 2;
                            else if (sel_ci_streq(lst,"lower-roman")||sel_streq(lst,"i")) lt = 3;
                            else if (sel_ci_streq(lst,"upper-roman")||sel_streq(lst,"I")) lt = 4;
                        } else {                                      // <ul>: 0 disc, 1 circle, 2 square, 3 none
                            if      (sel_ci_streq(lst,"circle")) lt = 1;
                            else if (sel_ci_streq(lst,"square")) lt = 2;
                            else if (sel_ci_streq(lst,"none"))   lt = 3;
                        }
                        uint16_t startc = 0;                              // <ol start="N">: first item shows N (counter starts at N-1)
                        uint8_t rev = 0;                                  // <ol reversed>: number downward
                        if (ord) { char sv[8] = {0}; extract_attr(body, j, lte, "start", sv, sizeof(sv));
                            int sval = 0, has_start = 0;
                            if (sv[0]) { for (const char* q = sv; *q >= '0' && *q <= '9'; q++) sval = sval * 10 + (*q - '0');
                                if (sval >= 1 && sval <= 9999) has_start = 1; }
                            if (sel_attr_present(body, j, lte, "reversed")) {   // reversed: first item = start (if given) else the item count
                                rev = 1;
                                int first = has_start ? sval : sel_count_li(body, lte + 1, len);
                                if (first < 1) first = 1; else if (first > 9999) first = 9999;
                                startc = (uint16_t)(first + 1);            // the <li> step (--, clamped) then shows `first`
                            } else if (has_start) startc = (uint16_t)(sval - 1);   // forward: ++ then shows `sval`
                        }
                        liststk[listdepth].ordered = (uint8_t)ord;
                        liststk[listdepth].counter = startc;
                        liststk[listdepth].type = lt;
                        liststk[listdepth].rev = rev;
                        listdepth++;
                    }
                } else {
                    if (listdepth > 0) listdepth--;
                    ti = sel_ensure_nl(txt, tlink, ti, len, listdepth == 0 ? 2 : 1);
                }
                last_space = 1;
            } else if (close && sel_streq(name, "li")) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);           // end an item with ONE break (tight list,
                last_space = 1;                                       // not the want=2 is_block_tag would give)
            } else if (!close && sel_streq(name, "li")) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);
                int lvl = listdepth > 0 ? listdepth : 1;              // a stray <li> (no list) acts as depth 1
                for (int d = 0; d < (lvl - 1) * SEL_LIST_INDENT && ti < len; d++) { txt[ti]=' '; tlink[ti]=0; ti++; }
                if (listdepth > 0 && liststk[listdepth-1].ordered) {  // ordered: marker + ". " (decimal / alpha / roman)
                    uint32_t lite = j; while (lite < len && body[lite] != '>') lite++;   // <li value="N">: restart this item's number at N
                    uint8_t rev = liststk[listdepth-1].rev;
                    char liv[8] = {0}; extract_attr(body, j, lite, "value", liv, sizeof(liv));
                    if (liv[0]) { int v = 0; for (const char* q = liv; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
                        if (v >= 1 && v <= 9999) liststk[listdepth-1].counter = (uint16_t)(rev ? v + 1 : v - 1); }   // step below shows N
                    uint16_t n;
                    if (rev) { if (liststk[listdepth-1].counter > 1) liststk[listdepth-1].counter--; n = liststk[listdepth-1].counter; }
                    else n = ++liststk[listdepth-1].counter;
                    uint8_t lt = liststk[listdepth-1].type;
                    char mark[16]; int ml;
                    if      (lt == 1) ml = sel_fmt_alpha(n, mark, 0);   // a, b, c
                    else if (lt == 2) ml = sel_fmt_alpha(n, mark, 1);   // A, B, C
                    else if (lt == 3) ml = sel_fmt_roman(n, mark, 0);   // i, ii, iii
                    else if (lt == 4) ml = sel_fmt_roman(n, mark, 1);   // I, II, III
                    else { char num[8]; int nn = 0; uint16_t m = n;     // decimal (default)
                        do { num[nn++] = (char)('0' + m % 10); m /= 10; } while (m > 0 && nn < 6);
                        ml = 0; while (nn > 0 && ml < 15) mark[ml++] = num[--nn]; mark[ml] = '\0'; }
                    for (int z = 0; z < ml && ti < len; z++) { txt[ti] = mark[z]; tlink[ti] = 0; ti++; }
                    if (ti + 1 < len) { txt[ti]='.'; tlink[ti]=0; ti++; txt[ti]=' '; tlink[ti]=0; ti++; }
                } else if (listdepth > 0 && liststk[listdepth-1].type == 3) {
                    /* <ul> list-style-type:none -- just the indent, no bullet */
                } else if (ti + 1 < len) {                             // unordered: a bullet glyph + ' '
                    char b = (char)0xF9;                               // disc (default): a small filled bullet
                    if (listdepth > 0) {
                        uint8_t lt = liststk[listdepth-1].type;
                        if      (lt == 1) b = (char)0xF8;              // list-style-type:circle (explicit, all levels)
                        else if (lt == 2) b = (char)0xFE;              // list-style-type:square (explicit, all levels)
                        else {                                        // unset: cycle disc -> circle -> square by nesting depth,
                            if      (listdepth == 2) b = (char)0xF8;  // matching a browser's default nested <ul> markers
                            else if (listdepth >= 3) b = (char)0xFE;  // (level 1 disc, level 2 circle, level 3+ square)
                        }
                    }
                    txt[ti]=b; tlink[ti]=0; ti++; txt[ti]=' '; tlink[ti]=0; ti++;
                }
                last_space = 1;
            } else if (sel_streq(name, "pre")) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 2);   // block break around a preformatted block
                pre_mode = !close;                            // inside: whitespace is preserved literally
                if (!close) pre_skip_nl = 1;                  // ...but a single newline right after <pre> is dropped
                last_space = 1;
            } else if (sel_streq(name, "blockquote")) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 2);   // block break around the quote
                if (!close) { if (quote_depth < SEL_QUOTE_MAXDEPTH) quote_depth++; }   // deeper left margin
                else        { if (quote_depth > 0) quote_depth--; }
                last_space = 1;
            } else if (sel_streq(name, "figure")) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 2);   // block break around the figure
                if (!close) { if (quote_depth < SEL_QUOTE_MAXDEPTH) quote_depth++; }   // indent the whole figure (a browser gives <figure> a left/right margin)
                else        { if (quote_depth > 0) quote_depth--; }
                last_space = 1;
            } else if (sel_streq(name, "figcaption")) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);   // the caption sits on its own line under the figure content, sharing the figure's indent
                if (!close) {                                 // text-align:center|right on the <figcaption> aligns its caption line
                    uint32_t fte = j; while (fte < len && body[fte] != '>') fte++;
                    char fstyle[80] = {0}, fal[16] = {0};
                    extract_attr(body, j, fte, "style", fstyle, sizeof(fstyle));
                    if (fstyle[0]) sel_css_get(fstyle, "text-align", fal, sizeof(fal));
                    if      (sel_ci_streq(fal, "center")) cur_align = 1;
                    else if (sel_ci_streq(fal, "right"))  cur_align = 2;
                } else cur_align = 0;                         // restore left alignment once the caption ends
                last_space = 1;
            } else if (sel_streq(name, "details")) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 2);   // block break around the disclosure widget
                if (!close) {
                    uint32_t dte = j; while (dte < len && body[dte] != '>') dte++;
                    if (!sel_attr_present(body, j, dte, "open")) {   // collapsed (no `open`): show ONLY the <summary>, hide the rest
                        uint32_t k = (dte < len) ? dte + 1 : len; int nest = 1; uint32_t cstart = k;
                        while (k < len && nest > 0) {           // scan to the matching </details> (nesting-aware)
                            if (body[k] != '<') { k++; continue; }
                            int dc; uint32_t dpast;
                            if (sel_tag_match(body, k, len, "details", &dc, &dpast)) { if (dc) nest--; else nest++; k = dpast; continue; }
                            k++;
                        }
                        uint32_t dend = k;                      // just past </details>'s '>' (or len)
                        char sbuf[120] = {0};                   // extract the first <summary>..</summary> text
                        for (uint32_t sp = cstart; sp < dend; ) {
                            if (body[sp] != '<') { sp++; continue; }
                            int sc; uint32_t spast;
                            if (sel_tag_match(body, sp, dend, "summary", &sc, &spast) && !sc) {
                                uint32_t e2 = dend;
                                for (uint32_t q = spast; q < dend; ) {
                                    if (body[q] != '<') { q++; continue; }
                                    int sc2; uint32_t sp2;
                                    if (sel_tag_match(body, q, dend, "summary", &sc2, &sp2) && sc2) { e2 = q; break; }
                                    q++;
                                }
                                sel_cell_text(body, spast, e2, sbuf, sizeof(sbuf), 0);
                                break;
                            }
                            sp++;
                        }
                        if (!sbuf[0]) { sbuf[0]='D'; sbuf[1]='e'; sbuf[2]='t'; sbuf[3]='a'; sbuf[4]='i'; sbuf[5]='l'; sbuf[6]='s'; sbuf[7]='\0'; }
                        const char* mk = "[+] ";                // collapsed disclosure marker; summary rendered bold
                        for (int z = 0; mk[z] && ti < len; z++) { txt[ti] = mk[z]; tlink[ti] = 0; tbold[ti] = 1; ti++; }
                        for (int z = 0; sbuf[z] && ti < len; z++) { txt[ti] = sbuf[z]; tlink[ti] = 0; tbold[ti] = 1; ti++; }
                        ti = sel_ensure_nl(txt, tlink, ti, len, 2);
                        i = dend; last_space = 1; continue;     // skip the whole collapsed subtree
                    }
                }
                last_space = 1;                                 // <details open> / </details>: block break only; content renders normally
            } else if (sel_streq(name, "summary")) {            // reached only inside an OPEN <details> (collapsed ones are skipped above)
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);
                if (!close) { const char* mk = "[-] ";          // expanded disclosure marker; summary rendered bold
                    for (int z = 0; mk[z] && ti < len; z++) { txt[ti] = mk[z]; tlink[ti] = 0; tbold[ti] = 1; ti++; } }
                last_space = 1;
            } else if (sel_streq(name, "dialog")) {
                // <dialog>: a modal/popover container. Per spec it is display:none UNLESS it carries the
                // `open` attribute, so a hidden dialog (the common case -- modal markup present in the DOM
                // but not shown) must render NOTHING instead of leaking its text into the page flow; an
                // open dialog renders as a block. Mirrors the <details> collapsed-skip machinery below
                // (nesting-aware, bounded by len). The main render loop's post-statement is empty, so
                // `i = k; continue;` resumes exactly past </dialog> with no char skipped.
                if (!close) {
                    uint32_t dte = j; while (dte < len && body[dte] != '>') dte++;
                    if (!sel_attr_present(body, j, dte, "open")) {     // no `open` -> hidden: skip the whole subtree
                        uint32_t k = (dte < len) ? dte + 1 : len; int nest = 1;
                        while (k < len && nest > 0) {                  // scan to the matching </dialog>
                            if (body[k] != '<') { k++; continue; }
                            int dc; uint32_t dpast;
                            if (sel_tag_match(body, k, len, "dialog", &dc, &dpast)) { if (dc) nest--; else nest++; k = dpast; continue; }
                            k++;
                        }
                        i = k; continue;                              // emit nothing; keep the surrounding flow intact
                    }
                }
                ti = sel_ensure_nl(txt, tlink, ti, len, 2);           // <dialog open> / </dialog>: shown -> block break
                last_space = 1;
            } else if (!close && (sel_streq(name, "progress") || sel_streq(name, "meter"))) {
                // <progress value max> / <meter value min max>: draw a CP437 block bar + a percentage,
                // inline in the text flow, then skip the element's fallback content up to its close tag.
                uint32_t pte = j; while (pte < len && body[pte] != '>') pte++;
                char vv[16] = {0}, mv[16] = {0}, nv[16] = {0};
                extract_attr(body, j, pte, "value", vv, sizeof(vv));
                extract_attr(body, j, pte, "max",   mv, sizeof(mv));
                extract_attr(body, j, pte, "min",   nv, sizeof(nv));
                int val = sel_parse_milli(vv); if (val < 0) val = 0;
                int mx  = sel_parse_milli(mv); if (mx <= 0) mx = 1000;   // default max = 1
                int mn  = sel_parse_milli(nv); if (mn < 0) mn = 0;       // <meter min> (progress has none)
                if (mx <= mn) mx = mn + 1000;
                if (val < mn) val = mn;
                if (val > mx) val = mx;
                int per = (int)(((long)(val - mn) * 1000) / (mx - mn));  // 0..1000 permille filled
                const int NB = 16; int filled = per * NB / 1000; if (filled > NB) filled = NB;
                uint8_t bd = (uint8_t)(cur_bold | (cur_ul << 1) | (cur_st << 2) | (cur_du << 3) | (cur_vo << 4) | (cur_ol << 6));
                for (int z = 0; z < NB && ti < len; z++) {              // filled = full block 0xDB, empty = light shade 0xB0
                    txt[ti] = (z < filled) ? (char)0xDB : (char)0xB0; tlink[ti] = 0; tfield[ti] = 0;
                    tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = bd;
                    talign[ti] = (uint8_t)cur_align; tindent[ti] = (uint8_t)quote_depth; ti++;
                }
                char pct[8]; int pp = 0, pv = per / 10;                 // trailing " NN%"
                pct[pp++] = ' ';
                if (pv >= 100) { pct[pp++] = '1'; pct[pp++] = '0'; pct[pp++] = '0'; }
                else { if (pv >= 10) pct[pp++] = (char)('0' + pv / 10); pct[pp++] = (char)('0' + pv % 10); }
                pct[pp++] = '%'; pct[pp] = '\0';
                for (int z = 0; pct[z] && ti < len; z++) {
                    txt[ti] = pct[z]; tlink[ti] = 0; tfield[ti] = 0;
                    tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = bd;
                    talign[ti] = (uint8_t)cur_align; tindent[ti] = (uint8_t)quote_depth; ti++;
                }
                uint32_t k = (pte < len) ? pte + 1 : len;               // skip fallback content to the matching close tag
                while (k < len) { int pc; uint32_t ppast;
                    if (body[k] == '<' && ((sel_tag_match(body, k, len, "progress", &pc, &ppast) && pc) ||
                                           (sel_tag_match(body, k, len, "meter", &pc, &ppast) && pc))) { k = ppast; break; }
                    k++;
                }
                i = k; last_space = 0; continue;
            } else if (!close && (sel_streq(name, "audio") || sel_streq(name, "video"))) {
                // <audio>/<video>: draw a small inline placeholder "[<glyph> audio/video: name]"
                // (CP437 music note 0x0E / play triangle 0x10), then skip the element's fallback content
                // and its <source> children up to the matching close tag. Non-breaking spaces keep the box whole.
                int isvid = sel_streq(name, "video");
                uint32_t ate = j; while (ate < len && body[ate] != '>') ate++;
                char src[80] = {0};
                extract_attr(body, j, ate, "src", src, sizeof(src));
                uint32_t k = (ate < len) ? ate + 1 : len;
                while (k < len) { int mc; uint32_t mpast;
                    if (body[k] == '<') {
                        if ((sel_tag_match(body, k, len, "audio", &mc, &mpast) && mc) ||
                            (sel_tag_match(body, k, len, "video", &mc, &mpast) && mc)) { k = mpast; break; }
                        if (!src[0] && sel_tag_match(body, k, len, "source", &mc, &mpast) && !mc) {   // grab a <source src> if the tag had none
                            uint32_t se = k; while (se < len && body[se] != '>') se++;
                            extract_attr(body, k, se, "src", src, sizeof(src));
                        }
                    }
                    k++;
                }
                const char* bn = src; for (const char* p = src; *p; p++) if (*p == '/') bn = p + 1;   // basename of the src
                char box[80]; int bp = 0;
                box[bp++] = '['; box[bp++] = isvid ? (char)0x10 : (char)0x0E; box[bp++] = (char)0xFF;   // 0xFF = non-breaking space
                const char* lbl = isvid ? "video" : "audio";
                for (int z = 0; lbl[z] && bp < 70; z++) box[bp++] = lbl[z];
                if (bn[0]) { box[bp++] = ':'; box[bp++] = (char)0xFF; for (int z = 0; bn[z] && z < 40 && bp < 76; z++) box[bp++] = bn[z]; }
                box[bp++] = ']'; box[bp] = '\0';
                uint8_t bd = (uint8_t)(cur_bold | (cur_ul << 1) | (cur_st << 2) | (cur_du << 3) | (cur_vo << 4) | (cur_ol << 6));
                for (int z = 0; box[z] && ti < len; z++) {
                    txt[ti] = box[z]; tlink[ti] = 0; tfield[ti] = 0;
                    tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = bd;
                    talign[ti] = (uint8_t)cur_align; tindent[ti] = (uint8_t)quote_depth; ti++;
                }
                i = k; last_space = 0; continue;
            } else if (!close && sel_streq(name, "select")) {
                // <select>: render a dropdown "[ option v]" showing the selected <option> (or the first if none
                // is selected), then skip the option list up to the matching close tag. 0x1F = CP437 down-triangle.
                uint32_t ste = j; while (ste < len && body[ste] != '>') ste++;
                uint32_t k = (ste < len) ? ste + 1 : len;
                char opt[64] = {0}; int have_opt = 0, have_sel = 0;
                while (k < len) { int oc; uint32_t opast;
                    if (body[k] == '<') {
                        if (sel_tag_match(body, k, len, "select", &oc, &opast) && oc) { k = opast; break; }   // </select>
                        if (!have_sel && sel_tag_match(body, k, len, "option", &oc, &opast) && !oc) {
                            uint32_t oend = k; while (oend < len && body[oend] != '>') oend++;
                            int is_sel = sel_attr_present(body, k, oend, "selected");
                            if (!have_opt || is_sel) {                            // first option, or a selected one (which wins)
                                uint32_t ts = (oend < len) ? oend + 1 : len, te2 = ts;
                                while (te2 < len && body[te2] != '<') te2++;
                                while (ts < te2 && (body[ts]==' '||body[ts]=='\n'||body[ts]=='\r'||body[ts]=='\t')) ts++;
                                int b = 0; for (uint32_t z = ts; z < te2 && b < 60; z++) { char c2 = (char)body[z]; if (c2=='\n'||c2=='\r'||c2=='\t') c2=' '; opt[b++]=c2; }
                                while (b > 0 && opt[b-1]==' ') b--;
                                opt[b]='\0';
                                have_opt = 1; if (is_sel) have_sel = 1;
                            }
                            k = opast; continue;
                        }
                    }
                    k++;
                }
                if (!opt[0]) { opt[0]='?'; opt[1]='\0'; }
                char box[80]; int bp = 0; box[bp++]='['; box[bp++]=(char)0xFF;
                for (int z = 0; opt[z] && bp < 72; z++) box[bp++] = (opt[z]==' ') ? (char)0xFF : opt[z];
                box[bp++]=(char)0xFF; box[bp++]=(char)0x1F; box[bp++]=']'; box[bp]='\0';
                uint8_t bd = (uint8_t)(cur_bold|(cur_ul<<1)|(cur_st<<2)|(cur_du<<3)|(cur_vo<<4)|(cur_ol<<6));
                for (int z = 0; box[z] && ti < len; z++) {
                    txt[ti]=box[z]; tlink[ti]=0; tfield[ti]=0;
                    tcolor[ti]=(uint8_t)cur_color; tbgcol[ti]=(uint8_t)cur_bg; tbold[ti]=bd;
                    talign[ti]=(uint8_t)cur_align; tindent[ti]=(uint8_t)quote_depth; ti++;
                }
                i = k; last_space = 0; continue;
            } else if (!close && sel_streq(name, "dd")) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);          // <dd>: the description on its own line, indented under its <dt> term
                for (int d = 0; d < SEL_QUOTE_INDENT && ti < len; d++) { txt[ti] = ' '; tlink[ti] = 0; ti++; }
                last_space = 1;
            } else if (sel_streq(name,"br") || sel_streq(name,"tr") || sel_streq(name,"dd") || sel_streq(name,"dt")) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 1);          // <dt>, </dd>, <br>, <tr>: a plain line break (term sits at the left margin)
                last_space = 1;
            } else if (sel_streq(name, "fieldset")) {                // <fieldset>: a form-grouping element -> render as a bordered card (like a bordered <div>, but always bordered). Its <legend> renders as ordinary text inside.
                ti = sel_ensure_nl(txt, tlink, ti, len, 2);
                if (!close) {
                    if (boxsp < SEL_BOX_MAXDEPTH) {
                        uint8_t bcol = 0;                            // honour a CSS border-color if the fieldset sets one, else the theme grey default
                        uint32_t fte = j; while (fte < len && body[fte] != '>') fte++;
                        char fst[160] = {0}; extract_attr(body, j, fte, "style", fst, sizeof(fst));
                        if (fst[0]) { char bc[40] = {0}; uint32_t frgb;
                            if (sel_css_get(fst, "border-color", bc, sizeof(bc)) && sel_parse_css_color(bc, &frgb)) bcol = sel_intern_color(s, frgb); }
                        boxstk[boxsp].bordered = 1; boxstk[boxsp].col = bcol; boxsp++;   // always bordered; pop on </fieldset>
                        if (ti < len) { txt[ti]=' '; tlink[ti]=0; tfield[ti]=0; timg[ti]=0; tcolor[ti]=bcol; tbgcol[ti]=0; tbold[ti]=0;
                            talign[ti]=0; trule[ti]=SEL_BOX_TOP; tindent[ti]=(uint8_t)quote_depth; ti++;
                            ti = sel_ensure_nl(txt, tlink, ti, len, 1); }
                    }
                } else if (boxsp > 0) {
                    uint8_t bcol = boxstk[--boxsp].col;
                    if (ti < len) { txt[ti]=' '; tlink[ti]=0; tfield[ti]=0; timg[ti]=0; tcolor[ti]=bcol; tbgcol[ti]=0; tbold[ti]=0;
                        talign[ti]=0; trule[ti]=SEL_BOX_BOT; tindent[ti]=(uint8_t)quote_depth; ti++;
                        ti = sel_ensure_nl(txt, tlink, ti, len, 1); }
                }
                last_space = 1;
            } else if (is_block_tag(name)) {
                ti = sel_ensure_nl(txt, tlink, ti, len, 2);
                int is_div = sel_streq(name, "div");
                if (!close) {                                         // CSS text-indent: indent this block's FIRST line only
                    uint32_t bte = j; while (bte < len && body[bte] != '>') bte++;
                    char bstyle[160] = {0}, tiv[24] = {0};
                    extract_attr(body, j, bte, "style", bstyle, sizeof(bstyle));
                    if (bstyle[0] && sel_css_get(bstyle, "text-indent", tiv, sizeof(tiv))) {
                        int cells = sel_parse_indent(tiv);              // leading spaces at a hard line start survive wrap_text
                        for (int d = 0; d < cells && ti < len; d++) {   // (bol=1), and are dropped on soft-wrapped continuations
                            txt[ti] = ' '; tlink[ti] = 0; tfield[ti] = 0; timg[ti] = 0;
                            tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = 0;
                            talign[ti] = (uint8_t)cur_align; trule[ti] = 0; tindent[ti] = (uint8_t)quote_depth; ti++;
                        }
                    }
                    if (is_div && boxsp < SEL_BOX_MAXDEPTH) {          // a <div> opens a box level: bordered iff its CSS declares a (non-zero) border
                        uint8_t bordered = 0, bcol = 0;
                        if (bstyle[0]) { char bv[48] = {0};
                            if (sel_css_get(bstyle, "border", bv, sizeof(bv)) || sel_css_get(bstyle, "border-width", bv, sizeof(bv)) ||
                                sel_css_get(bstyle, "border-top", bv, sizeof(bv)) || sel_css_get(bstyle, "border-style", bv, sizeof(bv))) {
                                if (!(sel_ci_streq(bv, "none") || bv[0] == '0')) bordered = 1;   // any border except none / 0[px]
                            }
                            if (bordered) {                            // box stroke colour: explicit border-color, else the colour token in the border shorthand (else grey default)
                                char bc[40] = {0}; uint32_t brgb;
                                if (sel_css_get(bstyle, "border-color", bc, sizeof(bc))) { if (sel_parse_css_color(bc, &brgb)) bcol = sel_intern_color(s, brgb); }
                                else for (int z = 0; bv[z]; ) {        // scan the shorthand's space-separated tokens for the first that parses as a colour (e.g. "1px solid #30363d")
                                    while (bv[z] == ' ') z++;
                                    int e2 = z;
                                    while (bv[e2] && bv[e2] != ' ') e2++;
                                    char tok[40]; int tl = 0; for (int q = z; q < e2 && tl < 39; q++) tok[tl++] = bv[q]; tok[tl] = '\0';
                                    if (tl && sel_parse_css_color(tok, &brgb)) { bcol = sel_intern_color(s, brgb); break; }
                                    z = e2; if (!bv[z]) break;
                                }
                            }
                        }
                        boxstk[boxsp].bordered = bordered; boxstk[boxsp].col = bcol; boxsp++;   // every <div> pushes (bordered or not) so the matching </div> pops correctly
                        if (bordered && ti < len) {                    // emit a box-TOP marker line: its own blank line, drawn as the top edge (tcolor carries the border colour)
                            txt[ti] = ' '; tlink[ti] = 0; tfield[ti] = 0; timg[ti] = 0; tcolor[ti] = bcol; tbgcol[ti] = 0; tbold[ti] = 0;
                            talign[ti] = 0; trule[ti] = SEL_BOX_TOP; tindent[ti] = (uint8_t)quote_depth; ti++;
                            ti = sel_ensure_nl(txt, tlink, ti, len, 1);
                        }
                    }
                } else if (is_div && boxsp > 0) {                      // </div>: close the box level; if it was bordered, emit a box-BOTTOM marker line in the same colour
                    uint8_t bordered = boxstk[--boxsp].bordered, bcol = boxstk[boxsp].col;
                    if (bordered && ti < len) {
                        txt[ti] = ' '; tlink[ti] = 0; tfield[ti] = 0; timg[ti] = 0; tcolor[ti] = bcol; tbgcol[ti] = 0; tbold[ti] = 0;
                        talign[ti] = 0; trule[ti] = SEL_BOX_BOT; tindent[ti] = (uint8_t)quote_depth; ti++;
                        ti = sel_ensure_nl(txt, tlink, ti, len, 1);
                    }
                }
                last_space = 1;
            }
            i = j;
            while (i < len && body[i] != '>') i++;
            if (i < len) i++;
            continue;
        }
        if (c == '&') {
            char eb[8]; uint32_t el, adv;
            pre_skip_nl = 0;                                   // an entity is content: a later <pre> newline counts
            if (decode_entity(body + i, len - i, eb, sizeof(eb), &el, &adv)) {
                for (uint32_t k = 0; k < el && ti < len; k++) {
                    char dec = eb[k];
                    if (dec == ' ') { if (pre_mode || !last_space) { txt[ti] = (cur_nowrap && !pre_mode) ? (char)0xFF : ' '; tlink[ti] = (uint8_t)cur_link; tfield[ti] = (uint8_t)cur_field; tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = (uint8_t)(cur_bold | (cur_ul << 1) | (cur_st << 2) | (cur_du << 3) | (cur_vo << 4) | (cur_ol << 6)); talign[ti] = (uint8_t)cur_align; tindent[ti] = (uint8_t)quote_depth; ti++; last_space = 1; } }
                    else { if (cur_hd && dec >= 'a' && dec <= 'z') dec -= 32;
                        if (cur_tt == 1 && dec >= 'a' && dec <= 'z') dec -= 32; else if (cur_tt == 2 && dec >= 'A' && dec <= 'Z') dec += 32; else if (cur_tt == 3 && last_space && dec >= 'a' && dec <= 'z') dec -= 32;
                        txt[ti] = dec; tlink[ti] = (uint8_t)cur_link; tfield[ti] = (uint8_t)cur_field; tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = (uint8_t)(cur_bold | (cur_ul << 1) | (cur_st << 2) | (cur_du << 3) | (cur_vo << 4) | (cur_ol << 6)); talign[ti] = (uint8_t)cur_align; tindent[ti] = (uint8_t)quote_depth; ti++; last_space = 0; }
                }
                i += adv;
            } else { txt[ti] = '&'; tlink[ti] = (uint8_t)cur_link; tfield[ti] = (uint8_t)cur_field; tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = (uint8_t)(cur_bold | (cur_ul << 1) | (cur_st << 2) | (cur_du << 3) | (cur_vo << 4) | (cur_ol << 6)); talign[ti] = (uint8_t)cur_align; tindent[ti] = (uint8_t)quote_depth; ti++; last_space = 0; i++; }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (pre_mode) {                                   // <pre>: keep whitespace literally
                if (c == '\r') { i++; continue; }             // drop CR (part of a CRLF)
                if (c == '\n') {
                    if (pre_skip_nl) { pre_skip_nl = 0; i++; continue; }   // swallow the single newline after <pre>
                    txt[ti] = '\n'; tlink[ti] = 0; tfield[ti] = 0; tindent[ti] = (uint8_t)quote_depth; ti++; last_space = 1;
                } else if (c == '\t') { pre_skip_nl = 0; for (int q = 0; q < SEL_PRE_TAB && ti < len; q++) { txt[ti]=' '; tlink[ti]=(uint8_t)cur_link; tfield[ti]=0; tcolor[ti]=(uint8_t)cur_color; tbgcol[ti]=(uint8_t)cur_bg; tbold[ti]=(uint8_t)(cur_bold|(cur_ul<<1)|(cur_st<<2)|(cur_du<<3)|(cur_vo<<4)|(cur_ol<<6)); talign[ti]=(uint8_t)cur_align; tindent[ti]=(uint8_t)quote_depth; ti++; } last_space = 0; }
                else { pre_skip_nl = 0; txt[ti] = ' '; tlink[ti] = (uint8_t)cur_link; tfield[ti] = 0; tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = (uint8_t)(cur_bold | (cur_ul << 1) | (cur_st << 2) | (cur_du << 3) | (cur_vo << 4) | (cur_ol << 6)); talign[ti] = (uint8_t)cur_align; tindent[ti] = (uint8_t)quote_depth; ti++; last_space = 0; }
                i++;
                continue;
            }
            if (!last_space) { txt[ti] = cur_nowrap ? (char)0xFF : ' '; tlink[ti] = (uint8_t)cur_link; tfield[ti] = (uint8_t)cur_field; tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = (uint8_t)(cur_bold | (cur_ul << 1) | (cur_st << 2) | (cur_du << 3) | (cur_vo << 4) | (cur_ol << 6)); talign[ti] = (uint8_t)cur_align; tindent[ti] = (uint8_t)quote_depth; ti++; last_space = 1; }
            i++;
            continue;
        }
        if (cur_hd && c >= 'a' && c <= 'z') c -= 32;          // upper-case h1/h2 text
        if (cur_tt == 1 && c >= 'a' && c <= 'z') c -= 32;                        // text-transform: uppercase
        else if (cur_tt == 2 && c >= 'A' && c <= 'Z') c += 32;                   // text-transform: lowercase
        else if (cur_tt == 3 && last_space && c >= 'a' && c <= 'z') c -= 32;     // text-transform: capitalize (word start)
        pre_skip_nl = 0;                                       // real content: a later <pre> newline is significant
        txt[ti] = c; tlink[ti] = (uint8_t)cur_link; tfield[ti] = (uint8_t)cur_field; tcolor[ti] = (uint8_t)cur_color; tbgcol[ti] = (uint8_t)cur_bg; tbold[ti] = (uint8_t)(cur_bold | (cur_ul << 1) | (cur_st << 2) | (cur_du << 3) | (cur_vo << 4) | (cur_ol << 6)); talign[ti] = (uint8_t)cur_align; tindent[ti] = (uint8_t)quote_depth; ti++; last_space = 0; i++;
    }
    txt[ti] = '\0';
    wrap_text(s, txt, tlink, tfield, timg, tcolor, tbgcol, tbold, talign, trule, tindent, line_start, ti);
    if (line_start) {                                            // resolve each in-cell box's line-start txt indices to final line numbers
        for (int b = 0; b < s->num_cell_boxes; b++) {
            int l0 = -1, l1 = -1;
            for (int li = 0; li < s->num_lines; li++) {
                if (line_start[li] == s->cell_boxes[b].txt0) l0 = li;
                if (line_start[li] == s->cell_boxes[b].txt1) l1 = li;
            }
            if (l0 >= 0 && l1 >= l0) { s->cell_boxes[b].line0 = (uint16_t)l0; s->cell_boxes[b].line1 = (uint16_t)l1; }
            else s->cell_boxes[b].used = 0;                       // couldn't map (soft-wrap / overflow) -> drop this box, never mis-draw
        }
        kfree(line_start);
    } else s->num_cell_boxes = 0;                                // no map -> no in-cell boxes this render
    kfree(txt); kfree(tlink); kfree(tfield); kfree(timg); kfree(tcolor); kfree(tbgcol); kfree(tbold); kfree(talign); kfree(trule); kfree(tindent);
    kfree(colstk);
}

// Parse http[://]host[:port][/path] into host/port/path (same shape as `httpget`).
static void parse_url(const char* url, char* host, uint16_t* port, char* path, int* is_https) {
    // Shared hardened parser (bounds the port, rejects an over-long host). Callers pass
    // host[128] / path[256] and treat an empty host as "couldn't parse".
    if (url_parse(url, host, 128, port, path, 256, is_https) != 0) host[0] = '\0';
}

static int find_iface(void) {
    for (int i = 0; i < 8; i++)
        if (net_interfaces[i].name[0] && strcmp(net_interfaces[i].name, "lo") != 0) return i;
    return -1;
}

// After a page renders, download + decode the first few <img>s so they can be drawn for real.
// Each image: resolve its src, fetch the bytes (http/https), and png_decode into RGBA (stored in
// the sel_img_t). A failed fetch/decode or an unsupported format simply leaves px = NULL, and the
// draw falls back to the framed "[img: alt]" placeholder. Bounded (count + per-image buffer).
static int  visible_rows(void);                        // defined below; used by the lazy image fetch
static void clamp_scroll(selene_ctx_t* s);

// Download + decode ONE <img> (image i) into RGBA on its sel_img_t. Retries once — NyxOS's first new
// TCP connection right after another fetch sometimes fails. The caller marks it `tried`.
static void selene_fetch_one(selene_ctx_t* s, int i, int iface) {
    if (!s->images[i].src[0]) return;
    char abs[256]; selene_resolve(s, s->images[i].src, abs);
    if (!abs[0]) return;
    char host[128] = {0}, path[256] = {0}; uint16_t port = 80; int is_https = 0;
    parse_url(abs, host, &port, path, &is_https);
    if (!host[0]) return;
    for (int attempt = 0; attempt < 2 && !s->images[i].px; attempt++) {
        http_response_t resp; int ok = 0;
        if (is_https) {
            uint8_t* raw = (uint8_t*)kmalloc(SEL_IMG_FETCH_CAP);
            if (!raw) break;
            int rn = tls_https_request(host, path, "GET", 0, 0, iface, raw, SEL_IMG_FETCH_CAP - 1, 0);
            if (rn > 0) { raw[rn] = '\0'; if (http_parse_response(raw, (uint32_t)rn, &resp) == 0) ok = 1; }
            kfree(raw);
        } else {
            if (http_request(host, port, path, "GET", 0, 0, &resp, iface) == 0) ok = 1;
        }
        if (!ok) continue;
        if (resp.body && resp.body_len > 8) {               // dispatch by magic bytes: PNG / BMP / JPEG / GIF
            image_t pi; int dec = -1;
            if (resp.body[0] == 0x89 && resp.body[1] == 'P')      dec = png_decode(resp.body, resp.body_len, &pi);
            else if (resp.body[0] == 'B' && resp.body[1] == 'M')  dec = bmp_decode(resp.body, resp.body_len, &pi);
            else if (resp.body[0] == 0xFF && resp.body[1] == 0xD8) dec = jpeg_decode(resp.body, resp.body_len, &pi);   // JPEG (baseline)
            else if (resp.body[0] == 'G' && resp.body[1] == 'I' && resp.body[2] == 'F') {   // GIF: decode all frames
                gif_anim_t ga;
                if (gif_decode_anim(resp.body, resp.body_len, &ga) == 0) {
                    s->images[i].frames = ga.frames; s->images[i].nframes = ga.nframes;
                    s->images[i].cur_frame = 0; s->images[i].anim_ms = 0;
                    s->images[i].loop_count = ga.loop_count; s->images[i].loops_done = 0;
                    s->images[i].px = ga.frames[0].pixels;              // show frame 0 (aliases frames[]; freed via selene_img_free)
                    s->images[i].iw = (uint16_t)ga.width; s->images[i].ih = (uint16_t)ga.height;
                }
            }
            if (dec == 0) {                                            // static image (PNG/BMP)
                s->images[i].px = pi.pixels;
                s->images[i].iw = (uint16_t)pi.width;
                s->images[i].ih = (uint16_t)pi.height;
            }
        }
        http_free(&resp);
    }
}

// The document line image i's box is anchored on (its label starts at col 0), or -1.
static int selene_img_anchor(selene_ctx_t* s, int i) {
    for (int li = 0; li < s->num_lines; li++) if (s->img_of[li][0] == i + 1) return li;
    return -1;
}

// The next <img> to load: the first currently-visible ([scroll, scroll+rows)), untried, not-yet-decoded
// one, or -1 if none are pending. Pure (no I/O), so `imgtest` can pin the visibility gating offline.
static int selene_next_img_index(selene_ctx_t* s, int rows) {
    for (int i = 0; i < s->num_imgs; i++) {
        if (s->images[i].tried || s->images[i].px) continue;
        int aline = selene_img_anchor(s, i);
        if (aline < 0 || aline < s->scroll || aline >= s->scroll + rows) continue;   // not visible now
        return i;
    }
    return -1;
}

// Fetch + decode AT MOST ONE visible, not-yet-tried image. Returns 1 if it fetched one (the caller
// should redraw to pop it in), 0 if none are pending. Driven once per compositor tick by
// selene_win_tick so image loading never freezes the browser: the page shows instantly and images
// stream in one per frame, with scrolling / clicks / tab switches handled in between.
static int selene_fetch_next(selene_ctx_t* s, int iface) {
    if (iface < 0) return 0;
    int i = selene_next_img_index(s, visible_rows());
    if (i < 0) return 0;
    selene_fetch_one(s, i, iface);
    s->images[i].tried = 1;
    return 1;
}

// Advance any VISIBLE animated GIF by one compositor tick (~33 ms). When a frame's delay elapses, step
// to the next frame and repoint px at it; on wrapping past the last frame a full loop has played, and a
// GIF with a finite NETSCAPE loop count freezes on its last frame once it has looped that many times.
// Returns 1 if any visible frame flipped; off-screen (or finished) animations are frozen (no wasted CPU).
static int selene_anim_tick(selene_ctx_t* s) {
    int rows = visible_rows(), changed = 0;
    for (int i = 0; i < s->num_imgs; i++) {
        sel_img_t* im = &s->images[i];
        if (!im->frames || im->nframes < 2) continue;            // static or single-frame: nothing to animate
        if (im->loop_count != 0 && im->loops_done >= im->loop_count) continue;   // finished looping: frozen
        int aline = selene_img_anchor(s, i);
        if (aline < 0 || aline < s->scroll || aline >= s->scroll + rows) continue;   // off-screen: freeze
        im->anim_ms += SELENE_TICK_MS;
        uint32_t need = (uint32_t)im->frames[im->cur_frame].delay_cs * 10;   // centiseconds -> ms
        if (im->anim_ms >= need) {
            im->anim_ms = 0;
            if (im->cur_frame + 1 >= im->nframes) {              // finishing a loop
                if (im->loop_count != 0 && ++im->loops_done >= im->loop_count) continue;   // last loop: stay on final frame
                im->cur_frame = 0;
            } else {
                im->cur_frame++;
            }
            im->px = im->frames[im->cur_frame].pixels;          // px aliases the new frame (not owned)
            changed = 1;
        }
    }
    return changed;
}

// Compositor ~30fps tick for a Selene window: (1) advance any on-screen animated GIF, then (2)
// cooperatively load one pending image — so neither animation nor a slow fetch freezes the UI.
// Returns 1 (redraw) when it changed something this tick, 0 when idle.
int selene_win_tick(window_t* win) {
    selene_tabs_t* T = (selene_tabs_t*)win->reserved;
    if (!T) return 0;
    selene_ctx_t* s = T->tab[T->active];
    if (!s) return 0;
    int changed = selene_anim_tick(s);                          // animate first (cheap, no I/O)
    if (selene_fetch_next(s, find_iface())) changed = 1;        // then fetch one pending image (may block briefly)
    return changed;
}

// A scroll changed: repaint immediately (responsive). Newly-visible images are picked up by
// selene_win_tick on the next frame — no blocking fetch here, so scrolling stays instant.
static void selene_after_scroll(selene_ctx_t* s) {
    clamp_scroll(s);
    compositor_redraw_now();
}

// Fetch ctx->url with `method` (+ optional form `body` for POST) and render the reply. Blocks
// (the fetch drives the net) - we paint a status first via compositor_redraw_now for progress.
#define SEL_MAX_REDIRECTS 5                  // cap on how many 3xx Location hops we follow
static void selene_set_url(selene_ctx_t* s, const char* u);   // defined below; used by the redirect follow

static void selene_load_ex(selene_ctx_t* s, const char* method, const uint8_t* body, uint32_t body_len) {
    s->num_lines = 0; s->scroll = 0; s->title[0] = '\0'; s->num_links = 0; s->sel_link = -1;
    s->find_active = 0; s->find_matches = 0; s->find_cur = 0;   // close find-in-page on navigation
    int iface = find_iface();
    if (iface < 0) { strncpy(s->status, "No network interface (boot with -nic)", 95); return; }
    if (net_interfaces[iface].ip == 0) {
        strncpy(s->status, "Getting an IP address (DHCP)...", 95);
        compositor_redraw_now();
        dhcp_request(iface);
    }

    // Fetch, following up to SEL_MAX_REDIRECTS 3xx redirects: a 301/302/303/307/308 with a Location
    // header re-fetches the resolved target (301/302/303 downgrade to GET; 307/308 keep method+body).
    const char* cur_method = method;
    const uint8_t* cur_body = body; uint32_t cur_body_len = body_len;
    char* final_host = s->load.final_host;
    final_host[0] = 0;
    http_response_t* resp = &s->load.response;

    for (int hop = 0; ; hop++) {
        char* host = s->load.host; char* path = s->load.path;
        host[0] = path[0] = 0; uint16_t port = 80; int is_https = 0;
        parse_url(s->url, host, &port, path, &is_https);
        if (!host[0]) { strncpy(s->status, "Enter a URL, e.g. example.com", 95); return; }
        strncpy(final_host, host, sizeof(s->load.final_host)-1); final_host[sizeof(s->load.final_host)-1] = '\0';
        int is_post = (cur_method && (cur_method[0] == 'P' || cur_method[0] == 'p'));
        // base for resolving this page's relative links (and a relative redirect target)
        strncpy(s->base_host, host, sizeof(s->base_host)-1); s->base_host[sizeof(s->base_host)-1] = '\0';
        s->base_port = port; s->base_https = is_https;
        strncpy(s->base_path, path, sizeof(s->base_path)-1); s->base_path[sizeof(s->base_path)-1] = '\0';
        snprintf(s->status, sizeof(s->status), "%s %s%s ...%s", is_post ? "Submitting to" : "Loading",
                 is_https ? "https://" : "", host, hop ? " (redirect)" : "");
        compositor_redraw_now();

        // Fetch, retrying once: NyxOS's first NEW TCP connection right after another fetch sometimes
        // fails (see v5.9.83), and a redirect chain opens several connections back-to-back.
        int ok = 0;
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            if (is_https) {
                // Secure fetch: a full TLS 1.2 handshake + encrypted request, then parsed like http.
                uint8_t* raw = (uint8_t*)kmalloc(HTTP_MAX_RESPONSE);
                if (!raw) { strncpy(s->status, "Out of memory", 95); return; }
                int rn = tls_https_request(host, path, cur_method, cur_body, cur_body_len, iface, raw, HTTP_MAX_RESPONSE - 1, 0);
                if (rn >= 0) { raw[rn] = '\0'; if (http_parse_response(raw, (uint32_t)rn, resp) == 0) ok = 1; }
                kfree(raw);
            } else {
                if (http_request(host, port, path, cur_method, cur_body, cur_body_len, resp, iface) == 0) ok = 1;
            }
        }
        if (!ok) { snprintf(s->status, sizeof(s->status), "Could not load %s", host); return; }

        int sc = resp->status_code;
        if ((sc == 301 || sc == 302 || sc == 303 || sc == 307 || sc == 308) && resp->location[0] && hop < SEL_MAX_REDIRECTS) {
            char* newurl = s->load.redirect; newurl[0] = 0;
            selene_resolve(s, resp->location, newurl);          // absolute / relative Location -> full URL
            if (newurl[0]) {
                selene_set_url(s, newurl);                     // follow it: the URL bar shows the final URL
                if (sc != 307 && sc != 308) { cur_method = "GET"; cur_body = 0; cur_body_len = 0; }
                http_free(resp);
                continue;
            }
        }
        break;   // final response: not a redirect, no Location, unresolvable, or the hop cap was reached
    }

    render_html(s, resp->body, resp->body_len);
    strncpy(s->cur_url, s->url, sizeof(s->cur_url)-1); s->cur_url[sizeof(s->cur_url)-1] = '\0';
    snprintf(s->status, sizeof(s->status), "%d %s  -  %s  -  %d links",
             resp->status_code, resp->status_text,
             s->title[0] ? s->title : final_host, s->num_links);
    http_free(resp);
    compositor_redraw_now();                 // show the page instantly; visible images stream in via selene_win_tick
}

static void selene_load(selene_ctx_t* s) { selene_load_ex(s, "GET", 0, 0); }

// Navigation helpers manage the Back history around selene_load.
static void push_hist(selene_ctx_t* s, const char* u) {
    if (!u[0]) return;
    if (s->hist_len >= SEL_HIST) {                    // drop the oldest
        for (int i = 1; i < SEL_HIST; i++) strcpy(s->hist[i-1], s->hist[i]);
        s->hist_len = SEL_HIST - 1;
    }
    strncpy(s->hist[s->hist_len], u, 255); s->hist[s->hist_len][255] = '\0';
    s->hist_len++;
}
// Same, for the Forward stack (pages left behind by going Back).
static void push_fwd(selene_ctx_t* s, const char* u) {
    if (!u[0]) return;
    if (s->fwd_len >= SEL_HIST) {
        for (int i = 1; i < SEL_HIST; i++) strcpy(s->fwd[i-1], s->fwd[i]);
        s->fwd_len = SEL_HIST - 1;
    }
    strncpy(s->fwd[s->fwd_len], u, 255); s->fwd[s->fwd_len][255] = '\0';
    s->fwd_len++;
}
static void selene_set_url(selene_ctx_t* s, const char* u) {
    strncpy(s->url, u, sizeof(s->url)-1); s->url[sizeof(s->url)-1] = '\0';
    s->url_len = (int)strlen(s->url);
}
static void selene_go(selene_ctx_t* s) {              // load the URL bar (a new navigation)
    if (s->cur_url[0] && strcmp(s->cur_url, s->url) != 0) push_hist(s, s->cur_url);
    s->fwd_len = 0;                                   // a fresh navigation discards the Forward stack
    selene_load(s);
}
static void selene_follow(selene_ctx_t* s, const char* url) {
    if (!url[0]) return;
    push_hist(s, s->cur_url);
    s->fwd_len = 0;                                   // a fresh navigation discards the Forward stack
    selene_set_url(s, url);
    selene_load(s);
}
// Pure Back/Forward stack steps (no network): move the current page across the two stacks and
// return the URL to load next, or NULL if that direction is empty. selene_back/forward set it and
// call selene_load; pinned directly (no fetch) by selene_nav_selftest.
static const char* nav_back(selene_ctx_t* s) {
    if (s->hist_len <= 0) return 0;
    if (s->cur_url[0]) push_fwd(s, s->cur_url);       // remember the current page for Forward
    s->hist_len--;
    return s->hist[s->hist_len];
}
static const char* nav_forward(selene_ctx_t* s) {
    if (s->fwd_len <= 0) return 0;
    if (s->cur_url[0]) push_hist(s, s->cur_url);      // the current page goes back onto the Back stack
    s->fwd_len--;
    return s->fwd[s->fwd_len];
}
static void selene_back(selene_ctx_t* s) {
    const char* u = nav_back(s);
    if (!u) return;
    selene_set_url(s, u);
    selene_load(s);
}
static void selene_forward(selene_ctx_t* s) {
    const char* u = nav_forward(s);
    if (!u) return;
    selene_set_url(s, u);
    selene_load(s);
}
// Reload: re-fetch the page currently shown WITHOUT touching either history stack (unlike
// selene_go, a fresh navigation that pushes Back + clears Forward). Bound to the toolbar
// reload button, F5, and Ctrl+R.
static void selene_reload(selene_ctx_t* s) {
    if (!s->cur_url[0]) return;              // nothing loaded yet -> nothing to reload
    selene_set_url(s, s->cur_url);           // make the URL bar match the page, then re-fetch
    selene_load(s);
}

// KAT (`selenenav`): Back/Forward across a visit sequence, both empty edges, and the rule that a
// fresh navigation discards the Forward stack. Drives the pure nav helpers with no network (cur_url
// is set by hand to stand in for a completed load).
int selene_nav_selftest(void) {
    static selene_ctx_t s;                            // static: the ctx is far larger than a KAT stack
    memset_asm(&s, 0, sizeof(s));
    strcpy(s.cur_url, "A");
    push_hist(&s, s.cur_url); s.fwd_len = 0; strcpy(s.cur_url, "B");   // A -> B (new nav)
    push_hist(&s, s.cur_url); s.fwd_len = 0; strcpy(s.cur_url, "C");   // B -> C
    const char* u = nav_back(&s);                     // C -> B
    if (!u || strcmp(u, "B")) return 1;
    strcpy(s.cur_url, u);
    u = nav_back(&s);                                 // B -> A
    if (!u || strcmp(u, "A")) return 2;
    strcpy(s.cur_url, u);
    if (nav_back(&s) != 0) return 3;                  // at the start: Back is a no-op
    u = nav_forward(&s);                              // A -> B
    if (!u || strcmp(u, "B")) return 4;
    strcpy(s.cur_url, u);
    u = nav_forward(&s);                              // B -> C
    if (!u || strcmp(u, "C")) return 5;
    strcpy(s.cur_url, u);
    if (nav_forward(&s) != 0) return 6;               // at the tip: Forward is a no-op
    u = nav_back(&s); strcpy(s.cur_url, u);           // C -> B, Forward stack now holds C
    push_hist(&s, s.cur_url); s.fwd_len = 0; strcpy(s.cur_url, "D");   // B -> D (new nav clears Forward)
    if (s.fwd_len != 0) return 7;
    if (nav_forward(&s) != 0) return 8;               // Forward was discarded by the new navigation
    return 0;
}

// Percent-encode a form value for a URL query (space -> '+', unreserved kept, else %XX).
static void sel_urlencode(char* dst, uint32_t cap, const char* src) {
    uint32_t n = 0; while (src[n]) n++;
    // Route through the shared, KAT'd RFC 3986 form codec (kernel/core/urlcodec.c):
    // byte-for-byte identical to the old inline version (verified), just de-duplicated.
    if (url_form_encode((const uint8_t*)src, n, dst, cap) < 0 && cap) dst[0] = '\0';
}

// Build the URL-encoded "name=value&..." data for the form `form` from its text/hidden fields
// (named controls only; submit buttons excluded). Returns the length written. Shared by the GET
// query and the POST body — so unit-testable without any navigation.
static uint32_t selene_build_query(selene_ctx_t* s, int form, char* query, uint32_t cap) {
    uint32_t q = 0; query[0] = '\0';
    for (int k = 0; k < s->num_fields; k++) {
        sel_field_t* f = &s->fields[k];
        if (f->form != form || f->kind == SEL_FLD_SUBMIT || !f->name[0]) continue;   // named data only
        char en[128], ev[256];
        sel_urlencode(en, sizeof(en), f->name);
        sel_urlencode(ev, sizeof(ev), f->value);
        uint32_t need = (uint32_t)strlen(en) + (uint32_t)strlen(ev) + 2;
        if (q + need >= cap) break;
        if (q) query[q++] = '&';
        for (const char* p = en; *p; p++) query[q++] = *p;
        query[q++] = '=';
        for (const char* p = ev; *p; p++) query[q++] = *p;
        query[q] = '\0';
    }
    return q;
}

// Build the GET submission URL for the form that owns field `fi`: "action?name=value&...". No
// navigation — so it's unit-testable. Empty out if `fi` is out of range.
static void selene_build_submit_url(selene_ctx_t* s, int fi, char* url, uint32_t cap) {
    url[0] = '\0';
    if (fi < 0 || fi >= s->num_fields) return;
    int form = s->fields[fi].form;
    const char* action = (form >= 0 && form < s->num_forms) ? s->forms[form].action : s->cur_url;
    char query[512]; selene_build_query(s, form, query, sizeof(query));
    const char* sep = "?";
    for (const char* p = action; *p; p++) if (*p == '?') { sep = "&"; break; }
    if (query[0]) snprintf(url, cap, "%s%s%s", action, sep, query);
    else          snprintf(url, cap, "%s", action);
}

// Submit the form owning field `fi`. GET forms navigate to "action?query"; POST forms send the
// query as an application/x-www-form-urlencoded request body to the action (over http or TLS).
static void selene_submit(selene_ctx_t* s, int fi) {
    if (fi < 0 || fi >= s->num_fields) return;
    int form = s->fields[fi].form;
    int is_post = (form >= 0 && form < s->num_forms && s->forms[form].method == 1);
    if (is_post) {
        const char* action = (form >= 0 && form < s->num_forms) ? s->forms[form].action : s->cur_url;
        char body[512]; uint32_t bl = selene_build_query(s, form, body, sizeof(body));
        push_hist(s, s->cur_url);
        selene_set_url(s, action);
        selene_load_ex(s, "POST", (const uint8_t*)body, bl);
    } else {
        char url[224];
        selene_build_submit_url(s, fi, url, sizeof(url));
        if (url[0]) selene_follow(s, url);
    }
}

// Create one page context (one tab). The window-level manager is selene_create_ctx below.
static selene_ctx_t* selene_new_ctx(void) {
    selene_ctx_t* s = (selene_ctx_t*)kmalloc(sizeof(selene_ctx_t));
    if (!s) return NULL;
    __builtin_memset(s, 0, sizeof(*s));
    s->lines    = (char(*)[SEL_LINE_COLS])kmalloc(SEL_MAX_LINES * SEL_LINE_COLS);
    s->link_of  = (uint8_t(*)[SEL_LINE_COLS])kmalloc(SEL_MAX_LINES * SEL_LINE_COLS);
    s->field_of = (uint8_t(*)[SEL_LINE_COLS])kmalloc(SEL_MAX_LINES * SEL_LINE_COLS);
    s->links    = (sel_link_t*)kmalloc(SEL_MAX_LINKS * sizeof(sel_link_t));
    s->fields   = (sel_field_t*)kmalloc(SEL_MAX_FIELDS * sizeof(sel_field_t));
    s->forms    = (sel_form_t*)kmalloc(SEL_MAX_FORMS * sizeof(sel_form_t));
    s->img_of   = (uint8_t(*)[SEL_LINE_COLS])kmalloc(SEL_MAX_LINES * SEL_LINE_COLS);
    s->images   = (sel_img_t*)kmalloc(SEL_MAX_IMGS * sizeof(sel_img_t));
    s->color_of = (uint8_t(*)[SEL_LINE_COLS])kmalloc(SEL_MAX_LINES * SEL_LINE_COLS);
    s->bgcolor_of = (uint8_t(*)[SEL_LINE_COLS])kmalloc(SEL_MAX_LINES * SEL_LINE_COLS);
    s->bold_of  = (uint8_t(*)[SEL_LINE_COLS])kmalloc(SEL_MAX_LINES * SEL_LINE_COLS);
    if (!s->lines || !s->link_of || !s->field_of || !s->links || !s->fields || !s->forms ||
        !s->img_of || !s->images || !s->color_of || !s->bgcolor_of || !s->bold_of) {
        if (s->lines) kfree(s->lines);
        if (s->link_of) kfree(s->link_of);
        if (s->field_of) kfree(s->field_of);
        if (s->links) kfree(s->links);
        if (s->fields) kfree(s->fields);
        if (s->forms) kfree(s->forms);
        if (s->img_of) kfree(s->img_of);
        if (s->images) kfree(s->images);
        if (s->color_of) kfree(s->color_of);
        if (s->bgcolor_of) kfree(s->bgcolor_of);
        if (s->bold_of) kfree(s->bold_of);
        kfree(s); return NULL;
    }
    s->sel_field = -1;
    selene_set_url(s, "example.com");
    s->sel_link = -1;
    strncpy(s->status, SELENE_NAME " " SELENE_VERSION " - press Enter to load, or edit the URL", 95);
    return s;
}

static void selene_free_ctx(selene_ctx_t* s) {
    if (!s) return;
    for (int i = 0; i < s->num_imgs; i++) selene_img_free(&s->images[i]);
    kfree(s->lines); kfree(s->link_of); kfree(s->field_of);
    kfree(s->links); kfree(s->fields); kfree(s->forms);
    kfree(s->img_of); kfree(s->images); kfree(s->color_of); kfree(s->bgcolor_of); kfree(s->bold_of); kfree(s);
}

// The window's `reserved` is this manager: an array of tab contexts + the active index.
void* selene_create_ctx(void) {
    selene_tabs_t* T = (selene_tabs_t*)kmalloc(sizeof(selene_tabs_t));
    if (!T) return NULL;
    __builtin_memset(T, 0, sizeof(*T));
    T->tab[0] = selene_new_ctx();
    if (!T->tab[0]) { kfree(T); return NULL; }
    T->ntabs = 1; T->active = 0;
    return T;
}

// window_destroy owns the manager itself; the browser owns each tab and its
// page grids, decoded images and history. Release those before the window goes.
void selene_win_close(window_t* win) {
    selene_tabs_t* T = (selene_tabs_t*)win->reserved;
    if (!T) return;
    for (int i = 0; i < T->ntabs; i++) {
        selene_free_ctx(T->tab[i]);
        T->tab[i] = NULL;
    }
    T->ntabs = 0; T->active = 0;
}

// Open a fresh tab (blank, URL bar pre-filled) and focus it. No-op if the window is full.
static void selene_tab_new(selene_tabs_t* T) {
    if (T->ntabs >= SEL_MAX_TABS) return;
    selene_ctx_t* n = selene_new_ctx();
    if (!n) return;
    T->tab[T->ntabs++] = n;
    T->active = T->ntabs - 1;
}

// Close tab i (freeing its context). Keeps at least one tab; keeps the active view sensible.
static void selene_tab_close(selene_tabs_t* T, int i) {
    if (T->ntabs <= 1 || i < 0 || i >= T->ntabs) return;
    selene_free_ctx(T->tab[i]);
    for (int k = i; k < T->ntabs - 1; k++) T->tab[k] = T->tab[k + 1];
    T->ntabs--;
    if (T->active >= T->ntabs) T->active = T->ntabs - 1;
    else if (T->active > i) T->active--;
}

// A short label for a tab: the page <title>, else the host of its URL, else "New Tab".
static void selene_tab_label(selene_ctx_t* s, char* out, int cap) {
    if (s->title[0]) { strncpy(out, s->title, cap - 1); out[cap - 1] = '\0'; return; }
    if (s->cur_url[0]) {
        const char* p = s->cur_url;
        if (!strncmp(p, "http://", 7)) p += 7; else if (!strncmp(p, "https://", 8)) p += 8;
        int n = 0; while (p[n] && p[n] != '/' && n < cap - 1) { out[n] = p[n]; n++; }
        out[n] = '\0'; if (out[0]) return;
    }
    strncpy(out, "New Tab", cap - 1); out[cap - 1] = '\0';
}

// Tab i's x-offset (from the client left) and drawn width; call with i==ntabs to get the [+] x.
static void selene_tab_geom(int ntabs, int i, int* x, int* w) {
    int avail = SELENE_W - SEL_TABS_NEWW;
    int tw = avail / (ntabs > 0 ? ntabs : 1);
    if (tw > 150) tw = 150;
    if (tw < 24) tw = 24;
    *x = i * tw; *w = tw - 1;
}

// Opening a browser must not wait for DHCP/DNS/TCP while the compositor owns
// input dispatch. The start page is local; Enter explicitly loads the URL bar.
void selene_first_load(window_t* win) {
    selene_tabs_t* T = (selene_tabs_t*)win->reserved;
    if (!T || !T->tab[T->active]) return;
    static const uint8_t page[] =
        "<html><head><title>Welcome to Selene</title></head><body>"
        "<h1>Selene</h1><p>Your NyxOS web browser.</p>"
        "<p>Type an address in the URL bar and press Enter to open a page.</p>"
        "<p>Ctrl+T opens a tab. Ctrl+F searches the current page.</p>"
        "</body></html>";
    selene_ctx_t* s = T->tab[T->active];
    render_html(s, page, sizeof(page) - 1);
    strncpy(s->status, "Ready - type a URL and press Enter", sizeof(s->status) - 1);
}

int selene_start_selftest(void) {
    window_t win = {0};
    win.reserved = selene_create_ctx();
    if (!win.reserved) return 1;
    selene_tabs_t* T = (selene_tabs_t*)win.reserved;
    selene_first_load(&win);     // must render without a network interface or fetch
    selene_ctx_t* s = T->tab[0];
    int rc = 0;
    if (s->num_lines <= 0 || strcmp(s->title, "Welcome to Selene")) rc = 2;
    else if (strcmp(s->url, "example.com") || s->cur_url[0] || s->hist_len) rc = 3;
    else if (s->num_imgs || s->num_links) rc = 4;
    selene_win_close(&win);
    if (T->ntabs || T->tab[0]) rc = 5;
    kfree(T);
    return rc;
}

static int visible_rows(void) {
    return (SELENE_H - SEL_BAR - SEL_TABS_H - SEL_STATUS - SEL_PAD) / SEL_LINE_H;
}

static void clamp_scroll(selene_ctx_t* s) {
    int maxs = s->num_lines - visible_rows();
    if (maxs < 0) maxs = 0;
    if (s->scroll > maxs) s->scroll = maxs;
    if (s->scroll < 0) s->scroll = 0;
}

// First (line,col) where a link/field id appears, packed as line*SEL_LINE_COLS+col (or a large
// sentinel if absent). Lets Tab order focus by document position rather than by array index.
static int link_pos(selene_ctx_t* s, int lk) {
    for (int li = 0; li < s->num_lines; li++)
        for (int c = 0; c < SEL_LINE_COLS; c++)
            if (s->link_of[li][c] == lk) return li * SEL_LINE_COLS + c;
    return 1 << 28;
}
static int field_pos(selene_ctx_t* s, int fk) {
    for (int li = 0; li < s->num_lines; li++)
        for (int c = 0; c < SEL_LINE_COLS; c++)
            if (s->field_of[li][c] == fk) return li * SEL_LINE_COLS + c;
    return 1 << 28;
}

// Advance focus (Tab) in DOCUMENT ORDER: URL bar -> the earliest link/field on the page -> the
// next by position -> ... -> back to the URL bar. So a search box reachable a few Tabs in, not
// after every link. Hidden fields are skipped; the focused item is scrolled into view.
static void select_link(selene_ctx_t* s, int dir) {
    (void)dir;                                          // forward only (Tab), for now
    int cur = -1;                                       // the URL bar precedes all page content
    if (s->sel_field >= 0)     cur = field_pos(s, s->sel_field + 1);
    else if (s->sel_link >= 0) cur = link_pos(s, s->sel_link + 1);

    int best = 1 << 30, best_field = 0, best_idx = -1;  // the focusable with the least pos > cur
    for (int i = 0; i < s->num_links; i++) {
        int p = link_pos(s, i + 1); if (p > cur && p < best) { best = p; best_field = 0; best_idx = i; }
    }
    for (int i = 0; i < s->num_fields; i++) {
        if (s->fields[i].kind == SEL_FLD_HIDDEN) continue;
        int p = field_pos(s, i + 1); if (p > cur && p < best) { best = p; best_field = 1; best_idx = i; }
    }
    if (best_idx < 0) { s->sel_link = -1; s->sel_field = -1; return; }   // past the last -> URL bar
    if (best_field) { s->sel_field = best_idx; s->sel_link = -1; }
    else            { s->sel_link = best_idx; s->sel_field = -1; }
    int line = best / SEL_LINE_COLS, rows = visible_rows();
    if (line < s->scroll) s->scroll = line;
    else if (line >= s->scroll + rows) s->scroll = line - rows + 1;
    clamp_scroll(s);
}

// ---- find-in-page (Ctrl+F) ----------------------------------------------------------

// Case-insensitive: does row `line` contain the query `q` (length ql) starting at column c?
static int find_at(const char* line, int c, const char* q, int ql) {
    for (int k = 0; k < ql; k++) {
        char a = line[c + k], b = q[k];
        if (a == '\0') return 0;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

// Count non-overlapping matches of ctx->find_q across all lines; if wline != NULL, store the
// (line, col) of the `want`-th match (0-based). Returns the total match count.
static int find_scan(selene_ctx_t* s, int want, int* wline, int* wcol) {
    int ql = s->find_len;
    if (ql <= 0) return 0;
    int count = 0;
    for (int li = 0; li < s->num_lines; li++) {
        const char* line = s->lines[li];
        int llen = (int)strlen(line);
        for (int c = 0; c + ql <= llen; c++) {
            if (find_at(line, c, s->find_q, ql)) {
                if (count == want && wline) { *wline = li; *wcol = c; }
                count++;
                c += ql - 1;                          // non-overlapping
            }
        }
    }
    return count;
}

// Scroll the current match into view (centred-ish).
static void find_scroll_to_cur(selene_ctx_t* s) {
    int wl = 0, wc = 0;
    if (s->find_matches > 0 && find_scan(s, s->find_cur, &wl, &wc)) {
        int rows = visible_rows();
        if (wl < s->scroll || wl >= s->scroll + rows) { s->scroll = wl - rows / 2; clamp_scroll(s); }
    }
}
// Recompute the match count after the query changed; reset to the first match.
static void find_recount(selene_ctx_t* s) {
    s->find_matches = find_scan(s, -1, 0, 0);
    if (s->find_cur >= s->find_matches) s->find_cur = 0;
    find_scroll_to_cur(s);
}
// Advance to the next match (wraps).
static void find_next(selene_ctx_t* s) {
    if (s->find_matches <= 0) { find_recount(s); return; }
    s->find_cur = (s->find_cur + 1) % s->find_matches;
    find_scroll_to_cur(s);
}

// A little crescent moon for the toolbar (Selene). Light disc, then carve it with an
// offset disc in the toolbar colour.
static void sel_disc(int ccx, int ccy, int r, uint32_t col) {
    for (int dy = -r; dy <= r; dy++) {
        int w = 0;
        while ((w + 1) * (w + 1) + dy * dy <= r * r) w++;
        fb_fill_rect(ccx - w, ccy + dy, 2 * w + 1, 1, col);
    }
}

// Toolbar geometry, shared by draw + hit-testing.
#define SEL_BACK_X   30
#define SEL_BACK_W   18
#define SEL_FWD_X    (SEL_BACK_X + SEL_BACK_W + 2)   // ">" forward button, just right of Back
#define SEL_FWD_W    18
#define SEL_RELOAD_X (SEL_FWD_X + SEL_FWD_W + 2)     // reload button, just right of Forward
#define SEL_RELOAD_W 18
#define SEL_URL_X    (SEL_RELOAD_X + SEL_RELOAD_W + 4)

// A 12x12 reload glyph: a circular arrow (ring with a top gap + arrowhead). 1 = draw a pixel.
static const uint8_t sel_reload_icon[12][12] = {
    {0,0,0,0,0,0,0,1,0,0,0,0},
    {0,0,0,1,0,0,1,1,1,0,0,0},
    {0,0,1,1,0,0,0,1,1,1,0,0},
    {0,1,1,0,0,0,0,0,1,1,1,0},
    {0,1,0,0,0,0,0,0,0,0,1,0},
    {0,1,0,0,0,0,0,0,0,0,1,0},
    {0,1,0,0,0,0,0,0,0,0,1,0},
    {0,1,0,0,0,0,0,0,0,0,1,0},
    {0,1,1,0,0,0,0,0,0,1,1,0},
    {0,0,1,1,0,0,0,0,1,1,0,0},
    {0,0,0,1,1,1,1,1,1,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
};

void selene_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    (void)cw; (void)ch;
    selene_tabs_t* T = (selene_tabs_t*)win->reserved;
    if (!T) return;
    selene_ctx_t* s = T->tab[T->active];
    if (!s) return;

    uint32_t bar = fb_rgb(46, 40, 70);                        // brand-purple toolbar
    fb_fill_rect(cx, cy, SELENE_W, SEL_BAR, bar);
    sel_disc(cx + 15, cy + SEL_BAR/2, 8, fb_rgb(232, 230, 245));  // moon
    sel_disc(cx + 19, cy + SEL_BAR/2 - 1, 7, bar);               // carve crescent

    // Back "<" and Forward ">" buttons — each bright when its stack has somewhere to go, dim otherwise.
    uint32_t bcol = s->hist_len > 0 ? fb_rgb(220, 215, 240) : fb_rgb(96, 90, 120);
    fb_fill_rect(cx + SEL_BACK_X, cy + 7, SEL_BACK_W, SEL_BAR - 14, fb_rgb(30, 26, 46));
    font_draw_string(cx + SEL_BACK_X + 5, cy + (SEL_BAR - FONT_HEIGHT)/2, "<", bcol, fb_rgb(30, 26, 46));
    uint32_t fcol = s->fwd_len > 0 ? fb_rgb(220, 215, 240) : fb_rgb(96, 90, 120);
    fb_fill_rect(cx + SEL_FWD_X, cy + 7, SEL_FWD_W, SEL_BAR - 14, fb_rgb(30, 26, 46));
    font_draw_string(cx + SEL_FWD_X + 5, cy + (SEL_BAR - FONT_HEIGHT)/2, ">", fcol, fb_rgb(30, 26, 46));

    // Reload button — bright once a page is loaded; re-fetches the current URL (F5 / Ctrl+R too).
    uint32_t rcol = s->cur_url[0] ? fb_rgb(220, 215, 240) : fb_rgb(96, 90, 120);
    fb_fill_rect(cx + SEL_RELOAD_X, cy + 7, SEL_RELOAD_W, SEL_BAR - 14, fb_rgb(30, 26, 46));
    {
        int ix = cx + SEL_RELOAD_X + (SEL_RELOAD_W - 12) / 2, iy = cy + (SEL_BAR - 12) / 2;
        for (int yy = 0; yy < 12; yy++)
            for (int xx = 0; xx < 12; xx++)
                if (sel_reload_icon[yy][xx]) fb_put_pixel(ix + xx, iy + yy, rcol);
    }

    int ux = cx + SEL_URL_X, uw = SELENE_W - SEL_URL_X - SEL_PAD, uy = cy + 6, uh = SEL_BAR - 12;
    fb_fill_rect(ux, uy, uw, uh, fb_rgb(22, 22, 30));         // URL box
    fb_fill_rect(ux, uy, uw, 1, fb_rgb(90, 80, 120));
    int uty = uy + (uh - FONT_HEIGHT) / 2;
    font_draw_string(ux + 6, uty, s->url, fb_rgb(235, 235, 245), fb_rgb(22, 22, 30));
    if (s->sel_link < 0) {                                   // caret only while editing the URL
        int caret_x = ux + 6 + s->url_len * FONT_WIDTH;
        if (caret_x < ux + uw - 2) fb_fill_rect(caret_x, uty, 2, FONT_HEIGHT, fb_rgb(180, 160, 230));
    }

    // --- tab strip (below the toolbar): one button per tab + a [+] new-tab button ---
    int tby = cy + SEL_BAR;
    fb_fill_rect(cx, tby, SELENE_W, SEL_TABS_H, fb_rgb(34, 30, 50));
    for (int t = 0; t < T->ntabs; t++) {
        int tx, tw; selene_tab_geom(T->ntabs, t, &tx, &tw);
        int act = (t == T->active);
        uint32_t tb = act ? fb_rgb(248, 248, 250) : fb_rgb(58, 50, 82);
        fb_fill_rect(cx + tx, tby + 2, tw, SEL_TABS_H - 2, tb);
        if (act) fb_fill_rect(cx + tx, tby, tw, 2, fb_rgb(150, 120, 220));   // active-tab accent
        char lbl[24]; selene_tab_label(T->tab[t], lbl, sizeof(lbl));
        int maxc = (tw - 20) / FONT_WIDTH; if (maxc < 0) maxc = 0; if (maxc > 22) maxc = 22;
        char show[24]; int z = 0; for (; z < maxc && lbl[z]; z++) show[z] = lbl[z]; show[z] = '\0';
        uint32_t fg = act ? fb_rgb(30, 30, 45) : fb_rgb(210, 205, 225);
        font_draw_string(cx + tx + 6, tby + (SEL_TABS_H - FONT_HEIGHT) / 2, show, fg, tb);
        if (tw > 50) font_draw_string(cx + tx + tw - 13, tby + (SEL_TABS_H - FONT_HEIGHT) / 2, "x",
                                      act ? fb_rgb(150, 110, 150) : fb_rgb(180, 170, 200), tb);
    }
    int nbx, nbw; selene_tab_geom(T->ntabs, T->ntabs, &nbx, &nbw);
    if (nbx > SELENE_W - SEL_TABS_NEWW) nbx = SELENE_W - SEL_TABS_NEWW;
    fb_fill_rect(cx + nbx, tby + 2, SEL_TABS_NEWW - 2, SEL_TABS_H - 2, fb_rgb(58, 50, 82));
    font_draw_string(cx + nbx + 8, tby + (SEL_TABS_H - FONT_HEIGHT) / 2, "+", fb_rgb(220, 215, 235), fb_rgb(58, 50, 82));

    int cyy = cy + SEL_BAR + SEL_TABS_H;
    int content_h = SELENE_H - SEL_BAR - SEL_TABS_H - SEL_STATUS;
    uint32_t pg = (s->page_bg && s->page_bg <= s->npalette) ? s->palette[s->page_bg - 1] : fb_rgb(248, 248, 250);   // <body bgcolor> page background, else light
    fb_fill_rect(cx, cyy, SELENE_W, content_h, pg);          // page

    int rows = visible_rows();
    int find_wl = -1, find_wc = -1;                          // the current find match's (line,col)
    if (s->find_active && s->find_len > 0 && s->find_matches > 0) find_scan(s, s->find_cur, &find_wl, &find_wc);
    int box_depth = 0; uint8_t boxcol[SEL_BOX_MAXDEPTH];     // bordered-<div> box nesting + per-level stroke colour at the first visible line: replay the box markers scrolled off the top
    for (int q = 0; q < s->scroll && q < s->num_lines; q++) {
        uint8_t lr = s->line_rule[q];
        if (lr == SEL_BOX_TOP) { if (box_depth < SEL_BOX_MAXDEPTH) boxcol[box_depth] = s->color_of[q][0]; box_depth++; }
        else if (lr == SEL_BOX_BOT && box_depth > 0) box_depth--;
    }
    uint32_t tintpx = sel_tint_px(pg);                       // subtle panel fill for a TOP-LEVEL bordered box (matches the in-cell band tint; only used while box_depth>0)
    for (int r = 0; r < rows; r++) {
        int idx = s->scroll + r;
        if (idx >= s->num_lines) break;
        int py = cyy + SEL_PAD + r * SEL_LINE_H;
        uint8_t lrb = s->line_rule[idx];
        if (lrb == SEL_BOX_TOP || lrb == SEL_BOX_BOT) {          // bordered-<div> box edge marker: draw the outline (top/bottom rule + this row's side verticals), no text
            int lvl = (lrb == SEL_BOX_TOP) ? box_depth : box_depth - 1;   // TOP uses the depth we enter; BOTTOM the depth we leave
            if (lvl < 0) lvl = 0;
            int inset = lvl * SEL_BOX_INSET;
            int bx0 = cx + SEL_PAD - 3 + inset, bx1 = cx + SELENE_W - SEL_PAD + 2 - inset;
            int vy = py - (SEL_LINE_H - FONT_HEIGHT) / 2;                 // row top; vertical spans SEL_LINE_H so rows join seamlessly
            if (bx1 - bx0 > 1) fb_fill_rect(bx0 + 1, vy, (uint32_t)(bx1 - bx0 - 1), SEL_LINE_H, tintpx);   // panel fill on the marker (padding) row, behind the outline
            uint8_t bck = s->color_of[idx][0];                           // the marker carries its box's border colour in the colour slot (0 = grey default)
            uint32_t boxc = (bck && bck <= s->npalette) ? s->palette[bck - 1] : fb_rgb(120, 128, 150);
            fb_fill_rect(bx0, vy, 1, SEL_LINE_H, boxc); fb_fill_rect(bx1, vy, 1, SEL_LINE_H, boxc);
            int ey = (lrb == SEL_BOX_TOP) ? vy : vy + SEL_LINE_H - 1;     // horizontal edge at the row top (open) or bottom (close)
            fb_fill_rect(bx0, ey, (uint32_t)(bx1 - bx0 + 1), 1, boxc);
            if (lrb == SEL_BOX_TOP) { if (box_depth < SEL_BOX_MAXDEPTH) boxcol[box_depth] = bck; box_depth++; }   // push this box's colour for the content rows' verticals
            else if (box_depth > 0) box_depth--;
            continue;
        }
        if (box_depth > 0) {                                     // a normal line inside a bordered box: fill the panel interior (behind the text) + draw the side verticals in the box's colour
            int inset = (box_depth - 1) * SEL_BOX_INSET;
            int bx0 = cx + SEL_PAD - 3 + inset, bx1 = cx + SELENE_W - SEL_PAD + 2 - inset;
            int vy = py - (SEL_LINE_H - FONT_HEIGHT) / 2;
            if (bx1 - bx0 > 1) fb_fill_rect(bx0 + 1, vy, (uint32_t)(bx1 - bx0 - 1), SEL_LINE_H, tintpx);   // subtle panel fill; the text below draws with the same tint as its bg so it blends
            uint8_t bck = (box_depth - 1 < SEL_BOX_MAXDEPTH) ? boxcol[box_depth - 1] : 0;
            uint32_t boxc = (bck && bck <= s->npalette) ? s->palette[bck - 1] : fb_rgb(120, 128, 150);
            fb_fill_rect(bx0, vy, 1, SEL_LINE_H, boxc); fb_fill_rect(bx1, vy, 1, SEL_LINE_H, boxc);
        }
        if (s->line_rule[idx]) {                                 // <hr>: a real 2px rule; width% = line_rule, alignment = line_align (no text/overlays on this line)
            int avail = SELENE_W - 2 * SEL_PAD;
            int pct = s->line_rule[idx]; if (pct > 100) pct = 100;
            int rw = avail * pct / 100; if (rw < 1) rw = 1;
            int rx0 = cx + SEL_PAD;
            if (s->line_align[idx] == 1) rx0 += (avail - rw) / 2;    // centre
            else if (s->line_align[idx] == 2) rx0 += (avail - rw);   // right
            uint8_t hc = s->color_of[idx][0];                        // rule colour (marker char's colour slot); 0 = default grey
            uint32_t rulecol = (hc && hc <= s->npalette) ? s->palette[hc - 1] : fb_rgb(150, 154, 168);
            int hsz = s->bgcolor_of[idx][0]; if (hsz < 1) hsz = 2; if (hsz > 8) hsz = 8;   // <hr size> thickness (px), carried in the marker's bg slot
            fb_fill_rect(rx0, py + FONT_HEIGHT / 2 - hsz / 2, (uint32_t)rw, (uint32_t)hsz, rulecol);
            continue;
        }
        // text-align: shift the whole line right by lpad for centre/right (line_align 0=left, unchanged)
        int lpad = 0;
        if (s->line_align[idx]) {
            int avail = SELENE_W - 2 * SEL_PAD;
            int lw = (int)strlen(s->lines[idx]) * FONT_WIDTH;
            if (s->line_align[idx] == 1) lpad = (avail - lw) / 2;
            else if (s->line_align[idx] == 2) lpad = avail - lw;
            if (lpad < 0) lpad = 0;
        }
        // base text, drawn in per-run inline-CSS colours: color_of foreground + bgcolor_of background
        // (0 = the default text colour / page background). A run breaks where either fg or bg changes.
        {
            int blen = (int)strlen(s->lines[idx]);
            int b0 = 0;
            while (b0 < blen) {
                uint8_t ck = s->color_of[idx][b0], bk = s->bgcolor_of[idx][b0], bd = s->bold_of[idx][b0];
                int b1 = b0; while (b1 < blen && s->color_of[idx][b1] == ck && s->bgcolor_of[idx][b1] == bk
                                              && s->bold_of[idx][b1] == bd) b1++;
                uint32_t fg = (ck && ck <= s->npalette) ? s->palette[ck - 1]
                              : (s->page_fg && s->page_fg <= s->npalette) ? s->palette[s->page_fg - 1] : fb_rgb(28, 30, 40);   // <body text> default fg, else dark
                uint32_t bg = (bk && bk <= s->npalette) ? s->palette[bk - 1] : (box_depth > 0 ? tintpx : pg);   // inside a top-level bordered box, un-styled text sits on the panel tint (matches the interior fill)
                char sub[SEL_LINE_COLS]; int k = 0;
                for (; k < b1 - b0 && k < SEL_LINE_COLS - 1; k++) sub[k] = s->lines[idx][b0 + k];
                sub[k] = '\0';
                int rx = cx + SEL_PAD + lpad + b0 * FONT_WIDTH;
                int vo = (bd >> 4) & 3;                                    // bits 4-5: 0 = normal, 1 = subscript, 2 = superscript
                int ry = py + (vo == 1 ? FONT_HEIGHT / 4 : vo == 2 ? -(FONT_HEIGHT / 4) : 0);  // <sub>/<sup> vertical shift (same-size glyph)
                font_draw_string(rx, ry, sub, fg, bg);
                if (bd & 1) font_draw_string_trans(rx + 1, ry, sub, fg);   // bit0: synthetic bold (2nd glyph pass, +1px)
                if (bd & 2) fb_fill_rect(rx, ry + FONT_HEIGHT - 1, (uint32_t)((b1 - b0) * FONT_WIDTH), 1, fg);  // bit1: underline
                if (bd & 4) fb_fill_rect(rx, ry + FONT_HEIGHT / 2, (uint32_t)((b1 - b0) * FONT_WIDTH), 1, fg);  // bit2: line-through
                if (bd & 8) { int uw2 = (b1 - b0) * FONT_WIDTH; for (int dx = 0; dx < uw2; dx += 2) fb_fill_rect(rx + dx, ry + FONT_HEIGHT - 1, 1, 1, fg); }  // bit3: dotted underline (<abbr>)
                if (bd & 64) fb_fill_rect(rx, ry, (uint32_t)((b1 - b0) * FONT_WIDTH), 1, fg);  // bit6: overline (rule at glyph top)
                b0 = b1;
            }
        }

        // overlay link runs on this line: colour + underline, highlight the selected one
        int llen = (int)strlen(s->lines[idx]);
        int c0 = 0;
        while (c0 < llen) {
            uint8_t lk = s->link_of[idx][c0];
            int c1 = c0; while (c1 < llen && s->link_of[idx][c1] == lk) c1++;
            if (lk != 0) {
                int px = cx + SEL_PAD + lpad + c0 * FONT_WIDTH;
                int wpx = (c1 - c0) * FONT_WIDTH;
                int seld = (lk == s->sel_link + 1);
                uint32_t fg = seld ? fb_rgb(20, 20, 45) : fb_rgb(48, 96, 210);
                uint32_t bg = seld ? fb_rgb(196, 208, 255) : (box_depth > 0 ? tintpx : pg);   // a link inside a top-level box sits on the panel tint too
                if (seld) fb_fill_rect(px - 1, py - 1, wpx + 2, FONT_HEIGHT + 2, bg);
                char sub[SEL_LINE_COLS];
                int k = 0; for (; k < c1 - c0; k++) sub[k] = s->lines[idx][c0 + k];
                sub[k] = '\0';
                font_draw_string(px, py, sub, fg, bg);
                fb_fill_rect(px, py + FONT_HEIGHT - 1, wpx, 1, fg);   // underline
            }
            c0 = c1;
        }

        // overlay form-field runs on this line: editable text boxes and submit buttons
        c0 = 0;
        while (c0 < llen) {
            uint8_t fk = s->field_of[idx][c0];
            int c1f = c0; while (c1f < llen && s->field_of[idx][c1f] == fk) c1f++;
            if (fk != 0 && fk - 1 < s->num_fields) {
                sel_field_t* f = &s->fields[fk - 1];
                int px = cx + SEL_PAD + lpad + c0 * FONT_WIDTH;
                int wpx = (c1f - c0) * FONT_WIDTH;
                int focused = (s->sel_field == fk - 1);
                if (f->kind == SEL_FLD_TEXT) {
                    uint32_t box = focused ? fb_rgb(255, 255, 255) : fb_rgb(232, 234, 244);
                    uint32_t brd = focused ? fb_rgb(120, 90, 210) : fb_rgb(96, 104, 130);
                    fb_fill_rect(px, py - 1, wpx, FONT_HEIGHT + 2, box);
                    fb_fill_rect(px, py - 1, wpx, 1, brd); fb_fill_rect(px, py + FONT_HEIGHT, wpx, 1, brd);
                    fb_fill_rect(px, py - 1, 1, FONT_HEIGHT + 2, brd); fb_fill_rect(px + wpx - 1, py - 1, 1, FONT_HEIGHT + 2, brd);
                    int maxc = (wpx - 6) / FONT_WIDTH; if (maxc < 0) maxc = 0;
                    int vlen = (int)strlen(f->value);
                    int start = (vlen > maxc) ? vlen - maxc : 0;         // scroll to show the tail
                    char vis[SEL_LINE_COLS]; int vl = 0;
                    for (int z = start; f->value[z] && vl < SEL_LINE_COLS - 1; z++) vis[vl++] = f->value[z];
                    vis[vl] = '\0';
                    font_draw_string(px + 3, py, vis, fb_rgb(20, 20, 30), box);
                    if (focused) { int caret = px + 3 + vl * FONT_WIDTH;
                        if (caret < px + wpx - 2) fb_fill_rect(caret, py, 1, FONT_HEIGHT, fb_rgb(120, 90, 210)); }
                } else if (f->kind == SEL_FLD_SUBMIT) {
                    // Honour the button's own CSS background/colour (carried in bgcolor_of/color_of at the
                    // pill's first cell); fall back to the theme purple pill with light text when unstyled.
                    uint8_t bbk = s->bgcolor_of[idx][c0], ffk = s->color_of[idx][c0];
                    uint32_t bg = focused ? fb_rgb(120, 90, 210)
                                : (bbk && bbk <= s->npalette) ? s->palette[bbk - 1] : fb_rgb(90, 80, 130);
                    uint32_t txc = (ffk && ffk <= s->npalette) ? s->palette[ffk - 1] : fb_rgb(240, 240, 250);
                    fb_fill_rect(px, py - 1, wpx, FONT_HEIGHT + 2, bg);
                    char sub[SEL_LINE_COLS]; int k = 0;
                    for (; k < c1f - c0 && k < SEL_LINE_COLS - 1; k++) sub[k] = s->lines[idx][c0 + k];
                    sub[k] = '\0';
                    font_draw_string(px, py, sub, txc, bg);
                }
            }
            c0 = c1f;
        }

        // (images are drawn as block boxes in a separate pass after this line loop, below)

        // find-in-page: highlight every match on this line, the current one accented orange
        if (s->find_active && s->find_len > 0) {
            int flen = (int)strlen(s->lines[idx]);
            for (int c = 0; c + s->find_len <= flen; c++) {
                if (find_at(s->lines[idx], c, s->find_q, s->find_len)) {
                    int px = cx + SEL_PAD + lpad + c * FONT_WIDTH, wpx = s->find_len * FONT_WIDTH;
                    int is_cur = (idx == find_wl && c == find_wc);
                    uint32_t bg = is_cur ? fb_rgb(255, 158, 40) : fb_rgb(250, 236, 130);
                    fb_fill_rect(px, py - 1, wpx, FONT_HEIGHT + 2, bg);
                    char sub[SEL_LINE_COLS]; int k = 0;
                    for (; k < s->find_len && k < SEL_LINE_COLS - 1; k++) sub[k] = s->lines[idx][c + k];
                    sub[k] = '\0';
                    font_draw_string(px, py, sub, fb_rgb(30, 25, 10), bg);
                    c += s->find_len - 1;
                }
            }
        }
    }
    if (s->num_lines == 0)
        font_draw_string(cx + SEL_PAD, cyy + SEL_PAD, "(no page loaded)", fb_rgb(150,150,160), pg);

    // Image blocks: draw each <img> as a box. Decoded images are scaled (nearest-neighbour) and
    // alpha-composited over the page; the rest fall back to a framed "[img: alt]" placeholder. Only
    // fully-visible boxes are drawn (keeps the blit unclipped); a box scrolls in/out as a whole.
    {
        int BW = SEL_IMG_BOX_W * FONT_WIDTH, BH = SEL_IMG_BOX_LINES * SEL_LINE_H;
        for (int im = 0; im < s->num_imgs; im++) {
            int aline = -1;
            for (int li = s->scroll; li < s->num_lines && li < s->scroll + rows; li++)
                if (s->img_of[li][0] == im + 1) { aline = li; break; }   // block images anchor at col 0
            if (aline < 0) continue;
            int bx = cx + SEL_PAD, by = cyy + SEL_PAD + (aline - s->scroll) * SEL_LINE_H - 1;
            if (by < cyy || by + BH > cyy + content_h) continue;         // draw only when fully in view
            sel_img_t* mi = &s->images[im];
            if (mi->px && mi->iw && mi->ih) {
                int dW, dH;                                              // aspect-fit into the box
                if ((int)mi->iw * BH >= (int)mi->ih * BW) { dW = BW; dH = (int)mi->ih * BW / (int)mi->iw; }
                else { dH = BH; dW = (int)mi->iw * BH / (int)mi->ih; }
                if (dW < 1) dW = 1;
                if (dH < 1) dH = 1;
                int ox = bx + (BW - dW) / 2, oy = by + (BH - dH) / 2;
                fb_fill_rect(bx, by, BW, BH, pg);                        // letterbox background
                for (int dy = 0; dy < dH; dy++) {
                    const uint8_t* srow = mi->px + (uint64_t)(dy * (int)mi->ih / dH) * mi->iw * 4;
                    for (int dx = 0; dx < dW; dx++) {
                        const uint8_t* sp = srow + (uint64_t)(dx * (int)mi->iw / dW) * 4;
                        uint32_t col;
                        if (sp[3] >= 250) col = fb_rgb(sp[0], sp[1], sp[2]);
                        else { int a = sp[3];
                            col = fb_rgb((sp[0]*a + 248*(255-a))/255, (sp[1]*a + 248*(255-a))/255, (sp[2]*a + 250*(255-a))/255); }
                        fb_put_pixel(ox + dx, oy + dy, col);
                    }
                }
            } else {                                                    // fallback placeholder
                fb_fill_rect(bx, by, BW, BH, fb_rgb(226, 230, 240));
                fb_fill_rect(bx, by, 3, BH, fb_rgb(120, 90, 210));       // purple media accent
                font_draw_string(bx + 8, by + BH/2 - FONT_HEIGHT/2, s->lines[aline], fb_rgb(60, 70, 100), fb_rgb(226, 230, 240));
            }
            uint32_t brd = fb_rgb(120, 130, 165);                       // border
            fb_fill_rect(bx, by, BW, 1, brd); fb_fill_rect(bx, by + BH - 1, BW, 1, brd);
            fb_fill_rect(bx, by, 1, BH, brd); fb_fill_rect(bx + BW - 1, by, 1, BH, brd);
        }
    }

    // in-cell bordered-<div> outlines: stroke each recorded cell box (top+bottom rule + side verticals), clipped to the content viewport
    for (int b = 0; b < s->num_cell_boxes; b++) {
        if (!s->cell_boxes[b].used) continue;
        int l0 = s->cell_boxes[b].line0, l1 = s->cell_boxes[b].line1;
        if (l1 < s->scroll || l0 >= s->scroll + rows) continue;         // fully scrolled out of view
        int ctop = cyy, cbot = cyy + content_h;
        int y0 = cyy + SEL_PAD + (l0 - s->scroll) * SEL_LINE_H - (SEL_LINE_H - FONT_HEIGHT) / 2;
        int y1 = cyy + SEL_PAD + (l1 - s->scroll) * SEL_LINE_H - (SEL_LINE_H - FONT_HEIGHT) / 2 + SEL_LINE_H;
        int cy0 = y0 < ctop ? ctop : y0, cy1 = y1 > cbot ? cbot : y1;   // vertical span clipped to the content area
        if (cy1 <= cy0) continue;
        int x0 = cx + SEL_PAD + (int)s->cell_boxes[b].col0 * FONT_WIDTH - 2;
        int x1 = cx + SEL_PAD + ((int)s->cell_boxes[b].col1 + 1) * FONT_WIDTH + 1;
        if (x0 < cx) x0 = cx;
        if (x1 > cx + SELENE_W - 2) x1 = cx + SELENE_W - 2;
        uint8_t ck = s->cell_boxes[b].col;
        uint32_t bc = (ck && ck <= s->npalette) ? s->palette[ck - 1] : fb_rgb(120, 128, 150);
        fb_fill_rect(x0, cy0, 1, (uint32_t)(cy1 - cy0), bc); fb_fill_rect(x1, cy0, 1, (uint32_t)(cy1 - cy0), bc);   // side verticals
        if (y0 >= ctop && y0 < cbot) fb_fill_rect(x0, y0, (uint32_t)(x1 - x0 + 1), 1, bc);          // top edge (top row visible)
        if (y1 - 1 >= ctop && y1 - 1 < cbot) fb_fill_rect(x0, y1 - 1, (uint32_t)(x1 - x0 + 1), 1, bc);   // bottom edge
    }

    if (s->num_lines > rows) {                               // scrollbar
        int track_h = content_h - 4;
        int thumb_h = track_h * rows / s->num_lines; if (thumb_h < 12) thumb_h = 12;
        int maxs = s->num_lines - rows;
        int thumb_y = cyy + 2 + (maxs ? (track_h - thumb_h) * s->scroll / maxs : 0);
        fb_fill_rect(cx + SELENE_W - 5, cyy + 2, 3, track_h, fb_rgb(225, 225, 232));
        fb_fill_rect(cx + SELENE_W - 5, thumb_y, 3, thumb_h, fb_rgb(150, 130, 200));
    }

    // status: the find bar (when active) else the selected link's target else the page status
    int sy = cy + SELENE_H - SEL_STATUS;
    if (s->find_active) {
        fb_fill_rect(cx, sy, SELENE_W, SEL_STATUS, fb_rgb(58, 48, 78));
        char fbuf[128];
        snprintf(fbuf, sizeof(fbuf), "Find: %s_   [%d/%d]   Enter=next  Esc=close",
                 s->find_q, s->find_matches ? s->find_cur + 1 : 0, s->find_matches);
        font_draw_string(cx + 6, sy + 3, fbuf, fb_rgb(255, 238, 180), fb_rgb(58, 48, 78));
    } else {
        fb_fill_rect(cx, sy, SELENE_W, SEL_STATUS, fb_rgb(32, 30, 44));
        const char* st = (s->sel_link >= 0) ? s->links[s->sel_link].url : s->status;
        font_draw_string(cx + 6, sy + 3, st, fb_rgb(200, 200, 220), fb_rgb(32, 30, 44));
    }
}

void selene_win_key(window_t* win, int key) {
    selene_tabs_t* T = (selene_tabs_t*)win->reserved;
    if (!T) return;
    if (key == 0x14) { selene_tab_new(T); return; }                       // Ctrl+T: new tab
    if (key == 0x17) { selene_tab_close(T, T->active); return; }           // Ctrl+W: close tab
    if (key == '\t' && is_ctrl_pressed()) { T->active = (T->active + 1) % T->ntabs; return; }  // Ctrl+Tab: next tab
    selene_ctx_t* s = T->tab[T->active];
    if (!s) return;
    int rows = visible_rows();

    if (key == KEY_F5 || key == 0x12) { selene_reload(s); return; }    // F5 / Ctrl+R: reload the page
    if (key == 0x06) { s->find_active = 1; find_recount(s); return; }   // Ctrl+F: open/refresh find
    if (s->find_active) {                                    // the find bar captures keystrokes
        if (key == 0x1B) { s->find_active = 0; return; }                 // Esc: close find
        if (key == '\n' || key == '\r') { find_next(s); return; }        // Enter: next match
        if (key == '\b' || key == 0x7F) {                                // Backspace: edit query
            if (s->find_len > 0) s->find_q[--s->find_len] = '\0';
            find_recount(s); return;
        }
        if (key >= 0x20 && key < 0x7F) {                                 // type into the query
            if (s->find_len < (int)sizeof(s->find_q) - 1) { s->find_q[s->find_len++] = (char)key; s->find_q[s->find_len] = '\0'; }
            find_recount(s); return;
        }
        // arrows / PgUp / PgDn fall through so the page still scrolls while finding
    }

    if (key == '\t') { select_link(s, +1); return; }         // Tab: next link/field (wraps to URL bar)
    if (key == 0x1B) { s->sel_link = -1; s->sel_field = -1; return; }   // Esc: back to the URL bar

    int in_text = (s->sel_field >= 0 && s->sel_field < s->num_fields &&
                   s->fields[s->sel_field].kind == SEL_FLD_TEXT);

    if (key == '\n' || key == '\r') {
        if (s->sel_field >= 0 && s->sel_field < s->num_fields) selene_submit(s, s->sel_field);  // submit the form
        else if (s->sel_link >= 0 && s->sel_link < s->num_links) selene_follow(s, s->links[s->sel_link].url);
        else selene_go(s);
        clamp_scroll(s);
        return;
    }
    if (key == '\b' || key == 0x7F) {
        if (in_text) {                                       // editing a text field: delete a char
            char* v = s->fields[s->sel_field].value; int vl = (int)strlen(v);
            if (vl > 0) v[vl - 1] = '\0';
        }
        // else: Back when a link is selected or the URL bar is untouched (still the current
        // page); once you start editing the URL, Backspace deletes a character instead.
        else if (s->sel_link >= 0 || strcmp(s->url, s->cur_url) == 0) selene_back(s);
        else if (s->url_len > 0) s->url[--s->url_len] = '\0';
        return;
    }

    if (key == KEY_UP || key == KEY_WHEEL_UP)     { s->scroll -= 3; selene_after_scroll(s); return; }
    if (key == KEY_DOWN || key == KEY_WHEEL_DOWN) { s->scroll += 3; selene_after_scroll(s); return; }
    if (key == KEY_PGUP && is_ctrl_pressed()) { T->active = (T->active + T->ntabs - 1) % T->ntabs; return; }  // Ctrl+PgUp: prev tab
    if (key == KEY_PGDN && is_ctrl_pressed()) { T->active = (T->active + 1) % T->ntabs; return; }             // Ctrl+PgDn: next tab
    if (key == KEY_PGUP) { s->scroll -= (rows - 1); selene_after_scroll(s); return; }
    if (key == KEY_PGDN) { s->scroll += (rows - 1); selene_after_scroll(s); return; }
    if (key == KEY_HOME) { s->scroll = 0; selene_after_scroll(s); return; }
    if (key == KEY_END)  { s->scroll = s->num_lines; selene_after_scroll(s); return; }

    if (key >= 0x20 && key < 0x7F) {                          // printable character
        if (in_text) {                                        // type into the focused text field
            char* v = s->fields[s->sel_field].value;
            int vl = (int)strlen(v);
            if (vl < (int)sizeof(s->fields[s->sel_field].value) - 1) { v[vl] = (char)key; v[vl + 1] = '\0'; }
        } else {                                              // otherwise edit the URL bar
            s->sel_link = -1; s->sel_field = -1;
            if (s->url_len < (int)sizeof(s->url) - 1) {
                s->url[s->url_len++] = (char)key;
                s->url[s->url_len] = '\0';
            }
        }
    }
}

// Mouse click: the Back button, the URL bar (focus it), or a link in the page.
void selene_win_click(window_t* win, int mx, int my, int btn) {
    (void)btn;
    selene_tabs_t* T = (selene_tabs_t*)win->reserved;
    if (!T) return;
    selene_ctx_t* s = T->tab[T->active];
    if (!s) return;
    int cx = WIN_CLIENT_X(win), cy = WIN_CLIENT_Y(win);

    if (my >= cy && my < cy + SEL_BAR) {                      // toolbar
        if (mx >= cx + SEL_BACK_X && mx < cx + SEL_BACK_X + SEL_BACK_W) { selene_back(s); return; }
        if (mx >= cx + SEL_FWD_X  && mx < cx + SEL_FWD_X  + SEL_FWD_W)  { selene_forward(s); return; }
        if (mx >= cx + SEL_RELOAD_X && mx < cx + SEL_RELOAD_X + SEL_RELOAD_W) { selene_reload(s); return; }
        s->sel_link = -1;                                    // clicking the URL bar edits it
        return;
    }
    int tby = cy + SEL_BAR;                                   // tab strip: switch tab / close 'x' / [+] new
    if (my >= tby && my < tby + SEL_TABS_H) {
        int rx = mx - cx;
        int nbx, nbw; selene_tab_geom(T->ntabs, T->ntabs, &nbx, &nbw);
        if (nbx > SELENE_W - SEL_TABS_NEWW) nbx = SELENE_W - SEL_TABS_NEWW;
        if (rx >= nbx && rx < nbx + SEL_TABS_NEWW) { selene_tab_new(T); return; }
        for (int t = 0; t < T->ntabs; t++) {
            int tx, tw; selene_tab_geom(T->ntabs, t, &tx, &tw);
            if (rx >= tx && rx < tx + tw) {
                if (tw > 50 && rx >= tx + tw - 16) selene_tab_close(T, t);   // clicked the tab's 'x'
                else T->active = t;
                return;
            }
        }
        return;
    }
    int cyy = cy + SEL_BAR + SEL_TABS_H;
    int content_h = SELENE_H - SEL_BAR - SEL_TABS_H - SEL_STATUS;
    if (my < cyy || my >= cyy + content_h) return;
    int row = (my - cyy - SEL_PAD) / SEL_LINE_H;
    int col = (mx - cx - SEL_PAD) / FONT_WIDTH;
    int idx = s->scroll + row;
    if (idx < 0 || idx >= s->num_lines || col < 0 || col >= SEL_LINE_COLS) return;
    uint8_t lk = s->link_of[idx][col];
    if (lk != 0 && lk - 1 < s->num_links) { s->sel_link = lk - 1; s->sel_field = -1; selene_follow(s, s->links[lk-1].url); return; }
    uint8_t fk = s->field_of[idx][col];                      // a form control?
    if (fk != 0 && fk - 1 < s->num_fields) {
        int fi = fk - 1;
        if (s->fields[fi].kind == SEL_FLD_SUBMIT) selene_submit(s, fi);
        else if (s->fields[fi].kind == SEL_FLD_TEXT) { s->sel_field = fi; s->sel_link = -1; }
    }
}

// ---- HTML-forms known-answer self-test (`formtest`) --------------------------------------
// Parse a known form, then build its GET submission URL. Pure logic — no network, no window.
int selene_form_selftest(void) {
    int pass = 0, total = 0;
    selene_ctx_t* s = selene_new_ctx();
    if (!s) { printf("selene-form: context alloc failed\n"); return -1; }
    strncpy(s->base_host, "example.com", sizeof(s->base_host)-1); s->base_host[sizeof(s->base_host)-1] = '\0';
    s->base_https = 0; s->base_port = 80;
    strncpy(s->base_path, "/", sizeof(s->base_path)-1); s->base_path[sizeof(s->base_path)-1] = '\0';
    strncpy(s->cur_url, "http://example.com/", sizeof(s->cur_url)-1); s->cur_url[sizeof(s->cur_url)-1] = '\0';

    static const char* HTML =
        "<h1>Search</h1>"
        "<form action=\"/search\" method=\"get\">"
        "<input type=\"text\" name=\"q\" value=\"\">"
        "<input type=\"hidden\" name=\"lang\" value=\"en\">"
        "<input type=\"submit\" value=\"Search\">"
        "</form>";
    render_html(s, (const uint8_t*)HTML, (uint32_t)strlen(HTML));

    total++;
    if (s->num_forms == 1 && s->forms[0].method == 0 && sel_streq(s->forms[0].action, "http://example.com/search"))
        { pass++; printf("selene-form: form parsed (GET action=%s) PASS\n", s->forms[0].action); }
    else printf("selene-form: form parse FAIL (nf=%d)\n", s->num_forms);

    total++;
    if (s->num_fields == 3 &&
        s->fields[0].kind == SEL_FLD_TEXT   && sel_streq(s->fields[0].name, "q") &&
        s->fields[1].kind == SEL_FLD_HIDDEN && sel_streq(s->fields[1].name, "lang") && sel_streq(s->fields[1].value, "en") &&
        s->fields[2].kind == SEL_FLD_SUBMIT)
        { pass++; printf("selene-form: fields parsed (text q, hidden lang=en, submit) PASS\n"); }
    else printf("selene-form: field parse FAIL (nf=%d)\n", s->num_fields);

    total++;
    strncpy(s->fields[0].value, "hello world", sizeof(s->fields[0].value)-1);   // fill the text field
    char url[224]; selene_build_submit_url(s, 2, url, sizeof(url));             // submit via the button
    if (sel_streq(url, "http://example.com/search?q=hello+world&lang=en"))
        { pass++; printf("selene-form: GET url built PASS (%s)\n", url); }
    else printf("selene-form: GET url build FAIL (%s)\n", url);

    // 4) a method=post form: parse it and build the URL-encoded request body.
    total++;
    {
        static const char* PHTML =
            "<form action=\"/login\" method=\"post\">"
            "<input type=\"text\" name=\"user\" value=\"\">"
            "<input type=\"password\" name=\"pass\" value=\"\">"
            "<input type=\"submit\" value=\"Sign in\"></form>";
        render_html(s, (const uint8_t*)PHTML, (uint32_t)strlen(PHTML));
        int okp = (s->num_forms == 1 && s->forms[0].method == 1 && s->num_fields == 3);   // method POST
        if (okp) {
            strncpy(s->fields[0].value, "alice",  sizeof(s->fields[0].value)-1);
            strncpy(s->fields[1].value, "p@ss w", sizeof(s->fields[1].value)-1);          // @ and space encode
        }
        char body[256]; selene_build_query(s, 0, body, sizeof(body));
        if (okp && sel_streq(body, "user=alice&pass=p%40ss+w"))
            { pass++; printf("selene-form: POST body built PASS (%s)\n", body); }
        else printf("selene-form: POST body FAIL (method=%d body=%s)\n",
                    s->num_forms ? s->forms[0].method : -1, body);
    }

    // 5) <img> placeholders: alt text captured, src filename fallback, and the rendered "[img: ...]" label.
    total++;
    {
        static const char* IHTML =
            "<p>Logo <img src=\"/logo.png\" alt=\"Company Logo\"> here</p>"
            "<img src=\"http://cdn.example/cat.jpg\">";
        render_html(s, (const uint8_t*)IHTML, (uint32_t)strlen(IHTML));
        int oki = (s->num_imgs == 2 &&
                   sel_streq(s->images[0].alt, "Company Logo") && sel_streq(s->images[0].src, "/logo.png") &&
                   s->images[1].alt[0] == '\0' && sel_streq(s->images[1].src, "http://cdn.example/cat.jpg"));
        int lbl_alt = 0, lbl_file = 0;                                    // the labels made it into the grid
        for (int li = 0; li < s->num_lines; li++) {
            const char* L = s->lines[li];
            for (int c = 0; L[c]; c++) {
                if (!strncmp(L + c, "[img: Company Logo]", 19)) lbl_alt = 1;
                if (!strncmp(L + c, "[img: cat.jpg]", 14)) lbl_file = 1;
            }
        }
        if (oki && lbl_alt && lbl_file)
            { pass++; printf("selene-form: <img> parsed (alt + filename fallback + labels) PASS\n"); }
        else printf("selene-form: <img> FAIL (ni=%d ok=%d altlbl=%d filelbl=%d)\n",
                    s->num_imgs, oki, lbl_alt, lbl_file);
    }

    // 6) <table> column layout: a small table renders as aligned, bordered rows (header th upper-cased).
    total++;
    {
        static const char* THTML =
            "<table>"
            "<tr><th>Name</th><th>Age</th></tr>"
            "<tr><td>Alice</td><td>30</td></tr>"
            "<tr><td>Bob</td><td>5</td></tr>"
            "</table>";
        render_html(s, (const uint8_t*)THTML, (uint32_t)strlen(THTML));
        int hdr = 0, r1 = 0, r2 = 0, rule = 0;                            // exact aligned lines expected
        for (int li = 0; li < s->num_lines; li++) {
            const char* L = s->lines[li];
            if (sel_streq(L, "| NAME  | AGE |")) hdr = 1;
            if (sel_streq(L, "| Alice | 30  |")) r1 = 1;
            if (sel_streq(L, "| Bob   | 5   |")) r2 = 1;
            if (sel_streq(L, "+-------+-----+"))  rule = 1;
        }
        if (hdr && r1 && r2 && rule)
            { pass++; printf("selene-form: <table> layout (aligned cols + header + border) PASS\n"); }
        else printf("selene-form: <table> FAIL (hdr=%d r1=%d r2=%d rule=%d)\n", hdr, r1, r2, rule);
    }

    // 7) tab manager: new / switch / close bookkeeping (no window, no network).
    total++;
    {
        selene_tabs_t* T = (selene_tabs_t*)selene_create_ctx();
        int ok = (T != 0);
        if (ok) {
            ok = ok && (T->ntabs == 1 && T->active == 0);
            selene_tab_new(T); ok = ok && (T->ntabs == 2 && T->active == 1);   // opens + focuses
            selene_tab_new(T); ok = ok && (T->ntabs == 3 && T->active == 2);
            T->active = 1; selene_tab_close(T, 0); ok = ok && (T->ntabs == 2 && T->active == 0);  // active follows the shift
            selene_tab_close(T, 1); ok = ok && (T->ntabs == 1);
            selene_tab_close(T, 0); ok = ok && (T->ntabs == 1);                 // never closes the last tab
            for (int t = 0; t < T->ntabs; t++) selene_free_ctx(T->tab[t]);
            kfree(T);
        }
        if (ok) { pass++; printf("selene-form: tab manager (new/switch/close) PASS\n"); }
        else printf("selene-form: tab manager FAIL\n");
    }

    // 8) cooperative image loader: the pure visibility gating selene_win_tick drives one-per-frame.
    // Two <img>s (top + far down); assert selene_next_img_index picks the visible, untried one only.
    total++;
    {
        for (int i = 0; i < s->num_imgs; i++) selene_img_free(&s->images[i]);
        for (int li = 0; li < SEL_MAX_LINES; li++) s->img_of[li][0] = 0;   // clear col-0 anchors
        s->num_imgs = 2; s->num_lines = 1150; s->scroll = 0;
        s->images[0].tried = 0; s->images[0].px = 0; s->images[0].frames = 0; s->images[0].nframes = 0;
        s->images[1].tried = 0; s->images[1].px = 0; s->images[1].frames = 0; s->images[1].nframes = 0;
        s->img_of[1][0]    = 1;                          // image 0 anchored on line 1 (near the top)
        s->img_of[1100][0] = 2;                          // image 1 anchored on line 1100 (far down)
        int rows = visible_rows();
        int a = selene_next_img_index(s, rows);          // img0 visible + untried  -> 0
        s->images[0].tried = 1;
        int b = selene_next_img_index(s, rows);          // img0 tried, img1 off-screen -> -1
        s->scroll = 1100;
        int c = selene_next_img_index(s, rows);          // img1 now scrolled in, untried -> 1
        s->images[1].px = (uint8_t*)1;                   // pretend it decoded
        int d = selene_next_img_index(s, rows);          // nothing pending -> -1
        s->images[1].px = 0;                             // clear the fake ptr before free
        if (a == 0 && b == -1 && c == 1 && d == -1)
            { pass++; printf("selene-form: image loader visibility gating (0,-1,1,-1) PASS\n"); }
        else printf("selene-form: image loader FAIL (a=%d b=%d c=%d d=%d)\n", a, b, c, d);
    }

    // 9) HTML entities: named + numeric decode to their display strings (&mdash;->"--", &hellip;->"...", etc.).
    total++;
    {
        static const char* EH = "<p>A&mdash;B&hellip;C&copy;D&amp;E&nbsp;F&middot;G&#8212;H</p>";
        static const char* WANT = "A--B...C(c)D&E F*G--H";
        render_html(s, (const uint8_t*)EH, (uint32_t)strlen(EH));
        int found = 0;
        for (int li = 0; li < s->num_lines && !found; li++) {
            const char* L = s->lines[li];
            for (int off = 0; L[off]; off++) {                       // substring search (ignore any padding)
                int m = 1; for (int q = 0; WANT[q]; q++) if (L[off + q] != WANT[q]) { m = 0; break; }
                if (m) { found = 1; break; }
            }
        }
        if (found) { pass++; printf("selene-form: HTML entities (mdash/hellip/copy/amp/nbsp/middot/#8212) PASS\n"); }
        else printf("selene-form: HTML entities FAIL\n");
    }

    // 10) HTTP redirect parse: a 3xx response's status code + Location header are extracted (so
    // selene_load_ex can follow it). Pure — feeds a canned response to http_parse_response.
    total++;
    {
        static const char* R =
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Server: nyx\r\n"
            "Location: https://example.com/final\r\n"
            "Content-Length: 0\r\n\r\n";
        uint8_t buf[256]; uint32_t n = (uint32_t)strlen(R);
        for (uint32_t k = 0; k <= n; k++) buf[k] = (uint8_t)R[k];   // include the NUL
        http_response_t rr;
        int pr = http_parse_response(buf, n, &rr);
        int ok = (pr == 0 && rr.status_code == 301 && sel_streq(rr.location, "https://example.com/final"));
        if (ok) { pass++; printf("selene-form: HTTP 301 redirect parse (status + Location) PASS\n"); }
        else printf("selene-form: HTTP redirect parse FAIL (code=%d loc=%s)\n", rr.status_code, rr.location);
        http_free(&rr);
    }

    // 11) table colspan/rowspan: a header spanning 2 columns renders as one wide cell, and a rowspan
    // keeps the following row's cell aligned in the right column (col 0 blank under the spanning cell).
    total++;
    {
        static const char* T1 = "<table><tr><th colspan=\"2\">Group</th></tr><tr><td>A</td><td>B</td></tr></table>";
        render_html(s, (const uint8_t*)T1, (uint32_t)strlen(T1));
        int cspan_ok = 0, data_ok = 0;
        for (int li = 0; li < s->num_lines; li++) {
            if (sel_streq(s->lines[li], "| GROUP |")) cspan_ok = 1;   // <th> is upper-cased, spanning both cols
            if (sel_streq(s->lines[li], "| A | B |")) data_ok = 1;
        }
        static const char* T2 = "<table><tr><td rowspan=\"2\">R</td><td>x</td></tr><tr><td>y</td></tr></table>";
        render_html(s, (const uint8_t*)T2, (uint32_t)strlen(T2));
        int rspan_ok = 0;
        for (int li = 0; li < s->num_lines; li++) if (sel_streq(s->lines[li], "|   | y |")) rspan_ok = 1;  // y aligned in col 1
        if (cspan_ok && data_ok && rspan_ok)
            { pass++; printf("selene-form: table colspan + rowspan (spanning header + aligned rowspan) PASS\n"); }
        else printf("selene-form: table span FAIL (cspan=%d data=%d rspan=%d)\n", cspan_ok, data_ok, rspan_ok);
    }

    // 12) nested <table>: a table inside a cell renders inline as a compact "[ a b / c d ]" (cells
    // space-separated, rows by " / ") and does NOT corrupt the outer grid — the outer data row keeps
    // exactly its 2 columns and the inner rows never leak in as extra outer rows/columns.
    total++;
    {
        static const char* NT =
            "<table>"
            "<tr><th>Item</th><th>Detail</th></tr>"
            "<tr><td>Row1</td>"
            "<td><table><tr><td>A</td><td>B</td></tr><tr><td>C</td><td>D</td></tr></table></td></tr>"
            "</table>";
        render_html(s, (const uint8_t*)NT, (uint32_t)strlen(NT));
        int hdr_ok = 0, nest_ok = 0, cols_ok = 0;
        for (int li = 0; li < s->num_lines; li++) {
            const char* L = s->lines[li];
            if (strstr(L, "ITEM") && strstr(L, "DETAIL")) hdr_ok = 1;              // header row intact
            if (strstr(L, "Row1") && strstr(L, "[ A B / C D ]")) {                 // nested table inline, same row
                nest_ok = 1;
                int pipes = 0; for (int q = 0; L[q]; q++) if (L[q] == '|') pipes++;
                if (pipes == 3) cols_ok = 1;                                       // exactly 2 columns (3 borders)
            }
        }
        if (hdr_ok && nest_ok && cols_ok)
            { pass++; printf("selene-form: nested <table> (inline flatten + outer grid intact) PASS\n"); }
        else printf("selene-form: nested table FAIL (hdr=%d nest=%d cols=%d)\n", hdr_ok, nest_ok, cols_ok);
    }

    // 13) ordered + nested lists: <ol> items are numbered "1. 2. 3." (not flat "- "), and a list nested
    // inside a parent <li> is indented one level (SEL_LIST_INDENT spaces) with its own numbering.
    total++;
    {
        static const char* OL = "<ol><li>First</li><li>Second</li><li>Third</li></ol>";
        render_html(s, (const uint8_t*)OL, (uint32_t)strlen(OL));
        int n1 = 0, n2 = 0, n3 = 0;
        for (int li = 0; li < s->num_lines; li++) {
            if (sel_streq(s->lines[li], "1. First"))  n1 = 1;
            if (sel_streq(s->lines[li], "2. Second")) n2 = 1;
            if (sel_streq(s->lines[li], "3. Third"))  n3 = 1;
        }
        static const char* NL = "<ul><li>A<ol><li>one</li><li>two</li></ol></li><li>B</li></ul>";
        render_html(s, (const uint8_t*)NL, (uint32_t)strlen(NL));
        int pa = 0, c1 = 0, c2 = 0, pb = 0;
        for (int li = 0; li < s->num_lines; li++) {
            if (sel_streq(s->lines[li], "- A"))      pa = 1;   // unordered parent marker
            if (sel_streq(s->lines[li], "  1. one")) c1 = 1;   // nested ordered item, indented 2
            if (sel_streq(s->lines[li], "  2. two")) c2 = 1;
            if (sel_streq(s->lines[li], "- B"))      pb = 1;   // parent numbering/marker resumes at col 0
        }
        if (n1 && n2 && n3 && pa && c1 && c2 && pb)
            { pass++; printf("selene-form: ordered + nested lists (numbered <ol> + indented nesting) PASS\n"); }
        else printf("selene-form: list FAIL (ol=%d,%d,%d nest a=%d 1=%d 2=%d b=%d)\n", n1, n2, n3, pa, c1, c2, pb);
    }

    // 14) <pre> preserves whitespace (multiple spaces + line breaks kept verbatim), and <blockquote>
    // indents every line of the quote by SEL_QUOTE_INDENT (4) spaces via the tindent margin in wrap_text.
    total++;
    {
        static const char* PRE = "<pre>a  b\n  c</pre>";               // 2 spaces mid-line, 2 leading on line 2
        render_html(s, (const uint8_t*)PRE, (uint32_t)strlen(PRE));
        int p1 = 0, p2 = 0;
        for (int li = 0; li < s->num_lines; li++) {
            if (sel_streq(s->lines[li], "a  b")) p1 = 1;               // interior double space kept
            if (sel_streq(s->lines[li], "  c"))  p2 = 1;               // leading indent kept
        }
        static const char* BQ = "<blockquote>Quoted text here</blockquote>";
        render_html(s, (const uint8_t*)BQ, (uint32_t)strlen(BQ));
        int q1 = 0;
        for (int li = 0; li < s->num_lines; li++)
            if (sel_streq(s->lines[li], "    Quoted text here")) q1 = 1;  // 4-space left margin
        if (p1 && p2 && q1)
            { pass++; printf("selene-form: <pre> whitespace + <blockquote> indent PASS\n"); }
        else printf("selene-form: pre/blockquote FAIL (pre a  b=%d, pre indent=%d, quote=%d)\n", p1, p2, q1);
    }

    selene_free_ctx(s);
    printf("selene-form: self-test %d/%d passed\n", pass, total);
    return (pass == total) ? 0 : -1;
}
