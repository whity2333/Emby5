/*
 * Emby5 — Emby for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The app: accounts ("Hvem ser på?", sign-in with a password or Quick Connect),
 * the tabbed UI (Hjem, Filmer, Serier, Søk, Innstillinger) drawn on the GPU,
 * detail pages, and playback on the native player with the PS5 device profile.
 *
 * Hardware bring-up follows Nuvio PS5 (and EVO Player before it): decoder
 * probes and module loads run before anything else touches the process.
 */
#include "app/catalogue_request_queue.h"
#include "emby5_playback.h"
#include "jf/jf_http.h"
#include "app/accounts.h"
#include "app/i18n.h"
#include "app/perf.h"
#include "app/remote.h"
#include "app/seerr_service.h"
#include "app/spawn.h"
#include "app/io_worker.h"
#include "app/syncplay.h"
#include "app/settings.h"
#include "app/user_data_update.h"
#include "app/update_service.h"
#include "platform/ime.h"
#include "jf/jf_client.h"
#include "nuvio_input.h"
#include "nuvio_player.h"
#include "gfx/art.h"
#include "gfx/gfx.h"
#include "gfx/gfx_pool.h"
#include "ui/album.h"
#include "ui/detail.h"
#include "ui/home.h"
#include "ui/library.h"
#include "ui/nav.h"
#include "ui/now_playing.h"
#include "ui/person.h"
#include "ui/login.h"
#include "ui/profiles.h"
#include "ui/screensaver.h"
#include "ui/search.h"
#include "ui/search_text.h"
#include "ui/iptv.h"
#include "ui/vod.h"
#include "ui/vod_detail.h"
#include "ui/trailer_qr.h"
#include "app/iptv_vod.h"
#include "app/iptv_live.h"
#include "app/iptv_navigation.h"
#include "app/home_sections.h"
#include "app/browse_state.h"
#include "ui/seerr_detail.h"
#include "ui/settings_screen.h"
#include "ui/syncplay_screen.h"
#include "ui_image.h"
#include "splash_art.inc"
#include "ui_assets.h"
#include "ui_text.h"

#include "evo_adec.h"
#include "evo_agc_runtime.h"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"
#include "evo_data_path.h"
#include "evo_direct_mem.h"
#include "evo_hw.h"
#include "evo_vdec.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <ucontext.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "cJSON.h"
#include <libavformat/avformat.h>
#include <libavutil/log.h>

int sceUserServiceInitialize(void *params);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
int scePadInit(void);
int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
int sceSystemServiceHideSplashScreen(void);
void evo_log_alloc_state(const char *when);
extern int g_ps5_user_id;
}

#ifndef EMBY5_SERVER
#define EMBY5_SERVER ""   /* set from EMBY_URL in .env.local at build time; else the login asks */
#endif
#ifndef EMBY5_VERSION
#define EMBY5_VERSION "0.0.1"
#endif

namespace evo {
extern int DisplayWidth;
extern int DisplayHeight;
}

namespace {

/* The player's decode surfaces (UI textures have their own pool, src/gfx). */
constexpr size_t kDirectMemPool = 128u * 1024u * 1024u;

int s_user = 0;

/* ---- app state: written by worker threads, read by the render loop ---------------- */
enum class Phase { Connecting, Gate, Loading, Home, Failed };
enum class Gate { None, Profiles, Login };

struct GateRequest {
    Gate kind = Gate::None;
    std::string server, user;
    bool can_cancel = false;
    bool known_server = false;   /* signed in to before: the login skips its address */
    bool return_to = false;      /* Profiles: the one the login came from, on the server it showed */
};

struct State {
    std::mutex lock;
    Phase phase = Phase::Connecting;
    std::string message;
    std::string server_name, server_version;
    ui::HomeModel model;
    std::vector<jf::Item> views;        /* the user's libraries, in their order */
    GateRequest gate;                   /* a gate screen the main thread should open */
    /* Seerr's tab: its rows, and how their loading goes. */
    ui::HomeModel discover;
    bool discover_loading = false, discover_failed = false;
    bool discover_more_loading = false;   /* a next page of one of its rows */
    double discover_more_retry_at = 0;    /* after a failed page: not before (now_s) */
    struct DiscoverAppend {
        int shelf, page;
        bool more;
        std::vector<jf::Item> items;
    };
    std::vector<DiscoverAppend> discover_appends;   /* next pages for the screen's rows, not yet taken */
    bool discover_again = false;        /* asked for while loading (the language moved on): once more */
    double discover_at = -1;            /* when it last loaded (now_s), -1 never */
};
State s_state;
std::atomic<unsigned> s_model_version{0};
unsigned s_home_version = 0;   /* model published / taken by Home */
std::atomic<unsigned> s_discover_version{0};   /* Seerr's tab: published (bumped off the render thread) */
unsigned s_discover_taken = 0;                 /* ... and taken */
std::atomic<unsigned> s_discover_appended{0};  /* a next page queued for its rows (a frame to take it) */
std::atomic<unsigned> s_session{0};                 /* bumped on every account change */

/* One client per session, configured before anyone uses it and never changed or
 * freed after: a request still running for the last account finishes on its own
 * client (and its result is dropped), never on the new account's half-set one. */
jf::Client *s_client = nullptr;
std::vector<std::unique_ptr<jf::Client>> s_clients;
std::string s_device;
std::atomic<bool> s_user_data_busy{false};
std::atomic<unsigned> s_home_request{0};
struct UserDataCompletion {
    unsigned session;
    emby5::UserDataResult result;
};
std::mutex s_user_data_lock;
std::vector<std::shared_ptr<UserDataCompletion>> s_user_data_done;
std::atomic<bool> s_user_data_refresh_requested{false};

jf::Client *new_client(const std::string &server, jf::ServerKind kind = jf::ServerKind::Emby)
{
    s_clients.emplace_back(new jf::Client(server, s_device, "PlayStation 5", kind));
    return s_clients.back().get();
}

jf::Client *client_for(const accounts::Account &a)
{
    jf::Client *c = new_client(a.server, jf::server_kind_from_id(a.server_type));
    c->set_session(a.token, a.user_id, a.user_name);
    c->note_image_tag(a.image_tag);
    return c;
}

void set_phase(Phase p, const std::string &message = std::string())
{
    std::lock_guard<std::mutex> g(s_state.lock);
    s_state.phase = p;
    s_state.message = message;
}

double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void notify(const char *text)
{
    struct { char pad[45]; char msg[3075]; } n;
    std::memset(&n, 0, sizeof n);
    std::snprintf(n.msg, sizeof n.msg, "%s", text);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
}

void av_log_to_boot_log(void *, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_WARNING)
        return;
    char line[512];
    int n = std::vsnprintf(line, sizeof line, fmt, vl);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line)
        n = (int)sizeof line - 1;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    /* Every JSON request over avio ends this way (no Content-Length): not news. */
    if (n && !std::strstr(line, "Stream ends prematurely"))
        evo_bt("ffmpeg[%d]: %s", level, line);
}

/* The app's start, as the linker lays it out (for the crash report's offsets).
 * Its code is the first segment, about 24 MB: offsets under 32 MB are code. */
extern "C" char __ehdr_start[] __attribute__((weak));
constexpr uintptr_t kCodeSpan = 32u << 20;

void crash_handler(int sig, siginfo_t *si, void *ctx)
{
    static volatile int s_in_crash = 0;
    if (s_in_crash++) {   /* a fault inside the report itself: let the first one through */
        signal(sig, SIG_DFL);
        return;
    }
    evo_bt("emby5: CRASH signal=%d addr=%p", sig, si ? si->si_addr : nullptr);
    /* Where: the faulting instruction and the code addresses on the stack, as
     * offsets into the app (llvm-symbolizer --obj=build/llvm-pie.elf 0x...). An
     * abort's own address is inside abort(); its callers are on the stack. Only
     * this thread's stack is read (its bounds from pthread), never past its top. */
    const uintptr_t lo = (uintptr_t)__ehdr_start, hi = lo + kCodeSpan;
    if (ctx && lo) {
        const ucontext_t *uc = (const ucontext_t *)ctx;
        const uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip;
        const uintptr_t rsp = (uintptr_t)uc->uc_mcontext.mc_rsp;
        uintptr_t top = 0;
        pthread_attr_t attr;
        void *base = nullptr;
        size_t size = 0;
        if (pthread_attr_init(&attr) == 0) {
            if (pthread_attr_get_np(pthread_self(), &attr) == 0 && pthread_attr_getstack(&attr, &base, &size) == 0)
                top = (uintptr_t)base + size;
            pthread_attr_destroy(&attr);
        }
        char line[512];
        int n = std::snprintf(line, sizeof line, "emby5: crash at %s%#lx; stack:", rip >= lo && rip < hi ? "+" : "",
                              (unsigned long)(rip >= lo && rip < hi ? rip - lo : rip));
        if (rsp && top > rsp) {
            const uintptr_t *sp = (const uintptr_t *)rsp;
            const size_t words = std::min<size_t>(1024, (top - rsp) / sizeof(uintptr_t));
            for (size_t i = 0, found = 0; i < words && found < 24 && n < (int)sizeof line - 16; i++)
                if (sp[i] >= lo && sp[i] < hi) {
                    n += std::snprintf(line + n, sizeof line - n, " +%#lx", (unsigned long)(sp[i] - lo));
                    found++;
                }
        } else {
            n += std::snprintf(line + n, sizeof line - n, " (bounds unknown)");
        }
        evo_bt("%s", line);
    }
    evo_boot_log_flush();
    signal(sig, SIG_DFL);   /* re-fault so the kernel writes its crash report */
}

void install_crash_handler()
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

int init_display()
{
    /*
     * The scanout used to be opened as SDR unconditionally, and that is exactly
     * what puts the console in SDR: registering SDR buffers is a request for
     * SDR output, so the home screen's HDR is dropped the moment Emby5 starts.
     * A later switch back is refused, which is why HDR never turned on no
     * matter what the disc carried.
     *
     * display.conf with hdr_init=1 opens in HDR10 from the first frame
     * instead. If the console refuses it the request file is removed and SDR
     * is used, so a bad request can never leave the app unable to start.
     */
    static int s_hdr_init = -1;
    if (s_hdr_init < 0) {
        s_hdr_init = 0;
        std::FILE *fp = std::fopen("/data/Emby5/display.conf", "r");
        if (fp) {
            char line[128];
            while (std::fgets(line, sizeof line, fp)) {
                int v = 0;
                if (std::sscanf(line, "hdr_init=%d", &v) == 1 && v > 0)
                    s_hdr_init = 1;
            }
            std::fclose(fp);
        }
        if (s_hdr_init)
            evo_boot_log("emby5: display.conf asks for an HDR10 bring-up");
    }

    if (s_hdr_init &&
        evo_agc_runtime_init(evo::DisplayWidth, evo::DisplayHeight, 1) == 0) {
        evo_boot_log("emby5: display initialised in HDR10");
        evo_agc_runtime_get_size(&evo::DisplayWidth, &evo::DisplayHeight);
        return 0;
    }
    if (s_hdr_init) {
        evo_boot_log("emby5: HDR10 bring-up refused - removing display.conf, using SDR");
        std::remove("/data/Emby5/display.conf");
        s_hdr_init = 0;
    }

    /* Only the native graphics runtime is needed before the first branded frame. */
    if (evo_agc_runtime_init(evo::DisplayWidth, evo::DisplayHeight, 0) != 0)
        return -1;
    evo_agc_runtime_get_size(&evo::DisplayWidth, &evo::DisplayHeight);
    return 0;
}

void init_services()
{
    /* These services are not required to present the splash artwork. */
    evo_vdec_probe();
    evo_adec_native_probe();
    evo_hw_probe();
    av_log_set_callback(av_log_to_boot_log);
    av_log_set_level(AV_LOG_WARNING);
    avformat_network_init();
    evo_direct_mem_init(kDirectMemPool);

    sceUserServiceInitialize(nullptr);
    scePadInit();
    int users[4] = {0};
    sceUserServiceGetLoginUserIdList(users);
    g_ps5_user_id = s_user = users[0];
    nuvio_player_init(users[0]);
}

/* ---- signing in --------------------------------------------------------------------- */
void request_gate(Gate kind, const std::string &server = std::string(), const std::string &user = std::string(),
                  bool can_cancel = false, bool known_server = false)
{
    std::lock_guard<std::mutex> g(s_state.lock);
    s_state.gate = {kind, server, user, can_cancel, known_server};
    s_state.phase = Phase::Gate;
}

/* The hero's titles are cached per user between launches: the server's random
 * pick takes over a second, so a start shows the last pick at once and fetches
 * the next one behind it. */
std::string hero_file(const jf::Client &c)
{
    // Keep cached home artwork scoped to server AND user, not merely a user ID.
    const std::string identity = std::string(c.server_type()) + "|" + c.server() + "|" + c.user_id();
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : identity) { hash ^= ch; hash *= 1099511628211ull; }
    char leaf[96];
    std::snprintf(leaf, sizeof leaf, "emby5/hero-%016llx.json", (unsigned long long)hash);
    return evo_data_path(leaf);
}

std::string read_file(const std::string &path)
{
    std::string out;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char buf[16384];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() < (4u << 20))
            out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

void write_file(const std::string &path, const std::string &data)
{
    evo_mkdir(evo_data_dir());
    evo_mkdir(evo_data_path("emby5"));
    const std::string tmp = path + ".tmp";
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
        const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        std::fclose(f);
        if (ok)
            std::rename(tmp.c_str(), path.c_str());
    }
}

