/* Терминал LimonOS: ввод строки, история и уникальные "лимонные" команды. */
#include "shell.h"
#include "format.h"
#include "kstring.h"
#include "sysinfo.h"
#include "timer.h"
#include "rtc.h"
#include "desktop.h"
#include "keyboard.h"
#include "gfx.h"
#include "net.h"
#include "pci.h"
#include "mem.h"
#include "http.h"
#include "tls.h"
#include "crypto.h"
#include "ftp.h"
#include "vfs.h"
#include "fat32.h"
#include "games.h"
#include "ai.h"
#include "user.h"
#include "sched.h"

#define PROMPT      "лимон> "
#define PROMPT_COLS 7

/* ---------- редактирование строки ---------- */

/* Строка ввода может быть длиннее экрана (ключи API, адреса) — прокручиваем её по горизонтали,
   так что курсор всегда виден. Когда что-то скрыто слева или справа, показываем « или » на краю. */
/* Та же задача, что у любой командной строки: строка длиннее экрана, курсор должен быть виден всегда.
   Без прокрутки помещается vis_full-1 символов (последний столбец бережём под курсор сразу за текстом,
   как и раньше). Если не влезает — включаем прокрутку и ВСЕГДА резервируем по столбцу под « и » сразу
   с обеих сторон, даже если нужен только один — так проще и курсор гарантированно не уедет за край. */
static void repaint(struct shell *s) {
    struct term *t = s->t;
    int row = t->cy, col = 0;
    int vis_full = TERM_COLS - PROMPT_COLS;
    int lo, hi, more_left, more_right;
    if (s->len <= vis_full - 1) {
        s->scroll = 0; lo = 0; hi = s->len; more_left = 0; more_right = 0;
    } else {
        int vis = vis_full - 2;
        if (s->pos < s->scroll) s->scroll = s->pos;
        if (s->pos - s->scroll >= vis) s->scroll = s->pos - vis + 1;
        if (s->scroll < 0) s->scroll = 0;
        if (s->scroll > s->len - vis) s->scroll = s->len - vis;
        lo = s->scroll; hi = lo + vis; if (hi > s->len) hi = s->len;
        more_left = lo > 0; more_right = hi < s->len;
    }
    for (int c = 0; c < TERM_COLS; c++) t->cell[row][c] = (struct tcell){ ' ', TC_DEFAULT };
    const char *p = PROMPT;
    while (*p) t->cell[row][col++] = (struct tcell){ utf8_next(&p), TC_ACCENT };
    if (more_left) t->cell[row][col++] = (struct tcell){ 0xAB, TC_GRAY };                         /* « */
    for (int i = lo; i < hi; i++) t->cell[row][col++] = (struct tcell){ s->line[i], TC_DEFAULT };
    if (more_right) t->cell[row][col++] = (struct tcell){ 0xBB, TC_GRAY };                        /* » */
    t->cx = PROMPT_COLS + (more_left ? 1 : 0) + (s->pos - lo);
}

static void show_prompt(struct shell *s) {
    if (s->t->cx != 0) term_putc(s->t, '\n');
    s->len = s->pos = s->scroll = 0;
    s->hist_idx = -1;
    term_set_fg(s->t, TC_DEFAULT);
    repaint(s);
}

static void load_line(struct shell *s, const char *utf8) {
    s->len = 0;
    while (*utf8 && s->len < SH_LINE_MAX) s->line[s->len++] = utf8_next(&utf8);
    s->pos = s->len;
    s->scroll = 0;
}

static void add_history(struct shell *s, const char *line) {
    if (s->hist_count > 0 && strcmp(s->hist[s->hist_count - 1], line) == 0) return;
    if (s->hist_count == SH_HIST) {
        for (int i = 1; i < SH_HIST; i++) memcpy(s->hist[i - 1], s->hist[i], SH_HIST_BYTES);
        s->hist_count--;
    }
    int n = 0;
    while (line[n] && n < SH_HIST_BYTES - 1) { s->hist[s->hist_count][n] = line[n]; n++; }
    s->hist[s->hist_count][n] = 0;
    s->hist_count++;
}

/* ---------- вспомогательное ---------- */

static void uptime_str(char *buf, size_t n) {
    uint64_t sec = timer_ticks / TIMER_HZ;
    ksnprintf(buf, n, "%02d:%02d:%02d", (int)(sec / 3600), (int)(sec / 60 % 60), (int)(sec % 60));
}

static unsigned long long mib(uint64_t bytes) { return (unsigned long long)(bytes >> 20); }

/* ---------- команды ---------- */

struct cmd {
    const char *name, *alt, *desc;
    void (*fn)(struct shell *, int, char **);
};

static void cmd_zest(struct shell *s, int argc, char **argv);
static void cmd_ring3(struct shell *s, int argc, char **argv);
static void cmd_run(struct shell *s, int argc, char **argv);
static void cmd_bg(struct shell *s, int argc, char **argv);
static void cmd_jobs(struct shell *s, int argc, char **argv);
static void cmd_go(struct shell *s, int argc, char **argv);

static void cmd_squeeze(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    term_clear(s->t);
}

static void cmd_peel(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct term *t = s->t;
    char up[16];
    uptime_str(up, sizeof up);

    char info[12][96];
    int n = 0;
    ksnprintf(info[n++], 96, "limon@os");
    ksnprintf(info[n++], 96, "--------");
    ksnprintf(info[n++], 96, "ОС:        LimonOS %s", LIMON_VERSION);
    ksnprintf(info[n++], 96, "Экран:     %ux%u", g_sys.fb_w, g_sys.fb_h);
    {
        const char *src = g_sys.cpu.brand[0] ? g_sys.cpu.brand : g_sys.cpu.vendor;
        char cpu[37];
        int k = 0;
        while (src[k] && k < 36) { cpu[k] = src[k]; k++; }
        cpu[k] = 0;
        ksnprintf(info[n++], 96, "Процессор: %s", cpu);
    }
    if (g_sys.mem_valid) ksnprintf(info[n++], 96, "Память:    %llu МиБ свободно", mib(g_sys.mem_usable));
    else                 ksnprintf(info[n++], 96, "Память:    нет данных");
    ksnprintf(info[n++], 96, "Работаю:   %s", up);
    ksnprintf(info[n++], 96, "Среда:     %s", cpuinfo_env_name(&g_sys.cpu));
    {
        const struct netinfo *ni = net_info();
        char ipb[16];
        if (ni->configured) { net_ip_str(ni->ip, ipb); ksnprintf(info[n++], 96, "Сеть:      %s", ipb); }
        else ksnprintf(info[n++], 96, "Сеть:      %s", ni->present ? "нет адреса" : "нет карты");
    }
    ksnprintf(info[n++], 96, "Тема:      %s", desktop_theme_name());
    ksnprintf(info[n++], 96, "Раскладка: %s", kbd_layout() == LAYOUT_RU ? "RU" : "EN");

    /* лимон из полублоков: сетка 20x14 "подпикселей" */
    for (int row = 0; row < 8; row++) {
        char art[28 * 3 + 1];
        int pos = 0;
        for (int c = 0; c < 28; c++) {
            int dx = 2 * c - 27;
            int top = 0, bot = 0;
            for (int h = 0; h < 2; h++) {
                int y = row * 2 + h;
                int dy = 2 * y - 15;
                int inside = (dx * dx * 225 + dy * dy * 729) <= 729 * 225;
                if (h == 0) top = inside; else bot = inside;
            }
            const char *ch = (top && bot) ? "█" : top ? "▀" : bot ? "▄" : " ";
            for (int k = 0; ch[k]; k++) art[pos++] = ch[k];
        }
        art[pos] = 0;
        term_set_fg(t, TC_ACCENT);
        term_printf(t, " %s  ", art);
        term_set_fg(t, row == 0 ? TC_ACCENT : TC_DEFAULT);
        if (row < n) term_printf(t, "%s", info[row]);
        term_putc(t, '\n');
    }
    term_set_fg(t, TC_DEFAULT);
    for (int k = 8; k < n; k++) term_printf(t, " %-28s  %s\n", "", info[k]);
}

static void bar(struct term *t, uint64_t part, uint64_t total, int width) {
    int filled = total ? (int)((part * (uint64_t)width) / total) : 0;
    term_set_fg(t, TC_ACCENT);
    for (int i = 0; i < width; i++) term_write(t, i < filled ? "▄" : "_");
    term_set_fg(t, TC_DEFAULT);
}

static void cmd_juice(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct term *t = s->t;
    if (!g_sys.mem_valid) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Карта памяти недоступна (загрузчик её не передал).\n");
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    uint64_t total = g_sys.mem_usable + g_sys.mem_reclaim + g_sys.mem_reserved;
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Сколько сока осталось\n");
    term_set_fg(t, TC_DEFAULT);
    term_printf(t, "  Свободно:        %6llu МиБ  ", mib(g_sys.mem_usable));
    bar(t, g_sys.mem_usable, total, 30); term_putc(t, '\n');
    term_printf(t, "  Освобождаемая:   %6llu МиБ  ", mib(g_sys.mem_reclaim));
    bar(t, g_sys.mem_reclaim, total, 30); term_putc(t, '\n');
    term_printf(t, "  Занято системой: %6llu МиБ  ", mib(g_sys.mem_reserved));
    bar(t, g_sys.mem_reserved, total, 30); term_putc(t, '\n');
    term_printf(t, "  Всего по карте:  %6llu МиБ\n", mib(total));
}

static void cmd_engine(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct term *t = s->t;
    const struct cpu_info *c = &g_sys.cpu;
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Мотор (процессор)\n");
    term_set_fg(t, TC_DEFAULT);
    term_printf(t, "  Производитель: %s\n", c->vendor);
    term_printf(t, "  Модель:        %s\n", c->brand[0] ? c->brand : "(не сообщает)");
    term_printf(t, "  Семейство %u, модель %u, ревизия %u\n", c->family, c->model, c->stepping);
    term_printf(t, "  Логических ядер: %u\n", c->logical);
    term_printf(t, "  Возможности:  ");
    if (c->feat_edx & (1u << 26)) term_printf(t, "SSE2 ");
    if (c->feat_ecx & (1u << 0))  term_printf(t, "SSE3 ");
    if (c->feat_ecx & (1u << 19)) term_printf(t, "SSE4.1 ");
    if (c->feat_ecx & (1u << 20)) term_printf(t, "SSE4.2 ");
    if (c->feat_ecx & (1u << 25)) term_printf(t, "AES ");
    if (c->feat_ecx & (1u << 28)) term_printf(t, "AVX ");
    if (c->feat7_ebx & (1u << 5)) term_printf(t, "AVX2 ");
    term_putc(t, '\n');
    term_printf(t, "  Среда: %s\n", cpuinfo_env_name(c));
}

static void cmd_sundial(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct rtc_time r;
    rtc_read(&r);
    term_printf(s->t, "Солнечные часы: %02d.%02d.%04d, %02d:%02d:%02d (часы BIOS)\n",
                r.day, r.month, r.year, r.hour, r.min, r.sec);
}

static void cmd_pulse(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    char up[16];
    uptime_str(up, sizeof up);
    term_printf(s->t, "Пульс стабильный. Работаю уже %s (%llu тиков, %d Гц)\n",
                up, (unsigned long long)timer_ticks, TIMER_HZ);
}

static void cmd_say(struct shell *s, int argc, char **argv) {
    for (int i = 1; i < argc; i++) term_printf(s->t, "%s%s", i > 1 ? " " : "", argv[i]);
    term_putc(s->t, '\n');
}

/* калькулятор: рекурсивный разбор выражения */
struct calc { const char *p; int err; };

static void skip_sp(struct calc *c) { while (*c->p == ' ') c->p++; }
static long long c_expr(struct calc *c);

static long long c_atom(struct calc *c) {
    skip_sp(c);
    if (*c->p == '(') {
        c->p++;
        long long v = c_expr(c);
        skip_sp(c);
        if (*c->p == ')') c->p++; else c->err = 1;
        return v;
    }
    if (*c->p == '-') { c->p++; return -c_atom(c); }
    if (*c->p == '+') { c->p++; return c_atom(c); }
    long long v = 0;
    if (c->p[0] == '0' && (c->p[1] == 'x' || c->p[1] == 'X')) {
        c->p += 2;
        int any = 0;
        for (;; c->p++) {
            int d;
            if (*c->p >= '0' && *c->p <= '9') d = *c->p - '0';
            else if (*c->p >= 'a' && *c->p <= 'f') d = *c->p - 'a' + 10;
            else if (*c->p >= 'A' && *c->p <= 'F') d = *c->p - 'A' + 10;
            else break;
            v = v * 16 + d; any = 1;
        }
        if (!any) c->err = 1;
        return v;
    }
    if (*c->p < '0' || *c->p > '9') { c->err = 1; return 0; }
    while (*c->p >= '0' && *c->p <= '9') v = v * 10 + (*c->p++ - '0');
    return v;
}

static long long c_term(struct calc *c) {
    long long v = c_atom(c);
    for (;;) {
        skip_sp(c);
        char op = *c->p;
        if (op != '*' && op != '/' && op != '%') return v;
        c->p++;
        long long r = c_atom(c);
        if (c->err) return 0;
        if (op == '*') v *= r;
        else if (r == 0) { c->err = 2; return 0; }
        else if (r == -1) v = op == '/' ? -v : 0;
        else v = op == '/' ? v / r : v % r;
    }
}

static long long c_expr(struct calc *c) {
    long long v = c_term(c);
    for (;;) {
        skip_sp(c);
        char op = *c->p;
        if (op != '+' && op != '-') return v;
        c->p++;
        long long r = c_term(c);
        v = op == '+' ? v + r : v - r;
    }
}

static void cmd_abacus(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    char expr[SH_HIST_BYTES];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && n < (int)sizeof expr - 1) expr[n++] = ' ';
        for (const char *p = argv[i]; *p && n < (int)sizeof expr - 1; p++) expr[n++] = *p;
    }
    expr[n] = 0;
    if (n == 0) { term_printf(t, "Пример: abacus 2+3*(4-1)\n"); return; }
    struct calc c = { expr, 0 };
    long long v = c_expr(&c);
    skip_sp(&c);
    if (*c.p) c.err = c.err ? c.err : 1;
    if (c.err) {
        term_set_fg(t, TC_RED);
        term_printf(t, c.err == 2 ? "На ноль делить нельзя, даже лимоны этого не позволяют.\n"
                                  : "Не понял выражение. Пример: abacus 2+3*(4-1)\n");
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "= %lld", v);
    term_set_fg(t, TC_GRAY);
    term_printf(t, "   (0x%llX)\n", (unsigned long long)v);
    term_set_fg(t, TC_DEFAULT);
}

