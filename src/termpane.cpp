// The terminal pane: the shown session's shell, on a pseudo-terminal the
// window owns (os_spawnpty), drawn from its Term (term.cpp)
//
// A session gets its terminal the first time the pane shows it: its
// own shell in its folder, or for a remote's session ssh -t there, a
// login shell in its folder, with keepalives, so a link that dies shows
// as an exit within about 45 s rather than a pane that stops. Its output
// arrives in the terminals' mailbox (termbox), is fed to the Term as it
// is drained, with or without frames (in the background too), and shows
// at most every STREAM_PERIOD. When the program exits, the screen stays,
// with its status, and Restart starts another.
//
// A terminal lives as long as the window does, or until its session is
// closed or gone: then it hangs up, and if its program is still there 2
// s later, the hard kill ends its tree. Its records are taken until the
// output ends, and only then are the Term and the Proc let go. Going to
// the background leaves terminals running, drained as ever; quitting
// ends them at once.
//
// Input, for now: what is typed, as text (an IME's too), and as keys
// Enter, Tab, Backspace, Escape, the arrows and editing keys, the
// function keys, Ctrl with a character (macOS's Control key), Alt as
// Meta elsewhere, macOS's Option by word on the arrows, and pasting,
// with Cmd+V or Ctrl+Shift+V. While the pane has focus it owns the keys
// it reads, so that ImGui's keyboard navigation leaves them alone, and
// asks the platform for text input, which SDL sends only then. Wheel
// scrolls the scrollback; typing goes back to the bottom.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    TERM_HANGUP_US = 2000000,  // closing: the hangup's grace, then the hard kill
};

enum : i32 {
    TERM_SCROLLBACK = 10000,   // lines kept
};

struct GuiTerm {
    GuiTerm    *next;
    Gui        *g;
    GuiSession *s;        // whose, null once closing
    u64         id;       // its Spawn's, which its records carry
    Proc       *proc;     // null once let go, or if it never started
    Term       *term;
    Str         why;      // why it did not start (g->perm)
    i32         code;     // its exit, once PROC_EXIT came
    i32         sig;
    b32         exited;
    b32         eof;      // PROC_EOF came: the threads are done
    b32         closing;  // let go once done
    i64         hardat;   // closing: the hard kill then, if it has not exited
    i32         scroll;   // rows back into the scrollback, 0 at the bottom
    f32         wheel;    // a trackpad's fractions of a row
    b32         focused;  // as of the last frame
};

// What the terminal sends its program: typed, pasted, or a reply.
static void termout(void *ctx, Str bytes)
{
    GuiTerm *t = (GuiTerm *)ctx;
    if (t->proc && !t->exited) {
        os_procwrite(t->g->os, t->proc, bytes);
    }
}

static GuiTerm *termfor(Gui *g, GuiSession *s)
{
    for (GuiTerm *t = g->ptys; t; t = t->next) {
        if (t->s == s) {
            return t;
        }
    }
    return 0;
}