void refresh_hero_cache(jf::Client &c, std::vector<jf::Item> *out)
{
    std::string raw;
    std::vector<jf::Item> fresh = c.featured(6, &raw);
    if (!fresh.empty())
        write_file(hero_file(c), raw);
    if (out)
        *out = std::move(fresh);
}

/* Loads the home rows, all requests in parallel. With keep_hero the featured
 * titles are kept (a refresh after playback only needs the rows). */
bool draw_connection(double now);   /* below: the note when the server is out of reach */

/* "00.001.001" or "v0.1.1" as one comparable number (major, minor, patch). */
long version_number(const std::string &v)
{
    int a = 0, b = 0, c = 0;
    std::sscanf(v.c_str() + (v[0] == 'v' ? 1 : 0), "%d.%d.%d", &a, &b, &c);
    return a * 1000000L + b * 1000L + c;
}

/* Once per launch, check GitHub for a newer release and show a notice only.
 * Never download or install updates. */
void check_for_update()
{
    static bool asked = false;
    if (asked)
        return;
    asked = true;
    std::thread([] {
        const jf::HttpResponse r =
            jf::http_request("GET", "https://api.github.com/repos/bornaradusin/Emby5/releases/latest",
                             {"Accept: application/vnd.github+json", "User-Agent: Emby5/" EMBY5_VERSION}, "", 10);
        if (!r.ok())
            return;
        cJSON *j = cJSON_Parse(r.body.c_str());
        const cJSON *tag = cJSON_GetObjectItemCaseSensitive(j, "tag_name");
        const std::string latest = cJSON_IsString(tag) && tag->valuestring ? tag->valuestring : "";
        cJSON_Delete(j);
        evo_bt("emby5: latest release %s, this is %s", latest.c_str(), EMBY5_VERSION);
        if (!latest.empty() && version_number(latest) > version_number(EMBY5_VERSION)) {
            char msg[160];
            std::snprintf(msg, sizeof msg, T("Emby5 %s er tilgjengelig – se GitHub"), latest.c_str());
            notify(msg);
        }
    }).detach();
}

void load_home(jf::Client &c, unsigned session, bool keep_hero = false)
{
    // Coalesced by callers. All publications are guarded by account/request ID.
    static std::mutex load_lock;
    std::lock_guard<std::mutex> one_load(load_lock);
    unsigned request;
    {std::lock_guard<std::mutex> g(s_state.lock);if(session!=s_session)return;
        request=++s_home_request;
        if(s_state.phase!=Phase::Home){s_state.phase=Phase::Loading;s_state.message=T("Henter biblioteket \xE2\x80\xA6");}}
    using Row=ui::HomeRow;
    // Keep the last successful order on a transient preference-read failure.
    static unsigned order_session=0;
    static std::vector<std::string> last_sections;
    if(order_session!=session){order_session=session;last_sections.clear();}
    std::vector<std::string> sections=last_sections;
    try{bool ok=false;auto fetched=c.home_sections(&ok);if(ok){sections=fetched;last_sections=std::move(fetched);}}catch(...){}
    auto valid=[&]{return session==s_session&&request==s_home_request;};
    auto publish=[&](Row row,bool ok){
        std::lock_guard<std::mutex> g(s_state.lock);if(!valid())return;
        home_sections::merge(s_state.model,std::move(row),ok);
        home_sections::order(s_state.model,sections,s_state.views);
        ++s_model_version;
        // Even a successfully empty response lets the shell offer Search/Retry.
        if(ok||!s_state.model.hero.empty()||!s_state.model.rows.empty()){
            s_state.phase=Phase::Home;s_state.message.clear();}
    };
    std::vector<std::function<void()>> jobs;
    auto collection=[&](Row::Kind kind,const std::string &title,const std::function<std::vector<jf::Item>(bool*)> &read){
        Row row;row.kind=kind;row.title=title;row.plays=kind==Row::Resume||kind==Row::NextUp;bool ok=false;
        try{if(valid())row.items=read(&ok);}catch(...){}
        publish(std::move(row),ok);
    };
    jobs.push_back([&]{collection(Row::Resume,T("Fortsett \xC3\xA5 se"),[&](bool *ok){return c.resume(20,{},ok);});});
    jobs.push_back([&]{collection(Row::NextUp,T("Neste episode"),[&](bool *ok){return c.next_up(20,{},ok);});});
    jobs.push_back([&]{collection(Row::MyList,T("Min liste"),[&](bool *ok){return c.favorites(30,ok);});});
    jobs.push_back([&]{
        bool ok=false;std::vector<jf::Item> views;try{if(valid())views=c.views(&ok);}catch(...){}
        if(!ok){Row row;row.kind=Row::Libraries;row.title=T("Biblioteker");publish(std::move(row),false);return;}
        bool music=false;for(const auto &v:views)music=music||v.collection_type=="music";
        Row libraries;libraries.kind=Row::Libraries;libraries.title=T("Biblioteker");
        std::vector<jf::Item> videos;
        for(const auto &v:views){const auto &t=v.collection_type;
            if(t=="homevideos"||t=="musicvideos"||t=="boxsets"||t.empty()||(t=="playlists"&&!music))libraries.items.push_back(v);
            if((t=="movies"||t=="tvshows"||t=="homevideos"||t=="musicvideos"||t.empty())&&
               std::find(c.latest_excludes().begin(),c.latest_excludes().end(),v.id)==c.latest_excludes().end())videos.push_back(v);
        }
        {std::lock_guard<std::mutex> g(s_state.lock);if(!valid())return;s_state.views=views;
            auto &rows=s_state.model.rows;rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const Row &r){
                return r.kind==Row::Latest && !r.parent.empty() && std::none_of(videos.begin(),videos.end(),[&](const jf::Item &v){return v.id==r.parent;});}),rows.end());}
        publish(std::move(libraries),true);
        std::vector<std::function<void()>> latest;
        for(const auto &v:videos)latest.push_back([&,v]{Row row;row.kind=Row::Latest;row.title=T("Nylig lagt til i ")+v.name;row.parent=v.id;
            bool success=false;try{if(valid())row.items=c.latest(v.id,20,&success);}catch(...){}publish(std::move(row),success);});
        emby5::run_io_jobs(latest);
    });
    jobs.push_back([&]{
        if(keep_hero)return;
        std::vector<jf::Item> hero;
        try{hero=c.featured_from(read_file(hero_file(c)),6);if(hero.empty())refresh_hero_cache(c,&hero);
            else emby5::spawn_io([&c]{refresh_hero_cache(c,nullptr);});}catch(...){}
        std::lock_guard<std::mutex> g(s_state.lock);if(!valid()||hero.empty())return;
        s_state.model.hero=std::move(hero);++s_model_version;s_state.phase=Phase::Home;s_state.message.clear();
    });
    // Five independent sections: a stalled favorites or views call does not keep
    // already completed rows waiting behind a whole-page completion barrier.
    std::vector<pthread_t> threads;
    for(auto &job:jobs){pthread_t thread{};if(emby5::io_worker::start(&thread,job,{},false))threads.push_back(thread);else job();}
    for(auto thread:threads)pthread_join(thread,nullptr);
    if(keep_hero){
        std::set<std::string> ids;
        {std::lock_guard<std::mutex> g(s_state.lock);if(!valid())return;
            for(const auto &it:s_state.model.hero)if(!it.id.empty())ids.insert(it.id);
            for(const auto &row:s_state.model.rows)if(row.kind==Row::Recommended||row.kind==Row::Genre)
                for(const auto &it:row.items)if(!it.id.empty())ids.insert(it.id);}
        if(!ids.empty()){
            std::string filter="&Ids=";for(const auto &id:ids)filter+=(filter.size()>5?",":"")+id;
            auto fresh=c.library("","","SortName",false,0,(int)ids.size(),filter);
            if(fresh.ok){std::lock_guard<std::mutex> g(s_state.lock);if(!valid())return;
                for(const auto &it:fresh.items){for(auto &hero:s_state.model.hero)emby5::reconcile_user_data(hero,it);
                    for(auto &row:s_state.model.rows)for(auto &item:row.items)emby5::reconcile_user_data(item,it);}
                ++s_model_version;}
        }
    }
    std::lock_guard<std::mutex> g(s_state.lock);if(valid()&&s_state.phase!=Phase::Home){
        s_state.phase=Phase::Failed;s_state.message=T("Could not load the library. Check the connection and retry.");}
}

/* Emby's recommendations ("Fordi du så ...") and a few genres, under the
 * rows. The server takes seconds over recommendations, so the home screen
 * never waits for them: they are added when they arrive. Genres are picked
 * once per session (rows should not reshuffle while browsing). */
/* TMDB's genre names (what Emby's metadata carries), in the interface's language. */
std::string genre_title(const std::string &g)
{
    static const char *const known[] = {N_("Action"), N_("Adventure"), N_("Action & Adventure"), N_("Animation"), N_("Comedy"), N_("Crime"), N_("Documentary"), N_("Drama"), N_("Family"), N_("Fantasy"), N_("History"), N_("Horror"), N_("Kids"), N_("Music"), N_("Mystery"), N_("Reality"), N_("Romance"), N_("Science Fiction"), N_("Sci-Fi & Fantasy"), N_("Talk"), N_("Thriller"), N_("TV Movie"), N_("War"), N_("War & Politics"), N_("Western"), N_("Soap"), N_("News")};
    for (const char *k : known)
        if (g == k)
            return T(k);
    return g;
}

void load_extras(jf::Client &c, unsigned session)
{
    using Row = ui::HomeRow;
    static std::mutex s_lock;   /* sign-in and a return from playback can both ask */
    std::lock_guard<std::mutex> once(s_lock);
    if (session != s_session) return;
    const unsigned request = s_home_request;
    static std::vector<std::string> s_genres;
    static unsigned s_genres_for = ~0u;
    std::vector<jf::Client::Recommendation> recs;
    std::thread rt([&] { recs = c.recommendations(4, 16); });
    std::vector<Row> genre_rows;
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        for (const Row &r : s_state.model.rows)
            if (r.kind == Row::Genre)
                genre_rows.push_back(r);
    }
    static unsigned s_genres_lang = ~0u;
    if (s_genres_for != session || s_genres_lang != i18n::generation()) {   /* new session, or new language */
        s_genres_for = session;
        s_genres_lang = i18n::generation();
        s_genres = c.genres();
        std::srand((unsigned)time(nullptr));
        for (size_t i = s_genres.size(); i > 1; i--)
            std::swap(s_genres[i - 1], s_genres[std::rand() % i]);
        if (s_genres.size() > 4)
            s_genres.resize(4);
        genre_rows.clear();
        std::vector<std::vector<jf::Item>> items(s_genres.size());
        std::vector<std::thread> jobs;
        for (size_t i = 0; i < s_genres.size(); i++)
            jobs.emplace_back([&, i] { items[i] = c.genre_items(s_genres[i], 20); });
        for (auto &j : jobs)
            j.join();
        for (size_t i = 0; i < s_genres.size(); i++)
            if (items[i].size() >= 6)
                genre_rows.push_back({genre_title(s_genres[i]), std::move(items[i]), false, Row::Genre});
    }
    rt.join();

    std::vector<Row> rows;
    for (auto &r : recs) {
        if (r.items.size() < 4 || r.baseline.empty())
            continue;
        std::string title;
        if (r.type == "SimilarToRecentlyPlayed") title = T("Fordi du så ") + r.baseline;
        else if (r.type == "SimilarToLikedItem") title = T("Fordi du likte ") + r.baseline;
        else if (r.type.find("Director") != std::string::npos) title = T("Regissert av ") + r.baseline;
        else if (r.type.find("Actor") != std::string::npos) title = T("Med ") + r.baseline;
        else continue;
        rows.push_back({title, std::move(r.items), false, Row::Recommended});
    }
    /* A recommendation, a genre, a recommendation ...: variety down the page. */
    std::vector<Row> mixed;
    for (size_t i = 0; i < std::max(rows.size(), genre_rows.size()); i++) {
        if (i < rows.size()) mixed.push_back(std::move(rows[i]));
        if (i < genre_rows.size()) mixed.push_back(genre_rows[i]);
    }

    std::lock_guard<std::mutex> g(s_state.lock);
    if (session != s_session || request != s_home_request || s_state.phase != Phase::Home)
        return;
    ui::HomeModel m = s_state.model;
    std::vector<Row> base, libraries;
    for (Row &r : m.rows) {
        if (r.kind == Row::Libraries) libraries.push_back(std::move(r));
        else if (r.kind != Row::Recommended && r.kind != Row::Genre) base.push_back(std::move(r));
    }
    m.rows = std::move(base);
    for (Row &r : mixed) m.rows.push_back(std::move(r));
    for (Row &r : libraries) m.rows.push_back(std::move(r));
    s_state.model = std::move(m);
    s_model_version++;
}

/* Seerr's tab: what is trending, popular and coming, and the viewer's own
 * requests, all asked for side by side (off the main thread). */
/* Seerr's titles as the tab's items. Titles the library has are Emby's own
 * items (fetched in one request): their card art, backdrop and logo, and they
 * open the library's page; Seerr has one TMDB backdrop per title. */