static void cmd_palette(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct term *t = s->t;
    term_printf(t, "Темы оформления:\n");
    for (int i = 0; i < desktop_theme_count(); i++) {
        term_set_fg(t, desktop_theme_accent(i));
        term_printf(t, "  ██████  ");
        term_set_fg(t, TC_DEFAULT);
        term_printf(t, "%s%s\n", desktop_theme_name_at(i), i == desktop_theme_index() ? "   ← сейчас" : "");
    }
    static const uint32_t rainbow[] = { 0xFF6B6B, 0xFFA94D, 0xF4D91A, 0x7BD88F, 0x4DD0E1, 0x6CB6FF, 0xB48EF0, 0xF06EAA };
    term_printf(t, "\nЦвета терминала:\n  ");
    for (unsigned i = 0; i < sizeof rainbow / sizeof rainbow[0]; i++) {
        term_set_fg(t, rainbow[i]);
        term_printf(t, "████ ");
    }
    term_set_fg(t, TC_DEFAULT);
    term_printf(t, "\nСмена темы: tint <название>\n");
}

static void cmd_tint(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) {
        term_printf(t, "Темы:");
        for (int i = 0; i < desktop_theme_count(); i++) term_printf(t, " %s", desktop_theme_name_at(i));
        term_printf(t, "\nПример: tint lime\n");
        return;
    }
    int idx = desktop_theme_find(argv[1]);
    if (idx < 0) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Нет такой темы: %s. Список: tint\n", argv[1]);
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    desktop_set_theme(idx);
    term_printf(t, "Тема сменена на ");
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "%s\n", desktop_theme_name());
    term_set_fg(t, TC_DEFAULT);
}

static void cmd_lang(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc >= 2) {
        if (strcmp(argv[1], "ru") == 0) kbd_set_layout(LAYOUT_RU);
        else if (strcmp(argv[1], "en") == 0) kbd_set_layout(LAYOUT_EN);
        else { term_printf(t, "Используй: lang ru  или  lang en\n"); return; }
    }
    term_printf(t, "Раскладка: %s (переключение: F2)\n", kbd_layout() == LAYOUT_RU ? "русская" : "английская");
}

static void cmd_twin(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    if (desktop_open_terminal() < 0) {
        term_set_fg(s->t, TC_RED);
        term_printf(s->t, "Слишком много терминалов (максимум 4).\n");
        term_set_fg(s->t, TC_DEFAULT);
    }
}

static const char *const omens[] = {
    "Если жизнь дала тебе лимон, сделай из него операционную систему.",
    "Кислое сегодня, сладкое завтра. Но компилятор всегда кислый.",
    "Хороший код как лимон: сначала кисло, потом освежает.",
    "Ядро не паникует. Ядро размышляет.",
    "Один бит в день и ОС готова.",
    "Не бойся segfault: лимонад тоже начинается с выжимки.",
    "Сегодня ты напишешь драйвер, и он заработает с первого раза. (Нет.)",
    "Лучший отладчик это стакан чая с лимоном.",
};

static void cmd_omen(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    static uint32_t rng = 12345;
    rng = rng * 1664525u + 1013904223u + (uint32_t)timer_ticks;
    unsigned n = sizeof omens / sizeof omens[0];
    term_set_fg(s->t, TC_ACCENT);
    term_printf(s->t, "Знамение: ");
    term_set_fg(s->t, TC_DEFAULT);
    term_printf(s->t, "%s\n", omens[(rng >> 16) % n]);
}

static void cmd_pci(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct term *t = s->t;
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Шина PCI: найдено устройств %d\n", pci_count());
    term_set_fg(t, TC_DEFAULT);
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_get(i);
        term_set_fg(t, TC_GRAY);
        term_printf(t, "  %02x:%02x.%d %04x:%04x ", d->bus, d->dev, d->fn, d->vendor, d->device);
        term_set_fg(t, TC_DEFAULT);
        term_printf(t, "%s\n", pci_device_name(d));
    }
}

static void print_ip(struct term *t, const char *label, uint32_t ip) {
    char b[16];
    net_ip_str(ip, b);
    term_printf(t, "  %-9s %s\n", label, ip ? b : "—");
}

static void cmd_vine(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc >= 2 && strcmp(argv[1], "dhcp") == 0) {
        term_printf(t, "Прошу адрес у сети (DHCP)...\n");
        desktop_refresh();
        if (net_dhcp() != 0) {
            term_set_fg(t, TC_RED);
            term_printf(t, "Адрес не получен. Проверь, что сеть подключена (в VirtualBox: NAT).\n");
            term_set_fg(t, TC_DEFAULT);
            return;
        }
    }
    const struct netinfo *ni = net_info();
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Лоза (сеть)\n");
    term_set_fg(t, TC_DEFAULT);
    if (!ni->present) {
        term_printf(t, "  Сетевая карта не найдена (поддерживаются Intel e1000 и Realtek RTL8139).\n");
        return;
    }
    term_printf(t, "  Карта:    %s, канал %s\n", ni->nic, ni->link ? "есть" : "нет");
    term_printf(t, "  MAC:      %02x:%02x:%02x:%02x:%02x:%02x\n", ni->mac[0], ni->mac[1], ni->mac[2], ni->mac[3], ni->mac[4], ni->mac[5]);
    print_ip(t, "Адрес:", ni->ip);
    print_ip(t, "Маска:", ni->mask);
    print_ip(t, "Шлюз:", ni->gw);
    print_ip(t, "DNS:", ni->dns);
    if (!ni->configured) term_printf(t, "  Получить адрес: vine dhcp\n");
}

static void cmd_sonar(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: sonar 8.8.8.8  или  sonar example.com\n"); return; }
    if (!net_info()->configured) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Нет сети. Набери: vine dhcp\n");
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    uint32_t ip;
    if (net_resolve(argv[1], &ip) != 0) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Не удалось найти адрес «%s»\n", argv[1]);
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    char b[16];
    net_ip_str(ip, b);
    term_printf(t, "Посылаю сигнал на %s (%s)\n", argv[1], b);
    int ok = 0, total = 4, sum = 0;
    for (int i = 0; i < total; i++) {
        desktop_refresh();
        int rtt = 0;
        if (net_ping(ip, 2000, &rtt) == 0) {
            ok++; sum += rtt;
            term_set_fg(t, TC_GREEN);
            if (rtt < 10) term_printf(t, "  ответ от %s: быстрее 10 мс\n", b);
            else term_printf(t, "  ответ от %s: %d мс\n", b, rtt);
        } else {
            term_set_fg(t, TC_RED);
            term_printf(t, "  нет ответа\n");
        }
        term_set_fg(t, TC_DEFAULT);
    }
    term_printf(t, "Получено %d из %d", ok, total);
    if (ok) term_printf(t, ", среднее %d мс", sum / ok);
    term_putc(t, '\n');
}

static void cmd_lookup(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: lookup example.com\n"); return; }
    if (!net_info()->configured) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Нет сети. Набери: vine dhcp\n");
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    uint32_t ip;
    int r = net_resolve(argv[1], &ip);
    if (r != 0) {
        term_set_fg(t, TC_RED);
        term_printf(t, r == -3 ? "Такого имени нет: %s\n" : "DNS не ответил для %s\n", argv[1]);
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    char b[16];
    net_ip_str(ip, b);
    term_printf(t, "%s = ", argv[1]);
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "%s\n", b);
    term_set_fg(t, TC_DEFAULT);
}

static void need_net_msg(struct term *t) {
    term_set_fg(t, TC_RED);
    term_printf(t, "Нет сети. Набери: vine dhcp\n");
    term_set_fg(t, TC_DEFAULT);
}

static void cmd_citrus(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: citrus example.com  или  citrus https://example.com/страница\n"); return; }
    if (!net_info()->configured) { need_net_msg(t); return; }
    term_printf(t, "Открываю %s ...\n", argv[1]);
    if (desktop_open_browser(argv[1]) < 0) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Слишком много окон браузера (максимум %d).\n", DESKTOP_MAX_BROWSERS);
        term_set_fg(t, TC_DEFAULT);
    }
}

/* ---------- загрузка и отправка файлов ---------- */

struct dl_ctx { struct term *t; size_t next_report; };

static void dl_progress(size_t got, size_t total, void *p) {
    struct dl_ctx *c = p;
    if (got < c->next_report) return;
    c->next_report = got + 256 * 1024;
    if (total) term_printf(c->t, "  %u из %u КиБ\n", (unsigned)(got / 1024), (unsigned)(total / 1024));
    else       term_printf(c->t, "  %u КиБ\n", (unsigned)(got / 1024));
    desktop_refresh();
}

static void print_hex(struct term *t, const uint8_t *d, size_t n) { for (size_t i = 0; i < n; i++) term_printf(t, "%02x", d[i]); }

static void show_tls_summary(struct term *t, const struct http_resp *r) {
    if (!r->https) { term_set_fg(t, TC_ORANGE); term_printf(t, "  соединение: http, без шифрования\n"); term_set_fg(t, TC_DEFAULT); return; }
    term_set_fg(t, TC_GREEN);
    term_printf(t, "  соединение: https, %s\n", r->cipher);
    term_set_fg(t, TC_ORANGE);
    term_printf(t, "  подлинность сервера НЕ проверена (цепочка сертификатов — следующий этап)\n");
    term_set_fg(t, TC_GRAY);
    if (r->have_cert) { term_printf(t, "  отпечаток сертификата SHA-256:\n  "); print_hex(t, r->cert_sha256, 32); term_putc(t, '\n'); }
    term_set_fg(t, TC_DEFAULT);
}

static const char *base_name(const char *url) {
    const char *p = url, *last = url;
    if (strncmp(p, "https://", 8) == 0) p += 8; else if (strncmp(p, "http://", 7) == 0) p += 7;
    while (*p && *p != '?' && *p != '#') { if (*p == '/') last = p + 1; p++; }
    return last;
}

/* 1, если после хоста в адресе есть путь (а не просто http://host:порт или http://host/) */
static int url_has_path(const char *url) {
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8; else if (strncmp(p, "http://", 7) == 0) p += 7;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    if (*p == '?' || *p == '#') return 0;
    return *p == '/' && p[1] && p[1] != '?' && p[1] != '#';
}

static int name_has_ext(const char *n, const char *ext) {
    size_t a = strlen(n), b = strlen(ext);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) {
        char c = n[a - b + i]; if (c >= 'A' && c <= 'Z') c += 32;
        if (c != ext[i]) return 0;
    }
    return 1;
}

static void cmd_grab(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: grab https://example.com/file.zip [имя]\n        или: grab http://192.168.0.5:8000 key.txt  (имя добавится к адресу)\n"); return; }
    if (!net_info()->configured) { need_net_msg(t); return; }
    char name[48];
    char url[640];
    ksnprintf(url, sizeof url, "%s", argv[1]);
    if (strlen(argv[1]) >= sizeof url) { term_set_fg(t, TC_RED); term_printf(t, "Адрес слишком длинный.\n"); term_set_fg(t, TC_DEFAULT); return; }
    if (argc >= 3) {
        ksnprintf(name, sizeof name, "%.47s", argv[2]);
        if (!url_has_path(url)) {
            /* grab http://host:8000 key.txt — в адресе нет файла, значит имя и есть файл на сервере */
            size_t ul = strlen(url);
            if (ul && url[ul - 1] == '/') url[--ul] = 0;
            if (ul + 1 + strlen(argv[2]) < sizeof url) ksnprintf(url + ul, sizeof url - ul, "/%s", argv[2]);
        }
    } else {
        const char *bn = base_name(url);
        char tmp[48]; int n = 0;
        while (*bn && *bn != '?' && *bn != '#' && n < 47) tmp[n++] = *bn++;
        tmp[n] = 0;
        ksnprintf(name, sizeof name, "%s", n ? tmp : "index.html");
    }
    if (!vfs_name_ok(name)) {
        term_set_fg(t, TC_RED); term_printf(t, "Недопустимое имя файла (нельзя / \\ и управляющие символы).\n"); term_set_fg(t, TC_DEFAULT);
        return;
    }
    term_printf(t, "Скачиваю %s ...\n", url);
    desktop_refresh();
    struct http_opts o; memset(&o, 0, sizeof o);
    o.max_bytes = 24u << 20;
    struct dl_ctx ctx = { t, 256 * 1024 };
    o.progress = dl_progress; o.pctx = &ctx;
    struct http_resp r;
    uint64_t t0 = timer_ticks;
    int rc = http_fetch(url, &o, &r);
    uint64_t ms = (timer_ticks - t0) * 1000 / TIMER_HZ; if (!ms) ms = 1;
    if (rc != HTTP_OK) {
        term_set_fg(t, TC_RED);
        if (rc == HTTP_E_TLS) term_printf(t, "Не вышло: TLS — %s\n", tls_strerror(r.tls_err));
        else term_printf(t, "Не вышло: %s\n", http_strerror(rc));
        term_set_fg(t, TC_DEFAULT);
        http_free(&r);
        return;
    }
    show_tls_summary(t, &r);
    if (r.status < 200 || r.status > 299) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Сервер ответил кодом %d — файл не сохранён.\n", r.status);
        term_set_fg(t, TC_DEFAULT);
        http_free(&r);
        return;
    }
    size_t len = r.len;
    int looks_html = r.content_type[0] && strstr_n(r.content_type, r.content_type + strlen(r.content_type), "html") != NULL
                     && !name_has_ext(name, ".html") && !name_has_ext(name, ".htm");
    uint8_t *data = r.data; r.data = NULL;
    uint8_t h[32]; sha256(data, len, h);
    if (vfs_put(s->cwd, name, data, len) == VFS_NONE) {
        term_set_fg(t, TC_RED); term_printf(t, "Не сохранить: в этой папке ещё нет места, или имя занято папкой. (ls, cd)\n"); term_set_fg(t, TC_DEFAULT);
        kfree(data);
        return;
    }
    term_set_fg(t, TC_GREEN);
    char cwdp[80]; vfs_path(s->cwd, cwdp, sizeof cwdp);
    term_printf(t, "Сохранено: %s%s%s, %u байт за %u мс (%u КиБ/с)\n", cwdp, cwdp[strlen(cwdp)-1]=='/'?"":"/", name, (unsigned)len, (unsigned)ms, (unsigned)(len * 1000 / ms / 1024));
    term_set_fg(t, TC_GRAY);
    term_printf(t, "  sha256: "); print_hex(t, h, 32); term_putc(t, '\n');
    if (looks_html) {
        term_set_fg(t, TC_ORANGE);
        term_printf(t, "  Внимание: сервер прислал HTML-страницу (вероятно, список каталога), а не файл «%s».\n  Проверь адрес: нужен полный путь, например  grab http://адрес:порт/%s\n", name, name);
    }
    term_set_fg(t, TC_DEFAULT);
}

