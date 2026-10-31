// The terminal pane's model, apart from ImGui and the process: a screen
// that libvterm keeps from the bytes a pty's child writes, the lines
// that scroll off it, and the bytes that keys, typing and pastes send
// back.
//
// Rows count from the top of the screen, 0 to rows-1, and on up the
// scrollback with negative numbers, -1 the line that scrolled off last,
// so the pane draws any window of them with termcell. A cell's colors
// stay symbolic, the default or an index into the 256-color palette, or
// else red, green and blue (TermColor); termrgb resolves them against
// the theme's 16 colors, so a change of theme recolors what is already
// on the screen. A cell holds a character and up to five more that
// combine with it (accents, variation selectors, a joiner), on the
// screen and in the scrollback alike. Which combine is libvterm's call,
// by code point, and like wcwidth's, so the cursor stays where programs
// count it: an emoji's skin tone, or the next emoji after a joiner, is a
// cell of its own, as many terminals lay them out (and as the pane, with
// no shaping, could only draw them).
//
// Keys go out as a terminal would send them to a shell, xterm's way:
// libvterm's own encoding, except that Ctrl with a character, and Enter,
// Tab, Backspace and Escape with Shift or Ctrl, send the classic control
// bytes rather than CSI u, which shells do not read. termmods turns the
// modifiers ImGui reports into the terminal's: on macOS ImGui reports
// Cmd as Ctrl and the Control key as Super, and Option is Meta only if
// asked; else it types its characters, and on the arrows and the
// deleting keys moves and deletes by word, as Terminal and iTerm do.
//
// libvterm allocates for itself, with malloc, as does the scrollback:
// a terminal lives as long as its session's, not a frame's or the
// GUI's, so no arena fits it. termfree gives everything back.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    TERM_SHIFT  = VTERM_MOD_SHIFT,
    TERM_ALT    = VTERM_MOD_ALT,
    TERM_CTRL   = VTERM_MOD_CTRL,
    TERM_OPTION = 8,  // macOS's Option, not as Meta: words, on a few keys
};

// A cell's color: TERM_DEFAULT (the foreground's or the background's,
// by where it is used), TERM_INDEXED|n for palette entry n, or 0xRRGGBB.
enum : u32 {
    TERM_DEFAULT = 1u << 24,
    TERM_INDEXED = 2u << 24,
};

enum : u8 {
    TERM_BOLD      = 1<<0,
    TERM_ITALIC    = 1<<1,
    TERM_UNDERLINE = 1<<2,
    TERM_REVERSE   = 1<<3,
    TERM_STRIKE    = 1<<4,
    TERM_CONCEAL   = 1<<5,
    TERM_BLINK     = 1<<6,
};

enum : i32 {
    TERM_CURSOR_BLOCK = VTERM_PROP_CURSORSHAPE_BLOCK,
    TERM_CURSOR_UNDERLINE = VTERM_PROP_CURSORSHAPE_UNDERLINE,
    TERM_CURSOR_BAR = VTERM_PROP_CURSORSHAPE_BAR_LEFT,
};

enum : i32 {
    TERM_MAXCHARS = VTERM_MAX_CHARS_PER_CELL,  // a character and five combining
};

struct TermCell {
    u32 ch[TERM_MAXCHARS];  // a character, then those that combine with it,
                            // ended by 0 if fewer; ch[0] 0 for none (blank,
                            // or a wide one's right half)
    u32 fg;                 // TermColor
    u32 bg;
    u8  attrs;              // TERM_BOLD...
    u8  width;              // 1, 2 for a wide character, 0 for its right half
};

// A cell as the scrollback keeps it, 16 bytes: the first character, and
// how many more combine with it, at more in its line's pool.
struct TermSbCell {
    u32 ch;
    u32 fg;
    u32 bg;
    u8  attrs;
    u8  shape;  // width | (characters that combine)<<2
    unsigned short more;  // where those start in the line's pool
};

struct TermLine {
    TermSbCell *cells;  // with the pool after them, in one allocation
    u32        *pool;
    i32         cols;   // trailing blanks dropped
};

struct Term {
    VTerm       *vt;
    VTermScreen *screen;
    i32          rows;
    i32          cols;

    // Scrollback: a ring of sbmax lines, sblen of them in use, the newest
    // at sbhead-1
    TermLine *sb;
    i32       sbmax;
    i32       sblen;
    i32       sbhead;