// The program a session's terminal runs, where, and with what
// environment, into sp; or why it cannot.
static Str termcommand(Gui *g, Arena *a, GuiSession *s, Spawn *sp)
{
    View    *v   = s->view;
    GuiHost *h   = s->host;
    Str      dir = v && v->cwd.len ? v->cwd : s->cwd;
    if (!dir.len) {
        return "Its folder is not known yet";
    }

    // TERM for what libvterm answers to; nothing of the terminal that
    // started this window, if one did
    Slice<Str> env = guienv(g, a);
    env = envset(a, env, "TERM", "xterm-256color", HOST_WINDOWS);
    env = envset(a, env, "COLORTERM", "truecolor", HOST_WINDOWS);
    env = envdel(a, env, "TERM_PROGRAM", HOST_WINDOWS);
    env = envdel(a, env, "TERM_PROGRAM_VERSION", HOST_WINDOWS);
    env = envdel(a, env, "TERMINFO", HOST_WINDOWS);
    env = envdel(a, env, "TERM_SESSION_ID", HOST_WINDOWS);
    if (g->mac && !envget(env, "LANG", 0).len && !envget(env, "LC_ALL", 0).len &&
            !envget(env, "LC_CTYPE", 0).len) {
        env = envset(a, env, "LANG", "en_US.UTF-8", 0);  // a desktop's launch sets none
    }
    Str home = envget(env, "HOME", HOST_WINDOWS);

    Slice<Str> argv = {};
    if (h->kind == GH_REMOTE) {
        Str ssh = remoteset(g, h->name, "ssh");
        Str wsl = ssh.len ? Str{} : remoteset(g, h->name, "wsl");
        if (ssh.len) {
            argv = termsshpane(a, linkprogram(g, a, 0), ssh, dir, hostwin(h));
        } else if (wsl.len) {
            Str prog = linkprogram(g, a, 1);
            argv = termwsl(a, prog.len ? prog : Str("wsl.exe"), wsl, dir);
        } else {
            return "Its section names no ssh destination";
        }
        dir = home.len ? home : Str("/");
    } else {
        // The session's own shell if it has one but the default, else the
        // user's, as a login shell, as a terminal starts it
        if (v) {
            argv = termshell(a, v->shellkind, v->shellpath, HOST_WINDOWS);
        }
        if (!argv.len) {
            Str shell = envget(env, "SHELL", HOST_WINDOWS);
            argv = push(a, argv, shell.len ? shell : Str("/bin/sh"));
            argv = push(a, argv, Str("-l"));
        }
    }
    sp->argv = argv;
    sp->cwd  = dir;
    sp->env  = env;
    return {};
}

// Start a session's terminal, rows by cols, into t (new or exited).
static void termstart(Gui *g, GuiTerm *t, GuiSession *s, i32 rows, i32 cols)
{
    Arena a = g->frame;
    t->g      = g;
    t->s      = s;
    t->why    = {};
    t->exited = 0;
    t->eof    = 0;
    t->code   = 0;
    t->sig    = 0;
    t->scroll = 0;
    t->wheel  = 0;
    if (!t->term) {
        t->term = termnew(rows, cols, TERM_SCROLLBACK, termout, t);
        if (!t->term) {
            t->why = "Out of memory";
            return;
        }
    } else {
        termresize(t->term, rows, cols);
        termwrite(t->term, "\x1b[0m\r\n");  // after what the last one left
    }
    Spawn sp = {};
    Str   why = termcommand(g, &a, s, &sp);
    if (why.len) {
        t->why = why;
        return;
    }
    sp.mbox = g->termbox;
    sp.id   = ++g->termid;
    t->id   = sp.id;
    t->proc = os_spawnpty(g->os, &sp, rows, cols, a);
    if (!t->proc) {
        t->why = HOST_WINDOWS ? Str("The terminal pane is not on Windows yet")
                              : clone(&g->perm, concat(&a, "Cannot start it: ",
                                                       os_lasterr(g->os, &a)));
    }
}

static GuiTerm *termnewfor(Gui *g, GuiSession *s)
{
    GuiTerm *t = g->termspare;
    if (t) {
        g->termspare = t->next;
    } else {
        t = alloc<GuiTerm>(&g->perm);
    }
    *t = {};
    t->g     = g;
    t->s     = s;
    t->next  = g->ptys;
    g->ptys = t;
    return t;
}

// Let a terminal go, now that nothing more comes of it.
static void termdrop(Gui *g, GuiTerm *t)
{
    for (GuiTerm **p = &g->ptys; *p; p = &(*p)->next) {
        if (*p == t) {
            *p = t->next;
            break;
        }
    }
    if (t->proc) {
        os_procfree(g->os, t->proc);
    }
    termfree(t->term);
    *t = {};
    t->next      = g->termspare;
    g->termspare = t;
}

// Close a terminal: a hangup, then the hard kill after TERM_HANGUP_US if
// its program is still there (at once with hard); let go once done.
static void termclose(Gui *g, GuiTerm *t, b32 hard)
{
    t->s       = 0;
    t->closing = 1;
    if (!t->proc || (t->exited && t->eof)) {
        termdrop(g, t);
        return;
    } else if (!t->exited) {
        os_kill(g->os, t->proc, 0);
        t->hardat = os_ticks(g->os) + TERM_HANGUP_US;
    }
    if (hard) {
        os_kill(g->os, t->proc, 1);
        t->hardat = 0;
    }
}

