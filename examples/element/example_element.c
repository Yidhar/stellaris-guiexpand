/* Example plugin: elements for declared panels, in plain C (built with /MT). A mod declares
 *
 *     demo_card = { title = MY_TITLE  content = { demo_meter = { resource = energy  label = MY_ENERGY }  demo_spark = { series = energy } } }
 *
 * and the host, which does not know `demo_card`, hands the entry to this plugin. It shows the four things a component needs: reading its parameters
 * from the node (value / number / text), drawing with the host's theme and scale, drawing the entries of a block (a container), and using the
 * host's data and actions (snapshot, history, panel visibility).
 *
 * demo_fault crashes when the plugin's folder has an consumer_element.cmd containing `fault`: it is how the host's protection is tested.
 * The event Local\gui_element_unload_<pid> unregisters the elements and unloads the DLL (development only: the launcher never unloads plugins). */
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "stellaris_gui_api.h"
#include "stellaris_gui_client.h"

static HMODULE g_module;
static char g_dir[MAX_PATH];
static FILE* g_log;
static const struct StlGuiApi* g_api;
static int g_handles[16];
static int g_count;
static volatile int g_armed;

static void log_(const char* msg) {
    SYSTEMTIME t;
    if (!g_log) return;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, msg);
    fflush(g_log);
}

static float scaled(const StlGuiCallbackCtx* c, float px) { return px * c->ui_scale; }

/* a card: a title in the theme's colour, a line, and the entries of `content` drawn by the host */
static void el_card(const StlGuiCallbackCtx* c, const StlGuiNode* n, void* user) {
    const StlGuiNodeApi* N = c->node;
    (void)user;
    c->ui->text_colored(c->theme->accent, N->text(n, "title", ""));
    c->ui->separator();
    N->draw_block(N->child(n, "content"));
    c->ui->dummy(1.f, scaled(c, 4.f));
}

/* a labelled bar of one resource: the stock against its cap, in the theme's colours */
static void el_meter(const StlGuiCallbackCtx* c, const StlGuiNode* n, void* user) {
    StlGuiSnapshot s;
    const char* key = c->node->value(n, "resource", "energy");
    char text[96];
    uint32_t i;
    (void)user;
    c->ui->text_colored(c->theme->text, c->node->text(n, "label", key));
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    if (!c->api->get_snapshot(&s)) {
        c->ui->text_colored(c->theme->text_dim, "not in a game");
        return;
    }
    for (i = 0; i < s.resource_count; ++i) {
        if (strcmp(s.resources[i].key, key)) continue;
        sprintf(text, "%.0f (%+.0f)", s.resources[i].stock, s.resources[i].net);
        c->ui->progress_bar(s.resources[i].max > 0 ? (float)(s.resources[i].stock / s.resources[i].max) : 0.f, -1.f, scaled(c, 14.f), text);
        return;
    }
    c->ui->text_colored(c->theme->bad, "unknown resource");
}

/* a sparkline of a series of the host's history */
static void el_spark(const StlGuiCallbackCtx* c, const StlGuiNode* n, void* user) {
    float v[160], lo = 1e30f, hi = -1e30f, pos[2], avail[2], h = scaled(c, (float)c->node->number(n, "height", 40.0));
    int count, i;
    (void)user;
    count = c->api->get_history(c->node->value(n, "series", "energy"), v, 160);
    c->ui->get_cursor_screen_pos(pos);
    c->ui->get_content_region_avail(avail);
    c->ui->dummy(avail[0], h + scaled(c, 4.f));
    if (count < 2) {
        c->ui->draw_text(pos[0], pos[1], c->theme->text_dim, "no history yet");
        return;
    }
    for (i = 0; i < count; ++i) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    if (hi - lo < 1e-3f) hi = lo + 1.f;
    for (i = 0; i + 1 < count; ++i) {
        float x0 = pos[0] + avail[0] * i / (count - 1), x1 = pos[0] + avail[0] * (i + 1) / (count - 1);
        float y0 = pos[1] + h * (1.f - (v[i] - lo) / (hi - lo)), y1 = pos[1] + h * (1.f - (v[i + 1] - lo) / (hi - lo));
        c->ui->draw_line(x0, y0, x1, y1, c->theme->accent2, scaled(c, 1.6f));
    }
}

/* a button that shows or hides another panel (by its id) */
static void el_toggle(const StlGuiCallbackCtx* c, const StlGuiNode* n, void* user) {
    const char* panel = c->node->value(n, "panel", "");
    (void)user;
    if (c->ui->button(c->node->text(n, "text", panel))) c->api->panel_visibility(panel, 2);
    if (c->api->panel_visibility(panel, -1) < 0) c->ui->tooltip("no such panel");
}