std::vector<jf::Item> discover_items(const std::vector<seerr::Title> &list)
{
    auto id_key = [](std::string id) {
        id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
        for (char &ch : id)
            ch = (char)std::tolower((unsigned char)ch);
        return id;
    };
    std::map<std::string, jf::Item> owned;
    if (jf::Client *jc = s_client) {
        std::set<std::string> ids;
        for (const seerr::Title &t : list)
            if (!t.jellyfin_id.empty())
                ids.insert(id_key(t.jellyfin_id));
        if (!ids.empty()) {
            std::string filter = "&Ids=";
            for (const std::string &id : ids)
                filter += (filter.size() > 5 ? "," : "") + id;
            for (const jf::Item &it : jc->library("", "Movie,Series", "SortName", false, 0, (int)ids.size(), filter).items)
                owned[id_key(it.id)] = it;
        }
    }
    std::vector<jf::Item> out;
    for (const seerr::Title &t : list) {
        auto own = t.jellyfin_id.empty() ? owned.end() : owned.find(id_key(t.jellyfin_id));
        if (own != owned.end()) {   /* the library's item, still saying where it stands (available, partly) */
            jf::Item it = own->second;
            it.ext.status = (int)t.status;
            it.ext.tmdb_ref = t.id;   /* the status register and "Mine forespørsler" find it by this */
            out.push_back(std::move(it));
        } else {
            out.push_back(seerr_service::to_item(t));
        }
    }
    return out;
}

/* The next page of one of Seerr's lists on its tab, appended to its row. */
void load_more_discover(unsigned session, int shelf, int page)
{
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        if (session != s_session || s_state.discover_more_loading || now_s() < s_state.discover_more_retry_at)
            return;
        s_state.discover_more_loading = true;
    }
    const bool started = emby5::spawn([session, shelf, page] {
        using Shelf = seerr::Client::Shelf;
        const Shelf shelves[] = {Shelf::Trending, Shelf::PopularMovies, Shelf::PopularTv, Shelf::UpcomingMovies,
                                 Shelf::UpcomingTv};
        std::vector<jf::Item> items;
        int pages = 0;   /* Seerr's total: the end of the list (not what was hidden of a page) */
        bool ok = false;
        if (std::shared_ptr<seerr::Client> c = seerr_service::client()) {
            items = discover_items(seerr_service::visible(c->discover(shelves[shelf], page, &pages)));
            ok = c->last_status() >= 200 && c->last_status() < 300;
        }
        std::lock_guard<std::mutex> g(s_state.lock);
        s_state.discover_more_loading = false;
        if (session != s_session)
            return;
        for (ui::HomeRow &r : s_state.discover.rows) {
            if (r.shelf != shelf || r.page + 1 != page)
                continue;
            std::set<std::string> have;   /* a title can move up a page between two loads */
            for (const jf::Item &it : r.items)
                have.insert(it.id + "/" + std::to_string(it.ext.tmdb_id));
            if (!ok) {   /* failed: the row stays as it was; asked again in five seconds */
                s_state.discover_more_retry_at = now_s() + 5.0;
                break;
            }
            std::vector<jf::Item> added;
            for (jf::Item &it : items)
                if (have.insert(it.id + "/" + std::to_string(it.ext.tmdb_id)).second) {
                    r.items.push_back(it);
                    added.push_back(std::move(it));
                }
            r.page = page;
            r.more = page < pages;
            /* The screen takes only these (no copy of the whole model on the render
             * thread, where it was a hitch as the rows grew). */
            s_state.discover_appends.push_back({shelf, page, r.more, std::move(added)});
            s_discover_appended++;
        }
    });
    if (!started) {   /* no thread: try again on a later frame */
        std::lock_guard<std::mutex> g(s_state.lock);
        s_state.discover_more_loading = false;
    }
}

/* The viewer's own requests, newest first: a request names a title, its page
 * has the rest (looked up side by side, kept in order). state: "tv:123" -> the
 * newest request's state; status: the list's HTTP status (whether the session holds). */
std::vector<seerr::Title> load_mine(seerr::Client &c, std::map<std::string, int> *state, int *status)
{
    const int uid = seerr_service::snapshot().user.id;
    const std::vector<seerr::Request> reqs = c.requests(uid, 12);
    *status = c.last_status();
    std::vector<const seerr::Request *> unique;   /* a series asked for season by season: once */
    std::set<std::string> seen;
    for (const seerr::Request &rq : reqs) {
        const std::string key = (rq.tv ? "tv:" : "movie:") + std::to_string(rq.tmdb_id);
        if (seen.insert(key).second) {
            unique.push_back(&rq);
            (*state)[key] = (int)rq.status;   /* the newest request's (Seerr lists them newest first) */
        }
    }
    std::vector<seerr::Detail> found(unique.size());
    std::vector<std::function<void()>> jobs;
    for (size_t i = 0; i < unique.size(); i++)
        jobs.push_back([&, i] {
            const seerr::Request &rq = *unique[i];
            if (!(rq.tv ? c.tv(rq.tmdb_id, &found[i]) : c.movie(rq.tmdb_id, &found[i])))
                found[i].title.id = 0;
        });
    emby5::run_all(jobs);
    std::vector<seerr::Title> out;
    for (const seerr::Detail &d : found)
        if (d.title.id > 0)
            out.push_back(d.title);
    return out;
}

/* "Mine forespørsler" as a row of the tab, each card with its request's state. */
ui::HomeRow mine_row(const std::vector<seerr::Title> &mine, const std::map<std::string, int> &state)
{
    ui::HomeRow r;
    r.title = T("Mine forespørsler");
    r.kind = ui::HomeRow::Latest;
    r.shelf = -1;
    r.items = discover_items(mine);
    for (jf::Item &it : r.items) {
        const auto st = state.find((it.type == "Series" ? "tv:" : "movie:") +
                                   std::to_string(it.external() ? it.ext.tmdb_id : it.ext.tmdb_ref));
        if (st != state.end())
            it.ext.request = st->second;
    }
    return r;
}

/* After a request or a withdrawal: only "Mine forespørsler" is read again (the
 * other rows keep their pages; their chips follow the status register). */
void refresh_mine(unsigned session)
{
    if (!seerr_service::available())
        return;
    emby5::spawn([session] {
        std::shared_ptr<seerr::Client> c = seerr_service::client();
        if (!c)
            return;
        std::map<std::string, int> state;
        int status = 0;
        const std::vector<seerr::Title> mine = load_mine(*c, &state, &status);
        if (status == 401 || status == 403) {
            seerr_service::session_lost();
            return;
        }
        if (status < 200 || status >= 300)
            return;   /* the row stays as it was */
        ui::HomeRow row = mine_row(mine, state);
        std::lock_guard<std::mutex> g(s_state.lock);
        if (session != s_session || s_state.discover.rows.empty())
            return;   /* not loaded yet: the whole tab comes with it */
        std::vector<ui::HomeRow> &rows = s_state.discover.rows;
        auto at = std::find_if(rows.begin(), rows.end(), [](const ui::HomeRow &r) { return r.shelf == -1; });
        if (at != rows.end()) {
            if (mine.empty())
                rows.erase(at);
            else
                *at = std::move(row);
        } else if (!mine.empty()) {   /* after "Trender nå", as load_discover puts it */
            const bool trending = rows.front().shelf == 0;
            rows.insert(rows.begin() + (trending ? 1 : 0), std::move(row));
        }
        s_discover_version++;
    });
}

void load_discover(unsigned session)
{
    std::shared_ptr<seerr::Client> c = seerr_service::client();
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        if (session != s_session || s_state.discover_loading)
            return;
        s_state.discover_loading = true;
        s_state.discover_failed = false;
    }
    using Shelf = seerr::Client::Shelf;
    const Shelf shelves[] = {Shelf::Trending, Shelf::PopularMovies, Shelf::PopularTv, Shelf::UpcomingMovies,
                             Shelf::UpcomingTv};
    std::vector<seerr::Title> lists[5], mine;
    std::map<std::string, int> mine_state;   /* "tv:123" -> its request's state */
    if (c) {
        seerr_service::load_genres();
        std::atomic<bool> lost{false};   /* a 401/403 on any of them (each reads its own call's status) */
        auto note = [&] {
            const int st = c->last_status();
            if (st == 401 || st == 403)
                lost = true;
        };
        std::vector<std::function<void()>> jobs;
        for (int i = 0; i < 5; i++)
            jobs.push_back([&, i] {
                lists[i] = seerr_service::visible(c->discover(shelves[i]));
                note();
            });
        jobs.push_back([&] {
            int st = 0;
            mine = load_mine(*c, &mine_state, &st);
            if (st == 401 || st == 403)
                lost = true;
        });
        emby5::run_all(jobs);   /* side by side; a job without a thread runs here */
        if (lost)
            seerr_service::session_lost();
    }
    ui::HomeModel m;
    auto row = [&](const char *title, const std::vector<seerr::Title> &list, int shelf) {
        if (list.empty())
            return;
        ui::HomeRow r;
        r.title = title;
        r.kind = ui::HomeRow::Latest;
        r.shelf = shelf;
        r.items = discover_items(list);
        m.rows.push_back(std::move(r));
    };
    row(T("Trender nå"), lists[0], 0);
    if (!mine.empty())   /* second, as Seerr's own page (only when there are any) */
        m.rows.push_back(mine_row(mine, mine_state));
    row(T("Populære filmer"), lists[1], 1);
    row(T("Populære serier"), lists[2], 2);
    row(T("Kommende filmer"), lists[3], 3);
    row(T("Kommende serier"), lists[4], 4);
    bool again;
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        s_state.discover_loading = false;
        again = s_state.discover_again && session == s_session;
        s_state.discover_again = false;
        if (session != s_session)
            return;
        s_state.discover_failed = m.rows.empty();
        if (!m.rows.empty()) {   /* a failed reload keeps what was there, and is tried again */
            s_state.discover = std::move(m);
            s_state.discover_at = now_s();
        }
        s_discover_version++;   /* either way the tab redraws (its "Henter …" ends) */
    }
    if (again)   /* e.g. the language was cycled past another one while this loaded */
        load_discover(session);
}

/* Loads Seerr's tab when it is due: never loaded this session, or older than
 * max_age seconds (each time the tab opens), or forced (the language changed). */
void refresh_discover(double max_age, bool force = false)
{
    if (!seerr_service::available())
        return;
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        if (s_state.discover_loading) {
            s_state.discover_again = s_state.discover_again || force;   /* what loads now may be stale */
            return;
        }
        if (!force && s_state.discover_at >= 0 && now_s() - s_state.discover_at < max_age)
            return;
    }
    const unsigned session = s_session;
    emby5::spawn([session] { load_discover(session); });   /* no thread: the tab's next opening tries again */
}

/* Saves an account; when its server answered at a new address, its Seerr
 * settings move there with its accounts. */
void remember_account(const accounts::Account &a)
{
    for (const std::string &from : accounts::remember(a))
        seerr_service::move_server(from, a.server);
}

/* Signs in with a saved account (off the main thread): check the token, then
 * preferences, server info and the home rows. A rejected token opens the
 * sign-in on that server again (Quick Connect first); an unreachable server is
 * retried until the account changes. */
void use_account(jf::Client &c, unsigned session, accounts::Account a)
{
    set_phase(Phase::Connecting, T("Kobler til ") + (a.server_name.empty() ? a.server : a.server_name) + " \xE2\x80\xA6");
    while (session == s_session) {
        std::string name, version, id;
        // Public probe first: do not send a saved token to a replaced server.
        const bool found = c.public_info(&name, &version, &id);
        if (session != s_session) return;
        if (found && (a.server_type != c.server_type() ||
                      (!a.server_id.empty() && a.server_id != id))) {
            request_gate(Gate::Login, a.server, a.user_name, true, true);
            return;
        }
        if (found && c.validate()) {   /* (before the check below: it waits on the server) */
            if (session != s_session)
                return;   /* switched away meanwhile: leave "last account" and prefs alone */
            a.server_type = c.server_type();
            a.user_name = c.user_name();
            a.image_tag = c.user_image_tag();
            if (!name.empty())   /* renamed; an answer without them keeps what was saved */
                a.server_name = name;
            if (!id.empty())     /* saved before the Id was kept */
                a.server_id = id;
            remember_account(a);   /* (also the last used) */
            settings::load_server(c);
            c.check_subtitle_search();
            {
                std::lock_guard<std::mutex> g(s_state.lock);
                s_state.server_name = name;
                s_state.server_version = version;
            }
            evo_bt("emby5: signed in as %s on %s %s", c.user_name().c_str(), name.c_str(), version.c_str());
            if (session == s_session) update_service::automatic_check();
            load_home(c, session);
            if (session == s_session)
                seerr_service::attach(&c);   /* Seerr, when this account has it on */
            /* Controllable from Emby's apps ("Spill på PS5") while this session lasts. */
            remote::start(&c, [session] { return session == s_session; });
            syncplay::attach(&c);
            load_extras(c, session);
            return;
        }
        const std::string err = c.last_error();
        evo_bt("emby5: sign-in check failed: %s", err.c_str());
        if (err.find("-> 401") != std::string::npos || err.find("-> 403") != std::string::npos) {
            request_gate(Gate::Login, a.server, a.user_name, true, true);
            return;
        }
        set_phase(Phase::Failed, T("Får ikke kontakt med ") + (a.server_name.empty() ? a.server : a.server_name) +
                                     T(" \xE2\x80\x93 prøver igjen \xE2\x80\xA6"));
        for (int i = 0; i < 50 && session == s_session; i++)
            usleep(100 * 1000);
    }
}