// A session's terminal, closed with it (sessclose, sessgone).
static void termsessclose(Gui *g, GuiSession *s)
{
    GuiTerm *t = termfor(g, s);
    if (t) {
        termclose(g, t, 0);
    }
}

// Every terminal, at once: quitting.
static void termcloseall(Gui *g)
{
    for (GuiTerm *t = g->ptys, *next; t; t = next) {
        next = t->next;
        termclose(g, t, 1);
    }
}

// The hard kills due by now, and when the next is due (0 if none).
static i64 termtick(Gui *g, i64 now)
{
    i64 next = 0;
    for (GuiTerm *t = g->ptys; t; t = t->next) {
        if (t->hardat && !t->exited) {
            if (now >= t->hardat) {
                os_kill(g->os, t->proc, 1);
                t->hardat = 0;
            } else {
                next = next && next<t->hardat ? next : t->hardat;
            }
        }
    }
    return next;
}

static GuiTerm *termbyid(Gui *g, u64 id)
{
    for (GuiTerm *t = g->ptys; t; t = t->next) {
        if (t->proc && t->id==id) {
            return t;
        }
    }
    return 0;
}

// The terminals' records into their Terms. Whether the shown session's
// changed, for drainmail's throttle.
static b32 drainterm(Gui *g)
{
    if (!g->termbox) {
        return 0;
    }
    b32  shown = 0;
    Mail m     = mailtake(g->termbox);
    for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
        GuiTerm *t = termbyid(g, r.id);
        if (!t) {
            continue;
        }
        switch (r.type) {
        case PROC_DATA:
            termwrite(t->term, r.data);
            break;
        case PROC_EXIT: {
            t->exited = 1;
            t->hardat = 0;
            t->code   = get32(r.data);
            t->sig    = get32(cuthead(r.data, 4));
            Arena a = g->frame;
            Buf   b(&a, 64);
            print(&b, "\x1b[0m\r\n[process ");
            if (t->sig) {
                print(&b, "killed by signal ");
                print(&b, (i64)t->sig);
            } else {
                print(&b, "exited ");
                print(&b, (i64)t->code);
            }
            print(&b, "]\r\n");
            termwrite(t->term, finish(&b));
        } break;
        case PROC_EOF:
            t->eof = 1;
            break;
        }
        shown |= t->s && t->s==g->sess;
    }
    // Done with: closed ones let go, exited ones' Procs freed
    for (GuiTerm *t = g->ptys, *next; t; t = next) {
        next = t->next;
        if (t->exited && t->eof) {
            if (t->closing) {
                termdrop(g, t);
            } else if (t->proc) {
                os_procfree(g->os, t->proc);
                t->proc = 0;
            }
        }
    }
    g->termdirty |= shown && g->panes[PANE_TERMINAL];
    return shown;
}


// For the checklist: whether the shown session's terminal shows text,
// in its scrollback or on its screen, each row a line, "\n" around all.
static b32 termsays(Gui *g, Str text)
{
    GuiTerm *t = g->sess ? termfor(g, g->sess) : 0;
    if (!t || !t->term) {
        return 0;
    }
    Arena a = g->frame;
    Buf   b(&a, 1<<16);
    putbyte(&b, '\n');
    for (i32 row = -t->term->sblen; row < t->term->rows; row++) {
        print(&b, termrowtext(t->term, &a, row));
        putbyte(&b, '\n');
    }
    return find(finish(&b), text) >= 0;
}


// Input

