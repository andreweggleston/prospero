// Terminal model tests: text, colors and attributes, wide characters,
// scrollback and its ring, the alternate screen, resizing, the title
// and other properties, the replies to queries, and the bytes that keys,
// characters with modifiers, and pastes send.

// What the terminal sent its child, kept for the test.
static void termsink(void *ctx, Str bytes)
{
    Buf *b = (Buf *)ctx;
    print(b, bytes);
}

// Bytes with control characters shown caret style: ESC as ^[, DEL as ^?.
static Str termvis(Arena *a, Str s)
{
    Buf b(a, s.len*2 + 1);
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (c < 0x20) {
            putbyte(&b, '^');
            putbyte(&b, (u8)(c + 64));
        } else if (c == 0x7f) {
            print(&b, "^?");
        } else {
            putbyte(&b, c);
        }
    }
    return finish(&b);
}

// What the terminal sends for one input, then forgotten.
static Str termsent(Arena *a, Buf *out)
{
    Str r = termvis(a, finish(out));
    out->len = 0;
    return r;
}

static Str termhex(Arena *a, u32 v)
{
    Buf b(a, 16);
    static char const digits[] = "0123456789abcdef";
    for (i32 shift = 28; shift >= 0; shift -= 4) {
        putbyte(&b, (u8)digits[v>>shift & 15]);
    }
    return finish(&b);
}