/* The account to start with, read before the first screens are made. */
accounts::Account s_boot_account;
bool s_boot_has_account = false;
jf::Client *s_boot_client = nullptr;

void *boot(void *)
{
    if (s_boot_has_account)
        use_account(*s_boot_client, 0, s_boot_account);
    else if (!accounts::load().empty())
        request_gate(Gate::Profiles);
    else
        request_gate(Gate::Login, EMBY5_SERVER, "", false);
    return nullptr;
}

/* ---- screens --------------------------------------------------------------------- */
std::unique_ptr<ui::Home> s_home, s_discover;   /* s_discover: Seerr's tab */
std::unique_ptr<ui::Library> s_movies, s_shows, s_music;
std::unique_ptr<ui::IPTV> s_iptv;
std::unique_ptr<ui::Vod> s_vod;
std::unique_ptr<ui::SettingsScreen> s_settings;
std::unique_ptr<ui::Profiles> s_profiles;
std::unique_ptr<ui::Login> s_login;
Gate s_gate = Gate::None;
ui::Nav s_nav;
std::vector<std::unique_ptr<ui::Screen>> s_stack;   /* detail pages over the tab */
ui::Screen *s_now_page = nullptr;                   /* the now-playing page, while it is on the stack */
/* The card a page was opened from: the page grows out of it as it comes in. */
struct Origin {
    ui::Screen::Card card;
    const ui::Screen *page = nullptr;
};
Origin s_origin;
ui::Screensaver s_saver;
constexpr double kSaverAfter = 300.0;   /* 5 minutes without a button */

/* The screensaver's slides: every backdrop on the home screen, once. */
std::vector<ui::Screensaver::Slide> saver_slides()
{
    std::vector<ui::Screensaver::Slide> out;
    std::set<std::string> seen;
    std::lock_guard<std::mutex> g(s_state.lock);
    auto add = [&](const jf::Item &it) {
        const std::string url = s_client->image_url(it.backdrop_owner, "Backdrop", it.backdrop_tag, 1920);
        if (url.empty() || !seen.insert(url).second)
            return;
        const bool ep = it.type == "Episode";
        std::string line = it.year ? std::to_string(it.year) : std::string();
        if (!it.genres.empty())
            line += (line.empty() ? "" : "  \xC2\xB7  ") + it.genres[0];
        out.push_back({url, it.backdrop_blurhash, ep ? it.series_name : it.name, line});
    };
    for (const jf::Item &it : s_state.model.hero)
        add(it);
    for (const ui::HomeRow &r : s_state.model.rows)
        if (r.kind != ui::HomeRow::Libraries)
            for (const jf::Item &it : r.items)
                add(it);
    return out;
}
std::vector<jf::Item> s_queue;                      /* what the chosen Play hands over as a queue */
size_t s_queue_start = 0;
int s_tab = ui::Nav::Home;
bool s_nav_focus = false;
int s_nav_tab = ui::Nav::Home;     /* focused tab while s_nav_focus */

/* ---- music behind the menus ------------------------------------------------------------ */
std::atomic<bool> s_music_on{false};      /* a track (and its queue) plays headless */
bool s_group_play = false;              /* this Play came from the SyncPlay group: play it here */

/* Ends the music and waits for its thread to let go of the player. */
/* ---- theme music --------------------------------------------------------------
 * On a film's or series' page its theme song plays quietly in the background
 * (Innstillinger: Temamusikk), as Emby's own apps do; it stops when the page
 * closes or anything else plays. The player runs headless for it on its own
 * thread; s_theme_gen tells a starting theme whether it is still wanted. */
std::atomic<bool> s_theme_on{false};
std::atomic<unsigned> s_theme_gen{0};
std::string s_theme_for;   /* the page whose theme is wanted (main thread) */

void stop_theme(bool wait)
{
    s_theme_for.clear();
    s_theme_gen++;
    if (!s_theme_on)
        return;
    remote::Command c;
    c.kind = remote::Command::Stop;
    remote::send(c);
    for (int i = 0; wait && i < 300 && s_theme_on; i++)
        usleep(10 * 1000);
}

/* Each frame: the theme the open page wants, started or stopped to match. */
void theme_follow()
{
    std::string want;
    if (!s_stack.empty() && !s_music_on && settings::get().local.theme_music)
        if (const auto *d = dynamic_cast<const ui::Detail *>(s_stack.back().get())) {
            const std::string &t = d->item().type;
            if (t == "Movie" || t == "Series" || t == "Season" || t == "Episode")
                want = d->item().id;
        }
    if (want == s_theme_for)
        return;
    stop_theme(false);
    s_theme_for = want;
    if (want.empty())
        return;
    const unsigned gen = s_theme_gen;
    jf::Client *c = s_client;
    std::thread([c, want, gen] {
        for (int i = 0; i < 300 && s_theme_on; i++)   /* the last one winding down */
            usleep(10 * 1000);
        usleep(600 * 1000);   /* a page only passed through starts nothing */
        if (gen != s_theme_gen || s_theme_on)
            return;
        const std::vector<jf::Item> songs = c->theme_songs(want);
        if (songs.empty() || gen != s_theme_gen || s_music_on)
            return;
        s_theme_on = true;
        nuvio_player_set_headless(1);
        emby5_play_theme(*c, songs.front());
        nuvio_player_set_headless(0);
        s_theme_on = false;
    }).detach();
}

void stop_music()
{
    if (!s_music_on)
        return;
    remote::Command c;
    c.kind = remote::Command::Stop;
    remote::send(c);
    for (int i = 0; i < 300 && s_music_on; i++)
        usleep(10 * 1000);
    if (s_music_on)
        evo_bt("emby5: music did not stop in 3 s");
}

void open_now_playing()
{
    if (s_now_page && !s_stack.empty() && s_stack.back().get() == s_now_page)
        return;
    s_stack.emplace_back(new ui::NowPlaying());
    s_now_page = s_stack.back().get();
    s_now_page->activate();
}

/* Music plays on the player's thread without a picture; the menus stay up. */
void start_music(const jf::Item &item, bool shuffle, const std::vector<jf::Item> *queue, size_t start)
{
    stop_theme(true);
    stop_music();
    jf::Client *c = s_client;
    const std::vector<jf::Item> q = queue ? *queue : std::vector<jf::Item>();
    s_music_on = true;
    std::thread([c, item, shuffle, q, start] {
        nuvio_player_set_headless(1);
        std::string error;
        const bool ok = q.size() > 1 ? emby5_play_queue(*c, q, start, &error) : emby5_play(*c, item, &error, shuffle);
        nuvio_player_set_headless(0);
        if (!ok)
            notify((T("Emby5: kunne ikke spille av\n") + error).c_str());
        s_music_on = false;
    }).detach();
    open_now_playing();
}

/* Fresh screens for a new account (nothing of the last one's library survives). */
void reset_screens()
{
    s_stack.clear();
    s_now_page = nullptr;
    s_home.reset(new ui::Home(*s_client));
    s_discover.reset(new ui::Home(*s_client, true));
    s_movies.reset(new ui::Library(*s_client, T("Filmer"), "Movie"));
    s_shows.reset(new ui::Library(*s_client, T("Serier"), "Series"));
    s_music.reset(new ui::Library(*s_client, T("Musikk"), "MusicAlbum"));
    s_iptv.reset(new ui::IPTV(*s_client));
    s_vod.reset(new ui::Vod(s_client));
    iptv_vod::refresh();
    iptv_live::refresh(s_client,true);
    s_settings.reset(new ui::SettingsScreen(*s_client));
    s_tab = s_nav_tab = ui::Nav::Home;
    s_nav_focus = false;
    std::lock_guard<std::mutex> g(s_state.lock);
    s_state.model = ui::HomeModel();
    s_state.views.clear();
    s_home_version = ~0u;
    s_state.discover = ui::HomeModel();
    s_state.discover_at = -1;
    s_state.discover_failed = false;
    s_state.discover_again = false;
    /* The last account's loads may still be running: they find the session moved
     * and drop what they bring; the new one need not wait for them. */
    s_state.discover_loading = false;
    s_state.discover_more_loading = false;
    s_state.discover_more_retry_at = 0;
    s_state.discover_appends.clear();
    s_discover_taken = ~0u;
}

/* Opens the gate screen a worker asked for (main thread). */
void open_gate(const GateRequest &r)
{
    s_gate = r.kind;
    if (r.kind == Gate::Profiles) {
        if (!r.return_to || !s_profiles)
            s_profiles.reset(new ui::Profiles());
        s_profiles->activate();
    } else if (r.kind == Gate::Login) {
        /* The build's default server only before any account is saved: "+ Server" starts empty. */
        const std::string server = r.server.empty() && accounts::load().empty() ? std::string(EMBY5_SERVER) : r.server;
        s_login.reset(new ui::Login(*s_client, server, r.user, r.can_cancel, r.known_server));
        s_login->activate();
    }
}

void switch_to(const accounts::Account &a)
{
    stop_music();
    seerr_service::detach();
    if (syncplay::active())
        syncplay::leave();
    const unsigned session = ++s_session;
    jf::Client *c = client_for(a);
    s_client = c;
    reset_screens();
    s_gate = Gate::None;
    set_phase(Phase::Connecting, T("Kobler til \xE2\x80\xA6"));
    std::thread([c, session, a] { use_account(*c, session, a); }).detach();
}

void gate_input(uint32_t p)
{
    if (s_gate == Gate::Profiles && s_profiles) {
        s_profiles->input(p);
        ui::Profiles::Choice ch;
        if (s_profiles->take_choice(&ch)) {
            if (ch.add)   /* another user on that server */
                open_gate({Gate::Login, ch.account.server, "", true, true});
            else if (ch.add_server)
                open_gate({Gate::Login, "", "", true});
            else
                switch_to(ch.account);
        }
    } else if (s_gate == Gate::Login && s_login) {
        const ui::Action a = s_login->input(p);
        if (a.kind == ui::Action::Back) {
            GateRequest r{accounts::load().empty() ? Gate::Login : Gate::Profiles, s_client->server(), "", false};
            r.return_to = true;
            open_gate(r);
        }
    }
}

/* Called every frame while a gate is up: a finished sign-in moves on. */
void gate_poll()
{
    accounts::Account a;
    if (s_gate == Gate::Login && s_login && s_login->take_result(&a)) {
        remember_account(a);
        switch_to(a);
    }
}

ui::Screen *screen_for(int tab)
{
    if (!s_stack.empty())
        return s_stack.back().get();
    switch (tab) {
    case ui::Nav::Movies: return s_movies.get();
    case ui::Nav::Shows: return s_shows.get();
    case ui::Nav::Music: return s_music.get();
    case ui::Nav::Discover: return s_discover.get();
    case ui::Nav::IPTV: return s_iptv.get();
    case ui::Nav::VOD: return s_vod.get();
    case ui::Nav::Settings: return s_settings.get();
    default: return s_home.get();
    }
}

void open_tab(int tab)
{
    s_tab = tab;
    if (tab == ui::Nav::Settings) {
        std::lock_guard<std::mutex> g(s_state.lock);
        s_settings->set_server_info(s_state.server_name, s_state.server_version);
    }
    if (tab == ui::Nav::Discover)
        refresh_discover(300);   /* where titles stand moves: fresh after five minutes */
    screen_for(tab)->activate();
}

void refresh_user_data_screens()
{
    ui::Detail::invalidate_user_data();
    if (s_movies) s_movies->defer_user_data_refresh();
    if (s_shows) s_shows->defer_user_data_refresh();
    if (s_music) s_music->defer_user_data_refresh();
    for (auto &screen : s_stack) screen->defer_user_data_refresh();
    if (auto *visible = screen_for(s_tab)) visible->ensure_user_data_refresh();
}

bool poll_user_data_results()
{
    std::vector<std::shared_ptr<UserDataCompletion>> done;
    { std::lock_guard<std::mutex> g(s_user_data_lock); done.swap(s_user_data_done); }
    bool changed = false;
    for (const auto &completion : done) {
        const auto &d = *completion;
        s_user_data_busy = false;
        if (d.session != s_session) continue; // results belong to the account that sent them
        changed = true;
        if (d.result.refreshed) {
            const jf::Item &fresh = d.result.item;
            if (s_home) s_home->reconcile_user_data(fresh);
            if (s_movies) s_movies->reconcile_user_data(fresh);
            if (s_shows) s_shows->reconcile_user_data(fresh);
            if (s_music) s_music->reconcile_user_data(fresh);
            for (auto &screen : s_stack) screen->reconcile_user_data(fresh);
            std::lock_guard<std::mutex> g(s_state.lock);
            for (auto &it : s_state.model.hero) emby5::reconcile_user_data(it, fresh);
            for (auto &row : s_state.model.rows)
                for (auto &it : row.items) emby5::reconcile_user_data(it, fresh);
            ++s_model_version;
        }
        s_user_data_refresh_requested = true;
        if (!d.result.accepted && !(d.result.refreshed && d.result.matched))
            notify(T("The server did not confirm the change. Refreshing the server state."));
        else if (d.result.refreshed && !d.result.matched)
            notify(T("The server returned a different status. The server state will be shown."));
        // Successful writes are quiet, including empty 2xx acknowledgements.
    }
    return changed;
}