// The terminal's key for ImGui's, or VTERM_KEY_NONE.
static i32 termkeyof(ImGuiKey k)
{
    switch (k) {
    case ImGuiKey_Enter:       return VTERM_KEY_ENTER;
    case ImGuiKey_KeypadEnter: return VTERM_KEY_ENTER;
    case ImGuiKey_Tab:         return VTERM_KEY_TAB;
    case ImGuiKey_Backspace:   return VTERM_KEY_BACKSPACE;
    case ImGuiKey_Escape:      return VTERM_KEY_ESCAPE;
    case ImGuiKey_UpArrow:     return VTERM_KEY_UP;
    case ImGuiKey_DownArrow:   return VTERM_KEY_DOWN;
    case ImGuiKey_LeftArrow:   return VTERM_KEY_LEFT;
    case ImGuiKey_RightArrow:  return VTERM_KEY_RIGHT;
    case ImGuiKey_Insert:      return VTERM_KEY_INS;
    case ImGuiKey_Delete:      return VTERM_KEY_DEL;
    case ImGuiKey_Home:        return VTERM_KEY_HOME;
    case ImGuiKey_End:         return VTERM_KEY_END;
    case ImGuiKey_PageUp:      return VTERM_KEY_PAGEUP;
    case ImGuiKey_PageDown:    return VTERM_KEY_PAGEDOWN;
    default: break;
    }
    if (k>=ImGuiKey_F1 && k<=ImGuiKey_F12) {
        return VTERM_KEY_FUNCTION(1 + (k - ImGuiKey_F1));
    }
    return VTERM_KEY_NONE;
}

// The character of a key, as Ctrl or Meta send it, or 0.
static u32 termcharof(ImGuiKey k)
{
    if (k>=ImGuiKey_A && k<=ImGuiKey_Z) {
        return 'a' + (u32)(k - ImGuiKey_A);
    } else if (k>=ImGuiKey_0 && k<=ImGuiKey_9) {
        return '0' + (u32)(k - ImGuiKey_0);
    }
    switch (k) {
    case ImGuiKey_Space:        return ' ';
    case ImGuiKey_LeftBracket:  return '[';
    case ImGuiKey_RightBracket: return ']';
    case ImGuiKey_Backslash:    return '\\';
    case ImGuiKey_Minus:        return '-';
    case ImGuiKey_Slash:        return '/';
    case ImGuiKey_GraveAccent:  return '`';
    case ImGuiKey_Period:       return '.';
    case ImGuiKey_Comma:        return ',';
    case ImGuiKey_Semicolon:    return ';';
    case ImGuiKey_Apostrophe:   return '\'';
    case ImGuiKey_Equal:        return '=';
    default:                    return 0;
    }
}

// Keys and typing into a focused terminal.
static void terminput(Gui *g, GuiTerm *t, ImGuiID id)
{
    ImGuiIO &io   = ImGui::GetIO();
    Term    *tm   = t->term;
    i32      mods = termmods(io.KeyCtrl, io.KeyShift, io.KeyAlt, io.KeySuper, g->mac, 0);
    b32      sent = 0;

    // The keys read here are the terminal's while it has focus, not
    // ImGui's navigation's
    for (i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
        if (termkeyof((ImGuiKey)k) != VTERM_KEY_NONE) {
            ImGui::SetKeyOwner((ImGuiKey)k, id);
        }
    }

    // Pasting: Cmd+V, or Ctrl+Shift+V
    b32 paste = g->mac ? io.KeyCtrl && !io.KeyShift && !io.KeyAlt
                       : io.KeyCtrl && io.KeyShift && !io.KeyAlt;
    if (paste && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        char const *clip = ImGui::GetClipboardText();
        if (clip && *clip) {
            termpaste(tm, g->frame, {(u8 *)clip, (iz)__builtin_strlen(clip)});
            sent = 1;
        }
        mods = -1;  // and nothing else of it
    }

    if (mods >= 0) {
        for (i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
            ImGuiKey key = (ImGuiKey)k;
            i32      vk  = termkeyof(key);
            if (vk != VTERM_KEY_NONE) {
                if (ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, id)) {
                    termkey(tm, vk, mods);
                    sent = 1;
                }
            } else if (termcharkey(mods)) {
                u32 c = termcharof(key);
                if (c && ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, ImGuiKeyOwner_Any)) {
                    termchar(tm, c, mods);
                    sent = 1;
                }
            }
        }
    }

    // Typed text, unless the keys above took it (Ctrl, Meta)
    if (mods>=0 && !termcharkey(mods) && io.InputQueueCharacters.Size) {
        Arena a = g->frame;
        Buf   b(&a, io.InputQueueCharacters.Size*4);
        for (i32 i = 0; i < io.InputQueueCharacters.Size; i++) {
            u32 c = io.InputQueueCharacters[i];
            if (c>=0x20 && c!=0x7f) {
                u8 enc[4];
                print(&b, Str{enc, utf8encode(enc, (i32)c)});
            }
        }
        if (b.len) {
            termtype(tm, finish(&b));
            sent = 1;
        }
    }
    if (sent) {
        t->scroll = 0;
        t->wheel  = 0;
    }
}