    // What the screen says of itself
    i32  cursorrow, cursorcol;
    b32  cursorvisible;
    b32  cursorblink;
    i32  cursorshape;  // TERM_CURSOR_*
    b32  altscreen;
    b32  reverse;      // the whole screen reversed (DECSCNM)
    i32  mouse;        // VTERM_PROP_MOUSE_*: what the program asks to hear
    u8   title[256];
    iz   titlelen;
    u8   titlenext[256];  // a title still arriving in fragments
    iz   titlenextlen;

    // For the pane: something changed since it last looked, how many
    // bells rang
    b32  dirty;
    i32  bells;

    // Bytes for the child, typed or answered (a query's reply)
    void (*out)(void *ctx, Str bytes);
    void  *outctx;
};


// Colors

static u32 termcolor(VTermColor c, b32 fg)
{
    if (fg ? VTERM_COLOR_IS_DEFAULT_FG(&c) : VTERM_COLOR_IS_DEFAULT_BG(&c)) {
        return TERM_DEFAULT;
    } else if (VTERM_COLOR_IS_INDEXED(&c)) {
        return TERM_INDEXED | c.indexed.idx;
    }
    return (u32)c.rgb.red<<16 | (u32)c.rgb.green<<8 | c.rgb.blue;
}

static VTermColor vtermcolor(u32 c, b32 fg)
{
    VTermColor r = {};
    if (c == TERM_DEFAULT) {
        vterm_color_indexed(&r, 0);
        r.type |= fg ? VTERM_COLOR_DEFAULT_FG : VTERM_COLOR_DEFAULT_BG;
    } else if (c & TERM_INDEXED) {
        vterm_color_indexed(&r, (u8)c);
    } else {
        vterm_color_rgb(&r, (u8)(c>>16), (u8)(c>>8), (u8)c);
    }
    return r;
}

// xterm's palette past the first 16: a 6x6x6 cube, then 24 grays.
static u32 termpalette(i32 n)
{
    if (n < 232) {
        static u8 const level[] = {0, 95, 135, 175, 215, 255};
        n -= 16;
        return (u32)level[n/36]<<16 | (u32)level[n/6%6]<<8 | level[n%6];
    }
    u32 g = (u32)(8 + 10*(n - 232));
    return g<<16 | g<<8 | g;
}

// A TermColor as 0xRRGGBB: the default, and the first 16 of the
// palette, the theme's own (ansi).
static u32 termrgb(u32 c, u32 deflt, u32 const *ansi)
{
    if (c == TERM_DEFAULT) {
        return deflt;
    } else if (c & TERM_INDEXED) {
        i32 n = (i32)(c & 0xff);
        return n<16 ? ansi[n] : termpalette(n);
    }
    return c & 0xffffff;
}


// Cells

static TermCell termfromvterm(VTermScreenCell const *v)
{
    TermCell c = {};
    u32 ch = v->chars[0];
    if (ch != (u32)-1) {
        for (i32 i = 0; i<TERM_MAXCHARS && v->chars[i]; i++) {
            c.ch[i] = v->chars[i];
        }
    }
    c.width = ch==(u32)-1 ? 0 : (u8)v->width;
    c.fg    = termcolor(v->fg, 1);
    c.bg    = termcolor(v->bg, 0);
    c.attrs = (u8)((v->attrs.bold      ? TERM_BOLD      : 0) |
                    (v->attrs.italic    ? TERM_ITALIC    : 0) |
                    (v->attrs.underline ? TERM_UNDERLINE : 0) |
                    (v->attrs.reverse   ? TERM_REVERSE   : 0) |
                    (v->attrs.strike    ? TERM_STRIKE    : 0) |
                    (v->attrs.conceal   ? TERM_CONCEAL   : 0) |
                    (v->attrs.blink     ? TERM_BLINK     : 0));
    return c;
}

static VTermScreenCell termtovterm(TermCell c)
{
    VTermScreenCell v = {};
    for (i32 i = 0; i < TERM_MAXCHARS; i++) {
        v.chars[i] = c.ch[i];
    }
    v.chars[0]          = c.width ? c.ch[0] : (u32)-1;
    v.width             = (char)(c.width ? c.width : 1);
    v.fg                = vtermcolor(c.fg, 1);
    v.bg                = vtermcolor(c.bg, 0);
    v.attrs.bold        = !!(c.attrs & TERM_BOLD);
    v.attrs.italic      = !!(c.attrs & TERM_ITALIC);
    v.attrs.underline   = c.attrs & TERM_UNDERLINE ? VTERM_UNDERLINE_SINGLE : 0;
    v.attrs.reverse     = !!(c.attrs & TERM_REVERSE);
    v.attrs.strike      = !!(c.attrs & TERM_STRIKE);
    v.attrs.conceal     = !!(c.attrs & TERM_CONCEAL);
    v.attrs.blink       = !!(c.attrs & TERM_BLINK);
    return v;
}