/* находит файл по пути (из cwd) и показывает понятную ошибку; возвращает -1, если не файл */
static int resolve_file(struct shell *s, const char *path, int *out) {
    struct term *t = s->t;
    int i = vfs_resolve(s->cwd, path);
    if (i == VFS_NONE) { term_set_fg(t, TC_RED); term_printf(t, "Нет «%s» (ls — список; pwd — где я)\n", path); term_set_fg(t, TC_DEFAULT); return -1; }
    if (vfs_is_dir(i)) { term_set_fg(t, TC_RED); term_printf(t, "«%s» — папка, а не файл (cd, чтобы зайти)\n", path); term_set_fg(t, TC_DEFAULT); return -1; }
    *out = i; return 0;
}

static void cmd_sha(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: sha file.zip\n"); return; }
    int i; if (resolve_file(s, argv[1], &i)) return;
    uint8_t h[32]; sha256(vfs_data(i), vfs_len(i), h);
    print_hex(t, h, 32); term_printf(t, "  %s\n", vfs_name(i));
}

static void cmd_peek(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: peek file.txt  (текст)  или  peek file.bin hex\n"); return; }
    int fi; if (resolve_file(s, argv[1], &fi)) return;
    const uint8_t *data = vfs_data(fi); size_t len = vfs_len(fi);
    int hex = argc >= 3 && strcmp(argv[2], "hex") == 0;
    size_t n = len < 512 ? len : 512;
    if (hex) {
        for (size_t i = 0; i < n; i += 16) {
            term_set_fg(t, TC_GRAY); term_printf(t, "%06x ", (unsigned)i); term_set_fg(t, TC_DEFAULT);
            for (size_t j = i; j < i + 16 && j < n; j++) term_printf(t, "%02x ", data[j]);
            term_putc(t, '\n');
        }
    } else {
        for (size_t i = 0; i < n; i++) { uint8_t c = data[i]; term_putc(t, (c == '\n' || (c >= 32 && c < 127) || c >= 0x80) ? (char)c : '.'); }
        term_putc(t, '\n');
    }
    if (len > n) term_printf(t, "... (показано %u из %u байт)\n", (unsigned)n, (unsigned)len);
}

static void cmd_drop(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: drop file.zip  (папку можно, только если пуста)\n"); return; }
    int i = vfs_resolve(s->cwd, argv[1]);
    if (i == VFS_NONE) { term_set_fg(t, TC_RED); term_printf(t, "Нет «%s»\n", argv[1]); term_set_fg(t, TC_DEFAULT); return; }
    if (i == s->cwd) { term_set_fg(t, TC_RED); term_printf(t, "Это текущая папка — сначала выйди (cd ..)\n"); term_set_fg(t, TC_DEFAULT); return; }
    if (vfs_delete(i) == 0) term_printf(t, "Удалён %s\n", argv[1]);
    else { term_set_fg(t, TC_RED); term_printf(t, "Не вышло: папка не пуста, или это корень/системная папка.\n"); term_set_fg(t, TC_DEFAULT); }
}

static void cmd_mkdir(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: mkdir Проект\n"); return; }
    if (vfs_mkdir(s->cwd, argv[1]) == VFS_NONE) { term_set_fg(t, TC_RED); term_printf(t, "Не вышло: плохое имя или уже занято.\n"); term_set_fg(t, TC_DEFAULT); return; }
    term_printf(t, "Папка создана: %s\n", argv[1]);
}

static void cmd_pwd(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    char p[96]; vfs_path(s->cwd, p, sizeof p);
    term_printf(s->t, "%s\n", p);
}

static void cmd_cd(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    const char *dst = argc >= 2 ? argv[1] : "/";
    int i = vfs_resolve(s->cwd, dst);
    if (i == VFS_NONE || !vfs_is_dir(i)) { term_set_fg(t, TC_RED); term_printf(t, "Нет такой папки: %s\n", dst); term_set_fg(t, TC_DEFAULT); return; }
    s->cwd = i;
}

struct ls_row { struct term *t; int n; };
static void ls_cb(int idx, const char *name, int is_dir, size_t sz, void *ctx) {
    (void)idx;
    struct ls_row *r = ctx;
    if (is_dir) { term_set_fg(r->t, TC_ACCENT); term_printf(r->t, "  %s/\n", name); term_set_fg(r->t, TC_DEFAULT); }
    else term_printf(r->t, "  %-30s %8u байт\n", name, (unsigned)sz);
    r->n++;
}

static void cmd_ls(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    int dir = argc >= 2 ? vfs_resolve(s->cwd, argv[1]) : s->cwd;
    if (dir == VFS_NONE || !vfs_is_dir(dir)) { term_set_fg(t, TC_RED); term_printf(t, "Нет такой папки: %s\n", argc >= 2 ? argv[1] : "?"); term_set_fg(t, TC_DEFAULT); return; }
    struct ls_row r = { t, 0 };
    vfs_list(dir, ls_cb, &r);
    if (!r.n) term_printf(t, "  Пусто. Скачать файл: grab <адрес>, или mkdir <имя>\n");
    else term_printf(t, "Итого: %d, в памяти всего %u КиБ (до выключения)\n", r.n, (unsigned)(vfs_total_bytes() / 1024));
}

static void cmd_cp_mv(struct shell *s, int argc, char **argv, int move) {
    struct term *t = s->t;
    if (argc < 3) { term_printf(t, move ? "Пример: mv старое.txt новое.txt  (или mv файл Папка/)\n" : "Пример: cp файл.txt копия.txt\n"); return; }
    int src; if (resolve_file(s, argv[1], &src)) return;
    int ddir = s->cwd; char nm[48];
    int maybe_dir = vfs_resolve(s->cwd, argv[2]);
    if (maybe_dir != VFS_NONE && vfs_is_dir(maybe_dir)) { ddir = maybe_dir; ksnprintf(nm, sizeof nm, "%s", vfs_name(src)); }
    else {
        const char *slash = NULL; for (const char *p = argv[2]; *p; p++) if (*p == '/') slash = p;
        if (slash) { char dp[300]; size_t dl = (size_t)(slash - argv[2]); if (dl >= sizeof dp) dl = sizeof dp - 1; memcpy(dp, argv[2], dl); dp[dl] = 0;
            int d = vfs_resolve(s->cwd, dp);
            if (d == VFS_NONE || !vfs_is_dir(d)) { term_set_fg(t, TC_RED); term_printf(t, "Нет такой папки в пути «%s»\n", argv[2]); term_set_fg(t, TC_DEFAULT); return; }
            ddir = d; ksnprintf(nm, sizeof nm, "%.47s", slash + 1);
        } else ksnprintf(nm, sizeof nm, "%.47s", argv[2]);
    }
    int r = move ? (vfs_rename(src, ddir, nm) == 0 ? src : VFS_NONE) : vfs_copy(src, ddir, nm);
    if (r == VFS_NONE) { term_set_fg(t, TC_RED); term_printf(t, "Не вышло: имя уже занято или путь неверный.\n"); term_set_fg(t, TC_DEFAULT); return; }
    term_set_fg(t, TC_GREEN); term_printf(t, "%s: %s -> %s\n", move ? "Перемещено" : "Скопировано", argv[1], argv[2]); term_set_fg(t, TC_DEFAULT);
}
static void cmd_cp(struct shell *s, int argc, char **argv) { cmd_cp_mv(s, argc, argv, 0); }
static void cmd_mv(struct shell *s, int argc, char **argv) { cmd_cp_mv(s, argc, argv, 1); }

/* upload <файл> <адрес> — отправить файл из памяти на сервер (POST); upload <файл> <адрес> put — метод PUT */
static void cmd_upload(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 3) { term_printf(t, "Пример: upload file.zip https://server.example/upload  [put]\n"); return; }
    if (!net_info()->configured) { need_net_msg(t); return; }
    int fi; if (resolve_file(s, argv[1], &fi)) return;
    struct http_opts o; memset(&o, 0, sizeof o);
    o.method = (argc >= 4 && strcmp(argv[3], "put") == 0) ? "PUT" : "POST";
    o.body = vfs_data(fi); o.body_len = vfs_len(fi); o.content_type = "application/octet-stream";
    o.max_bytes = 1u << 20;
    term_printf(t, "Отправляю %s (%u байт) на %s ...\n", vfs_name(fi), (unsigned)vfs_len(fi), argv[2]);
    desktop_refresh();
    struct http_resp r;
    int rc = http_fetch(argv[2], &o, &r);
    if (rc != HTTP_OK) {
        term_set_fg(t, TC_RED);
        if (rc == HTTP_E_TLS) term_printf(t, "Не вышло: TLS — %s\n", tls_strerror(r.tls_err));
        else term_printf(t, "Не вышло: %s\n", http_strerror(rc));
        term_set_fg(t, TC_DEFAULT);
        http_free(&r); return;
    }
    show_tls_summary(t, &r);
    term_set_fg(t, r.status >= 200 && r.status < 300 ? TC_GREEN : TC_RED);
    term_printf(t, "Ответ сервера: %d\n", r.status);
    term_set_fg(t, TC_DEFAULT);
    if (r.len) { size_t n = r.len < 300 ? r.len : 300; for (size_t i = 0; i < n; i++) { char c = (char)r.data[i]; term_putc(t, (c == '\n' || (c >= 32 && (unsigned char)c < 127)) ? c : ' '); } term_putc(t, '\n'); }
    http_free(&r);
}

/* seal <хост> [порт] — проверить TLS-рукопожатие и показать шифр и отпечаток сертификата */
/* ---------- scan: свой сканер портов (вместо nmap, который на LimonOS не запустить) ---------- */

static const struct { uint16_t port; const char *name; } well_known[] = {
    { 21, "ftp" }, { 22, "ssh" }, { 23, "telnet" }, { 25, "smtp" }, { 53, "dns" },
    { 80, "http" }, { 110, "pop3" }, { 143, "imap" }, { 443, "https" }, { 445, "smb" },
    { 3306, "mysql" }, { 3389, "rdp" }, { 5432, "postgres" }, { 6379, "redis" }, { 8080, "http-alt" },
};
static const char *port_name(uint16_t p) {
    for (size_t i = 0; i < sizeof well_known / sizeof well_known[0]; i++) if (well_known[i].port == p) return well_known[i].name;
    return "";
}

#define SCAN_MAX_PORTS 1024

static void cmd_scan(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: scan 10.0.2.1            (частые порты)\n        scan 10.0.2.1 1-1024    (диапазон)\n        scan 10.0.2.1 22         (один порт)\n"); return; }
    if (!net_info()->configured) { need_net_msg(t); return; }
    uint32_t ip;
    if (net_parse_ip(argv[1], &ip) != 0 && net_resolve(argv[1], &ip) != 0) {
        term_set_fg(t, TC_RED); term_printf(t, "Не нашёл адрес «%s»\n", argv[1]); term_set_fg(t, TC_DEFAULT); return;
    }
    char ipstr[16]; net_ip_str(ip, ipstr);
    unsigned from, to;
    if (argc >= 3) {
        const char *p = argv[2]; from = 0;
        while (*p >= '0' && *p <= '9') from = from * 10 + (unsigned)(*p++ - '0');
        if (*p == '-') { p++; to = 0; while (*p >= '0' && *p <= '9') to = to * 10 + (unsigned)(*p++ - '0'); }
        else to = from;
        if (!from || from > 65535 || to > 65535 || to < from) { term_set_fg(t, TC_RED); term_printf(t, "Порты: число или диапазон 1-65535, например 1-1024\n"); term_set_fg(t, TC_DEFAULT); return; }
        if (to - from + 1 > SCAN_MAX_PORTS) { term_set_fg(t, TC_RED); term_printf(t, "Не больше %d портов за раз (долго без параллельности)\n", SCAN_MAX_PORTS); term_set_fg(t, TC_DEFAULT); return; }
    } else {
        from = to = 0;   /* особое значение: список частых портов */
    }
    term_printf(t, "Сканирую %s ", ipstr);
    if (from) term_printf(t, "(порты %u-%u)...\n", from, to); else term_printf(t, "(частые порты)...\n");
    term_set_fg(t, TC_GRAY); term_printf(t, "ПОРТ       СОСТОЯНИЕ   СЛУЖБА\n"); term_set_fg(t, TC_DEFAULT);
    int open_n = 0, closed_n = 0, filtered_n = 0;
    uint64_t t0 = timer_ticks;
    size_t count = from ? (to - from + 1) : (sizeof well_known / sizeof well_known[0]);
    for (size_t i = 0; i < count; i++) {
        uint16_t port = from ? (uint16_t)(from + i) : well_known[i].port;
        int id = tcp_connect(ip, port, 700);
        if (id >= 0) {
            term_set_fg(t, TC_GREEN); term_printf(t, "%-10u открыт      %s\n", (unsigned)port, port_name(port)); term_set_fg(t, TC_DEFAULT);
            tcp_close(id); open_n++;
        } else if (id == -3) {
            closed_n++;
            if (from == to) { term_set_fg(t, TC_GRAY); term_printf(t, "%-10u закрыт\n", (unsigned)port); term_set_fg(t, TC_DEFAULT); }
        } else {
            filtered_n++;
            if (from == to) { term_set_fg(t, TC_ORANGE); term_printf(t, "%-10u не отвечает (фильтр или выключено)\n", (unsigned)port); term_set_fg(t, TC_DEFAULT); }
        }
        if ((i & 7) == 0) desktop_refresh();     /* обновляем экран по ходу — сканирование не мгновенное */
    }
    uint32_t secs = (uint32_t)((timer_ticks - t0) / TIMER_HZ);
    term_set_fg(t, TC_GRAY);
    term_printf(t, "Готово за %u с. Открыто: %d, закрыто: %d, не ответили: %d (всего проверено %u)\n", secs, open_n, closed_n, filtered_n, (unsigned)count);
    term_set_fg(t, TC_DEFAULT);
}