static void test_term(Test *t)
{
    Arena a   = t->perm;
    Buf   out(&a, 1<<12);

    // Text, the cursor after it, and lines
    Term *tm = termnew(4, 20, 100, termsink, &out);
    if (!tm) {
        expect(t, "term.new", "null", "a terminal");
        return;
    }
    termwrite(tm, "hello");
    expect(t, "term.text", termrowtext(tm, &a, 0), "hello");
    expect(t, "term.cursor", istr(&a, tm->cursorrow*100 + tm->cursorcol), "5");
    expect(t, "term.dirty", istr(&a, tm->dirty), "1");
    termwrite(tm, "\r\nworld  \r\n");
    expect(t, "term.second", termrowtext(tm, &a, 1), "world");
    expect(t, "term.blankrow", termrowtext(tm, &a, 3), "");
    expect(t, "term.outside", istr(&a, termcell(tm, 9, 0).width), "1");

    // Scrolling off the top into the scrollback, newest at -1
    termwrite(tm, "l3\r\nl4\r\nl5\r\nl6");
    expect(t, "term.scrolled", termrowtext(tm, &a, 0), "l3");
    expect(t, "term.bottom", termrowtext(tm, &a, 3), "l6");
    expect(t, "term.sblen", istr(&a, tm->sblen), "2");
    expect(t, "term.sb1", termrowtext(tm, &a, -1), "world");
    expect(t, "term.sb2", termrowtext(tm, &a, -2), "hello");
    expect(t, "term.sbpast", termrowtext(tm, &a, -3), "");

    // Taller: lines come back from the scrollback
    termresize(tm, 6, 20);
    expect(t, "term.taller", termrowtext(tm, &a, 0), "hello");
    expect(t, "term.tallersb", istr(&a, tm->sblen), "0");
    expect(t, "term.tallerrows", istr(&a, tm->rows), "6");
    termresize(tm, 4, 20);
    expect(t, "term.shorter", istr(&a, tm->sblen), "2");
    expect(t, "term.shorterrow", termrowtext(tm, &a, 0), "l3");

    // Erasing the scrollback (CSI 3 J)
    termwrite(tm, "\x1b[3J");
    expect(t, "term.sbclear", istr(&a, tm->sblen), "0");
    termfree(tm);

    // The ring keeps the newest sbmax lines
    tm = termnew(2, 10, 3, termsink, &out);
    termwrite(tm, "a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng");
    expect(t, "term.ringlen", istr(&a, tm->sblen), "3");
    expect(t, "term.ringnew", termrowtext(tm, &a, -1), "e");
    expect(t, "term.ringold", termrowtext(tm, &a, -3), "c");
    termfree(tm);

    // No scrollback at all
    tm = termnew(2, 10, 0, termsink, &out);
    termwrite(tm, "a\r\nb\r\nc");
    expect(t, "term.nosb", istr(&a, tm->sblen), "0");
    expect(t, "term.nosbrow", termrowtext(tm, &a, -1), "");
    termfree(tm);

    // Colors: the 16, the 256, true color, and the defaults
    tm = termnew(3, 20, 10, termsink, &out);
    termwrite(tm, "\x1b[31mR\x1b[38;5;200mP\x1b[38;2;1;2;3mT\x1b[0mD"
                  "\x1b[44mB\x1b[1;3;4;7;9mA\x1b[0m");
    expect(t, "term.fg16", termhex(&a, termcell(tm, 0, 0).fg), termhex(&a, TERM_INDEXED|1));
    expect(t, "term.fg256", termhex(&a, termcell(tm, 0, 1).fg), termhex(&a, TERM_INDEXED|200));
    expect(t, "term.fgrgb", termhex(&a, termcell(tm, 0, 2).fg), "00010203");
    expect(t, "term.fgdef", termhex(&a, termcell(tm, 0, 3).fg), termhex(&a, TERM_DEFAULT));
    expect(t, "term.bgdef", termhex(&a, termcell(tm, 0, 3).bg), termhex(&a, TERM_DEFAULT));
    expect(t, "term.bg", termhex(&a, termcell(tm, 0, 4).bg), termhex(&a, TERM_INDEXED|4));
    expect(t, "term.attrs", istr(&a, termcell(tm, 0, 5).attrs),
           istr(&a, TERM_BOLD|TERM_ITALIC|TERM_UNDERLINE|TERM_REVERSE|TERM_STRIKE));
    expect(t, "term.plain", istr(&a, termcell(tm, 0, 6).attrs), "0");

    // Colors keep through the scrollback
    termwrite(tm, "\r\n\r\n\r\n");
    expect(t, "term.sbcolor", termhex(&a, termcell(tm, -1, 0).fg), termhex(&a, TERM_INDEXED|1));
    expect(t, "term.sbrgb", termhex(&a, termcell(tm, -1, 2).fg), "00010203");
    expect(t, "term.sbattrs", istr(&a, termcell(tm, -1, 5).attrs),
           istr(&a, TERM_BOLD|TERM_ITALIC|TERM_UNDERLINE|TERM_REVERSE|TERM_STRIKE));
    termfree(tm);

    // Resolving colors against the theme's 16
    u32 ansi[16];
    for (i32 i = 0; i < 16; i++) {
        ansi[i] = 0x111111u * (u32)(i % 16);
    }
    expect(t, "term.rgbdef", termhex(&a, termrgb(TERM_DEFAULT, 0xabcdef, ansi)), "00abcdef");
    expect(t, "term.rgbansi", termhex(&a, termrgb(TERM_INDEXED|3, 0, ansi)), "00333333");
    expect(t, "term.rgbcube", termhex(&a, termrgb(TERM_INDEXED|196, 0, ansi)), "00ff0000");
    expect(t, "term.rgbcube2", termhex(&a, termrgb(TERM_INDEXED|110, 0, ansi)), "0087afd7");
    expect(t, "term.rgbgray", termhex(&a, termrgb(TERM_INDEXED|232, 0, ansi)), "00080808");
    expect(t, "term.rgbgray2", termhex(&a, termrgb(TERM_INDEXED|255, 0, ansi)), "00eeeeee");
    expect(t, "term.rgbtrue", termhex(&a, termrgb(0x123456, 0, ansi)), "00123456");

    // Wide characters take two cells, the second empty
    tm = termnew(3, 10, 10, termsink, &out);
    termwrite(tm, "\xe3\x81\x82" "a\xe2\x9c\x93");  // あ a ✓
    expect(t, "term.wide", termhex(&a, termcell(tm, 0, 0).ch[0]), "00003042");
    expect(t, "term.widew", istr(&a, termcell(tm, 0, 0).width), "2");
    expect(t, "term.widehalf", istr(&a, termcell(tm, 0, 1).width), "0");
    expect(t, "term.widenext", termhex(&a, termcell(tm, 0, 2).ch[0]), "00000061");
    expect(t, "term.widetext", termrowtext(tm, &a, 0), "\xe3\x81\x82" "a\xe2\x9c\x93");
    expect(t, "term.widecursor", istr(&a, tm->cursorcol), "4");

    // A wide character keeps through the scrollback, and back
    termwrite(tm, "\r\n\r\n\r\n");
    expect(t, "term.sbwide", termrowtext(tm, &a, -1), "\xe3\x81\x82" "a\xe2\x9c\x93");
    expect(t, "term.sbwidew", istr(&a, termcell(tm, -1, 1).width), "0");
    termresize(tm, 4, 10);
    expect(t, "term.popwide", termrowtext(tm, &a, 0), "\xe3\x81\x82" "a\xe2\x9c\x93");
    expect(t, "term.popwidew", istr(&a, termcell(tm, 0, 0).width), "2");
    termfree(tm);

    // Characters that combine stay with the one before them, on the
    // screen and through the scrollback and back: an accent, joiners
    tm = termnew(3, 12, 10, termsink, &out);
    Str accent = "e\xcc\x81!";                                    // e + U+0301
    Str tone   = "\xf0\x9f\x91\x8b\xf0\x9f\x8f\xbd";              // U+1F44B U+1F3FD
    Str family = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
                 "\xf0\x9f\x91\xa7";                                // man ZWJ woman ZWJ girl
    termwrite(tm, accent);
    termwrite(tm, "\r\n");
    termwrite(tm, tone);
    termwrite(tm, family);
    TermCell e = termcell(tm, 0, 0);
    expect(t, "term.combine", istr(&a, termnchars(&e)), "2");
    expect(t, "term.combine2", termhex(&a, e.ch[1]), "00000301");
    expect(t, "term.combinenext", termhex(&a, termcell(tm, 0, 1).ch[0]), "00000021");
    expect(t, "term.combinetext", termrowtext(tm, &a, 0), accent);
    // An emoji modifier is a cell of its own, and a joiner combines with
    // the character before it, as wcwidth counts them: hand, tone, man
    // and joiner, woman and joiner, girl, each two columns
    TermCell w = termcell(tm, 1, 2);
    expect(t, "term.tone", termhex(&a, w.ch[0]), "0001f3fd");
    expect(t, "term.tonew", istr(&a, w.width), "2");
    TermCell f = termcell(tm, 1, 4);
    expect(t, "term.zwj", istr(&a, termnchars(&f)*10 + f.width), "22");
    expect(t, "term.zwj2", termhex(&a, f.ch[1]), "0000200d");
    expect(t, "term.zwjcursor", istr(&a, tm->cursorcol), "10");
    expect(t, "term.zwjtext", termrowtext(tm, &a, 1), concat(&a, tone, family));
    termwrite(tm, "\r\n\r\n\r\n");
    expect(t, "term.sbcombine", termrowtext(tm, &a, -2), accent);
    expect(t, "term.sbzwj", termrowtext(tm, &a, -1), concat(&a, tone, family));
    TermCell sf = termcell(tm, -1, 6);
    expect(t, "term.sbzwjn", istr(&a, termnchars(&sf)*10 + sf.width), "22");
    termresize(tm, 5, 12);
    expect(t, "term.popcombine", termrowtext(tm, &a, 0), accent);
    expect(t, "term.popzwj", termrowtext(tm, &a, 1), concat(&a, tone, family));
    termfree(tm);

    // The alternate screen: its own, nothing to the scrollback, and the
    // main screen as it was after
    tm = termnew(3, 10, 10, termsink, &out);
    termwrite(tm, "main");
    termwrite(tm, "\x1b[?1049h");
    expect(t, "term.alt", istr(&a, tm->altscreen), "1");
    expect(t, "term.altblank", termrowtext(tm, &a, 0), "");
    termwrite(tm, "1\r\n2\r\n3\r\n4\r\n5");
    expect(t, "term.altsb", istr(&a, tm->sblen), "0");
    termwrite(tm, "\x1b[?1049l");
    expect(t, "term.altoff", istr(&a, tm->altscreen), "0");
    expect(t, "term.altback", termrowtext(tm, &a, 0), "main");

    // Properties: the title in one piece or in several, the cursor's
    // visibility and shape, the bell, mouse reporting
    termwrite(tm, "\x1b]2;a title\x07");
    expect(t, "term.title", termtitle(tm), "a title");
    termwrite(tm, "\x1b]0;in two");
    termwrite(tm, " parts\x1b\\");
    expect(t, "term.title2", termtitle(tm), "in two parts");
    termwrite(tm, "\x1b[?25l");
    expect(t, "term.hidden", istr(&a, tm->cursorvisible), "0");
    termwrite(tm, "\x1b[?25h\x1b[6 q");
    expect(t, "term.shown", istr(&a, tm->cursorvisible), "1");
    expect(t, "term.bar", istr(&a, tm->cursorshape), istr(&a, TERM_CURSOR_BAR));
    expect(t, "term.steady", istr(&a, tm->cursorblink), "0");
    termwrite(tm, "\x1b[3 q");
    expect(t, "term.underline", istr(&a, tm->cursorshape), istr(&a, TERM_CURSOR_UNDERLINE));
    termwrite(tm, "\x07\x07");
    expect(t, "term.bells", istr(&a, tm->bells), "2");
    expect(t, "term.nomouse", istr(&a, termwantsmouse(tm)), "0");
    termwrite(tm, "\x1b[?1000h\x1b[?1006h");
    expect(t, "term.mouse", istr(&a, termwantsmouse(tm)), "1");
    termmouse(tm, 1, 2, 1, 1, 0);
    expect(t, "term.mousepress", termsent(&a, &out), "^[[<0;3;2M");
    termmouse(tm, 1, 2, 1, 0, 0);
    expect(t, "term.mouserelease", termsent(&a, &out), "^[[<0;3;2m");
    termwrite(tm, "\x1b[?1000l");
    expect(t, "term.mouseoff", istr(&a, termwantsmouse(tm)), "0");

    // Replies to queries go to the child: the cursor's position, device
    // attributes
    termwrite(tm, "\x1b[H\x1b[2;3H\x1b[6n");
    expect(t, "term.dsr", termsent(&a, &out), "^[[2;3R");
    termwrite(tm, "\x1b[c");
    expect(t, "term.da", istr(&a, startswith(termsent(&a, &out), "^[[?")), "1");

    // Focus, reported only if asked
    termfocus(tm, 1);
    expect(t, "term.nofocus", termsent(&a, &out), "");
    termwrite(tm, "\x1b[?1004h");
    termfocus(tm, 0);
    expect(t, "term.focusout", termsent(&a, &out), "^[[O");
    termfocus(tm, 1);
    expect(t, "term.focusin", termsent(&a, &out), "^[[I");

    // Typed text goes as it is
    termtype(tm, "h\xc3\xa9");
    expect(t, "term.type", termsent(&a, &out), "h\xc3\xa9");

    // Characters with Ctrl and Alt: control bytes, ESC in front
    termchar(tm, 'c', TERM_CTRL);
    expect(t, "term.ctrlc", termsent(&a, &out), "^C");
    termchar(tm, 'C', TERM_CTRL|TERM_SHIFT);
    expect(t, "term.ctrlshiftc", termsent(&a, &out), "^C");
    termchar(tm, 'x', TERM_CTRL|TERM_ALT);
    expect(t, "term.ctrlaltx", termsent(&a, &out), "^[^X");
    termchar(tm, 'b', TERM_ALT);
    expect(t, "term.altb", termsent(&a, &out), "^[b");
    termchar(tm, ' ', TERM_CTRL);
    expect(t, "term.ctrlspace", termsent(&a, &out), "^@");
    termchar(tm, '[', TERM_CTRL);
    expect(t, "term.ctrlbracket", termsent(&a, &out), "^[");
    termchar(tm, 'i', TERM_CTRL);
    expect(t, "term.ctrli", termsent(&a, &out), "^I");
    termchar(tm, 'm', TERM_CTRL);
    expect(t, "term.ctrlm", termsent(&a, &out), "^M");
    termchar(tm, '\\', TERM_CTRL);
    expect(t, "term.ctrlbackslash", termsent(&a, &out), "^\\");
    termchar(tm, '-', TERM_CTRL|TERM_SHIFT);
    expect(t, "term.ctrlunderscore", termsent(&a, &out), "^_");
    termchar(tm, '/', TERM_CTRL);
    expect(t, "term.ctrlslash", termsent(&a, &out), "^_");
    termchar(tm, '8', TERM_CTRL);
    expect(t, "term.ctrl8", termsent(&a, &out), "^?");
    termchar(tm, '1', TERM_CTRL);
    expect(t, "term.ctrl1", termsent(&a, &out), "1");
    termchar(tm, 0xe9, TERM_ALT);
    expect(t, "term.alte", termsent(&a, &out), "^[\xc3\xa9");

    // Keys: cursor keys, application mode, function keys, editing keys
    termkey(tm, VTERM_KEY_UP, 0);
    expect(t, "term.up", termsent(&a, &out), "^[[A");
    termkey(tm, VTERM_KEY_RIGHT, TERM_CTRL);
    expect(t, "term.ctrlright", termsent(&a, &out), "^[[1;5C");
    termkey(tm, VTERM_KEY_HOME, 0);
    expect(t, "term.home", termsent(&a, &out), "^[[H");
    termkey(tm, VTERM_KEY_DEL, 0);
    expect(t, "term.del", termsent(&a, &out), "^[[3~");
    termkey(tm, VTERM_KEY_PAGEUP, TERM_SHIFT);
    expect(t, "term.shiftpgup", termsent(&a, &out), "^[[5;2~");
    termkey(tm, VTERM_KEY_FUNCTION(1), 0);
    expect(t, "term.f1", termsent(&a, &out), "^[OP");
    termkey(tm, VTERM_KEY_FUNCTION(5), 0);
    expect(t, "term.f5", termsent(&a, &out), "^[[15~");
    termwrite(tm, "\x1b[?1h");
    termkey(tm, VTERM_KEY_UP, 0);
    expect(t, "term.appup", termsent(&a, &out), "^[OA");
    termwrite(tm, "\x1b[?1l");

    // Enter, Tab, Backspace and Escape: Shift and Ctrl change nothing,
    // but Shift+Tab and Ctrl+Backspace; Alt puts ESC in front
    termkey(tm, VTERM_KEY_ENTER, 0);
    expect(t, "term.enter", termsent(&a, &out), "^M");
    termkey(tm, VTERM_KEY_ENTER, TERM_SHIFT);
    expect(t, "term.shiftenter", termsent(&a, &out), "^M");
    termkey(tm, VTERM_KEY_ENTER, TERM_CTRL);
    expect(t, "term.ctrlenter", termsent(&a, &out), "^M");
    termkey(tm, VTERM_KEY_ENTER, TERM_ALT);
    expect(t, "term.altenter", termsent(&a, &out), "^[^M");
    termkey(tm, VTERM_KEY_TAB, 0);
    expect(t, "term.tab", termsent(&a, &out), "^I");
    termkey(tm, VTERM_KEY_TAB, TERM_SHIFT);
    expect(t, "term.backtab", termsent(&a, &out), "^[[Z");
    termkey(tm, VTERM_KEY_TAB, TERM_CTRL);
    expect(t, "term.ctrltab", termsent(&a, &out), "^I");
    termkey(tm, VTERM_KEY_BACKSPACE, 0);
    expect(t, "term.backspace", termsent(&a, &out), "^?");
    termkey(tm, VTERM_KEY_BACKSPACE, TERM_SHIFT);
    expect(t, "term.shiftbackspace", termsent(&a, &out), "^?");
    termkey(tm, VTERM_KEY_BACKSPACE, TERM_CTRL);
    expect(t, "term.ctrlbackspace", termsent(&a, &out), "^H");
    termkey(tm, VTERM_KEY_BACKSPACE, TERM_ALT);
    expect(t, "term.altbackspace", termsent(&a, &out), "^[^?");

    // Pastes: newlines as Enter, bracketed once the program asks
    termpaste(tm, a, "a\nb\r\nc\rd");
    expect(t, "term.paste", termsent(&a, &out), "a^Mb^Mc^Md");
    termwrite(tm, "\x1b[?2004h");
    termpaste(tm, a, "ls\n");
    expect(t, "term.bracketed", termsent(&a, &out), "^[[200~ls^M^[[201~");
    termfree(tm);
    termfree(0);

    // Modifiers as ImGui reports them: on macOS Cmd (ctrl) is Prospero's,
    // the Control key (super) is Ctrl, and Option is Meta only if asked
    //                        ctrl shift alt super mac meta
    expect(t, "term.modsmaccmd", istr(&a, termmods(1, 0, 0, 0, 1, 0)), "-1");
    expect(t, "term.modsmacctrl", istr(&a, termmods(0, 0, 0, 1, 1, 0)), istr(&a, TERM_CTRL));
    expect(t, "term.modsmacopt", istr(&a, termmods(0, 0, 1, 0, 1, 0)), istr(&a, TERM_OPTION));
    expect(t, "term.modsmacmeta", istr(&a, termmods(0, 1, 1, 0, 1, 1)),
           istr(&a, TERM_ALT|TERM_SHIFT));
    expect(t, "term.modsctrl", istr(&a, termmods(1, 0, 0, 0, 0, 0)), istr(&a, TERM_CTRL));
    expect(t, "term.modsalt", istr(&a, termmods(0, 0, 1, 0, 0, 0)), istr(&a, TERM_ALT));
    expect(t, "term.modssuper", istr(&a, termmods(0, 0, 0, 1, 0, 0)), "0");
    expect(t, "term.charkey", istr(&a, termcharkey(TERM_CTRL)), "1");
    expect(t, "term.charkeyalt", istr(&a, termcharkey(TERM_ALT|TERM_SHIFT)), "1");
    expect(t, "term.charkeyshift", istr(&a, termcharkey(TERM_SHIFT)), "0");
    expect(t, "term.charkeymac", istr(&a, termcharkey(-1)), "0");
    expect(t, "term.charkeyopt", istr(&a, termcharkey(TERM_OPTION)), "0");
}
