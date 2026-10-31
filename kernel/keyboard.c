#include "keyboard.h"
#include "io.h"
#include "kstring.h"

#define QSIZE 64
static struct key_event queue[QSIZE];
static volatile uint32_t qhead, qtail;

static int layout = LAYOUT_EN;
static int lshift, rshift, ctrl, alt, caps, got_e0;

static char     en_n[128], en_s[128];
static uint32_t ru_map[128];
static int      tables_ready;

static const uint32_t ru_q[] = { 0x439,0x446,0x443,0x43A,0x435,0x43D,0x433,0x448,0x449,0x437,0x445,0x44A };  /* й ц у к е н г ш щ з х ъ */
static const uint32_t ru_a[] = { 0x444,0x44B,0x432,0x430,0x43F,0x440,0x43E,0x43B,0x434,0x436,0x44D };        /* ф ы в а п р о л д ж э */
static const uint32_t ru_z[] = { 0x44F,0x447,0x441,0x43C,0x438,0x442,0x44C,0x431,0x44E };                    /* я ч с м и т ь б ю */

static void build_tables(void) {
    static const char n1[] = "1234567890-=",  s1[] = "!@#$%^&*()_+";
    static const char n2[] = "qwertyuiop[]",  s2[] = "QWERTYUIOP{}";
    static const char n3[] = "asdfghjkl;'`",  s3[] = "ASDFGHJKL:\"~";
    static const char n4[] = "\\zxcvbnm,./",  s4[] = "|ZXCVBNM<>?";
    en_n[0x01] = en_s[0x01] = 27;
    for (int i = 0; i < 12; i++) { en_n[0x02 + i] = n1[i]; en_s[0x02 + i] = s1[i]; }
    en_n[0x0E] = en_s[0x0E] = '\b';
    en_n[0x0F] = en_s[0x0F] = '\t';
    for (int i = 0; i < 12; i++) { en_n[0x10 + i] = n2[i]; en_s[0x10 + i] = s2[i]; }
    en_n[0x1C] = en_s[0x1C] = '\n';
    for (int i = 0; i < 12; i++) { en_n[0x1E + i] = n3[i]; en_s[0x1E + i] = s3[i]; }
    for (int i = 0; i < 11; i++) { en_n[0x2B + i] = n4[i]; en_s[0x2B + i] = s4[i]; }
    en_n[0x39] = en_s[0x39] = ' ';

    for (int i = 0; i < 12; i++) ru_map[0x10 + i] = ru_q[i];
    for (int i = 0; i < 11; i++) ru_map[0x1E + i] = ru_a[i];
    ru_map[0x29] = 0x451;                                  /* ё */
    for (int i = 0; i < 9; i++)  ru_map[0x2C + i] = ru_z[i];
    tables_ready = 1;
}

static uint32_t to_upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;
    if (cp == 0x451) return 0x401;
    return cp;
}

static void push(uint32_t cp, uint8_t mods) {
    uint32_t next = (qhead + 1) % QSIZE;
    if (next == qtail) return;                 /* очередь полна */
    queue[qhead].cp = cp;
    queue[qhead].mods = mods;
    qhead = next;
}

void kbd_init(void) {
    if (!tables_ready) build_tables();
    while (inb(0x64) & 1) inb(0x60);
}

void kbd_irq(void) { kbd_feed_scancode(inb(0x60)); }

int kbd_layout(void) { return layout; }
void kbd_set_layout(int l) { layout = l ? LAYOUT_RU : LAYOUT_EN; }

void kbd_feed_scancode(uint8_t sc) {
    if (!tables_ready) build_tables();
    if (sc == 0xE0) { got_e0 = 1; return; }
    if (sc == 0xE1) return;                    /* Pause — игнорируем */
    int release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    int ext = got_e0;
    got_e0 = 0;

    if (!ext && code == 0x2A) { lshift = !release; return; }
    if (!ext && code == 0x36) { rshift = !release; return; }
    if (code == 0x1D) { ctrl = !release; return; }
    if (code == 0x38) { alt = !release; return; }
    if (release) return;
    if (!ext && code == 0x3A) { caps = !caps; return; }

    int shift = lshift || rshift;
    uint8_t mods = (uint8_t)((shift ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0));

    if (ext) {
        switch (code) {
        case 0x48: push(KEY_UP, mods); break;
        case 0x50: push(KEY_DOWN, mods); break;
        case 0x4B: push(KEY_LEFT, mods); break;
        case 0x4D: push(KEY_RIGHT, mods); break;
        case 0x47: push(KEY_HOME, mods); break;
        case 0x4F: push(KEY_END, mods); break;
        case 0x53: push(KEY_DELETE, mods); break;
        case 0x49: push(KEY_PGUP, mods); break;
        case 0x51: push(KEY_PGDN, mods); break;
        case 0x52: push(KEY_INSERT, mods); break;
        case 0x1C: push('\n', mods); break;
        case 0x35: push('/', mods); break;
        default: break;
        }
        return;
    }
    if (code >= 0x3B && code <= 0x44) { push(KEY_F(code - 0x3B + 1), mods); return; }
    if (code == 0x57) { push(KEY_F(11), mods); return; }
    if (code == 0x58) { push(KEY_F(12), mods); return; }
    if (code >= 0x59) return;

    char base = en_n[code];
    if (!base) return;

    /* Сочетания с Ctrl/Alt всегда по латинской раскладке — горячие клавиши не зависят от языка */
    if (ctrl || alt) { push((uint32_t)(uint8_t)base, mods); return; }

    uint32_t cp;
    if (layout == LAYOUT_RU && ru_map[code]) {
        cp = ru_map[code];
        if (shift != caps) cp = to_upper(cp);
    } else if (layout == LAYOUT_RU && code == 0x35) {
        cp = shift ? ',' : '.';
    } else {
        int letter = (base >= 'a' && base <= 'z');
        if (letter) cp = (shift != caps) ? (uint32_t)en_s[code] : (uint32_t)base;
        else        cp = (uint32_t)(uint8_t)(shift ? en_s[code] : base);
    }
    push(cp, mods);
}

int kbd_poll(struct key_event *ev) {
    if (qtail == qhead) return 0;
    *ev = queue[qtail];
    qtail = (qtail + 1) % QSIZE;
    return 1;
}