/* ---------- google: поиск прямо из терминала ---------- */

static void urlencode(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        uint8_t c = (uint8_t)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = "0123456789ABCDEF"[c >> 4]; out[o++] = "0123456789ABCDEF"[c & 15]; }
    }
    out[o] = 0;
}

/* ---------- ftp: настоящий клиент FTP/FTPS (RFC 959/4217) — USER/PASS, AUTH TLS, EPSV/PASV, LIST, RETR ----------
 * Протокол и логика живут в kernel/ftp.c (его же гоняют хост-тесты, tests/run_ftp_test.sh, против настоящих серверов);
 * здесь — только переходники к нашему TCP/TLS-стеку и вывод в терминал. */

struct ftp_term_ctx {
    struct term *t; const char *label;
    int verbose, debug;     /* как в ftp.exe: verbose — показывать ответы сервера (по умолчанию да), debug — показывать команды «---> USER x» */
    int hold;               /* 1 — придержать «226 ...», пока не выведен список/файл: как в ftp.exe, 226 идёт ПОСЛЕ данных */
    char held[200];
};

static void *ftpk_dial(void *ud, uint32_t ip, uint16_t port, int timeout_ms, int *err) {
    (void)ud;
    int id = tcp_connect(ip, port, timeout_ms);
    if (id < 0) { *err = id; return NULL; }
    return (void *)(intptr_t)(id + 1);     /* +1: чтобы id 0 не путался с NULL */
}
static int ftpk_send(void *h, const void *d, size_t n) { return tcp_send((int)((intptr_t)h - 1), d, n); }
static int ftpk_recv(void *h, void *b, size_t max, int to) { return tcp_recv((int)((intptr_t)h - 1), b, max, to); }
static void ftpk_close(void *h) { tcp_close((int)((intptr_t)h - 1)); }

static void ftpk_say(void *ud, int kind, const char *text) {
    struct ftp_term_ctx *c = ud;
    switch (kind) {
    case FTP_SAY_CMD:
        if (c->debug) { term_set_fg(c->t, TC_GRAY); term_printf(c->t, "---> %s\n", text); }
        break;
    case FTP_SAY_REPLY: {
        int err = text[0] == '4' || text[0] == '5';
        if (!c->verbose && !err) break;                                   /* verbose off: молчим, кроме ошибок */
        if (c->hold && !err && strncmp(text, "226", 3) == 0 && !c->held[0]) { ksnprintf(c->held, sizeof c->held, "%s", text); break; }
        term_set_fg(c->t, err ? TC_RED : TC_DEFAULT);
        term_printf(c->t, "%s\n", text);
        break;
    }
    case FTP_SAY_WARN:  term_set_fg(c->t, TC_ORANGE); term_printf(c->t, "%s\n", text); break;
    case FTP_SAY_OK:    term_set_fg(c->t, TC_GREEN);  term_printf(c->t, "%s\n", text); break;
    default:            term_set_fg(c->t, TC_DEFAULT); term_printf(c->t, "%s\n", text); break;
    }
    term_set_fg(c->t, TC_DEFAULT);
    desktop_refresh();
}
static void ftpk_tick(void *ud, size_t bytes) { (void)ud; (void)bytes; desktop_refresh(); }

/* Запрашивает строку прямо в терминале, как это делает ftp.exe в Windows («User (host:(none)):»,
 * «Password:»): блокирует shell, пока не нажат Enter. mask: 0 — показывать как есть,
 * 2 — не показывать вообще (пароль, как в cmd.exe — ни символов, ни звёздочек).
 * Возвращает число байт в out, или -1 по Ctrl+C. */
static int prompt_line(struct term *t, const char *label, int mask, char *out, size_t cap) {
    term_set_fg(t, TC_ACCENT); term_printf(t, "%s", label); term_set_fg(t, TC_DEFAULT);
    desktop_refresh();
    size_t n = 0;
    for (;;) {
        struct key_event ev;
        if (!kbd_poll(&ev)) { desktop_refresh(); continue; }
        if (ev.mods & MOD_CTRL) {
            if (ev.cp == 'c') { term_printf(t, "^C\n"); out[0] = 0; return -1; }
            continue;
        }
        if (ev.cp == '\n') { term_putc(t, '\n'); out[n] = 0; return (int)n; }
        if (ev.cp == '\b') {
            if (n) {
                /* откатываемся на один символ UTF-8, а не на один байт */
                size_t p = n - 1;
                while (p && (out[p] & 0xC0) == 0x80) p--;
                n = p;
                if (mask == 0) term_write(t, "\b \b");
            }
            continue;
        }
        if (ev.cp >= 0x20 && ev.cp < 0x110000 && n + 4 < cap) {
            char enc[4]; int bl = utf8_encode(ev.cp, enc);
            memcpy(out + n, enc, (size_t)bl); n += (size_t)bl;
            if (mask == 1) term_putc(t, '*');
            else if (mask == 0) { enc[bl] = 0; term_write(t, enc); }
        }
    }
}

/* ===== ftp — клиент «как ftp.exe в Windows» =====
 * Ничего не делается само: `ftp` или `ftp хост` только открывают приглашение «ftp>» (и, если дан хост,
 * подключаются и спрашивают логин/пароль у ВАС). Дальше каждая команда выполняется только когда вы её
 * набрали и нажали Enter. Выйти можно ТОЛЬКО командой bye/quit — ни ошибка, ни обрыв, ни пустой
 * ввод, ни Ctrl+C из «ftp>» не выкидывают в терминал. */

struct ftp_cli {
    struct shell *s; struct term *t;
    struct ftp_term_ctx ctx;
    struct ftp_ops ops;
    struct ftp_session fs;
    char host[64];
    int sec, implicit;
};

/* Сообщение об ошибке: если сервер уже сказал сам (ответ напечатан) — не дублируем */
static void cli_err(struct ftp_cli *c, const struct ftp_result *res) {
    if (res->code > 0 && c->ctx.verbose) return;
    if (!res->msg[0]) return;
    term_set_fg(c->t, TC_RED); term_printf(c->t, "%s\n", res->msg); term_set_fg(c->t, TC_DEFAULT);
}

/* Сервер закрыл соединение (421 или обрыв)? Тогда честно говорим об этом и остаёмся в «ftp>», как ftp.exe. */
static void cli_check_dead(struct ftp_cli *c, const struct ftp_result *res) {
    if (!c->fs.open) return;
    if (res->code == 421 || res->code == -2) {
        ftp_close(&c->fs);
        term_set_fg(c->t, TC_ORANGE); term_printf(c->t, "Соединение закрыто удалённым узлом.\n"); term_set_fg(c->t, TC_DEFAULT);
    }
}

static void cli_flush_held(struct ftp_cli *c) {
    c->ctx.hold = 0;
    if (c->ctx.held[0]) { term_printf(c->t, "%s\n", c->ctx.held); c->ctx.held[0] = 0; }
}

static void cli_stats(struct ftp_cli *c, size_t bytes, uint64_t t0, const char *verb) {
    uint64_t dt = timer_ticks - t0; if (!dt) dt = 1;                            /* сотые доли секунды */
    unsigned kbs100 = (unsigned)((uint64_t)bytes * 10000 / (1024 * dt));
    term_printf(c->t, "ftp: %u байт %s за %u.%02u (сек) со скоростью %u.%02u (КБ/сек).\n",
                (unsigned)bytes, verb, (unsigned)(dt / 100), (unsigned)(dt % 100), kbs100 / 100, kbs100 % 100);
}

static void cli_print_data(struct ftp_cli *c, const struct ftp_result *res) {
    for (size_t i = 0; i < res->len; i++) if (res->data[i] != '\r') term_putc(c->t, (char)res->data[i]);
    if (res->len && res->data[res->len - 1] != '\n') term_putc(c->t, '\n');
}

/* Логин: спрашиваем имя и пароль у пользователя, как «Пользователь (host:(none)):» и «Пароль:» в ftp.exe.
 * Пароль не показывается вообще (как в cmd.exe). Ошибка входа НЕ разрывает соединение — можно повторить командой user. */
static void cli_login(struct ftp_cli *c, const char *user_arg) {
    struct term *t = c->t;
    char user[64], pass[64]; struct ftp_result res;
    if (user_arg && *user_arg) ksnprintf(user, sizeof user, "%.63s", user_arg);
    else {
        char label[112]; ksnprintf(label, sizeof label, "Пользователь (%s:(none)): ", c->host);
        if (prompt_line(t, label, 0, user, sizeof user) < 0) return;
    }
    if (!user[0]) { term_printf(t, "Имя не введено — вход пропущен. Команда «user имя» — войти.\n"); return; }
    int r = ftp_user(&c->fs, user, &res);
    if (r == FTP_NEED_PASS) {
        if (prompt_line(t, "Пароль: ", 2, pass, sizeof pass) < 0) { term_printf(t, "Вход отменён.\n"); return; }
        r = ftp_pass(&c->fs, pass, &res);
        memset(pass, 0, sizeof pass);
    }
    if (r != FTP_OK) { cli_err(c, &res); term_printf(t, "Вход не выполнен.\n"); cli_check_dead(c, &res); }
}

static int parse_port(const char *s) {
    int n = 0; if (!*s) return -1;
    for (; *s; s++) { if (*s < '0' || *s > '9') return -1; n = n * 10 + (*s - '0'); if (n > 65535) return -1; }
    return n ? n : -1;
}

static void cli_open(struct ftp_cli *c, const char *host, const char *port_arg) {
    struct term *t = c->t;
    if (c->fs.open) { term_printf(t, "Уже подключено к %s — сначала команда close.\n", c->host); return; }
    if (!net_info()->configured) { need_net_msg(t); return; }
    int port = c->implicit ? 990 : 21;
    if (port_arg) { port = parse_port(port_arg); if (port < 0) { term_printf(t, "Неверный порт: %s\n", port_arg); return; } }
    uint32_t ip;
    if (net_parse_ip(host, &ip) != 0 && net_resolve(host, &ip) != 0) {
        term_set_fg(t, TC_RED); term_printf(t, "Неизвестный хост %s.\n", host); term_set_fg(t, TC_DEFAULT); return;
    }
    ksnprintf(c->host, sizeof c->host, "%.63s", host);
    char ipstr[16]; net_ip_str(ip, ipstr);
    term_printf(t, "Подключаюсь к %s:%d (%s) ...\n", ipstr, port, c->implicit ? "FTPS, неявный" : "FTP");
    desktop_refresh();
    struct ftp_req req = { c->host, ip, (uint16_t)port, "", "", FTP_LIST, NULL, c->sec, c->implicit };
    struct ftp_result res;
    memset(&c->fs, 0, sizeof c->fs);
    int r = ftp_open(&c->ops, &req, &c->fs, &res);
    if (r != FTP_OK) { term_set_fg(t, TC_RED); term_printf(t, "%s\n", res.msg[0] ? res.msg : "Не удалось подключиться."); term_set_fg(t, TC_DEFAULT); return; }
    term_printf(t, "Подключено к %s.\n", c->host);
    cli_login(c, NULL);
}

/* Разбор строки: слова через пробел, "в кавычках" — одно слово (папки вроде "Program Files (x86)"). */
static int cli_split(char *line, char **av, int max) {
    int ac = 0; char *p = line;
    while (*p && ac < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') { av[ac++] = ++p; while (*p && *p != '"') p++; if (*p) *p++ = 0; }
        else { av[ac++] = p; while (*p && *p != ' ' && *p != '\t') p++; if (*p) *p++ = 0; }
    }
    return ac;
}

static void ftp_help(struct term *t) {
    term_printf(t, "Команды (можно вводить в любом регистре):\n\n"
        "  open хост [порт]   подключиться      close / disconnect  отключиться\n"
        "  user [имя]         войти заново      bye / quit          выйти из ftp\n"
        "  ls [путь]          только имена      dir [путь]          подробный список\n"
        "  cd путь            зайти в папку     cdup                на уровень вверх\n"
        "  pwd                где я на сервере  lcd [путь]          локальная папка (Memoria)\n"
        "  get файл [имя]     скачать           put файл [имя]      отправить\n"
        "  recv / send        то же, что get / put\n"
        "  delete файл        удалить файл      mkdir имя           создать папку\n"
        "  rmdir имя          удалить папку     rename старое новое переименовать\n"
        "  binary / ascii     тип передачи      type [a|i]          показать/сменить тип\n"
        "  verbose            ответы сервера вкл/выкл     debug     показывать команды вкл/выкл\n"
        "  quote КОМАНДА      сырая команда серверу       status    состояние\n"
        "  help / ?           этот список\n"
        "  Имена с пробелами — в кавычках: cd \"Program Files\"\n");
}