void play_provider(const ui::Action &a) {
        iptv_catalogue::PlaybackPriority catalogue_priority;
        if (a.iptv_url.empty()) return;
        if(!a.vod_scope.empty()&&a.vod_scope!=browse_state::provider_scope(*s_client))return;
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req,"url",a.iptv_url.c_str());
        cJSON_AddStringToObject(req,"title",a.iptv_title.c_str());
        cJSON_AddNumberToObject(req,"startPosition",a.vod_position);
        cJSON_AddStringToObject(req,"itemType","movie");
        cJSON_AddStringToObject(req,"playMethod","DirectPlay");
        cJSON *stream=cJSON_CreateObject();
        cJSON_AddStringToObject(stream,"addon","IPTV");
        cJSON_AddStringToObject(stream,"title",a.iptv_is_vod?"VOD":T("Live TV"));
        cJSON_AddItemToObject(req,"stream",stream);
        char *json=cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (json) {
            stop_theme(true);stop_music();
            browse_state::clear_playback();
            if(a.iptv_is_vod&&!a.vod_scope.empty()){
                browse_state::Progress progress;progress.source=a.vod_source;progress.id=a.vod_id;progress.parent=a.vod_parent;
                progress.episode=a.vod_episode;progress.title=a.iptv_title;
                browse_state::begin_playback(a.vod_scope,progress);
            }
            // Match the normal Emby video playback transition: the player
            // owns the controller while active, and the shell reopens it
            // when returning to IPTV. Without this the shell loses input.
            nuvio_input_close();
            emby5_playback_prepare();   /* Settings › Playback: sound and network buffer */
            nuvio_player_run(json);
            nuvio_player_leave();browse_state::clear_playback();
            nuvio_input_open(s_user);
            std::free(json);
        }

}

// Each switch is consumed only after the previous player has stopped and the
// Emby/Jellyfin reporter/encoding cleanup has returned. No concurrent decoders.
void play_live_tv() {
    iptv_catalogue::PlaybackPriority catalogue_priority;
    struct Finish {
        ~Finish(){iptv_navigation::end();iptv_live::set_playing_channel({});}
    } finish;
    if(!s_client)return;
    if(s_user_data_busy){notify(T("A server change is still pending. Try again when it finishes."));return;}
    if(syncplay::active()){notify(T("Leave SyncPlay before switching Live TV channels."));return;}
    stop_theme(true);stop_music();nuvio_player_set_headless(0);nuvio_input_close();
    try {
        do {
            iptv_navigation::Selection live;
            if(!iptv_navigation::current(&live))break;
            if(s_iptv)s_iptv->focus_channel(live.group_id,live.channel.id);
            iptv_live::set_playing_channel(live.channel.id);
            iptv_live::request_guide(s_client,live.channel);
            if(live.channel.source=="Emby" || live.channel.source=="Jellyfin") {
                if(live.channel.source!=s_client->server_product()){
                    notify(T("The server changed. Refresh Live TV before playing this channel."));break;
                }
                jf::Item item;item.id=live.channel.emby_id;item.name=live.channel.name;item.type="LiveTvChannel";
                std::string error;
                browse_state::clear_playback();
                const bool ok=emby5_play(*s_client,item,&error);
                if(!error.empty())notify(error.c_str());
                if(!ok)break;
            } else {
                std::string url;
                if(live.channel.source=="M3U")url=live.channel.extension;
                else if(live.channel.source=="Xtream") {
                    iptv_xtream::Credentials credentials;
                    if(!iptv_xtream::load_credentials(&credentials)){notify(T("Xtream credentials are unavailable."));break;}
                    auto channel=live.channel;
                    if(channel.id.rfind("xtream:",0)==0)channel.id.erase(0,7);
                    url=iptv_xtream::live_url(credentials,channel);
                }
                if(url.empty()){notify(T("This channel has no playable stream."));break;}
                cJSON *req=cJSON_CreateObject();
                cJSON_AddStringToObject(req,"url",url.c_str());
                cJSON_AddStringToObject(req,"title",live.channel.name.c_str());
                cJSON_AddStringToObject(req,"itemType","movie");
                cJSON_AddStringToObject(req,"playMethod","DirectPlay");
                cJSON_AddStringToObject(req,"liveChannelId",live.channel.id.c_str());
                cJSON_AddStringToObject(req,"liveGroupName",live.group_name.c_str());
                cJSON_AddStringToObject(req,"liveNavigationToken",live.token.c_str());
                cJSON *stream=cJSON_CreateObject();
                cJSON_AddStringToObject(stream,"addon","IPTV");
                cJSON_AddStringToObject(stream,"title","Live TV");cJSON_AddItemToObject(req,"stream",stream);
                char *json=cJSON_PrintUnformatted(req);cJSON_Delete(req);
                if(!json){notify(T("Unable to prepare Live TV playback."));break;}
                const std::string request(json);std::free(json);
                browse_state::clear_playback();
                emby5_playback_prepare();   /* Settings › Playback: sound and network buffer */
                nuvio_player_run(request.c_str());nuvio_player_leave();
            }
        } while(iptv_navigation::advance());
    } catch(...) {notify(T("Live TV playback could not continue. Returning to channels."));}
    nuvio_player_leave();nuvio_input_open(s_user);
}

class TrailerPage final : public ui::Screen {
    ui::TrailerQr qr;
public:
    explicit TrailerPage(const jf::Item &item){qr.open(ui::first_trailer_url(item.remote_trailers));}
    ui::Action input(uint32_t p)override{qr.input(p);ui::Action a;if(!qr.active())a.kind=ui::Action::Back;return a;}
    void draw(double,float)override{gfx::fill({0,0,gfx::W,gfx::H},ui::theme_bg());qr.draw();}
    bool animating()const override{return false;}
    float nav_alpha()const override{return 0;}
};
void open_search(const ui::SearchContext &context)
{
    if (!s_client || !context.valid() || s_stack.size() >= 8 || ime::active()) return;
    auto *client = s_client;
    auto *origin = screen_for(s_tab);
    const auto session = s_session.load();
    const int tab = s_tab;
    const bool from_nav = s_nav_focus;
    // Keep the original screen/focus untouched if the keyboard is cancelled.
    ime::request(ime::Kind::Search, TF("Search {section}", {{"section", context.title}}), "",
        [context, client, origin, session, tab, from_nav](const std::string &raw) {
            const auto query = ui::search_text(raw);
            if (query.empty() || session != s_session || client != s_client ||
                tab != s_tab || origin != screen_for(s_tab) || s_stack.size() >= 8) return;
            s_nav_focus = false;
            s_stack.emplace_back(new ui::Search(*client, context, query, from_nav));
            s_stack.back()->activate();
        });
}

/* Top-level input once signed in: the tab bar, or the active screen. */
void shell_input(uint32_t p, jf::Item *play, bool *chose, bool *from_start, bool *shuffle)
{
    if ((p & NUVIO_BTN_TRIANGLE) && s_stack.empty() && !screen_for(s_tab)->modal()) {
        // Each screen owns its search. Home returns None; Live/VOD filter locally.
        const auto request = screen_for(s_tab)->search(s_nav_focus);
        if (request.kind == ui::Action::Search) open_search(request.search_context);
        return;
    }
    if ((p & NUVIO_BTN_TOUCHPAD) && s_music_on) {   /* the mini player: the now-playing page */
        s_nav_focus = false;
        open_now_playing();
        return;
    }
    /* L1/R1: the previous / next tab, as across the PS5's own menus. */
    if ((p & (NUVIO_BTN_L1 | NUVIO_BTN_R1)) && s_stack.empty() && !screen_for(s_tab)->modal()) {
        std::vector<int> order = s_nav.tabs();
        order.push_back(ui::Nav::Settings);
        const int at = (int)(std::find(order.begin(), order.end(), s_tab) - order.begin());
        const int to = at + ((p & NUVIO_BTN_R1) ? 1 : -1);
        if (at < (int)order.size() && to >= 0 && to < (int)order.size()) {
            s_nav_tab = order[to];
            open_tab(order[to]);
            if (!s_nav_focus)
                screen_for(order[to])->enter_from_top();
        } else {
            nuvio_input_pulse(70, 45);   /* the end: a soft bump */
        }
        return;
    }
    if (s_nav_focus && s_stack.empty()) {
        const int before = s_nav_tab;
        std::vector<int> order = s_nav.tabs();
        order.push_back(ui::Nav::Settings);   /* the avatar, last */
        const int at = (int)(std::find(order.begin(), order.end(), s_nav_tab) - order.begin());
        if ((p & NUVIO_BTN_LEFT) && at > 0 && at < (int)order.size())
            s_nav_tab = order[at - 1];
        else if ((p & NUVIO_BTN_RIGHT) && at + 1 < (int)order.size())
            s_nav_tab = order[at + 1];
        else if (p & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS)) {
            s_nav_focus = false;
            screen_for(s_tab)->enter_from_top();
        }
        else if ((p & NUVIO_BTN_CIRCLE) && s_tab != ui::Nav::Home)
            s_nav_tab = ui::Nav::Home;   /* back from any tab goes home, as on Netflix */
        if (s_nav_tab != before && s_nav_tab != s_tab)
            open_tab(s_nav_tab);         /* tvOS-style: focusing a tab opens it */
        return;
    }
    if (s_tab == ui::Nav::Discover && s_stack.empty() && s_discover->empty() && (p & NUVIO_BTN_CROSS)) {
        if (seerr_service::snapshot().state == seerr_service::State::Ready)
            refresh_discover(0, true);   /* the empty tab: ✕ asks Seerr again */
        else
            seerr_service::reconnect();  /* not signed in, or not answering: connect again (the rows follow) */
        return;
    }
    ui::Screen *in_screen = screen_for(s_tab);
    const ui::Action a = in_screen->input(p);
    if (in_screen->take_bump())
        nuvio_input_pulse(70, 45);   /* a soft bump at the edge */
    if (a.kind == ui::Action::Play || a.kind == ui::Action::PlayFromStart || a.kind == ui::Action::PlayShuffled)
        nuvio_input_pulse(150, 70);  /* a firmer one to start */
    switch (a.kind) {
    case ui::Action::Search: open_search(a.search_context); break;
    case ui::Action::RetryHome:s_user_data_refresh_requested=true;break;
    case ui::Action::ViewCollection:
        if(a.collection>=0&&a.collection<=jf::Client::LatestCollection&&s_stack.size()<8){
            s_stack.emplace_back(new ui::Library(*s_client,a.collection_title,"Movie,Series,Episode,Video",a.collection_parent,true,"",a.collection));
            s_stack.back()->activate();s_nav_focus=false;
        }break;
    case ui::Action::Trailer:
        if(s_stack.size()<8){s_stack.emplace_back(new TrailerPage(a.item));s_nav_focus=false;}break;
    case ui::Action::OpenVOD: {
        if(s_stack.size()>=8)break;
        iptv_vod::Item item;item.source=a.vod_source;item.id=a.vod_episode?a.vod_parent:a.vod_id;
        item.name=a.iptv_title;item.series=a.vod_series;
        s_stack.emplace_back(new ui::VodDetail(*s_client,item,a.vod_episode?a.vod_id:std::string()));
        s_stack.back()->activate();s_nav_focus=false;break;
    }
    case ui::Action::ToNav:
        if (s_stack.empty()) {
            s_nav_focus = true;
            s_nav_tab = s_tab;
        }
        break;
    case ui::Action::Play:
    case ui::Action::PlayFromStart:
    case ui::Action::PlayShuffled:
        *play = a.item;
        *chose = true;
        *from_start = a.kind == ui::Action::PlayFromStart;
        *shuffle = a.kind == ui::Action::PlayShuffled;
        s_queue = a.queue;
        s_queue_start = a.queue_start;
        break;
    case ui::Action::PlayLiveTV:
        play_live_tv();
        break;
    case ui::Action::PlayIPTV:
        play_provider(a);
        break;
    case ui::Action::PlayMix: {   /* Emby's Instant Mix from an album (or song, artist) */
        std::vector<jf::Item> mix = s_client->instant_mix(a.item.id, 60);
        if (mix.empty()) {
            notify(T("Emby5: Emby fant ingen miks her"));
            break;
        }
        *play = mix.front();
        *chose = true;
        s_queue = std::move(mix);
        s_queue_start = 0;
        break;
    }
    case ui::Action::Open: {
        /* A Seerr title opens Seerr's page when the server does not have it, or
         * only some of it (the rest can be requested there); else the server's. */
        const bool seerr_page = a.item.external() && (a.item.ext.jellyfin_id.empty() ||
                                                      a.item.ext.status == (int)seerr::Status::PartiallyAvailable);
        ui::Screen::Card from;
        const bool have_from = screen_for(s_tab)->focused_card(&from);
        if (s_stack.size() >= 8)
            s_stack.erase(s_stack.begin());   /* "more like this" chains stay bounded */
        /* Seasons and episodes open their series' page. */
        const std::string requested_season=a.item.type=="Season"?a.item.id:std::string();
        jf::Item target = a.item;
        if (target.external() && !seerr_page) {   /* a Seerr title the server has: the server's own page */
            jf::Item own;
            own.id = target.ext.jellyfin_id;
            own.type = target.type;
            own.name = target.name;
            target = own;
        }
        if ((target.type == "Season" || target.type == "Episode") && !target.series_id.empty()) {
            target = jf::Item();
            target.id = a.item.series_id;
            target.type = "Series";
            target.name = a.item.series_name;
            /* An episode carries its series' backdrop and logo: shown at once. */
            target.backdrop_owner = a.item.backdrop_owner;
            target.backdrop_tag = a.item.backdrop_tag;
            target.backdrop_blurhash = a.item.backdrop_blurhash;
            target.logo_owner = a.item.logo_owner;
            target.logo_tag = a.item.logo_tag;
        }
        if (seerr_page)
            s_stack.emplace_back(new ui::SeerrDetail(*s_client, a.item));
        else if (target.type == "SyncPlay")
            s_stack.emplace_back(new ui::SyncPlayScreen(s_client->user_name()));
        else if (target.type == "Person")
            s_stack.emplace_back(new ui::Person(*s_client, target));
        else if (target.type == "MusicAlbum" || target.type == "Playlist")
            s_stack.emplace_back(new ui::Album(*s_client, target));
        else if (target.type == "MusicArtist")   /* an artist: their albums */
            s_stack.emplace_back(new ui::Library(*s_client, target.name, "MusicAlbum", "", true,
                                                 "&AlbumArtistIds=" + target.id));
        else if (target.type == "CollectionFolder" || target.type == "UserView")
            s_stack.emplace_back(new ui::Library(*s_client, target.name,
                                                 ui::Library::types_for(target.collection_type), target.id, true));
        else {
            auto detail=new ui::Detail(*s_client,target);
            if(!requested_season.empty())detail->initial_season(requested_season);
            s_stack.emplace_back(detail);
        }
        s_stack.back()->activate();
        s_origin.page = have_from ? s_stack.back().get() : nullptr;
        if (have_from)
            s_origin.card = from;
        break;
    }
    case ui::Action::Back:
        if (!s_stack.empty()) {
            if (s_stack.back().get() == s_now_page)
                s_now_page = nullptr;
            const auto *search = dynamic_cast<const ui::Search *>(s_stack.back().get());
            const bool restore_nav = search && search->return_to_navigation();
            s_stack.pop_back();
            if (s_stack.empty() && restore_nav) s_nav_focus = true;
        }
        break;
    case ui::Action::Changed: {
        if (s_user_data_busy.exchange(true)) {
            notify(T("A server change is still pending. Please try again when it finishes."));
            break;
        }
        jf::Client *c = s_client; // session clients are immutable and retained by s_clients
        const unsigned session = s_session;
        const ui::UserDataChange ch = a.change;
        ++s_home_request; // an older in-flight Home snapshot cannot overwrite this change
        // Keep the result on the heap and publish it immediately after the
        // server acknowledgement. Do not keep a mutation thread alive through a full
        // Home/Next Up rebuild or touch any UI object from that worker.
        auto completion = std::make_shared<UserDataCompletion>();
        completion->session = session;
        auto publish = [completion] {
            std::lock_guard<std::mutex> g(s_user_data_lock);
            s_user_data_done.push_back(completion);
        };
        if (!emby5::spawn_io([c, session, ch, completion, publish] {
            if (session == s_session) {
                emby5::UserDataRequest request;
                request.id = ch.id;
                request.favorite_set = ch.favorite_set; request.favorite = ch.favorite;
                request.played_set = ch.played_set; request.played = ch.played;
                request.resume_cleared = ch.resume_cleared;
                evo_bt("userdata: begin %s kind=%s", request.id.c_str(),
                       request.favorite_set ? "favorite" : request.played_set ? "played" : "resume");
                completion->result = emby5::update_user_data(*c, request);
                evo_bt("userdata: finished accepted=%d refreshed=%d matched=%d",
                       completion->result.accepted, completion->result.refreshed, completion->result.matched);
            }
            publish();
        }, publish)) {
            s_user_data_busy = false;
            notify(T("Could not send the server change. No change was sent."));
        }

        break;
    }
    case ui::Action::SwitchUser:
        stop_music();
        seerr_service::detach();
        s_session++;
        open_gate({Gate::Profiles});
        set_phase(Phase::Gate);
        break;
    case ui::Action::SignOut: {
        stop_music();
        seerr_service::detach();
        accounts::forget(s_client->server(), s_client->user_id());
        s_session++;
        s_client = new_client(s_client->server());   /* signed out; screens are remade on the next sign-in */
        const bool others = !accounts::load().empty();
        open_gate({others ? Gate::Profiles : Gate::Login, s_client->server(), "", false});
        set_phase(Phase::Gate);
        break;
    }
    default:
        break;
    }
}