// Drawing

// IM_COL32 (red low) and 0xRRGGBB, termrgb's
static u32 termrrggbb(u32 c)
{
    return (c&255)<<16 | (c>>8&255)<<8 | (c>>16&255);
}

static u32 termcol32(u32 c)
{
    return 0xff000000u | (c&255)<<16 | (c>>8&255)<<8 | (c>>16&255);
}

struct TermLook {
    u32 ansi[16];  // 0xRRGGBB
    u32 fg, bg;
};

// A cell's colors as drawn: resolved, reversed if it is.
static void termcolors(TermLook *l, Term *tm, TermCell c, u32 *fg, u32 *bg)
{
    u32 f = termrgb(c.fg, l->fg, l->ansi);
    u32 b = termrgb(c.bg, l->bg, l->ansi);
    if (!!(c.attrs & TERM_REVERSE) != !!tm->reverse) {
        u32 x = f;
        f = b;
        b = x;
    }
    *fg = termcol32(f);
    *bg = termcol32(b);
}

// The grid, rows by cols at o, cells cw by ch, from the scrollback's
// row top.
static void termgrid(Gui *g, GuiTerm *t, ImVec2 o, f32 cw, f32 lh, b32 focused)
{
    Term        *tm    = t->term;
    ImDrawList  *dl    = ImGui::GetWindowDrawList();
    Fonts       *f     = &g->fonts;
    f32          size  = stylesize(f, STYLE_CODE);
    ImFont      *plain = stylefont(f, STYLE_CODE);
    ImFont      *bold  = stylefont(f, STYLE_CODE|STYLE_BOLD);
    // A font whose advance is not the cell's (synthetic bold, wider by its
    // emboldening) draws cell by cell, else its runs drift off the grid
    f32          eps   = 0.01f;
    b32          runsplain = __builtin_fabsf(stylebaked(f, STYLE_CODE)->GetCharAdvance('M') - cw) < eps;
    b32          runsbold  = __builtin_fabsf(stylebaked(f, STYLE_CODE|STYLE_BOLD)->GetCharAdvance('M') - cw) < eps;
    f32          dy    = (lh - size)/2;
    TermLook     look  = {};
    for (i32 i = 0; i < 16; i++) {
        look.ansi[i] = termrrggbb(g->pal->ansi[i]);
    }
    look.fg = termrrggbb(g->pal->text);
    look.bg = termrrggbb(g->pal->panel);
    u32 panel = g->pal->panel;

    Arena a = g->frame;
    Buf   run(&a, tm->cols*4*TERM_MAXCHARS + 8);
    for (i32 r = 0; r < tm->rows; r++) {
        i32 row = r - t->scroll;
        f32 y   = o.y + (f32)r*lh;

        // Backgrounds, by runs of one color
        i32 beg = 0;
        u32 cur = panel;
        for (i32 col = 0; col <= tm->cols; col++) {
            u32 fg = 0, bg = panel;
            if (col < tm->cols) {
                termcolors(&look, tm, termcell(tm, row, col), &fg, &bg);
            }
            if (col==tm->cols || bg!=cur) {
                if (cur != panel && col > beg) {
                    dl->AddRectFilled(v2(o.x + (f32)beg*cw, y),
                                      v2(o.x + (f32)col*cw, y + lh), cur);
                }
                beg = col;
                cur = bg;
            }
        }

        // Text: runs of one color and weight, of single-width characters
        // that need no cell of their own, else cell by cell
        i32     start = 0;
        u32     runfg = 0;
        ImFont *runfont = plain;
        run.len = 0;
        for (i32 col = 0; col <= tm->cols; col++) {
            TermCell c = {};
            u32      fg = 0, bg = 0;
            ImFont  *font = plain;
            b32      simple = 1;
            if (col < tm->cols) {
                c = termcell(tm, row, col);
                termcolors(&look, tm, c, &fg, &bg);
                font   = c.attrs & TERM_BOLD ? bold : plain;
                simple = c.width==1 && c.ch[0]<0x80 && !c.ch[1] &&
                         (font==bold ? runsbold : runsplain);
            }
            if (run.len && (col==tm->cols || !simple || fg!=runfg || font!=runfont)) {
                dl->AddText(runfont, size, v2(o.x + (f32)start*cw, y + dy), runfg,
                            (char *)run.data, (char *)run.data + run.len);
                run.len = 0;
            }
            if (col == tm->cols) {
                break;
            }
            b32 shown = c.ch[0] && c.ch[0]!=' ' && !(c.attrs & TERM_CONCEAL);
            if (shown && simple) {
                if (!run.len) {
                    start   = col;
                    runfg   = fg;
                    runfont = font;
                }
                putbyte(&run, (u8)c.ch[0]);
            } else if (shown && c.width) {
                Buf one(&a, 4*TERM_MAXCHARS);
                for (i32 i = 0; i < termnchars(&c); i++) {
                    u8 enc[4];
                    print(&one, Str{enc, utf8encode(enc, (i32)c.ch[i])});
                }
                ImVec2 at = v2(o.x + (f32)col*cw, y);
                ImVec4 clip = ImVec4(at.x, at.y, at.x + cw*c.width, at.y + lh);
                dl->AddText(font, size, v2(at.x, y + dy), fg, (char *)one.data,
                            (char *)one.data + one.len, 0, &clip);
            } else if (run.len) {
                putbyte(&run, ' ');  // a blank inside a run keeps it whole
            }
            if (c.attrs & (TERM_UNDERLINE|TERM_STRIKE) && c.width) {
                f32 x0 = o.x + (f32)col*cw, x1 = x0 + cw*c.width;
                f32 th = (f32)(i32)(size/14 + 1);
                if (c.attrs & TERM_UNDERLINE) {
                    dl->AddRectFilled(v2(x0, y + lh - th - 1), v2(x1, y + lh - 1), fg);
                }
                if (c.attrs & TERM_STRIKE) {
                    dl->AddRectFilled(v2(x0, y + lh/2), v2(x1, y + lh/2 + th), fg);
                }
            }
        }
    }

    // The cursor, where the program put it, when the bottom shows
    i32 crow = tm->cursorrow + t->scroll;
    if (!tm->cursorvisible || t->exited || crow>=tm->rows || !t->proc) {
        return;
    }
    TermCell c = termcell(tm, tm->cursorrow, tm->cursorcol);
    u32      fg = 0, bg = 0;
    termcolors(&look, tm, c, &fg, &bg);
    f32    w  = cw * (c.width==2 ? 2 : 1);
    ImVec2 lo = v2(o.x + (f32)tm->cursorcol*cw, o.y + (f32)crow*lh);
    ImVec2 hi = v2(lo.x + w, lo.y + lh);
    if (!focused) {
        dl->AddRect(lo, hi, fg);
    } else if (tm->cursorshape == TERM_CURSOR_BAR) {
        dl->AddRectFilled(lo, v2(lo.x + (f32)(i32)(size/8 + 1), hi.y), fg);
    } else if (tm->cursorshape == TERM_CURSOR_UNDERLINE) {
        dl->AddRectFilled(v2(lo.x, hi.y - (f32)(i32)(size/8 + 1)), hi, fg);
    } else {
        dl->AddRectFilled(lo, hi, fg);
        if (c.ch[0] && c.ch[0]!=' ') {
            Buf one(&a, 4*TERM_MAXCHARS);
            for (i32 i = 0; i < termnchars(&c); i++) {
                u8 enc[4];
                print(&one, Str{enc, utf8encode(enc, (i32)c.ch[i])});
            }
            dl->AddText(c.attrs & TERM_BOLD ? bold : plain, size, v2(lo.x, lo.y + dy), bg,
                        (char *)one.data, (char *)one.data + one.len);
        }
    }
}