static b32 termblank(TermCell c)
{
    return !c.ch[0] && c.width==1 && c.bg==TERM_DEFAULT && !(c.attrs & TERM_REVERSE);
}

// How many characters a cell holds, the first one's among them.
static i32 termnchars(TermCell const *c)
{
    i32 n = 0;
    while (n<TERM_MAXCHARS && c->ch[n]) {
        n++;
    }
    return n;
}

static TermCell termfromsb(TermLine *l, i32 col)
{
    TermSbCell s = l->cells[col];
    TermCell   c = {};
    c.ch[0] = s.ch;
    for (i32 i = 0; i < s.shape>>2; i++) {
        c.ch[1+i] = l->pool[s.more + i];
    }
    c.fg    = s.fg;
    c.bg    = s.bg;
    c.attrs = s.attrs;
    c.width = s.shape & 3;
    return c;
}

// The cell at row (negative: scrollback) and col: blank past a line's
// end, or out of range.
static TermCell termcell(Term *t, i32 row, i32 col)
{
    TermCell blank = {{}, TERM_DEFAULT, TERM_DEFAULT, 0, 1};
    if (col<0 || col>=t->cols || row>=t->rows || row<-t->sblen) {
        return blank;
    } else if (row < 0) {
        TermLine *l = &t->sb[((t->sbhead + row) % t->sbmax + t->sbmax) % t->sbmax];
        return col<l->cols ? termfromsb(l, col) : blank;
    }
    VTermScreenCell v;
    VTermPos        p = {row, col};
    if (!vterm_screen_get_cell(t->screen, p, &v)) {
        return blank;
    }
    return termfromvterm(&v);
}

// A row's text, trailing blanks dropped, a wide character once.
static Str termrowtext(Term *t, Arena *a, i32 row)
{
    Buf b(a, t->cols*4 + 4);
    iz  keep = 0;
    for (i32 col = 0; col < t->cols; col++) {
        TermCell c = termcell(t, row, col);
        if (!c.width) {
            continue;
        }
        u8 enc[4];
        if (!c.ch[0]) {
            print(&b, " ");
        }
        for (i32 i = 0; i < termnchars(&c); i++) {
            print(&b, Str{enc, utf8encode(enc, (i32)c.ch[i])});
        }
        keep = c.ch[0] && c.ch[0]!=' ' ? b.len : keep;
    }
    b.len = keep;
    return finish(&b);
}


// libvterm's callbacks

static void termsbfree(TermLine *l)
{
    free(l->cells);
    *l = {};
}

static int termdamage(VTermRect, void *user)
{
    ((Term *)user)->dirty = 1;
    return 1;
}

static int termmoverect(VTermRect, VTermRect, void *user)
{
    ((Term *)user)->dirty = 1;
    return 1;
}

static int termmovecursor(VTermPos pos, VTermPos, int visible, void *user)
{
    Term *t = (Term *)user;
    t->cursorrow     = pos.row;
    t->cursorcol     = pos.col;
    t->cursorvisible = visible;
    t->dirty         = 1;
    return 1;
}

static int termprop(VTermProp prop, VTermValue *val, void *user)
{
    Term *t = (Term *)user;
    t->dirty = 1;
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: t->cursorvisible = val->boolean; break;
    case VTERM_PROP_CURSORBLINK:   t->cursorblink   = val->boolean; break;
    case VTERM_PROP_CURSORSHAPE:   t->cursorshape   = val->number;  break;
    case VTERM_PROP_ALTSCREEN:     t->altscreen     = val->boolean; break;
    case VTERM_PROP_REVERSE:       t->reverse       = val->boolean; break;
    case VTERM_PROP_MOUSE:         t->mouse         = val->number;  break;
    case VTERM_PROP_TITLE: {
        // In fragments, kept whole once the last arrives, cut short if long
        VTermStringFragment f = val->string;
        if (f.initial) {
            t->titlenextlen = 0;
        }
        iz room = countof(t->titlenext) - t->titlenextlen;
        iz n    = (iz)f.len<room ? (iz)f.len : room;
        copybytes(t->titlenext + t->titlenextlen, (void *)f.str, n);
        t->titlenextlen += n;
        if (f.final) {
            Str s = utf8head({t->titlenext, t->titlenextlen}, countof(t->title));
            copybytes(t->title, s.data, s.len);
            t->titlelen = s.len;
        }
    } break;
    default: return 0;
    }
    return 1;
}