/* The tabs and their pills from the user's libraries (in the order they keep in
 * Emby): Filmer, Serier and Musikk show when there is such a library; a tab
 * with several offers them as pills (Serier · Anime). Music: Album · Artister ·
 * Spillelister. */
void apply_views(const std::vector<jf::Item> &views)
{
    using Source = ui::Library::Source;
    std::vector<Source> movies, shows, music;
    std::string playlists;
    std::vector<const jf::Item *> music_libs;
    for (const jf::Item &v : views) {
        if (v.collection_type == "movies") movies.push_back({v.name, v.id, "Movie", ""});
        if (v.collection_type == "tvshows") shows.push_back({v.name, v.id, "Series", ""});
        if (v.collection_type == "music") music_libs.push_back(&v);
        if (v.collection_type == "playlists") playlists = v.id;
    }
    for (const jf::Item *v : music_libs)
        music.push_back({music_libs.size() > 1 ? v->name : std::string(T("Album")), v->id, "MusicAlbum", ""});
    if (!music_libs.empty()) {
        music.push_back({T("Artister"), music_libs.size() == 1 ? music_libs[0]->id : "", "MusicArtist", ""});
        if (!playlists.empty())
            music.push_back({T("Spillelister"), playlists, "Playlist", ""});
    }
    std::vector<int> tabs{ui::Nav::Home};
    if (!movies.empty()) { tabs.push_back(ui::Nav::Movies); s_movies->set_sources(movies); }
    if (!shows.empty()) { tabs.push_back(ui::Nav::Shows); s_shows->set_sources(shows); }
    const auto live_status = iptv_live::summary();
    if (live_status.emby_count + live_status.xtream_count + live_status.m3u_count) tabs.push_back(ui::Nav::IPTV);
    { const auto vod=iptv_vod::snapshot(); if (vod.movie_count || vod.show_count) tabs.push_back(ui::Nav::VOD); }
    if (!music.empty()) { tabs.push_back(ui::Nav::Music); s_music->set_sources(music); }
    if (seerr_service::available()) tabs.push_back(ui::Nav::Discover);
    s_nav.set_tabs(tabs);
    /* A tab that went away (another account, a library removed): back home. */
    auto shown = [&](int t) { return t == ui::Nav::Settings || std::find(tabs.begin(), tabs.end(), t) != tabs.end(); };
    if (!shown(s_tab))
        s_tab = ui::Nav::Home;
    if (!shown(s_nav_tab))
        s_nav_tab = s_tab;
}

/* ---- drawing (GPU, src/gfx) ------------------------------------------------------ */
/* Native startup artwork: fixed illustration + one GPU-rendered glass progress bar.
 * Progress reflects initialization phases; it never uses an artificial timer.
 * Online VOD/Live TV refreshes continue after Home becomes usable. */
const gfx::Texture *startup_art()
{
    static gfx::Texture *image = nullptr;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        ui_image decoded{};
        if (ui_image_decode(kSplashArt, sizeof kSplashArt, 1672, 941, &decoded) == 0) {
            image = gfx::texture_from_image(&decoded);
            ui_image_free(&decoded);
        }
    }
    return image;
}

void draw_launch(double t, float a, const std::string &title, const std::string &line, bool busy,
                 const std::string &hint = "", float progress = 0.2f)
{
    (void)t; (void)line; (void)busy;
    if (a <= 0.f) return;
    gfx::push_opacity(a);
    if (const gfx::Texture *art = startup_art())
        gfx::image({0, 0, gfx::W, gfx::H}, art, 1.f, 0, true);
    else
        gfx::fill({0, 0, gfx::W, gfx::H}, 0xff061225u);

    /* The original graphic has no loading bar. This is the sole glass bar. */
    const gfx::Rect track{gfx::W * (458.f / 1672.f), gfx::H * (595.f / 941.f),
                          gfx::W * (734.f / 1672.f), gfx::H * (39.f / 941.f)};
    gfx::fill(track, 0xbd08253du, 19.f);
    gfx::fill({track.x + 3, track.y + 3, track.w - 6, track.h - 6}, 0x55456d82u, 16.f);
    const float p = std::max(0.f, std::min(1.f, progress));
    if (p > 0.f) {
        const gfx::Rect inner{track.x + 6, track.y + 6, (track.w - 12) * p, track.h - 12};
        gfx::fill_hgradient(inner, 0xff0bd54cu, 0xff53ff7eu, 13.f);
        gfx::fill({inner.x + 2, inner.y + 2, std::max(0.f, inner.w - 4), 3}, 0x99e6ffdfu, 2.f);
    }
    gfx::rim(track, 19.f, .65f);
    if (!hint.empty())
        ui::draw_pad_hints(gfx::W / 2, 992, {{ui::PadButton::Circle, hint}}, 1);
    (void)title;  /* Design's loading caption is already part of the static artwork. */
    gfx::pop_opacity();
}

ui::Anim s_splash;                 /* 1 while starting, fades to 0 over the home screen */

/* Returns true when it drew something that will keep changing. */
bool draw_frame(double t, float dt)
{
    Phase phase;
    std::string message;
    int more_shelf = 0, more_page = 0;
    bool more = false;
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        phase = s_state.phase;
        message = s_state.message;
        if (phase == Phase::Home && s_model_version != s_home_version) {
            s_home_version = s_model_version;
            s_home->set_model(s_state.model);
            apply_views(s_state.views);
            const std::string &tag = s_client->user_image_tag();
            s_nav.set_user(s_client->user_name(),
                           s_client->user_image_url(s_client->user_id(), tag, 440));
        }
        if (s_discover_version != s_discover_taken) {
            s_discover_taken = s_discover_version;
            s_discover->set_model(s_state.discover);
            s_state.discover_appends.clear();   /* the whole model has them already */
        } else if (!s_state.discover_appends.empty()) {
            for (const auto &ap : s_state.discover_appends)
                s_discover->append(ap.shelf, ap.page, ap.more, ap.items);
            s_state.discover_appends.clear();
        }
        more = s_tab == ui::Nav::Discover && s_stack.empty() && s_discover->wants_more(&more_shelf, &more_page);
    }
    /* A request or a withdrawal here: "Mine forespørsler" and the rows read Seerr again. */
    static unsigned seen_seerr_changes = 0;
    if (seerr_service::changes() != seen_seerr_changes) {
        seen_seerr_changes = seerr_service::changes();
        refresh_mine(s_session);
    }
    /* Near the end of one of Seerr's rows: its next page. Outside the lock, which
     * load_more_discover takes itself (taking it twice aborted the app). */
    if (more)
        load_more_discover(s_session, more_shelf, more_page);
    if (phase == Phase::Home)
        if (auto *visible = screen_for(s_tab)) visible->ensure_user_data_refresh();
    art::tick();
    gfx::begin_frame();
    bool animating = true;
    if (s_saver.on() && phase == Phase::Home) {
        s_saver.draw(t);
        gfx::end_frame();
        return true;
    }
    switch (phase) {
    case Phase::Connecting:
    case Phase::Loading:
        s_splash.snap(1.f);
        draw_launch(t, 1.f, message, "", true, "", phase == Phase::Loading ? .72f : .24f);
        break;
    case Phase::Failed:
        draw_launch(t, 1.f, message, "", true, T("Bytt bruker eller server"), .72f);   /* (it tries again) */
        break;
    case Phase::Gate:
        s_splash.snap(0.f);
        if (s_gate == Gate::Profiles && s_profiles) {
            s_profiles->draw(t, dt);
            animating = s_profiles->animating();
        } else if (s_gate == Gate::Login && s_login) {
            s_login->draw(t, dt);
        }
        break;
    case Phase::Home:
        if (s_tab == ui::Nav::Home && s_stack.empty() && s_home->empty()) {
            draw_launch(t, 1.f, T("Ingenting å vise ennå"), T("Legg til filmer eller serier i Emby."), false);
        } else {
            ui::Screen *scr = screen_for(s_tab);
            const float enter = scr->enter();
            if (enter < 1.f) {
                /* A page fading in: the screen below stays under it until it is opaque. */
                ui::Screen *below = s_stack.size() >= 2 ? s_stack[s_stack.size() - 2].get()
                                                        : (s_tab == ui::Nav::Movies   ? (ui::Screen *)s_movies.get()
                                                           : s_tab == ui::Nav::Shows  ? (ui::Screen *)s_shows.get()
                                                           : s_tab == ui::Nav::Music  ? (ui::Screen *)s_music.get()
                                                           : s_tab == ui::Nav::Discover ? (ui::Screen *)s_discover.get()
                                                           : s_tab == ui::Nav::IPTV ? (ui::Screen *)s_iptv.get()
                                                           : s_tab == ui::Nav::VOD ? (ui::Screen *)s_vod.get()
                                                                                      : (ui::Screen *)s_home.get());
                below->draw(t, dt);
                /* The card it was opened from lifts a little toward the viewer and fades as
                 * the page comes in: a hint of where the page came from, easy on the eye. */
                if (s_origin.page == scr) {
                    const ui::Screen::Card &c = s_origin.card;
                    const float e = ui::smoothstep(std::min(1.f, enter * 1.4f));
                    const float k = 1.f + 0.12f * e;
                    const gfx::Rect r{c.rect.x - c.rect.w * (k - 1) / 2, c.rect.y - c.rect.h * (k - 1) / 2, c.rect.w * k,
                                      c.rect.h * k};
                    const float fade = 1.f - e;
                    if (const gfx::Texture *tex = art::get(c.url, 640, 720))
                        gfx::image(r, tex, fade, c.radius * k, true);
                    else if (const gfx::Texture *bh = art::blurhash(c.blurhash))
                        gfx::image(r, bh, fade, c.radius * k, true);
                }
                gfx::push_opacity(enter);
            }
            scr->set_focused(!s_nav_focus || !s_stack.empty());
            scr->draw(t, dt);
            bool discover_wait = false;
            if (s_tab == ui::Nav::Discover && s_stack.empty() && s_discover->empty()) {
                /* Seerr's tab before its rows have come (or when they could not). */
                bool failed;
                {
                    std::lock_guard<std::mutex> g(s_state.lock);
                    failed = s_state.discover_failed && !s_state.discover_loading;
                }
                /* Seerr answered with nothing, or did not answer: either way ✕ asks again. */
                const seerr_service::State st = seerr_service::snapshot().state;
                const bool ready = st == seerr_service::State::Ready;
                const bool fetching = st == seerr_service::State::Connecting || (ready && !failed);
                const std::string text = fetching ? T("Henter \xE2\x80\xA6")
                                         : ready ? T("Ingenting å vise akkurat nå \xE2\x80\x93 \xE2\x9C\x95 for å prøve igjen")
                                         : st == seerr_service::State::SignedOut
                                             ? T("Ikke pålogget Seerr \xE2\x80\x93 \xE2\x9C\x95 for å logge på igjen")
                                             : T("Seerr svarer ikke \xE2\x80\x93 \xE2\x9C\x95 for å prøve igjen");
                gfx::text(gfx::W / 2, gfx::H / 2, text, {gfx::SemiBold, 34}, fetching ? ui::theme_text3() : ui::theme_text2(), 1);
                discover_wait = fetching;
            }
            if (enter < 1.f)
                gfx::pop_opacity();
            animating = scr->animating() || discover_wait;   /* "Henter …" until the rows come */
            const float nav = !s_stack.empty() ? 0.f : s_nav_focus ? 1.f : scr->nav_alpha();
            s_nav.draw(nav, s_tab, s_nav_focus ? s_nav_tab : -1, dt, &animating);
            /* The music page closes when the music ends; elsewhere the mini player shows it. */
            if (s_now_page && !s_music_on && !s_stack.empty() && s_stack.back().get() == s_now_page) {
                s_stack.pop_back();
                s_now_page = nullptr;
            }
            if (s_music_on && (s_stack.empty() || s_stack.back().get() != s_now_page)) {
                ui::draw_mini_player(t, 1.f);
                animating = true;
            }
            if (draw_connection(t))
                animating = true;
            theme_follow();
        }
        /* The splash fades away over the first frames of the home screen. */
        s_splash.to(0.f);
        if (s_splash.step(dt, 6.f))
            animating = true;
        draw_launch(t, s_splash.value, "", "", true, "", 1.f);   /* (the line fades with it) */
        break;
    }
    gfx::end_frame();
    return animating;
}