// The pane: the shown session's terminal, started the first time it
// shows, its grid filling the pane, and below it Restart once its
// program has exited.
static void drawterminal(Gui *g)
{
    GuiSession *s = g->sess;
    if (!s) {
        ImGui::TextDisabled("No session is shown");
        return;
    }
    Fonts      *f    = &g->fonts;
    ImGuiStyle &st   = ImGui::GetStyle();
    f32         cw   = stylebaked(f, STYLE_CODE)->GetCharAdvance('M');
    f32         lh   = fontmetrics(f, STYLE_CODE).lineh;
    GuiTerm    *t    = termfor(g, s);
    b32         done = t && (t->exited || !t->proc);
    ImVec2      room = ImGui::GetContentRegionAvail();
    if (done) {
        room.y -= ImGui::GetFrameHeightWithSpacing();
    }
    i32 rows = (i32)(room.y / lh);
    i32 cols = (i32)(room.x / (cw>0 ? cw : 1));
    rows = rows<2 ? 2 : rows;
    cols = cols<10 ? 10 : cols;

    if (!t) {
        t = termnewfor(g, s);
        termstart(g, t, s, rows, cols);
        done = t->exited || !t->proc;
    }
    if (!t->term) {
        text(t->why);
        return;
    }
    if (rows!=t->term->rows || cols!=t->term->cols) {
        termresize(t->term, rows, cols);
        if (t->proc && !t->exited) {
            os_ptyresize(g->os, t->proc, rows, cols);
        }
    }

    // The grid, an item that takes the keys while the pane has focus
    ImGuiID id = ImGui::GetID("##terminal");
    ImVec2  o  = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##terminal", v2(room.x, (f32)rows*lh));
    itemnamed(g, "terminal");
    b32 focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                  !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (focused != t->focused) {
        termfocus(t->term, focused);
        t->focused = focused;
    }
    if (focused && t->proc && !t->exited) {
        terminput(g, t, id);
    }

    // The wheel scrolls the scrollback, a notch three rows
    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && io.MouseWheel!=0 && !io.KeyCtrl && !t->term->altscreen) {
        t->wheel += io.MouseWheel*3;
        i32 steps = (i32)t->wheel;
        t->wheel -= (f32)steps;
        t->scroll += steps;
        t->scroll = t->scroll<0 ? 0 : t->scroll>t->term->sblen ? t->term->sblen : t->scroll;
    }

    ImGui::PushClipRect(o, v2(o.x + room.x, o.y + (f32)rows*lh), true);
    termgrid(g, t, o, cw, lh, focused);
    ImGui::PopClipRect();

    // Where an IME composes, and text input on (SDL sends none else)
    if (focused && t->proc && !t->exited) {
        ImGuiContext *ctx = ImGui::GetCurrentContext();
        ctx->PlatformImeData.WantVisible     = true;
        ctx->PlatformImeData.WantTextInput   = true;
        ctx->PlatformImeData.InputPos        = v2(o.x + (f32)t->term->cursorcol*cw,
                                                  o.y + (f32)(t->term->cursorrow + t->scroll)*lh);
        ctx->PlatformImeData.InputLineHeight = lh;
        ctx->PlatformImeData.ViewportId      = ImGui::GetWindowViewport()->ID;
    }

    if (done) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + st.ItemSpacing.y);
        if (t->why.len) {
            text(t->why);
            ImGui::SameLine();
        }
        b32 restart = ImGui::Button("Restart");
        itemnamed(g, "terminal restart");
        if (restart) {
            if (t->proc) {
                os_procfree(g->os, t->proc);  // exited: its output may still end
                t->proc = 0;
            }
            termstart(g, t, s, rows, cols);
            scheddirty(&g->sched);
        }
    }
}