static void ftp_shell(struct shell *s, const char *host, const char *port_arg, int sec, int implicit) {
    struct term *t = s->t;
    static struct ftp_cli cli;                              /* большая — не на стеке */
    memset(&cli, 0, sizeof cli);
    cli.s = s; cli.t = t; cli.sec = sec; cli.implicit = implicit;
    cli.ctx.t = t; cli.ctx.verbose = 1;
    cli.ops = (struct ftp_ops){ &cli.ctx, ftpk_dial, ftpk_send, ftpk_recv, ftpk_close, ftpk_say, ftpk_tick };
    struct ftp_cli *c = &cli;
    struct ftp_result res;
    cli.ctx.label = cli.host;

    if (host) cli_open(c, host, port_arg);

    char line[512], raw[512];
    for (;;) {
        int n = prompt_line(t, "ftp> ", 0, line, sizeof line);
        if (n < 0) continue;                                 /* Ctrl+C: просто новая строка, выходит только bye */
        ksnprintf(raw, sizeof raw, "%s", line);
        char *av[8]; int ac = cli_split(line, av, 8);
        if (ac == 0) continue;
        for (char *q = av[0]; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
        const char *c0 = av[0];
        /* «остаток строки» для команд с одним путём: cd Program Files тоже работает без кавычек */
        const char *rest = raw; while (*rest == ' ' || *rest == '\t') rest++;
        while (*rest && *rest != ' ' && *rest != '\t') rest++;
        while (*rest == ' ' || *rest == '\t') rest++;
        char one[256]; ksnprintf(one, sizeof one, "%s", rest);
        for (int k = (int)strlen(one); k > 0 && (one[k - 1] == ' ' || one[k - 1] == '\t'); k--) one[k - 1] = 0;
        const char *a1 = ac >= 2 ? (one[0] == '"' ? av[1] : one) : NULL;

        if (!strcmp(c0, "bye") || !strcmp(c0, "quit") || !strcmp(c0, "exit")) { if (c->fs.open) ftp_close(&c->fs); break; }
        if (!strcmp(c0, "help") || !strcmp(c0, "?")) { ftp_help(t); continue; }
        if (!strcmp(c0, "open")) {
            if (ac < 2) { term_printf(t, "Пример: open хост [порт]\n"); continue; }
            cli_open(c, av[1], ac > 2 ? av[2] : NULL); continue;
        }
        if (!strcmp(c0, "close") || !strcmp(c0, "disconnect")) {
            if (!c->fs.open) { term_printf(t, "Не подключено.\n"); continue; }
            ftp_close(&c->fs); continue;
        }
        if (!strcmp(c0, "verbose")) { c->ctx.verbose = !c->ctx.verbose; term_printf(t, "Подробный вывод %s.\n", c->ctx.verbose ? "включён" : "выключен"); continue; }
        if (!strcmp(c0, "debug"))   { c->ctx.debug = !c->ctx.debug; term_printf(t, "Отладка %s.\n", c->ctx.debug ? "включена" : "выключена"); continue; }
        if (!strcmp(c0, "status")) {
            if (c->fs.open) term_printf(t, "Подключено к %s%s.\n", c->host, c->fs.control_tls ? " (шифрованный канал)" : ""); else term_printf(t, "Не подключено.\n");
            term_printf(t, "Тип: %s. Подробный вывод: %s. Отладка: %s.\n", c->fs.type == 'A' ? "текстовый" : "бинарный",
                        c->ctx.verbose ? "вкл" : "выкл", c->ctx.debug ? "вкл" : "выкл");
            continue;
        }
        if (!strcmp(c0, "binary") || !strcmp(c0, "ascii") || !strcmp(c0, "type")) {
            char want = 0;
            if (!strcmp(c0, "binary")) want = 'I'; else if (!strcmp(c0, "ascii")) want = 'A';
            else if (ac >= 2) { char x = av[1][0]; if (x == 'a' || x == 'A') want = 'A'; else if (x == 'i' || x == 'I' || x == 'b' || x == 'B') want = 'I'; else { term_printf(t, "Пример: type a  или  type i\n"); continue; } }
            if (want) c->fs.type = want;
            term_printf(t, "Тип передачи: %s.\n", (c->fs.type ? c->fs.type : 'I') == 'A' ? "текстовый (ascii)" : "бинарный (binary)");
            continue;
        }
        if (!strcmp(c0, "lcd")) {
            if (ac >= 2) {
                int i = vfs_resolve(s->cwd, a1);
                if (i == VFS_NONE || !vfs_is_dir(i)) { term_set_fg(t, TC_RED); term_printf(t, "Нет такой локальной папки: %s\n", a1); term_set_fg(t, TC_DEFAULT); continue; }
                s->cwd = i;
            }
            char pth[160]; vfs_path(s->cwd, pth, sizeof pth); term_printf(t, "Локальная папка сейчас %s\n", pth);
            continue;
        }
        if (!strcmp(c0, "user")) {
            if (!c->fs.open) { term_printf(t, "Не подключено.\n"); continue; }
            cli_login(c, ac >= 2 ? av[1] : NULL); continue;
        }

        /* дальше — команды, которым нужно соединение */
        int known = !strcmp(c0, "ls") || !strcmp(c0, "dir") || !strcmp(c0, "cd") || !strcmp(c0, "cwd") || !strcmp(c0, "cdup") ||
                    !strcmp(c0, "pwd") || !strcmp(c0, "get") || !strcmp(c0, "recv") || !strcmp(c0, "put") || !strcmp(c0, "send") ||
                    !strcmp(c0, "mkdir") || !strcmp(c0, "rmdir") || !strcmp(c0, "delete") || !strcmp(c0, "del") || !strcmp(c0, "rm") ||
                    !strcmp(c0, "rename") || !strcmp(c0, "quote");
        if (!known) { term_printf(t, "Неверная команда. help — список.\n"); continue; }
        if (!c->fs.open) { term_printf(t, "Не подключено.\n"); continue; }

        int r = FTP_OK;
        if (!strcmp(c0, "ls") || !strcmp(c0, "dir")) {
            uint64_t t0 = timer_ticks;
            c->ctx.hold = 1; c->ctx.held[0] = 0;
            r = !strcmp(c0, "ls") ? ftp_nlst(&c->fs, a1, &res) : ftp_list(&c->fs, a1, &res);
            if (r == FTP_OK) {
                cli_print_data(c, &res); cli_flush_held(c);
                if (res.truncated) { term_set_fg(t, TC_ORANGE); term_printf(t, "%s\n", res.msg); term_set_fg(t, TC_DEFAULT); }
                cli_stats(c, res.len, t0, "получено"); kfree(res.data);
            } else { cli_flush_held(c); cli_err(c, &res); cli_check_dead(c, &res); }
        } else if (!strcmp(c0, "cd") || !strcmp(c0, "cwd")) {
            if (ac < 2) { term_printf(t, "Пример: cd папка\n"); continue; }
            r = ftp_cwd(&c->fs, a1, &res);
            if (r != FTP_OK) { cli_err(c, &res); cli_check_dead(c, &res); }
        } else if (!strcmp(c0, "cdup")) {
            r = ftp_cdup(&c->fs, &res);
            if (r != FTP_OK) { cli_err(c, &res); cli_check_dead(c, &res); }
        } else if (!strcmp(c0, "pwd")) {
            char cur[256];
            r = ftp_pwd(&c->fs, cur, sizeof cur, &res);
            if (r != FTP_OK) { cli_err(c, &res); cli_check_dead(c, &res); }
            else if (!c->ctx.verbose) term_printf(t, "%s\n", cur);
        } else if (!strcmp(c0, "get") || !strcmp(c0, "recv")) {
            if (ac < 2) { term_printf(t, "Пример: get файл [локальное_имя]   (имена с пробелами — в кавычках)\n"); continue; }
            const char *remote = ac >= 3 ? av[1] : a1;
            uint64_t t0 = timer_ticks;
            c->ctx.hold = 1; c->ctx.held[0] = 0;
            r = ftp_get(&c->fs, remote, &res);
            if (r == FTP_OK) {
                char name[48]; const char *bn = ac >= 3 ? av[2] : remote;
                if (ac < 3) { const char *q = bn; for (; *q; q++) if (*q == '/' || *q == '\\') bn = q + 1; }
                ksnprintf(name, sizeof name, "%.47s", *bn ? bn : "ftp.bin");
                cli_flush_held(c);
                if (!vfs_name_ok(name) || vfs_put(s->cwd, name, res.data, res.len) == VFS_NONE) {
                    term_set_fg(t, TC_RED); term_printf(t, "Не сохранилось локально: имя «%s» недопустимо или занято.\n", name); term_set_fg(t, TC_DEFAULT); kfree(res.data);
                } else {
                    if (res.truncated) { term_set_fg(t, TC_ORANGE); term_printf(t, "%s\n", res.msg); term_set_fg(t, TC_DEFAULT); }
                    cli_stats(c, res.len, t0, "получено");
                    term_printf(t, "Сохранено локально: %s\n", name);
                }
            } else { cli_flush_held(c); cli_err(c, &res); cli_check_dead(c, &res); }
        } else if (!strcmp(c0, "put") || !strcmp(c0, "send")) {
            if (ac < 2) { term_printf(t, "Пример: put локальный_файл [имя_на_сервере]\n"); continue; }
            const char *local = ac >= 3 ? av[1] : a1;
            int idx = vfs_resolve(s->cwd, local);
            if (idx == VFS_NONE || vfs_is_dir(idx)) { term_set_fg(t, TC_RED); term_printf(t, "%s: нет такого локального файла (команда files — список).\n", local); term_set_fg(t, TC_DEFAULT); continue; }
            const char *remote = ac >= 3 ? av[2] : local;
            if (ac < 3) { const char *q = remote; for (; *q; q++) if (*q == '/' || *q == '\\') remote = q + 1; }
            uint64_t t0 = timer_ticks;
            c->ctx.hold = 1; c->ctx.held[0] = 0;
            r = ftp_put(&c->fs, remote, vfs_data(idx), vfs_len(idx), &res);
            cli_flush_held(c);
            if (r == FTP_OK) cli_stats(c, vfs_len(idx), t0, "отправлено");
            else { cli_err(c, &res); cli_check_dead(c, &res); }
        } else if (!strcmp(c0, "mkdir")) {
            if (ac < 2) { term_printf(t, "Пример: mkdir имя\n"); continue; }
            r = ftp_mkdir(&c->fs, a1, &res);
            if (r != FTP_OK) { cli_err(c, &res); cli_check_dead(c, &res); }
        } else if (!strcmp(c0, "rmdir")) {
            if (ac < 2) { term_printf(t, "Пример: rmdir имя\n"); continue; }
            r = ftp_rmdir(&c->fs, a1, &res);
            if (r != FTP_OK) { cli_err(c, &res); cli_check_dead(c, &res); }
        } else if (!strcmp(c0, "delete") || !strcmp(c0, "del") || !strcmp(c0, "rm")) {
            if (ac < 2) { term_printf(t, "Пример: delete файл\n"); continue; }
            r = ftp_rm(&c->fs, a1, &res);
            if (r != FTP_OK) { cli_err(c, &res); cli_check_dead(c, &res); }
        } else if (!strcmp(c0, "rename")) {
            if (ac < 3) { term_printf(t, "Пример: rename старое новое   (имена с пробелами — в кавычках)\n"); continue; }
            r = ftp_rename(&c->fs, av[1], av[2], &res);
            if (r != FTP_OK) { cli_err(c, &res); cli_check_dead(c, &res); }
        } else if (!strcmp(c0, "quote")) {
            if (ac < 2) { term_printf(t, "Пример: quote SYST\n"); continue; }
            r = ftp_quote(&c->fs, one, &res);
            if (r != FTP_OK) { cli_err(c, &res); cli_check_dead(c, &res); }
            else if (!c->ctx.verbose) term_printf(t, "(код %d)\n", res.code);
        }
        (void)r;
    }
}

static void cmd_ftp(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    char *rest[12]; int rc = 0;
    int sec = FTP_SEC_AUTO, implicit = 0;
    for (int i = 1; i < argc && rc < 12; i++) {
        if (strcmp(argv[i], "--tls") == 0) sec = FTP_SEC_TLS;
        else if (strcmp(argv[i], "--plain") == 0) sec = FTP_SEC_PLAIN;
        else if (strcmp(argv[i], "--implicit") == 0) implicit = 1;
        else rest[rc++] = argv[i];
    }
    /* Старые «ftp list хост» / «ftp get хост файл» выполняли команду сами и выкидывали обратно в терминал.
       Теперь они просто заходят в ftp> — ls и get вы вводите сами. */
    if (rc >= 2 && (strcmp(rest[0], "list") == 0 || strcmp(rest[0], "get") == 0)) {
        term_set_fg(t, TC_ORANGE);
        term_printf(t, "«ftp %s» больше не выполняет команду сам. Захожу в ftp> — вводите ls / get сами.\n", rest[0]);
        term_set_fg(t, TC_DEFAULT);
        rest[0] = rest[1]; rc--;
    }
    term_printf(t, "ftp: клиент как ftp.exe. help — команды, bye — выйти.\n");
    ftp_shell(s, rc >= 1 ? rest[0] : NULL, rc >= 2 ? rest[1] : NULL, sec, implicit);
}

/* DuckDuckGo отдаёт простую HTML-страницу без скриптов специально для таких случаев —
   результаты лежат в <a ... class="result__a" href="...">. Ссылка обычно завёрнута в их
   редиректор //duckduckgo.com/l/?uddg=НАСТОЯЩИЙ_АДРЕС&... — его нужно раскодировать (%XX). */
static void cmd_google(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: google LimonOS википедия\n"); return; }
    if (!net_info()->configured) { need_net_msg(t); return; }
    char q[256] = {0};
    for (int i = 1; i < argc; i++) { if (i > 1) strncat_s(q, " ", sizeof q); strncat_s(q, argv[i], sizeof q); }
    char enc[512]; urlencode(q, enc, sizeof enc);
    char url[600]; ksnprintf(url, sizeof url, "https://html.duckduckgo.com/html/?q=%s", enc);
    term_printf(t, "Ищу: %s ...\n", q);
    desktop_refresh();
    struct http_opts o; memset(&o, 0, sizeof o);
    o.max_bytes = 1u << 20;
    struct http_resp r;
    int rc = http_fetch(url, &o, &r);
    if (rc != HTTP_OK) {
        term_set_fg(t, TC_RED);
        if (rc == HTTP_E_TLS) term_printf(t, "Не вышло: TLS — %s\n", tls_strerror(r.tls_err));
        else term_printf(t, "Не вышло: %s\n", http_strerror(rc));
        term_set_fg(t, TC_DEFAULT);
        http_free(&r); return;
    }
    show_tls_summary(t, &r);
    const char *p = (const char *)r.data, *end = p + r.len;
    int n = 0;
    while (p < end && n < 10) {
        const char *m = strstr_n(p, end, "class=\"result__a\" href=\"");
        if (!m) m = strstr_n(p, end, "class=\"result__url\" href=\"");
        if (!m) break;
        m = strstr_n(m, end, "href=\"") + 6;
        const char *hend = m; while (hend < end && *hend != '"') hend++;
        char raw[400]; size_t rl = (size_t)(hend - m); if (rl > sizeof raw - 1) rl = sizeof raw - 1;
        memcpy(raw, m, rl); raw[rl] = 0;
        char link[400];
        const char *tag = strstr_n(raw, raw + strlen(raw), "uddg=");
        if (tag) { tag += 5; const char *tend = tag; while (*tend && *tend != '&') tend++; urldecode_n(tag, tend, link, sizeof link); }
        else { if (strncmp(raw, "//", 2) == 0) ksnprintf(link, sizeof link, "https:%s", raw); else ksnprintf(link, sizeof link, "%s", raw); }
        /* титул ссылки — текст сразу после закрывающей > этого же <a> */
        const char *title = hend; while (title < end && *title != '>') title++; title++;
        const char *tclose = title; while (tclose < end && *tclose != '<') tclose++;
        if (strncmp(link, "http", 4) == 0 && !strstr_n(link, link + strlen(link), "duckduckgo.com")) {
            term_set_fg(t, TC_GREEN); term_printf(t, "%d. ", ++n); term_set_fg(t, TC_DEFAULT);
            size_t tl = (size_t)(tclose - title); if (tl > 70) tl = 70;
            if (tl) term_printf(t, "%.*s\n   ", (int)tl, title);
            size_t sh = strlen(link) > 96 ? 96 : strlen(link);
            term_set_fg(t, TC_GRAY); term_printf(t, "%.*s\n", (int)sh, link); term_set_fg(t, TC_DEFAULT);
        }
        p = hend;
    }
    if (!n) { term_set_fg(t, TC_ORANGE); term_printf(t, "Не нашёл ссылок (поисковик мог поменять разметку или показать капчу). Попробуй: citrus https://html.duckduckgo.com/html/?q=%s\n", enc); term_set_fg(t, TC_DEFAULT); }
    else term_printf(t, "Открыть: citrus <ссылка>\n");
    http_free(&r);
}

/* ---------- ai: разговор с Claude (Anthropic API) прямо из терминала ---------- */

static void ai_line_cb(const char *text, void *ctx) {
    struct term *t = ctx;
    term_printf(t, "%s\n", text);
    desktop_refresh();
}

static void cmd_aikey(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) {
        if (ai_proxy()) term_printf(t, "Работает через прокси %s (формат %s), модель: %s. Убрать: aikey proxy off\n", ai_proxy(), ai_provider_name(), ai_model());
        else if (ai_has_key()) term_printf(t, "Ключ задан: %s, модель: %s. Чтобы убрать: aikey off\n", ai_provider_name(), ai_model());
        else term_printf(t,
            "Ключ не задан. Понимаю два вида:\n"
            "  aikey sk-ant-...   — ключ Anthropic, получить: https://console.anthropic.com/settings/keys\n"
            "  aikey sk-or-v1-... — ключ OpenRouter,  получить: https://openrouter.ai/keys\n"
            "Поставщик определяется сам по виду ключа. Ключ хранится только в памяти и пропадёт при выключении — в коде LimonOS его нет.\n"
            "Сменить модель: aikey model имя-модели\n"
            "\n"
            "Не хочешь вручную набирать длинный ключ — принеси его из Windows по сети:\n"
            "  1. На Windows (в той же WSL, где собирал систему) положи ключ в файл и подними сервер:\n"
            "     echo твой-ключ > key.txt && python3 -m http.server 8000\n"
            "  2. В LimonOS:  grab http://10.0.2.2:8000/key.txt\n"
            "  3. Потом:      aikey file key.txt\n"
            "  (10.0.2.2 — это адрес хост-компьютера изнутри сети VirtualBox NAT; если не подошёл, посмотри свой через vine)\n");
        return;
    }
    if (strcmp(argv[1], "off") == 0) { ai_set_key(NULL); term_printf(t, "Ключ убран из памяти.\n"); return; }
    if (strcmp(argv[1], "proxy") == 0) {
        if (argc < 3) {
            if (ai_proxy()) term_printf(t, "Прокси: %s (выключить: aikey proxy off)\n", ai_proxy());
            else term_printf(t, "Прокси не задан. Старому ПК не нужно шифрование, а ключ не попадает в LimonOS:\n"
                                "  1. На основном ПК: LIMON_AI_KEY=sk-... python3 tools/ai_proxy.py\n"
                                "  2. В LimonOS:      aikey proxy http://192.168.0.5:8787 [anthropic|openrouter]\n");
            return;
        }
        if (strcmp(argv[2], "off") == 0) { ai_set_proxy("", AI_PROVIDER_NONE); term_printf(t, "Прокси выключен.\n"); return; }
        enum ai_provider pv = (argc >= 4 && strcmp(argv[3], "openrouter") == 0) ? AI_PROVIDER_OPENROUTER : AI_PROVIDER_ANTHROPIC;
        if (ai_set_proxy(argv[2], pv) != 0) { term_set_fg(t, TC_RED); term_printf(t, "Нужен адрес вида http://192.168.0.5:8787\n"); term_set_fg(t, TC_DEFAULT); return; }
        term_set_fg(t, TC_GREEN); term_printf(t, "Прокси принят: %s, формат: %s. Теперь: ai твой вопрос\n", ai_proxy(), pv == AI_PROVIDER_OPENROUTER ? "OpenRouter" : "Anthropic"); term_set_fg(t, TC_DEFAULT);
        return;
    }
    if (strcmp(argv[1], "model") == 0 && argc >= 3) { ai_set_model(argv[2]); term_printf(t, "Модель: %s\n", ai_model()); return; }
    if (strcmp(argv[1], "file") == 0) {
        if (argc < 3) { term_printf(t, "Пример: aikey file ключ.txt  (сначала скачай файл: grab http://10.0.2.2:ПОРТ/ключ.txt)\n"); return; }
        int fi = vfs_resolve(s->cwd, argv[2]);
        if (fi == VFS_NONE || vfs_is_dir(fi)) {
            term_set_fg(t, TC_RED); term_printf(t, "Нет файла «%s» (ls — список; сначала grab, или cd в папку с ключом)\n", argv[2]);
            term_set_fg(t, TC_DEFAULT);
            struct ls_row rr = { t, 0 };
            term_printf(t, "В текущей папке: "); vfs_list(s->cwd, ls_cb, &rr); if (!rr.n) term_printf(t, "(пусто)\n");
            return;
        }
        const uint8_t *fdata = vfs_data(fi);
        if (vfs_len(fi) && fdata[0] == '<') {
            term_set_fg(t, TC_RED); term_printf(t, "«%s» — это HTML-страница, а не ключ. Скорее всего, grab скачал список каталога: укажи адрес с именем файла.\n", argv[2]); term_set_fg(t, TC_DEFAULT);
            return;
        }
        size_t n = vfs_len(fi);
        while (n > 0 && (fdata[n-1] == '\n' || fdata[n-1] == '\r' || fdata[n-1] == ' ' || fdata[n-1] == '\t')) n--;   /* обрезаем перевод строки и пробелы по краям файла */
        size_t start = 0;
        while (start < n && (fdata[start] == ' ' || fdata[start] == '\t')) start++;
        n -= start;
        if (!n || n >= 160) { term_set_fg(t, TC_RED); term_printf(t, "В файле «%s» после обрезки пробелов %u байт — не похоже на ключ.\n", argv[2], (unsigned)n); term_set_fg(t, TC_DEFAULT); return; }
        char key[160]; memcpy(key, fdata + start, n); key[n] = 0;
        ai_set_key(key);
        secure_zero(key, sizeof key);                       /* копия на стеке больше не нужна */
        if (ai_get_provider() == AI_PROVIDER_NONE) {
            term_set_fg(t, TC_RED); term_printf(t, "Прочитал %u байт из файла, но не узнал поставщика по виду ключа (жду sk-ant-... или sk-or-v1-...).\n", (unsigned)n); term_set_fg(t, TC_DEFAULT);
            return;
        }
        term_set_fg(t, TC_GREEN);
        term_printf(t, "Ключ %s принят из файла «%s» (%u символов), модель: %s.\n", ai_provider_name(), argv[2], (unsigned)n, ai_model());
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    ai_set_key(argv[1]);
    if (ai_get_provider() == AI_PROVIDER_NONE) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Не узнал поставщика по виду ключа (жду sk-ant-... или sk-or-v1-...). Ключ не сохранён.\n");
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    term_set_fg(t, TC_GREEN);
    term_printf(t, "Ключ %s принят, модель по умолчанию: %s (хранится только в памяти этого сеанса).\n", ai_provider_name(), ai_model());
    term_set_fg(t, TC_DEFAULT);
}

static void cmd_ai(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) {
        term_printf(t,
            "Пример: ai как устроен TCP?\nКлюч: aikey sk-ant-... (однократно на сеанс)\n"
            "Если сервер отвечает ошибкой и непонятно почему: ai dump твой вопрос\n"
            "  — покажет РОВНО то, что уйдёт на сервер (адрес, заголовки, ключ как он есть, тело запроса),\n"
            "    ничего не отправляя. Сверь ключ в этом выводе посимвольно с панелью поставщика.\n");
        return;
    }
    int dump = strcmp(argv[1], "dump") == 0;
    int qstart = dump ? 2 : 1;
    if (dump && argc < 3) { term_printf(t, "Пример: ai dump твой вопрос\n"); return; }
    char q[1800] = {0};
    for (int i = qstart; i < argc; i++) { if (i > qstart) strncat_s(q, " ", sizeof q); strncat_s(q, argv[i], sizeof q); }
    if (dump) { ai_dump_request(q, ai_line_cb, t); return; }
    if (!net_info()->configured) { need_net_msg(t); return; }
    if (!ai_has_key()) { term_set_fg(t, TC_RED); term_printf(t, "Сначала: aikey sk-ant-твой-ключ или sk-or-v1-твой-ключ\n"); term_set_fg(t, TC_DEFAULT); return; }
    term_set_fg(t, TC_GRAY); term_printf(t, "(%s думает...)\n", ai_model()); term_set_fg(t, TC_DEFAULT);
    desktop_refresh();
    ai_ask(q, ai_line_cb, t);
}