/* The server out of reach: requests get no answer at all (jf::unreachable_streak).
 * A note at the top says so while the app asks the server every 4 s; when it
 * answers, the note says so briefly and the home rows reload. Returns true while
 * it shows (frames are wanted). */
bool draw_connection(double now)
{
    static bool s_down = false, s_pinging = false;
    static double s_last_ping = 0, s_back_at = -10;
    static std::mutex s_ping_lock;
    const bool down = jf::unreachable_streak() >= 2;
    if (down) {
        s_down = true;
        std::lock_guard<std::mutex> g(s_ping_lock);
        if (!s_pinging && now - s_last_ping > 4.0) {
            s_pinging = true;
            s_last_ping = now;
            jf::Client *c = s_client;
            std::thread([c] {
                c->ping();
                std::lock_guard<std::mutex> g2(s_ping_lock);
                s_pinging = false;
            }).detach();
        }
    } else if (s_down) {
        s_down = false;
        s_back_at = now;
        jf::Client *c = s_client;
        const unsigned session = s_session;
        std::thread([c, session] { load_home(*c, session, true); }).detach();
    }
    const bool back = now - s_back_at < 2.5;
    if (!s_down && !back)
        return false;
    const std::string text = s_down ? T("Ingen kontakt med Emby-serveren \xE2\x80\x93 pr\xC3\xB8ver igjen \xE2\x80\xA6")
                                    : T("Tilkoblet igjen");
    const gfx::TextStyle ts{gfx::SemiBold, 24};
    const float w = gfx::text_width(text, ts) + 72;
    const gfx::Rect r{gfx::W / 2 - w / 2, 136, w, 60};
    ui::glass_panel(r, 30, 1.f, true);
    gfx::fill({r.x + 26, r.y + 25, 10, 10}, s_down ? 0xffff9f0au : 0xff30d158u, 5);   /* amber: away, green: back */
    gfx::text(r.x + 48, r.y + 39, text, ts, ui::theme_text());
    return true;
}

/* What a chosen item plays: a series starts at its next episode. */
bool resolve_playable(jf::Item *item)
{
    if (item->type == "Movie" || item->type == "Episode" || item->type == "Video" || item->type == "Trailer" ||
        item->type == "MusicVideo" || item->type == "Audio" || item->type == "LiveTvChannel")
        return true;
    if (item->type == "MusicAlbum") {   /* from Min liste or search: its first track */
        for (const jf::Item &t : s_client->children(item->id, "ParentIndexNumber,IndexNumber,SortName", 1))
            if (t.type == "Audio") {
                *item = t;
                return true;
            }
        return false;
    }
    if (item->type == "Season") {
        /* A season (the "recently added" rows group episodes by season):
         * its first unwatched episode, else its first. */
        const std::vector<jf::Item> eps = s_client->episodes(item->series_id, item->id);
        for (const jf::Item &e : eps)
            if (!e.played) {
                *item = e;
                return true;
            }
        if (eps.empty())
            return false;
        *item = eps.front();
        return true;
    }
    if (item->type != "Series")
        return false;
    std::vector<jf::Item> next = s_client->next_up(1, item->id);
    if (next.empty())
        next = s_client->episodes(item->id, std::string());
    if (next.empty()) {
        evo_bt("emby5: nothing to play in %s: %s", item->name.c_str(), s_client->last_error().c_str());
        return false;
    }
    *item = next.front();
    return true;
}

void play(jf::Item item, bool from_start, bool shuffle = false, const std::vector<jf::Item> *queue = nullptr,
          size_t start = 0)
{
    if (s_user_data_busy) {
        notify(T("A server change is still pending. Start playback after it finishes."));
        return;
    }

    stop_theme(true);   /* the page's theme song makes way */
    if (queue && !queue->empty())
        item = (*queue)[std::min(start, queue->size() - 1)];
    if (from_start)
        item.position_ticks = 0;
    /* Side-by-side and top-and-bottom 3D: the PS5 has no 3D output, and the two
     * squeezed pictures are no way to watch. MVC (3D Blu-ray) plays its 2D view. */
    if (item.video3d.find("SideBySide") != std::string::npos || item.video3d.find("TopAndBottom") != std::string::npos) {
        notify(T("Emby5: 3D-filer st\xC3\xB8ttes ikke p\xC3\xA5 PS5"));
        return;
    }
    if (!resolve_playable(&item)) {
        notify(T("Emby5: fant ingenting å spille av her"));
        return;
    }
    if (syncplay::active() && !s_group_play) {   /* in a group: everyone plays it */
        syncplay::play(item);
        notify(T("Emby5: startes for hele gruppen"));
        return;
    }
    s_group_play = false;
    if (item.type == "Audio") {   /* music: behind the menus */
        start_music(item, shuffle, queue, start);
        return;
    }
    stop_music();   /* a picture to show: the music ends first */
    nuvio_player_set_headless(0);
    /* Hand over to the player without a seam: this frame is the player's own
     * loading screen (its colour, the title's backdrop at 92 %, its gradient),
     * and the art it is about to ask for is already being fetched. */
    const std::string backdrop = s_client->image_url(item.backdrop_owner, "Backdrop", item.backdrop_tag, 1920);
    if (!backdrop.empty())
        ui_image_request(backdrop.c_str(), 1920, 1080, 0);
    gfx::begin_frame();
    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    if (item.type == "Audio") {   /* the music screen's ground: its colour and the cover's hues */
        gfx::fill(full, ui::theme_bg());
        const std::string &hash = !item.album_blurhash.empty() ? item.album_blurhash : item.primary_blurhash;
        if (const gfx::Texture *bh = art::blurhash(hash))
            gfx::image(full, bh, 0.55f, 0, true);
        gfx::fill_hgradient(full, 0x8c07070au, 0xd907070au);
    } else {
        gfx::fill(full, 0xff080b10u);
        if (const gfx::Texture *t = art::get(backdrop, 1920, 1080))
            gfx::image(full, t, 0.92f, 0, true);
        gfx::fill_vgradient({0, 0, gfx::W, 378}, 0x4d000000u, 0x99000000u);
        gfx::fill_vgradient({0, 378, gfx::W, 378}, 0x99000000u, 0xcc000000u);
        gfx::fill_vgradient({0, 756, gfx::W, 324}, 0xcc000000u, 0xe6000000u);
    }
    gfx::end_frame();

    nuvio_input_close();
    std::string error;
    const bool ok = queue && queue->size() > 1 ? emby5_play_queue(*s_client, *queue, start, &error)
                                               : emby5_play(*s_client, item, &error, shuffle);
    if (!ok)
        notify((T("Emby5: kunne ikke spille av\n") + error).c_str());
    else if (!error.empty())
        notify(error.c_str());
    nuvio_input_open(s_user);
    /* Back at once; positions and "next up" refresh behind the screen. */
    refresh_user_data_screens();
    jf::Client *c = s_client;
    const unsigned session = s_session;
    std::thread([c, session] {
        emby5_wait_reports(4000);   /* the position just reported, before reading it back */
        load_home(*c, session, true);
        load_extras(*c, session);   /* what was just watched shapes "Fordi du så" */
    }).detach();
}

/* A command from a phone while the menus are up: play what it sent, or show
 * its message. (During playback the player takes them itself.) */
void remote_idle(const remote::Command &rc)
{
    if (rc.kind == remote::Command::Message) {
        notify((rc.header.empty() ? rc.text : rc.header + "\n" + rc.text).c_str());
        return;
    }
    if (rc.kind != remote::Command::Play)
        return;
    std::vector<jf::Item> q;
    s_group_play = rc.play_command == "SyncPlay";
    if (rc.play_command == "PlayInstantMix") {
        q = s_client->instant_mix(rc.item_ids.front(), 60);
    } else {
        for (const std::string &id : rc.item_ids) {
            jf::Item it;
            if (s_client->item(id, &it))
                q.push_back(it);
        }
    }
    if (q.empty()) {
        notify(T("Emby5: fant ikke det som ble sendt"));
        return;
    }
    size_t start = std::min((size_t)std::max(0, rc.start_index), q.size() - 1);
    if (rc.play_command == "PlayShuffle") {
        std::srand((unsigned)time(nullptr));
        for (size_t i = q.size(); i > 1; i--)
            std::swap(q[i - 1], q[std::rand() % i]);
        start = 0;
    }
    q[start].position_ticks = rc.start_ticks;   /* the phone says where to start */
    evo_bt("emby5: remote play %s (%zu in queue)", q[start].name.c_str(), q.size());
    s_stack.clear();   /* back from playback on the home screen */
    s_tab = s_nav_tab = ui::Nav::Home;
    s_nav_focus = false;
    play(q[start], false, false, &q, start);
}

} // namespace

#ifdef EMBY5_LOG_HOST
/* What is on screen, for the log's frame timing (development builds). */
static const char *screen_label()
{
    if (!s_stack.empty()) {
        ui::Screen *s = s_stack.back().get();
        if (dynamic_cast<ui::SeerrDetail *>(s))
            return s->modal() ? "Seerr page (sheet)" : "Seerr page";
        if (dynamic_cast<ui::Detail *>(s))
            return s->modal() ? "detail page (sheet)" : "detail page";
        return "a page";
    }
    switch (s_tab) {
    case ui::Nav::Movies: case ui::Nav::Shows: case ui::Nav::Music: return "a library";
    case ui::Nav::Settings: return "settings";
    default: return "home";
    }
}
#endif

/* Imports the console left NULL (see scripts/build.sh). */
extern "C" __attribute__((weak)) int nuvio_import_count(void) { return 0; }
extern "C" __attribute__((weak)) const char *nuvio_import_null(int) { return nullptr; }