static int termbell(void *user)
{
    ((Term *)user)->bells++;
    return 1;
}

static int termresized(int rows, int cols, void *user)
{
    Term *t = (Term *)user;
    t->rows  = rows;
    t->cols  = cols;
    t->dirty = 1;
    return 1;
}

static int termpushline(int cols, VTermScreenCell const *cells, void *user)
{
    Term *t = (Term *)user;
    if (!t->sbmax) {
        return 0;
    }
    i32 keep = cols;
    while (keep > 0) {
        TermCell c = termfromvterm(&cells[keep-1]);
        if (!termblank(c)) {
            break;
        }
        keep--;
    }
    TermLine l = {};
    if (keep) {
        // The characters that combine go to the pool, as far as a u16
        // reaches, which a line would pass only past 13,000 columns
        iz extra = 0;
        for (i32 i = 0; i < keep; i++) {
            TermCell c = termfromvterm(&cells[i]);
            extra += termnchars(&c) - (c.ch[0] ? 1 : 0);
        }
        extra = extra>0xffff ? 0xffff : extra;
        l.cells = (TermSbCell *)malloc(sizeof(TermSbCell)*(uz)keep + sizeof(u32)*(uz)extra);
        if (!l.cells) {
            return 0;  // the line is lost
        }
        l.pool = (u32 *)(l.cells + keep);
        iz used = 0;
        for (i32 i = 0; i < keep; i++) {
            TermCell c = termfromvterm(&cells[i]);
            i32      n = c.ch[0] ? termnchars(&c) - 1 : 0;
            n = used+n>extra ? 0 : n;
            for (i32 j = 0; j < n; j++) {
                l.pool[used + j] = c.ch[1+j];
            }
            l.cells[i] = {c.ch[0], c.fg, c.bg, c.attrs, (u8)(c.width | n<<2), (unsigned short)used};
            used += n;
        }
        l.cols = keep;
    }
    TermLine *slot = &t->sb[t->sbhead];
    termsbfree(slot);  // the oldest, once the ring is full
    *slot = l;
    t->sbhead = (t->sbhead + 1) % t->sbmax;
    t->sblen += t->sblen < t->sbmax;
    t->dirty  = 1;
    return 1;
}

static int termpopline(int cols, VTermScreenCell *cells, void *user)
{
    Term *t = (Term *)user;
    if (!t->sblen) {
        return 0;
    }
    t->sbhead = (t->sbhead - 1 + t->sbmax) % t->sbmax;
    t->sblen--;
    TermLine *l     = &t->sb[t->sbhead];
    TermCell  blank = {{}, TERM_DEFAULT, TERM_DEFAULT, 0, 1};
    for (i32 i = 0; i < cols; i++) {
        cells[i] = termtovterm(i<l->cols ? termfromsb(l, i) : blank);
    }
    termsbfree(l);
    t->dirty = 1;
    return 1;
}

static int termsbclear(void *user)
{
    Term *t = (Term *)user;
    for (i32 i = 0; i < t->sbmax; i++) {
        termsbfree(&t->sb[i]);
    }
    t->sblen  = 0;
    t->sbhead = 0;
    t->dirty  = 1;
    return 1;
}

static void termoutput(char const *s, size_t len, void *user)
{
    Term *t = (Term *)user;
    if (t->out) {
        t->out(t->outctx, {(u8 *)s, (iz)len});
    }
}

static VTermScreenCallbacks const termcallbacks = {
    termdamage,
    termmoverect,
    termmovecursor,
    termprop,
    termbell,
    termresized,
    termpushline,
    termpopline,
    termsbclear,
};


// A terminal