static int seal_send(void *ctx, const void *d, size_t n) { return tcp_send(*(int *)ctx, d, n); }
static int seal_recv(void *ctx, void *b, size_t m, int to) { return tcp_recv(*(int *)ctx, b, m, to); }

static void cmd_seal(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: seal github.com\n"); return; }
    if (!net_info()->configured) { need_net_msg(t); return; }
    uint32_t ip;
    if (net_resolve(argv[1], &ip) != 0) { term_set_fg(t, TC_RED); term_printf(t, "Не нашёл адрес «%s»\n", argv[1]); term_set_fg(t, TC_DEFAULT); return; }
    int port = 443;
    if (argc >= 3) { port = 0; for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) port = port * 10 + (*p - '0'); }
    char b[16]; net_ip_str(ip, b);
    term_printf(t, "Соединяюсь с %s:%d (%s) ...\n", argv[1], port, b);
    desktop_refresh();
    uint64_t t0 = timer_ticks;
    int id = tcp_connect(ip, (uint16_t)port, 6000);
    if (id < 0) { term_set_fg(t, TC_RED); term_printf(t, "TCP не соединился\n"); term_set_fg(t, TC_DEFAULT); return; }
    uint64_t t1 = timer_ticks;
    struct tls_io io = { seal_send, seal_recv, &id };
    int err = 0;
    struct tls *tl = tls_open(&io, argv[1], &err);
    uint64_t t2 = timer_ticks;
    if (!tl) {
        term_set_fg(t, TC_RED); term_printf(t, "TLS не вышел: %s (код %d)\n", tls_strerror(err), err); term_set_fg(t, TC_DEFAULT);
        tcp_close(id); return;
    }
    const struct tls_info *ti = tls_info(tl);
    term_set_fg(t, TC_GREEN); term_printf(t, "TLS 1.3 установлен, шифр: %s\n", ti->cipher); term_set_fg(t, TC_DEFAULT);
    term_printf(t, "  TCP: %u мс, рукопожатие: %u мс\n", (unsigned)((t1 - t0) * 1000 / TIMER_HZ), (unsigned)((t2 - t1) * 1000 / TIMER_HZ));
    if (ti->have_cert) { term_set_fg(t, TC_GRAY); term_printf(t, "  сертификат SHA-256:\n  "); print_hex(t, ti->cert_sha256, 32); term_putc(t, '\n'); term_set_fg(t, TC_DEFAULT); }
    term_set_fg(t, TC_ORANGE); term_printf(t, "  подлинность сервера не проверена — сверь отпечаток с openssl на другом ПК\n"); term_set_fg(t, TC_DEFAULT);
    tls_close(tl);
    tcp_close(id);
}

/* ---------- lima: установка программ ---------- */

#define LIMA_PLANNED 0
#define LIMA_NET     1

struct lima_pkg {
    const char *name, *alt, *owner, *repo, *note;
    int kind;
    int installed;
    char *readme;      /* kmalloc, если установлен */
};

static struct lima_pkg lima_catalog[] = {
    { "wifiscan",     0,            "AnonimusShamshiAlex", "wifiscan",      "сканер Wi-Fi сетей",              LIMA_NET, 0, 0 },
    { "brutepass",    0,            "AnonimusShamshiAlex", "brutepass",     "подбор паролей",                  LIMA_NET, 0, 0 },
    { "iotlamp",      0,            "AnonimusShamshiAlex", "iotlamp",       "работа с IoT-устройствами",       LIMA_NET, 0, 0 },
    { "exifosint",    0,            "AnonimusShamshiAlex", "exifosint",     "разбор EXIF для OSINT",           LIMA_NET, 0, 0 },
    { "hashbrutes",   0,            "AnonimusShamshiAlex", "hashbrutes",    "подбор по хешам",                 LIMA_NET, 0, 0 },
    { "cscan",        0,            "AnonimusShamshiAlex", "cscan",         "сканер сети и портов",            LIMA_NET, 0, 0 },
    { "parsing-site", 0,            "AnonimusShamshiAlex", "parsing-site",  "парсинг сайтов",                  LIMA_NET, 0, 0 },
    { "nmap",         0,            0, 0,                                  "сканер портов (чужая программа)", LIMA_PLANNED, 0, 0 },
    { "python",       0,            0, 0,                                  "язык программирования Python",    LIMA_PLANNED, 0, 0 },
    { "git",          0,            0, 0,                                  "система контроля версий",         LIMA_PLANNED, 0, 0 },
    { "telegram",     "телеграм",   0, 0,                                  "мессенджер Telegram",             LIMA_PLANNED, 0, 0 },
    { "msfconsole",   "metasploit", 0, 0,                                  "фреймворк Metasploit (Ruby)",     LIMA_PLANNED, 0, 0 },
    { "sqlmap",       0,            0, 0,                                  "поиск SQL-инъекций (Python)",     LIMA_PLANNED, 0, 0 },
    { "wireshark",    0,            0, 0,                                  "перехват и разбор пакетов",       LIMA_PLANNED, 0, 0 },
};
#define NLIMA ((int)(sizeof lima_catalog / sizeof lima_catalog[0]))