int main()
{
    install_crash_handler();
    evo_bt("emby5: app start " EMBY5_VERSION);
    for (int i = 0; i < nuvio_import_count(); i++)
        if (const char *name = nuvio_import_null(i))
            evo_bt("emby5: import %s is NULL on this console", name);

    if (init_display() != 0) {
        notify(T("Emby5: skjermen kunne ikke startes"));
        for (;;)
            usleep(1000 * 1000);
    }

    /* First app-owned frame: no decoder, controller, network, settings, account,
     * sandbox promotion, or font loading is required to draw the static artwork. */
    const bool graphics_ok = gfx::init();
    auto present_splash = [&](float progress) {
        if (!graphics_ok) return;
        gfx::begin_frame();
        draw_launch(0., 1.f, "", "", true, "", progress);
        gfx::end_frame();
    };
    present_splash(0.05f);
    if (graphics_ok)
        sceSystemServiceHideSplashScreen();

    /* Keep presenting completed milestones rather than delaying launch for a
     * simulated timer. Subsequent online catalogue refresh remains asynchronous. */
    init_services();
    present_splash(0.28f);

    /* /data/Emby5 is provided by ShadowMountPlus; do not request permissions. */
    if (evo_mkdir("/data/Emby5") != 0 ||
        evo_mkdir("/data/Emby5/config") != 0 ||
        evo_mkdir("/data/Emby5/cache") != 0 ||
        evo_mkdir("/data/Emby5/logs") != 0) {
        evo_bt("emby5: /data/Emby5 unavailable (check ShadowMountPlus)");
    }
    evo_boot_log_flush();
    present_splash(0.42f);

    const bool text_ok = ui_text_init() == 0;
    if (!text_ok || !graphics_ok) {
        evo_bt("emby5: ui init failed");
    }
    present_splash(0.52f);
    nuvio_input_open(s_user);

    char device[48];
    std::snprintf(device, sizeof device, "emby5-ps5-%d", s_user);
    s_device = device;
    settings::load_local();
    update_service::set_automatic_enabled(settings::get().local.check_updates);
    {
        const int mode = settings::get().local.upscale_mode;
        evo_agc_upscale_set_mode(mode == 0 ? EVO_AGC_UPSCALE_OFF
            : mode == 1 ? EVO_AGC_UPSCALE_AUTO
            : mode == 2 ? EVO_AGC_UPSCALE_SHARP : EVO_AGC_UPSCALE_AI);
    }
    accounts::set_ps5_user(s_user);
    browse_state::set_user(s_user);   /* each PS5 user keeps their own Emby account */
    i18n::use_language(settings::get().local.language);   /* the PS5's, unless one is chosen */
    /* Restore the saved refresh rate in both directions. The video output may
     * have been left at 120 Hz by a previous process or by early initialization. */
    const bool requested_120hz = settings::get().local.refresh_120 && evo_agc_runtime_supports_120hz();
    const int refresh_rc = evo_agc_runtime_set_120hz(requested_120hz ? 1 : 0);
    if (refresh_rc != 0)
        evo_bt("emby5: applying saved refresh rate %s failed: %d", requested_120hz ? "120Hz" : "60Hz", refresh_rc);
    s_boot_has_account = accounts::last(&s_boot_account);
    s_client = s_boot_client = s_boot_has_account ? client_for(s_boot_account) : new_client(EMBY5_SERVER);
    reset_screens();
    present_splash(0.66f);

    pthread_t w;
    pthread_create(&w, nullptr, boot, nullptr);
    pthread_detach(w);

    /* No self-exit: exit() teardown faults in an app process (SIGSYS, seen on
     * hardware and in EVO). The app is closed with the PS button. */
    const double t0 = now_s();
    double last = t0;
    bool animating = true;
    Phase last_phase = Phase::Connecting;
    unsigned last_model = ~0u, last_discover = ~0u, last_appended = 0;
    int idle_frames = 0;
    unsigned frames = 0;
    unsigned lang_gen = i18n::generation();
    unsigned seerr_gen = seerr_service::generation();
    [[maybe_unused]] bool waited = true;   /* the loop chose to wait since the last frame (nothing moved; read by
                                            * the development builds' frame timing) */
    for (;;) {
        nuvio_input_state in;
        nuvio_input_poll(&in);
        const bool keyboard_was_active = ime::active();
        ime::poll();
        if (keyboard_was_active || ime::active())
            in.pressed = 0;   /* the system keyboard has the controller */

        Phase phase;
        GateRequest gate;
        {
            std::lock_guard<std::mutex> g(s_state.lock);
            phase = s_state.phase;
            if (s_state.gate.kind != Gate::None) {
                gate = s_state.gate;
                s_state.gate = GateRequest();
            }
        }
        if (gate.kind != Gate::None)
            open_gate(gate);
        bool user_data_changed = poll_user_data_results();

        /* The screensaver: on after a while without a button; any button wakes the
         * app and is only that. */
        static double last_input = now_s();
        if (in.pressed) {
            last_input = now_s();
            if (s_saver.on()) {
                s_saver.stop();
                in.pressed = 0;
                animating = true;
            }
        } else if (!s_saver.on() && phase == Phase::Home && !ime::active() && now_s() - last_input > kSaverAfter) {
            s_saver.start(saver_slides(), now_s() - t0);
            animating = true;
        }

        jf::Item chosen;
        bool chose = false, from_start = false, shuffle = false;
        if (in.pressed) {
            if (phase == Phase::Gate)
                gate_input(in.pressed);
            else if (phase == Phase::Failed && (in.pressed & NUVIO_BTN_CIRCLE)) {
                s_session++;
                open_gate({accounts::load().empty() ? Gate::Login : Gate::Profiles, s_client->server(), "", false});
                set_phase(Phase::Gate);
            } else if (phase == Phase::Home && s_home_version == s_model_version) {
                // Native repeats handle normal speed; held navigation takes over after 1 second.
                const uint32_t pressed = ((s_tab == ui::Nav::IPTV && s_iptv &&
                    !s_nav_focus && s_stack.empty() && s_iptv->accelerating(now_s())) ||
                    (s_tab==ui::Nav::Settings && s_settings && !s_nav_focus && s_stack.empty() &&
                     s_settings->accelerating(now_s())))
                    ? in.pressed & ~(in.repeats & (NUVIO_BTN_UP | NUVIO_BTN_DOWN)) : in.pressed;
                if (pressed) shell_input(pressed, &chosen, &chose, &from_start, &shuffle);
            }
        }
        // IPTV acceleration uses actual held state, not D-pad repeat events.
        if (phase == Phase::Home && s_tab == ui::Nav::IPTV && s_stack.empty() && !s_nav_focus && !ime::active())
            s_iptv->update_hold(in.held,now_s());
        else if (s_iptv)
            s_iptv->update_hold(0,now_s());
        if(s_settings) {
            const bool settings_active=phase==Phase::Home && s_tab==ui::Nav::Settings &&
                s_stack.empty() && !s_nav_focus && !ime::active() && !s_saver.on();
            s_settings->update_hold(settings_active?in.held:0,now_s());
        }
        if (phase == Phase::Gate)
            gate_poll();
        if (lang_gen != i18n::generation()) {   /* Innstillinger -> Språk: rebuild the text that was built */
            lang_gen = i18n::generation();
            s_movies->set_title(T("Filmer"));
            s_shows->set_title(T("Serier"));
            s_music->set_title(T("Musikk"));
            {
                std::lock_guard<std::mutex> g(s_state.lock);
                apply_views(s_state.views);   /* the pills' labels */
            }
            if (phase == Phase::Home) {
                jf::Client *c = s_client;
                const unsigned session = s_session;
                std::thread([c, session] {
                    load_home(*c, session, true);
                    load_extras(*c, session);
                }).detach();
                seerr_service::set_language();   /* Seerr's titles and rows in the new language */
                refresh_discover(0, true);
            }
        }
        if (!chose && !s_music_on && phase == Phase::Home && s_home_version == s_model_version) {
            remote::Command rc;
            if (remote::take(&rc)) {
                remote_idle(rc);
                last = now_s();
                waited = true;
                continue;
            }
        }
        if (chose) {
            const std::vector<jf::Item> queue = std::move(s_queue);
            s_queue.clear();
            play(chosen, from_start, shuffle, queue.empty() ? nullptr : &queue, s_queue_start);
            last = now_s();
            waited = true;
            continue;
        }
        static bool remote_dirty = false;
        static double remote_refresh_at = 0;
        static std::atomic<bool> remote_refreshing{false};
        if (remote::take_user_data_changed()) remote_dirty = true;
        if (s_user_data_refresh_requested.exchange(false)) remote_dirty = true;
        if (phase == Phase::Home && !s_user_data_busy && !remote_refreshing &&
            remote_dirty && now_s() >= remote_refresh_at) {
            remote_dirty = false;
            remote_refresh_at = now_s() + 2.0;
            refresh_user_data_screens();
            user_data_changed = true;
            const unsigned session = s_session;
            jf::Client *c = s_client;
            remote_refreshing = true;
            if (!emby5::spawn_io([c, session] {
                load_home(*c, session, true);
                remote_refreshing = false;
            }, [] { remote_refreshing = false; })) {
                remote_refreshing = false;
                remote_dirty = true;
            }
        }
        /* Frames only while something moves; idle, the last frame stays up. */
        /* Seerr's state moves on its own (the settings show it). */
        seerr_service::poll();
        {const auto notice=update_service::take_notification();
         if(!notice.empty()) notify(notice.c_str()); }
        const bool seerr_moved = seerr_gen != seerr_service::generation();
        seerr_gen = seerr_service::generation();
        if (seerr_moved && phase == Phase::Home) {   /* Seerr's tab comes and goes with it */
            {
                std::lock_guard<std::mutex> g(s_state.lock);
                apply_views(s_state.views);
            }
            bool failed;
            {
                std::lock_guard<std::mutex> g(s_state.lock);
                failed = s_state.discover_failed;
            }
            /* The first time it is there, or again when it comes back to rows that failed. */
            const bool again = failed && seerr_service::snapshot().state == seerr_service::State::Ready;
            refresh_discover(again ? 0 : 1e9, again);
        }
        static uint64_t seen_iptv = 0;
        static uint64_t seen_vod = 0;
        static double catalogue_poll_at=0;
        const bool poll_catalogues=phase==Phase::Home&&now_s()>=catalogue_poll_at;
        if(poll_catalogues){catalogue_poll_at=now_s()+1.;iptv_vod::refresh();}
        const auto vod_now=iptv_vod::snapshot();
        const bool vod_moved=vod_now.generation!=seen_vod;
        if(vod_moved && phase==Phase::Home) {
            seen_vod=vod_now.generation;
            std::lock_guard<std::mutex> g(s_state.lock);
            apply_views(s_state.views);
        }
        if(poll_catalogues)iptv_live::refresh(s_client);
        const auto live_now=iptv_live::summary();
        const bool iptv_moved=live_now.generation!=seen_iptv;
        if (iptv_moved && phase==Phase::Home) {
            seen_iptv=live_now.generation;
            std::lock_guard<std::mutex> g(s_state.lock);
            apply_views(s_state.views);
        }
        static uint64_t seen_update_revision = 0;
        const auto update_revision = update_service::generation();
        const bool update_moved = update_revision != seen_update_revision;
        seen_update_revision = update_revision;
        ui::Screen *active_screen = phase == Phase::Home ? screen_for(s_tab) : nullptr;
        const int search_focus = active_screen ? active_screen->take_search_focus() : 0;
        if (search_focus) { s_nav_focus = search_focus == 2; s_nav_tab = s_tab; }
        const bool screen_moved = active_screen && active_screen->needs_redraw();
        const bool changed = keyboard_was_active || search_focus || screen_moved || update_moved || user_data_changed || vod_moved || iptv_moved || in.pressed || phase != last_phase || s_model_version != last_model ||
                             gate.kind != Gate::None || seerr_moved || s_discover_version != last_discover ||
                             s_discover_appended != last_appended;
        if (changed || animating || idle_frames < 2) {
            const double now = now_s();
            const float dt = (float)std::min(0.1, now - last);
            last = now;
            animating = draw_frame(now - t0, dt);
#ifdef EMBY5_LOG_HOST   /* development builds: frame timing in the log */
            static perf::Frames ui_perf("ui");
            static double last_drawn = 0;
            const double drawn = now_s();
            /* A pause the loop chose (nothing moved, waiting for a button) is not a
             * gap; a frame that was wanted at once and came late is, and says where. */
            if (waited)
                ui_perf.pause();
            else if (last_drawn > 0 && drawn - last_drawn > 0.1)
                evo_bt("perf ui: %.0f ms between frames (this one drew in %.1f ms) on %s",
                       (drawn - last_drawn) * 1000.0, (drawn - now) * 1000.0, screen_label());
            ui_perf.note((drawn - now) * 1000.0, drawn);
            last_drawn = drawn;
#endif
            waited = false;
            idle_frames = (changed || animating) ? 0 : idle_frames + 1;
            last_phase = phase;
            last_model = s_model_version;
            last_discover = s_discover_version;
            last_appended = s_discover_appended;
            if ((++frames % 120) == 0)
                gfx::collect();
        } else {
            usleep(8000);
            last = now_s();
            waited = true;
        }
        if (s_saver.on())
            usleep(25000);   /* the screensaver drifts slowly: ~30 frames a second is plenty */
        /* Once a minute, what the app holds (idle too): anything that only grows
         * shows here over a long session. Development builds only. */
#ifdef EMBY5_LOG_HOST
        {
            static double last_health = 0;
            const double hnow = now_s();
            if (hnow - last_health >= 60.0) {
                last_health = hnow;
                size_t art_bytes, art_n, hashes;
                art::stats(&art_bytes, &art_n, &hashes);
                evo_bt("health: up %.0f min, art %zu MB in %zu, blurhash %zu, text %zu, pool %zu/%zu MB, clients %zu",
                       (hnow - t0) / 60.0, art_bytes >> 20, art_n, hashes, gfx::text_cache_size(),
                       gfx::pool_used() >> 20, gfx::pool_size() >> 20, s_clients.size());
                evo_log_alloc_state("ui");
            }
        }
#endif
    }
}
