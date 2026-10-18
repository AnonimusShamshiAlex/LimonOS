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

#define PROMPT      "лимон> "
#define PROMPT_COLS 7

/* ---------- редактирование строки ---------- */

static void repaint(struct shell *s) {
    struct term *t = s->t;
    int row = t->cy, col = 0;
    for (int c = 0; c < TERM_COLS; c++) t->cell[row][c] = (struct tcell){ ' ', TC_DEFAULT };
    const char *p = PROMPT;
    while (*p) t->cell[row][col++] = (struct tcell){ utf8_next(&p), TC_ACCENT };
    for (int i = 0; i < s->len; i++) t->cell[row][col++] = (struct tcell){ s->line[i], TC_DEFAULT };
    t->cx = PROMPT_COLS + s->pos;
}

static void show_prompt(struct shell *s) {
    if (s->t->cx != 0) term_putc(s->t, '\n');
    s->len = s->pos = 0;
    s->hist_idx = -1;
    term_set_fg(s->t, TC_DEFAULT);
    repaint(s);
}

static void load_line(struct shell *s, const char *utf8) {
    s->len = 0;
    while (*utf8 && s->len < SH_LINE_MAX) s->line[s->len++] = utf8_next(&utf8);
    s->pos = s->len;
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
        term_printf(t, "  Сетевая карта не найдена (поддерживается Intel e1000).\n");
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
    { "omen",     "знамение","лимонное предсказание",                  cmd_omen },
    { "rebirth",  "заново",  "перезагрузить компьютер",                cmd_rebirth },
    { "bye",      "пока",    "выключить компьютер",                    cmd_bye },
};
static const int ncommands = (int)(sizeof commands / sizeof commands[0]);

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
    term_set_fg(s->t, TC_RED);
    term_printf(s->t, "Неизвестная команда «%s». Набери zest, чтобы увидеть список.\n", argv[0]);
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
        add_history(s, buf);
        run_command(s, buf);
    }
    show_prompt(s);
}

void shell_init(struct shell *s, struct term *t) {
    s->t = t;
    s->hist_count = 0;
    s->hist_idx = -1;
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
        else if (cp == 'u') { s->len = s->pos = 0; repaint(s); }
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