/* one figure of the country against the strongest empire of the galaxy (the appended members of the snapshot) */
static void el_stat(const StlGuiCallbackCtx* c, const StlGuiNode* n, void* user) {
    StlGuiSnapshot s;
    const char* stat = c->node->value(n, "stat", "military_power");
    double v = 0, mx = 1;
    char text[96];
    (void)user;
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    c->ui->text_colored(c->theme->text, c->node->text(n, "label", stat));
    if (!c->api->get_snapshot(&s) || s.size < sizeof(s)) {
        c->ui->text_colored(c->theme->text_dim, "not in a game");
        return;
    }
    if (!strcmp(stat, "military_power")) v = s.military_power, mx = s.military_power_max;
    else if (!strcmp(stat, "tech_power")) v = s.tech_power, mx = s.tech_power_max;
    else if (!strcmp(stat, "economy_power")) v = s.economy_power, mx = s.economy_power_max;
    else if (!strcmp(stat, "colonies")) v = s.colonies, mx = s.colonies_max;
    else if (!strcmp(stat, "pops")) v = s.pops, mx = s.pops_max;
    else {
        c->ui->text_colored(c->theme->bad, "unknown stat");
        return;
    }
    sprintf(text, "%.0f of the strongest %.0f", v, mx);
    c->ui->progress_bar(mx > 0 ? (float)(v / mx) : 0.f, -1.f, scaled(c, 14.f), text);
}

/* a button per theme of the host; the current one is marked */
static void el_themes(const StlGuiCallbackCtx* c, const StlGuiNode* n, void* user) {
    int i;
    (void)n;
    (void)user;
    for (i = 0; i < (int)c->theme->theme_count; ++i) {
        StlGuiTheme t;
        char label[96];
        memset(&t, 0, sizeof(t));
        t.size = sizeof(t);
        if (!c->api->theme_info(i, &t)) continue;
        sprintf(label, "%s%s", (uint32_t)i == c->theme->index ? "> " : "", t.name);
        if (c->ui->button(label)) c->api->set_theme(i);
    }
}

/* raises an access violation once armed (consumer_element.cmd: `fault`) */
static void el_fault(const StlGuiCallbackCtx* c, const StlGuiNode* n, void* user) {
    (void)n;
    (void)user;
    c->ui->text_colored(g_armed ? c->theme->bad : c->theme->text_dim, g_armed ? "demo_fault: armed, crashing now" : "demo_fault: not armed");
    if (g_armed) *(volatile int*)0 = 1;
}

static void register_all(void) {
    static const struct {
        const char* name;
        StlGuiElementFn fn;
    } els[] = { { "demo_card", el_card }, { "demo_meter", el_meter }, { "demo_spark", el_spark }, { "demo_toggle", el_toggle }, { "demo_fault", el_fault },
                 { "demo_stat", el_stat }, { "demo_themes", el_themes } };
    char msg[160];
    size_t i;
    for (i = 0; i < sizeof(els) / sizeof(els[0]); ++i) {
        StlGuiElementDesc d;
        memset(&d, 0, sizeof(d));
        d.size = sizeof(d);
        d.name = els[i].name;
        d.provider = "example-element";
        d.draw = els[i].fn;
        g_handles[g_count] = g_api->register_element(&d);
        sprintf(msg, "element %s: handle %d", els[i].name, g_handles[g_count]);
        log_(msg);
        ++g_count;
    }
}

static void poll_cmd(void) {
    char path[MAX_PATH], line[64];
    FILE* f;
    sprintf(path, "%sconsumer_element.cmd", g_dir);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "fault", 5)) {
            g_armed = 1;
            log_("armed: demo_fault will crash");
        } else if (!strncmp(line, "disarm", 6)) {
            g_armed = 0;
            log_("disarmed");
        }
    }
    fclose(f);
    DeleteFileA(path);
}

static DWORD WINAPI worker(LPVOID arg) {
    char lp[MAX_PATH], name[64];
    HANDLE ev;
    DWORD waited = WAIT_TIMEOUT;
    int i;
    (void)arg;
    GetModuleFileNameA(g_module, g_dir, MAX_PATH);
    strrchr(g_dir, '\\')[1] = 0;
    sprintf(lp, "%sconsumer_element.log", g_dir);
    g_log = fopen(lp, "a");
    log_("loaded; looking for the GUI host");
    sprintf(name, "Local\\gui_element_unload_%lu", GetCurrentProcessId());
    ev = CreateEventA(NULL, TRUE, FALSE, name);
    ResetEvent(ev);  /* a named event outlives the instance that made it: an earlier unload must not unload this one too */
    for (i = 0; i < 4800 && !g_api; ++i) {
        g_api = stl_gui_try_connect(STL_GUI_HOST_DLL, STL_GUI_API_VERSION);
        if (g_api) break;
        waited = WaitForSingleObject(ev, 250);
        if (waited != WAIT_TIMEOUT) break;
    }
    if (g_api && waited == WAIT_TIMEOUT) {
        /* the element registry was appended after the first release of the interface: an older host does not have it */
        if (g_api->size < offsetof(StlGuiApi, set_theme) + sizeof(void*)) {
            log_("this host is too old for elements (no register_element)");
        } else {
            register_all();
            while (WaitForSingleObject(ev, 200) == WAIT_TIMEOUT) poll_cmd();
            waited = WAIT_OBJECT_0;
        }
    }
    if (waited == WAIT_OBJECT_0) {
        for (i = 0; i < g_count; ++i)
            if (g_handles[i]) g_api->unregister_element(g_handles[i]);
        log_("unregistered, leaving");
        Sleep(500);
    }
    if (g_log) fclose(g_log);
    CloseHandle(ev);
    if (waited == WAIT_OBJECT_0) FreeLibraryAndExitThread(g_module, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t;
        DisableThreadLibraryCalls(module);
        g_module = module;
        t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