static int lima_find(const char *name) {
    for (int i = 0; i < NLIMA; i++)
        if (strcmp(lima_catalog[i].name, name) == 0 || (lima_catalog[i].alt && strcmp(lima_catalog[i].alt, name) == 0))
            return i;
    return -1;
}

static void lima_list(struct term *t) {
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Твои программы (lima install <имя>):\n");
    term_set_fg(t, TC_DEFAULT);
    for (int i = 0; i < NLIMA; i++)
        if (lima_catalog[i].kind == LIMA_NET)
            term_printf(t, "  %-13s %s  %s\n", lima_catalog[i].name,
                        lima_catalog[i].installed ? "[✓ установлен]" : "              ", lima_catalog[i].note);
    term_set_fg(t, TC_GRAY);
    term_printf(t, "\nБольшие чужие программы (ждут слоя Pulp, пока только показывают статус):\n");
    term_set_fg(t, TC_DEFAULT);
    for (int i = 0; i < NLIMA; i++)
        if (lima_catalog[i].kind == LIMA_PLANNED)
            term_printf(t, "  %-13s %s\n", lima_catalog[i].name, lima_catalog[i].note);
}

static void lima_install(struct term *t, const char *name) {
    int i = lima_find(name);
    if (i < 0) { term_set_fg(t, TC_RED); term_printf(t, "Нет такого пакета. lima list — список.\n"); term_set_fg(t, TC_DEFAULT); return; }
    struct lima_pkg *pkg = &lima_catalog[i];
    if (pkg->kind == LIMA_PLANNED) {
        term_set_fg(t, TC_RED);
        term_printf(t, "«%s» — чужая программа (%s). LimonOS пока не умеет запускать\n", pkg->name, pkg->note);
        term_printf(t, "сторонний код (нужен загрузчик программ, он в планах). Похожее уже есть:\n");
        term_set_fg(t, TC_DEFAULT);
        if (strcmp(pkg->name, "nmap") == 0) term_printf(t, "  scan — свой сканер портов: scan 10.0.2.1 [порт(ы)]\n");
        else if (strcmp(pkg->name, "sqlmap") == 0 || strcmp(pkg->name, "msfconsole") == 0 || strcmp(pkg->name, "wireshark") == 0)
            term_printf(t, "  seal — проверить https/TLS сайта; scan — порты; ai — спросить модель про технику атаки/защиты\n");
        else if (strcmp(pkg->name, "python") == 0) term_printf(t, "  abacus — калькулятор с выражениями: abacus 2+3*(4-1)\n");
        else if (strcmp(pkg->name, "git") == 0) term_printf(t, "  grab — скачать один файл по http(s); полного git-клиента нет\n");
        return;
    }
    if (!net_info()->configured) { term_set_fg(t, TC_RED); term_printf(t, "Нет сети. Набери: vine dhcp\n"); term_set_fg(t, TC_DEFAULT); return; }
    char url[200];
    term_printf(t, "Скачиваю %s/%s с GitHub (https)...\n", pkg->owner, pkg->repo);
    desktop_refresh();
    struct http_opts o; memset(&o, 0, sizeof o);
    o.max_bytes = 1u << 20;
    struct http_resp r; int rc = -1;
    const char *branches[2] = { "main", "master" };
    for (int k = 0; k < 2; k++) {
        ksnprintf(url, sizeof url, "https://raw.githubusercontent.com/%s/%s/%s/README.md", pkg->owner, pkg->repo, branches[k]);
        rc = http_fetch(url, &o, &r);
        if (rc == HTTP_OK && r.status == 200) break;
        http_free(&r);
        if (rc != HTTP_OK && rc != HTTP_E_CLOSED) break;      /* сеть/TLS сломаны — вторая ветка не поможет */
    }
    if (rc != HTTP_OK || r.status != 200) {
        term_set_fg(t, TC_RED);
        if (rc == HTTP_E_TLS) term_printf(t, "Не вышло: TLS — %s\n", tls_strerror(r.tls_err));
        else if (rc != HTTP_OK) term_printf(t, "Не вышло: %s\n", http_strerror(rc));
        else term_printf(t, "Не удалось скачать (ответ сервера: %d). Проверь имя репозитория.\n", r.status);
        term_set_fg(t, TC_DEFAULT);
        if (rc == HTTP_OK) http_free(&r);
        return;
    }
    show_tls_summary(t, &r);
    char *buf = (char *)r.data; size_t body = 0; int n = (int)r.len;
    size_t rl = (size_t)n - body; if (rl > 400) rl = 400;
    if (pkg->readme) kfree(pkg->readme);
    pkg->readme = kmalloc(rl + 1);
    if (pkg->readme) { memcpy(pkg->readme, buf + body, rl); pkg->readme[rl] = 0; }
    pkg->installed = 1;
    http_free(&r); (void)buf;
    term_set_fg(t, TC_GREEN);
    term_printf(t, "Установлен «%s» (метаданные, %d байт README). Теперь можно набрать: %s\n", pkg->name, n, pkg->name);
    term_set_fg(t, TC_DEFAULT);
}

static void cmd_lima(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2 || strcmp(argv[1], "list") == 0) { lima_list(t); return; }
    if (strcmp(argv[1], "install") == 0) {
        if (argc < 3) { term_printf(t, "Пример: lima install cscan\n"); return; }
        lima_install(t, argv[2]);
        return;
    }
    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) { term_printf(t, "Пример: lima run hello.elf LimonOS\n"); return; }
        user_run_file(t, s->cwd, argc - 2, (const char *const *)&argv[2]);
        return;
    }
    if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3) { term_printf(t, "Пример: lima remove cscan\n"); return; }
        int i = lima_find(argv[2]);
        if (i >= 0 && lima_catalog[i].installed) {
            if (lima_catalog[i].readme) { kfree(lima_catalog[i].readme); lima_catalog[i].readme = 0; }
            lima_catalog[i].installed = 0;
            term_printf(t, "Удалён «%s»\n", argv[2]);
        } else term_printf(t, "Не установлен: %s\n", argv[2]);
        return;
    }
    term_printf(t, "Используй: lima list | lima install <имя> | lima remove <имя> | lima run <файл.elf> [аргументы]\n");
}

/* показывает установленный через lima пакет как команду: cscan, iotlamp и т.д. */
static int run_lima_package(struct shell *s, int idx, int argc, char **argv) {
    struct term *t = s->t;
    struct lima_pkg *pkg = &lima_catalog[idx];
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "%s", pkg->name);
    term_set_fg(t, TC_DEFAULT);
    term_printf(t, " — %s\n", pkg->note);
    term_printf(t, "Источник: github.com/%s/%s\n", pkg->owner, pkg->repo);
    if (argc > 1) {
        term_printf(t, "Параметры:");
        for (int i = 1; i < argc; i++) term_printf(t, " %s", argv[i]);
        term_putc(t, '\n');
    }
    if (pkg->readme && pkg->readme[0]) {
        term_set_fg(t, TC_GRAY);
        term_printf(t, "\nREADME (начало):\n%s\n", pkg->readme);
        term_set_fg(t, TC_DEFAULT);
    }
    term_set_fg(t, TC_ORANGE);
    term_printf(t, "\nЗапуск появится после слоя Pulp (загрузчик программ). Пока показаны только метаданные.\n");
    term_set_fg(t, TC_DEFAULT);
    return 0;
}

/* ---------- диск и FAT32 (только чтение) ---------- */
static int ensure_fat(struct term *t) {
    if (fat_mounted()) return 0;
    int n = 0;
    for (int i = 0; ata_disk(i); i++) {
        n++;
        if (fat_mount(ata_disk(i)) == 0) return 0;
    }
    term_set_fg(t, TC_RED);
    if (!n) term_printf(t, "Дисков не видно (поддерживаются IDE/SATA в режиме IDE; USB-флешки пока нет — используй grab по сети).\n");
    else term_printf(t, "Диск есть, но FAT32 на нём не найден (нужен FAT32 без GPT или с MBR).\n");
    term_set_fg(t, TC_DEFAULT);
    return -1;
}

static void cmd_disks(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct term *t = s->t;
    int n = 0;
    for (int i = 0; ata_disk(i); i++, n++) {
        const struct blkdev *d = ata_disk(i);
        term_printf(t, "  %-4s %u МиБ\n", d->name, (unsigned)(d->sectors / 2048));
    }
    if (!n) term_printf(t, "  Дисков не найдено (ATA/IDE).\n");
    else if (fat_mounted()) term_printf(t, "FAT32 подключён. ls — список, load файл — в память.\n");
    else { term_printf(t, "FAT32 пока не подключён: "); if (ensure_fat(t) == 0) term_printf(t, "готово.\n"); }
}

static void dls_line(const char *name, int is_dir, uint32_t size, void *ctx) {
    struct term *t = ctx;
    if (is_dir) { term_set_fg(t, TC_ACCENT); term_printf(t, "  %s/\n", name); term_set_fg(t, TC_DEFAULT); }
    else term_printf(t, "  %-36s %8u байт\n", name, (unsigned)size);
}

/* список файлов на настоящем диске (FAT32, только чтение) — не путать с ls (список в памяти, Memoria) */
static void cmd_dls(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (ensure_fat(t)) return;
    int r = fat_list(argc >= 2 ? argv[1] : "/", dls_line, t);
    if (r) { term_set_fg(t, TC_RED); term_printf(t, r == -3 ? "Ошибка чтения диска.\n" : "Нет такой папки.\n"); term_set_fg(t, TC_DEFAULT); }
}