// A terminal of rows by cols that keeps sbmax lines of scrollback,
// sending what it answers and what is typed to out. Null if out of
// memory.
static Term *termnew(i32 rows, i32 cols, i32 sbmax, void (*out)(void *, Str), void *outctx)
{
    rows  = rows<1 ? 1 : rows;
    cols  = cols<1 ? 1 : cols;
    sbmax = sbmax<0 ? 0 : sbmax;
    Term *t = (Term *)calloc(1, sizeof(Term));
    if (!t) {
        return 0;
    }
    t->sb = sbmax ? (TermLine *)calloc((uz)sbmax, sizeof(TermLine)) : 0;
    t->vt = vterm_new(rows, cols);
    if ((sbmax && !t->sb) || !t->vt) {
        if (t->vt) vterm_free(t->vt);
        free(t->sb);
        free(t);
        return 0;
    }
    t->rows          = rows;
    t->cols          = cols;
    t->sbmax         = sbmax;
    t->cursorvisible = 1;
    t->cursorblink   = 1;
    t->cursorshape   = TERM_CURSOR_BLOCK;
    t->out           = out;
    t->outctx        = outctx;
    vterm_set_utf8(t->vt, 1);
    vterm_output_set_callback(t->vt, termoutput, t);
    t->screen = vterm_obtain_screen(t->vt);
    vterm_screen_set_callbacks(t->screen, &termcallbacks, t);
    vterm_screen_set_damage_merge(t->screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(t->screen, 1);
    vterm_screen_enable_reflow(t->screen, true);
    vterm_screen_reset(t->screen, 1);
    return t;
}

static void termfree(Term *t)
{
    if (!t) {
        return;
    }
    vterm_free(t->vt);
    for (i32 i = 0; i < t->sbmax; i++) {
        termsbfree(&t->sb[i]);
    }
    free(t->sb);
    free(t);
}

// What the child wrote.
static void termwrite(Term *t, Str bytes)
{
    if (bytes.len > 0) {
        vterm_input_write(t->vt, (char const *)bytes.data, (size_t)bytes.len);
        vterm_screen_flush_damage(t->screen);
    }
}

static void termresize(Term *t, i32 rows, i32 cols)
{
    rows = rows<1 ? 1 : rows;
    cols = cols<1 ? 1 : cols;
    if (rows!=t->rows || cols!=t->cols) {
        vterm_set_size(t->vt, rows, cols);
        vterm_screen_flush_damage(t->screen);
        t->rows  = rows;
        t->cols  = cols;
        t->dirty = 1;
    }
}

static Str termtitle(Term *t)
{
    return {t->title, t->titlelen};
}


// Input

static void termsend(Term *t, Str bytes)
{
    if (t->out && bytes.len) {
        t->out(t->outctx, bytes);
    }
}

// Text typed, as it arrives from the keyboard's text input (an IME's
// too): sent as it is.
static void termtype(Term *t, Str text)
{
    termsend(t, text);
}

// A character with modifiers held, Ctrl or Alt (Meta) among them: what
// a terminal sends for it. Ctrl with a letter, or with @ [ \ ] ^ _ and
// the digits xterm maps onto those, is its control byte; Ctrl with
// Space is NUL; with anything else, Ctrl does nothing; Alt adds ESC in
// front.
static void termchar(Term *t, u32 c, i32 mods)
{
    if (mods & TERM_CTRL) {
        u32 ctl = 0x100;  // none
        u32 lc  = c>='A' && c<='Z' ? c + 32 : c;
        if (lc>='a' && lc<='z') {
            ctl = lc & 0x1f;
        } else {
            switch (c) {
            case ' ': case '@': case '2': case '`':  ctl = 0x00; break;
            case '[': case '3': case '{':            ctl = 0x1b; break;
            case '\\': case '4': case '|':           ctl = 0x1c; break;
            case ']': case '5': case '}':            ctl = 0x1d; break;
            case '^': case '6': case '~':            ctl = 0x1e; break;
            case '_': case '7': case '/': case '-':  ctl = 0x1f; break;
            case '8': case '?':                      ctl = 0x7f; break;
            }
        }
        if (ctl != 0x100) {
            u8 b[2] = {0x1b, (u8)ctl};
            b32 alt = !!(mods & TERM_ALT);
            termsend(t, {b + !alt, 1 + alt});
            return;
        }
    }
    // No control byte for it: the character alone, in UTF-8, which
    // libvterm's own encoding would not do after ESC
    u8 b[5] = {0x1b};
    b32 alt = !!(mods & TERM_ALT);
    iz  n   = utf8encode(b+1, (i32)c);
    termsend(t, {b + !alt, n + alt});
}

// A key that is not a character (VTermKey): arrows, Home and End, the
// function keys, Enter, Tab, Backspace, Escape. These last four ignore
// Shift and Ctrl, as xterm does, but for Shift+Tab (CSI Z) and
// Ctrl+Backspace (^H). macOS's Option, when not Meta, makes Left and
// Right a word back and forward (ESC b, ESC f) and Backspace and Delete
// delete a word (ESC DEL, ESC d), as readline and zle read them, and
// changes no other key.
static void termkey(Term *t, i32 key, i32 mods)
{
    if (mods & TERM_OPTION) {
        mods &= ~TERM_OPTION;
        u8 word = key==VTERM_KEY_LEFT      ? 'b'  :
                  key==VTERM_KEY_RIGHT     ? 'f'  :
                  key==VTERM_KEY_BACKSPACE ? 0x7f :
                  key==VTERM_KEY_DEL       ? 'd'  : 0;
        if (word && !(mods & (TERM_CTRL|TERM_ALT))) {
            u8 b[2] = {0x1b, word};
            termsend(t, {b, 2});
            return;
        }
    }
    switch (key) {
    case VTERM_KEY_BACKSPACE:
        if (mods & TERM_CTRL) {
            u8 b[2] = {0x1b, 0x08};
            b32 alt = !!(mods & TERM_ALT);
            termsend(t, {b + !alt, 1 + alt});
            return;
        }
        mods &= TERM_ALT;
        break;
    case VTERM_KEY_ENTER:
    case VTERM_KEY_ESCAPE:
        mods &= TERM_ALT;
        break;
    case VTERM_KEY_TAB:
        mods &= mods & TERM_SHIFT ? TERM_SHIFT|TERM_ALT : TERM_ALT;
        break;
    }
    vterm_keyboard_key(t->vt, (VTermKey)key, (VTermModifier)mods);
}

// A paste: bracketed if the program asked (so a shell does not run its
// lines as they arrive), and its newlines as Enter, CR.
static void termpaste(Term *t, Arena scratch, Str text)
{
    Buf b(&scratch, text.len + 16);
    for (iz i = 0; i < text.len; i++) {
        if (text[i] == '\r') {
            putbyte(&b, '\r');
            i += i+1<text.len && text[i+1]=='\n';
        } else {
            putbyte(&b, text[i]=='\n' ? '\r' : text[i]);
        }
    }
    vterm_keyboard_start_paste(t->vt);
    termsend(t, finish(&b));
    vterm_keyboard_end_paste(t->vt);
}

// Whether the program hears the mouse (VTERM_PROP_MOUSE): if not, the
// pane selects and scrolls with it instead.
static b32 termwantsmouse(Term *t)
{
    return t->mouse != VTERM_PROP_MOUSE_NONE;
}

// The mouse at a cell, and a button (1 left, 2 middle, 3 right, 4 and 5
// the wheel) pressed or released, for a program that hears it.
static void termmouse(Term *t, i32 row, i32 col, i32 button, b32 pressed, i32 mods)
{
    VTermModifier m = (VTermModifier)(mods & VTERM_ALL_MODS_MASK);
    vterm_mouse_move(t->vt, row, col, m);
    if (button) {
        vterm_mouse_button(t->vt, button, pressed, m);
    }
}

// Focus in and out, reported if the program asked (vim, tmux).
static void termfocus(Term *t, b32 in)
{
    VTermState *s = vterm_obtain_state(t->vt);
    if (in) {
        vterm_state_focus_in(s);
    } else {
        vterm_state_focus_out(s);
    }
}

// The modifiers ImGui reports held, as the terminal's (TERM_*), or -1
// if the chord is Prospero's (Cmd on macOS). On macOS ImGui reports Cmd
// as ctrl and the Control key as super, and Option is Alt only as Meta
// (optionmeta): otherwise it types its characters, which arrive as text,
// and is TERM_OPTION for termkey's words.
static i32 termmods(b32 ctrl, b32 shift, b32 alt, b32 super, b32 mac, b32 optionmeta)
{
    i32 m = shift ? TERM_SHIFT : 0;
    if (mac) {
        if (ctrl) {
            return -1;
        }
        m |= super ? TERM_CTRL : 0;
        m |= alt ? optionmeta ? TERM_ALT : TERM_OPTION : 0;
    } else {
        m |= ctrl ? TERM_CTRL : 0;
        m |= alt  ? TERM_ALT  : 0;
    }
    return m;
}

// Whether a character key with these modifiers goes as a key (termchar)
// rather than arriving as typed text: with Ctrl or Meta held.
static b32 termcharkey(i32 mods)
{
    return mods>=0 && (mods & (TERM_CTRL|TERM_ALT));
}