static void cmd_load(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: load KEY.TXT   или   load /папка/файл.txt [имя]  — читает с диска в память (files)\n"); return; }
    if (ensure_fat(t)) return;
    uint8_t *data; size_t len;
    int r = fat_read(argv[1], &data, &len, 8u << 20);
    if (r) {
        term_set_fg(t, TC_RED);
        term_printf(t, r == -1 ? "Нет файла «%s» на диске (ls — список).\n" : r == -2 ? "Файл больше 8 МиБ или не хватило памяти.\n" : "Ошибка чтения диска.\n", argv[1]);
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    char name[48];
    if (argc >= 3) ksnprintf(name, sizeof name, "%.47s", argv[2]);
    else { const char *b = argv[1], *p = argv[1]; for (; *p; p++) if (*p == '/') b = p + 1; ksnprintf(name, sizeof name, "%.47s", b); }
    if (vfs_put(s->cwd, name, data, len) == VFS_NONE) { kfree(data); term_set_fg(t, TC_RED); term_printf(t, "Не сохранить: имя недопустимо или папка занята файлом с этим именем.\n"); term_set_fg(t, TC_DEFAULT); return; }
    term_set_fg(t, TC_GREEN); term_printf(t, "Загружено в текущую папку: %s, %u байт\n", name, (unsigned)len); term_set_fg(t, TC_DEFAULT);
}

static void cmd_play(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    static const char *const names[GAME_COUNT] = { "snake", "tetris", "pong", "2048" };
    static const char *const ru[GAME_COUNT] = { "змейка", "тетрис", "понг", "2048" };
    if (argc >= 2) {
        for (char *p = argv[1]; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        for (int i = 0; i < GAME_COUNT; i++)
            if (strcmp(argv[1], names[i]) == 0 || strcmp(argv[1], ru[i]) == 0) {
                if (desktop_open_game(i) < 0) { term_set_fg(t, TC_RED); term_printf(t, "Нет места для окна — закрой что-нибудь.\n"); term_set_fg(t, TC_DEFAULT); }
                return;
            }
    }
    term_printf(t, "Игры: play snake | tetris | pong | 2048   (или змейка, тетрис, понг)\nЕщё их можно открыть значками на рабочем столе и из меню «Лимон».\n");
}

static void cmd_uname(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    term_printf(s->t, "LimonOS %s x86_64 (не Linux и не совместима с ним на уровне программ)\n", LIMON_VERSION);
}

static void cmd_whoami(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    term_printf(s->t, "лимон (в LimonOS нет пользователей и прав — всё под одной учёткой)\n");
}

static void cmd_top(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct term *t = s->t;
    int n = desktop_task_count();
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "№  ОКНО                 ТИП          \n");
    term_set_fg(t, TC_DEFAULT);
    for (int i = 0; i < n; i++) {
        char title[40], kind[24]; int min, foc;
        if (desktop_task_info(i, title, sizeof title, kind, sizeof kind, &min, &foc) != 0) continue;
        term_set_fg(t, foc ? TC_GREEN : TC_DEFAULT);
        term_printf(t, "%-2d %-20s %-12s %s\n", i + 1, title, kind, min ? "(свёрнуто)" : foc ? "(активно)" : "");
        term_set_fg(t, TC_DEFAULT);
    }
    if (!n) term_printf(t, "Открытых окон нет.\n");
    term_putc(t, '\n');
    term_set_fg(t, TC_ACCENT); term_printf(t, "Память:\n"); term_set_fg(t, TC_DEFAULT);
    term_printf(t, "  Куча:  занято %u КиБ из %u КиБ (автоматически растёт)\n", (unsigned)(heap_used() / 1024), (unsigned)(heap_total() / 1024));
    if (g_sys.mem_valid) term_printf(t, "  ОЗУ:   свободно %llu МиБ\n", mib(g_sys.mem_usable));
    term_printf(t, "  Файлы в памяти (Memoria): %u КиБ\n", (unsigned)(vfs_total_bytes() / 1024));
    term_printf(t, "\nЗакрыть окно: kill <№>\n");
}

static void cmd_kill(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: kill 2   (номер — из top)\n"); return; }
    int n = 0; for (const char *p = argv[1]; *p; p++) { if (*p < '0' || *p > '9') { term_set_fg(t, TC_RED); term_printf(t, "Нужен номер окна (top — список).\n"); term_set_fg(t, TC_DEFAULT); return; } n = n * 10 + (*p - '0'); }
    if (desktop_task_close(n - 1) != 0) { term_set_fg(t, TC_RED); term_printf(t, "Нет окна №%d (top — список).\n", n); term_set_fg(t, TC_DEFAULT); return; }
    term_printf(t, "Окно №%d закрыто.\n", n);
}

static void cmd_rebirth(struct shell *s, int argc, char **argv) {
    (void)s; (void)argc; (void)argv;
    desktop_power(1);
}

static void cmd_bye(struct shell *s, int argc, char **argv) {
    (void)s; (void)argc; (void)argv;
    desktop_power(0);
}

static const struct cmd commands[] = {
    { "zest",     "цедра",   "список команд",                          cmd_zest },
    { "help",     "помощь",  "то же, что zest",                        cmd_zest },
    { "squeeze",  "выжать",  "очистить экран терминала",               cmd_squeeze },
    { "peel",     "кожура",  "сведения о системе (с лимоном)",         cmd_peel },
    { "juice",    "сок",     "память: сколько сока осталось",          cmd_juice },
    { "engine",   "мотор",   "процессор и его возможности",            cmd_engine },
    { "sundial",  "время",   "дата и время (часы BIOS)",               cmd_sundial },
    { "pulse",    "пульс",   "сколько система уже работает",           cmd_pulse },
    { "say",      "сказать", "повторить сказанное",                    cmd_say },
    { "abacus",   "счёты",   "калькулятор: abacus 2+3*(4-1)",          cmd_abacus },
    { "palette",  "краски",  "цвета и темы оформления",                cmd_palette },
    { "tint",     "оттенок", "сменить тему: tint lime",                cmd_tint },
    { "lang",     "язык",    "раскладка клавиатуры: lang ru | en",     cmd_lang },
    { "twin",     "двойник", "открыть ещё один терминал",              cmd_twin },
    { "pci",      "шина",    "устройства на шине материнской платы",   cmd_pci },
    { "vine",     "лоза",    "сеть: адрес, шлюз, DNS; vine dhcp",      cmd_vine },
    { "sonar",    "эхо",     "проверить связь (ping): sonar 8.8.8.8",  cmd_sonar },
    { "lookup",   "найти",   "узнать адрес сайта: lookup example.com", cmd_lookup },
    { "ping",     "пинг",    "проверить связь (то же, что sonar)",     cmd_sonar },
    { "citrus",   "цитрус",  "простой браузер: citrus example.com",    cmd_citrus },
    { "lima",     "лима",    "ставить программы: lima install cscan",  cmd_lima },
    { "grab",     "качать",  "скачать файл в память: grab https://...", cmd_grab },
    { "ls",       "список",  "список файлов в текущей папке: ls [путь]", cmd_ls },
    { "files",    "файлы",   "то же, что ls",                           cmd_ls },
    { "pwd",      "гдея",    "текущая папка",                           cmd_pwd },
    { "cd",       "вход",    "перейти в папку: cd Downloads, cd ..",    cmd_cd },
    { "mkdir",    "папка",   "создать папку: mkdir Проект",             cmd_mkdir },
    { "cp",       "копия",   "скопировать файл: cp a.txt b.txt",        cmd_cp },
    { "mv",       "перенос", "переместить/переименовать: mv a.txt b.txt", cmd_mv },
    { "cat",      "вывод",   "показать файл (то же, что peek)",         cmd_peek },
    { "peek",     "глянуть", "показать файл: peek имя [hex]",          cmd_peek },
    { "sha",      "хеш",     "SHA-256 файла: sha имя",                 cmd_sha },
    { "drop",     "убрать",  "удалить файл из памяти: drop имя",       cmd_drop },
    { "upload",   "отдать",  "отправить файл: upload имя https://...", cmd_upload },
    { "seal",     "печать",  "проверить https/TLS: seal github.com",   cmd_seal },
    { "scan",     "скан",    "сканер портов: scan 10.0.2.1 [порт(ы)]", cmd_scan },
    { "ftp",      "фтп",     "FTP/FTPS как ftp.exe: ftp [хост [порт]] [--tls|--plain|--implicit], дальше ls/cd/get/put/bye", cmd_ftp },
    { "run",      "запуск",  "запустить программу (ELF): run hello.elf LimonOS", cmd_run },
    { "ring3",    "кольцо3", "проверка пользовательского режима (фаза 3)", cmd_ring3 },
    { "google",   "гугл",    "поиск в интернете: google запрос",       cmd_google },
    { "ai",       "аи",      "спросить ИИ (Claude): ai твой вопрос",   cmd_ai },
    { "aikey",    "аиключ",  "задать ключ Anthropic API: aikey sk-...", cmd_aikey },
    { "play",     "играть",  "игры: play snake|tetris|pong|2048",      cmd_play },
    { "disks",    "диски",   "диски и FAT32 (чтение)",                 cmd_disks },
    { "dls",      "дискдир", "список файлов на диске: dls [/папка]",   cmd_dls },
    { "load",     "взять",   "файл с диска в память: load KEY.TXT",     cmd_load },
    { "omen",     "знамение","лимонное предсказание",                  cmd_omen },
    { "top",      "диспетчер","открытые окна и память (как диспетчер задач)", cmd_top },
    { "ps",       "процессы","то же, что top",                         cmd_top },
    { "kill",     "закрыть", "закрыть окно по номеру из top: kill 2",  cmd_kill },
    { "free",     "память2", "память (то же, что juice)",              cmd_juice },
    { "df",       "диски2",  "диски (то же, что disks)",               cmd_disks },
    { "uname",    "осимя",   "имя и версия системы",                   cmd_uname },
    { "whoami",   "ктоя",    "кто ты в системе",                       cmd_whoami },
    { "apt",      "апт",     "ставить программы (см. lima)",           cmd_lima },
    { "apt-get",  "аптгет",  "то же, что apt",                         cmd_lima },
    { "bg",       "фон",     "поставить ELF в очередь: bg hello.elf (не запускает — см. go)", cmd_bg },
    { "jobs",     "задачи",  "список очереди Фазы 7 (bg/go)",         cmd_jobs },
    { "go",       "поехали", "запустить очередь: несколько процессов по кругу, настоящая вытесняющая многозадачность", cmd_go },
    { "rebirth",  "заново",  "перезагрузить компьютер",                cmd_rebirth },
    { "bye",      "пока",    "выключить компьютер",                    cmd_bye },
};
static const int ncommands = (int)(sizeof commands / sizeof commands[0]);

static void cmd_run(struct shell *s, int argc, char **argv) {
    if (argc < 2) { term_printf(s->t, "Пример: run hello.elf LimonOS   (файл ищется в текущей папке, затем в /System)\n"); return; }
    user_run_file(s->t, s->cwd, argc - 1, (const char *const *)&argv[1]);
}

/* ---------- Фаза 7: настоящая многозадачность — несколько ELF одновременно, по кругу ---------- */
static void cmd_bg(struct shell *s, int argc, char **argv) {
    struct term *t = s->t;
    if (argc < 2) { term_printf(t, "Пример: bg hello.elf LimonOS   — поставить в очередь (не запускает; запуск: go)\n"); return; }
    int idx = vfs_resolve(s->cwd, argv[1]);
    if (idx == VFS_NONE && vfs_system != VFS_NONE) idx = vfs_resolve(vfs_system, argv[1]);
    if (idx == VFS_NONE || vfs_is_dir(idx)) {
        term_set_fg(t, TC_RED); term_printf(t, "Нет такого файла: %s\n", argv[1]); term_set_fg(t, TC_DEFAULT);
        return;
    }
    int slot = sched_spawn(vfs_data(idx), vfs_len(idx), argc - 1, (const char *const *)&argv[1]);
    if (slot < 0) {
        term_set_fg(t, TC_RED); term_printf(t, "Не встал в очередь: %s\n", sched_spawn_err(slot)); term_set_fg(t, TC_DEFAULT);
        return;
    }
    term_set_fg(t, TC_GREEN); term_printf(t, "В очереди: слот %d — %s\n", slot, argv[1]); term_set_fg(t, TC_DEFAULT);
}
static const char *proc_state_str(enum proc_state st) {
    switch (st) {
    case PROC_RUNNABLE: return "ждёт «go»";
    case PROC_DONE:      return "завершён";
    case PROC_FAULTED:   return "упал";
    case PROC_TIMEOUT:   return "таймаут";
    default:              return "?";
    }
}
static void cmd_jobs(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct term *t = s->t;
    struct proc_info info[SCHED_MAX_PROC];
    int n = sched_list(info, SCHED_MAX_PROC);
    if (!n) { term_printf(t, "Очередь пуста. bg <файл.elf> — поставить задачу, go — запустить очередь.\n"); return; }
    term_set_fg(t, TC_ACCENT); term_printf(t, "слот  имя                состояние    квантов\n"); term_set_fg(t, TC_DEFAULT);
    for (int i = 0; i < n; i++)
        term_printf(t, "  %-4d %-19s %-12s %u\n", info[i].slot, info[i].name, proc_state_str(info[i].state), info[i].quanta);
}
static void cmd_go(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    sched_run_queue(s->t);
}

static void cmd_ring3(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    user_selftest(s->t);
}

static void cmd_zest(struct shell *s, int argc, char **argv) {
    (void)argc; (void)argv;
    struct term *t = s->t;
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Лимонный справочник LimonOS %s\n", LIMON_VERSION);
    term_set_fg(t, TC_DEFAULT);
    for (int i = 0; i < ncommands; i++) {
        term_set_fg(t, TC_ACCENT);
        term_printf(t, "  %-9s", commands[i].name);
        term_set_fg(t, TC_GRAY);
        term_printf(t, "%-8s", commands[i].alt);
        term_set_fg(t, TC_DEFAULT);
        term_printf(t, "%s\n", commands[i].desc);
    }
    term_set_fg(t, TC_GRAY);
    term_printf(t, "Клавиши: F2 язык, F3 терминал, F4 закрыть окно, F5 тема, Alt+Tab окна\n");
    term_set_fg(t, TC_DEFAULT);
}

/* ---------- выполнение ---------- */

static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (argc < max) argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

static void run_command(struct shell *s, char *line) {
    char *argv[12];
    int argc = tokenize(line, argv, 12);
    if (argc == 0) return;
    for (char *p = argv[0]; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    for (int i = 0; i < ncommands; i++) {
        if (strcmp(argv[0], commands[i].name) == 0 || strcmp(argv[0], commands[i].alt) == 0) {
            commands[i].fn(s, argc, argv);
            return;
        }
    }
    int li = lima_find(argv[0]);
    if (li >= 0 && lima_catalog[li].installed) { run_lima_package(s, li, argc, argv); return; }
    term_set_fg(s->t, TC_RED);
    term_printf(s->t, "Неизвестная команда «%s». Набери zest, чтобы увидеть список.\n", argv[0]);
    if (li >= 0) term_printf(s->t, "Подсказка: набери  lima install %s\n", argv[0]);
    term_set_fg(s->t, TC_DEFAULT);
}

static void submit(struct shell *s) {
    struct term *t = s->t;
    char buf[SH_HIST_BYTES];
    int n = 0;
    for (int i = 0; i < s->len; i++) n += utf8_encode(s->line[i], buf + n);
    buf[n] = 0;
    t->cx = PROMPT_COLS + s->len;
    term_putc(t, '\n');
    if (n > 0) {
        /* ключи не кладём в историю стрелкой ↑: aikey sk-... не должен там оставаться */
        static const char sec1[] = "aikey sk-", sec2[] = "аиключ sk-";
        int secret = strncmp(buf, sec1, sizeof sec1 - 1) == 0 || strncmp(buf, sec2, sizeof sec2 - 1) == 0;
        if (!secret) add_history(s, buf);
        run_command(s, buf);
        secure_zero(buf, sizeof buf);
    }
    show_prompt(s);
}

void shell_init(struct shell *s, struct term *t) {
    s->t = t;
    s->hist_count = 0;
    s->hist_idx = -1;
    s->cwd = vfs_downloads >= 0 ? vfs_downloads : VFS_ROOT;
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "LimonOS %s", LIMON_VERSION);
    term_set_fg(t, TC_DEFAULT);
    term_printf(t, " — терминал. Набери ");
    term_set_fg(t, TC_ACCENT);
    term_printf(t, "zest");
    term_set_fg(t, TC_DEFAULT);
    term_printf(t, ", чтобы увидеть команды.\n\n");
    show_prompt(s);
}

void shell_key(struct shell *s, const struct key_event *ev) {
    uint32_t cp = ev->cp;
    if (ev->mods & MOD_CTRL) {
        if (cp == 'l') { term_clear(s->t); show_prompt(s); }
        else if (cp == 'c') { s->t->cx = PROMPT_COLS + s->len; term_printf(s->t, "^C\n"); show_prompt(s); }
        else if (cp == 'u') { s->len = s->pos = s->scroll = 0; repaint(s); }
        return;
    }
    switch (cp) {
    case '\n': submit(s); return;
    case '\b':
        if (s->pos > 0) {
            for (int i = s->pos; i < s->len; i++) s->line[i - 1] = s->line[i];
            s->pos--; s->len--;
        }
        break;
    case KEY_DELETE:
        if (s->pos < s->len) {
            for (int i = s->pos + 1; i < s->len; i++) s->line[i - 1] = s->line[i];
            s->len--;
        }
        break;
    case KEY_LEFT:  if (s->pos > 0) s->pos--; break;
    case KEY_RIGHT: if (s->pos < s->len) s->pos++; break;
    case KEY_HOME:  s->pos = 0; break;
    case KEY_END:   s->pos = s->len; break;
    case KEY_UP:
        if (s->hist_count == 0) return;
        if (s->hist_idx < 0) s->hist_idx = s->hist_count - 1;
        else if (s->hist_idx > 0) s->hist_idx--;
        load_line(s, s->hist[s->hist_idx]);
        break;
    case KEY_DOWN:
        if (s->hist_idx < 0) return;
        if (++s->hist_idx >= s->hist_count) { s->hist_idx = -1; s->len = s->pos = 0; }
        else load_line(s, s->hist[s->hist_idx]);
        break;
    default:
        if (cp >= 0x20 && cp < 0x110000 && s->len < SH_LINE_MAX) {
            for (int i = s->len; i > s->pos; i--) s->line[i] = s->line[i - 1];
            s->line[s->pos++] = cp;
            s->len++;
        }
        break;
    }
    repaint(s);
}
