#include "mikupan/mikupan_ps5.h"

#include "mikupan/debug/mikupan_logging.h"
#include "mikupan/debug/mikupan_logging_c.h"

#include "platform.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

namespace
{
/* Layout used by the ps5-payload-sdk's notification samples. */
struct NotifyRequest
{
    char reserved[45];
    char message[3075];
};
} // namespace

extern "C" int sceKernelSendNotificationRequest(int device, NotifyRequest *request,
                                                size_t size, int blocking);

extern "C" void MikuPan_Ps5Notify(const char *fmt, ...)
{
    NotifyRequest request;
    std::memset(&request, 0, sizeof(request));

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(request.message, sizeof(request.message), fmt, args);
    va_end(args);

    info_log("Notification: %s", request.message);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

extern "C" int sceKernelUsleep(unsigned int microseconds);
extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv);

extern "C" void MikuPan_Ps5InstallCrashHandler(void);

/* Before main (and before SDL starts): standard error into klog and the
 * shell's splash screen dismissed, which otherwise covers every frame
 * (ps5/platform/platform.c). */
__attribute__((constructor)) static void MikuPan_Ps5Startup()
{
    platform_init(MIKUPAN_PS5_TITLE_NAME);
    /* RADV's own messages (Mesa's log) beside MikuPan's, rather than only in
     * the console's klog. */
    static std::string mesa_log;
    mesa_log = std::string(MikuPan_GetPs5UserDirectory()) + "mesa.log";
    setenv("MESA_LOG_FILE", mesa_log.c_str(), 1);
    setenv("MESA_LOG_LEVEL", "debug", 1);
    /* RADV's compiled shaders, kept between launches with the rest of
     * MikuPan's files (its default is the title's own /app0/radv-shader-cache). */
    static std::string shader_cache;
    shader_cache = std::string(MikuPan_GetPs5UserDirectory()) + "shader-cache";
    setenv("MESA_SHADER_CACHE_DIR", shader_cache.c_str(), 1);
    MikuPan_Ps5InstallCrashHandler();
}

/* Called by the title's entry point (ps5/native/app_crt.cpp) when main
 * returns. A title gets SIGSYS when it calls exit(), which shows a crash
 * dialog, so ask the shell to close it instead, as PS5_Vulkan's titles do.
 * The shell closes it asynchronously: wait, and never return. */
extern "C" [[noreturn]] void catchReturnFromMain(int status)
{
    if (status != 0)
    {
        MikuPan_Ps5Notify("MikuPan has stopped (status %d). See "
                          "/data/MikuPan/mikupan.log",
                          status);
    }
    MikuPan_FlushLog();
    std::fflush(nullptr);
    (void)sceSystemServiceLoadExec("exit", nullptr);
    for (;;)
    {
        sceKernelUsleep(100000);
    }
}

/* ------------------------------------------------------------ diagnostics */

#include <SDL3/SDL_log.h>

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

namespace
{
void ForwardSdlLog(void *, int category, SDL_LogPriority priority, const char *message)
{
    static const char *const names[] = {"", "trace", "verbose", "debug", "info",
                                        "warn", "error", "critical"};
    const int index = priority >= 0 && priority < (int)(sizeof(names) / sizeof(names[0]))
                          ? (int)priority
                          : 0;
    info_log("SDL [%d %s] %s", category, names[index], message);
}
} // namespace

extern "C" void MikuPan_Ps5ForwardSdlLog(void)
{
    SDL_SetLogOutputFunction(ForwardSdlLog, nullptr);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_VERBOSE);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_VERBOSE);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_SYSTEM, SDL_LOG_PRIORITY_VERBOSE);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_ERROR, SDL_LOG_PRIORITY_VERBOSE);
}

extern "C" void MikuPan_Ps5VulkanReport(void)
{
    info_log("Vulkan report: start");
    auto get = [](VkInstance instance, const char *name) {
        return vk_icdGetInstanceProcAddr(instance, name);
    };
#define PS5_VK(instance, name) auto name = reinterpret_cast<PFN_##name>(get(instance, #name))
    PS5_VK(VK_NULL_HANDLE, vkEnumerateInstanceExtensionProperties);
    PS5_VK(VK_NULL_HANDLE, vkEnumerateInstanceVersion);
    PS5_VK(VK_NULL_HANDLE, vkCreateInstance);
    if (!vkEnumerateInstanceExtensionProperties || !vkCreateInstance)
    {
        info_log("Vulkan report: RADV gave no global entry points");
        return;
    }
    uint32_t version = 0;
    if (vkEnumerateInstanceVersion)
    {
        vkEnumerateInstanceVersion(&version);
    }
    info_log("Vulkan report: instance version %u.%u.%u", VK_API_VERSION_MAJOR(version),
             VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));

    uint32_t count = 0;
    VkResult rc = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> instance_extensions(count);
    if (count)
    {
        rc = vkEnumerateInstanceExtensionProperties(nullptr, &count, instance_extensions.data());
    }
    std::string list;
    for (const auto &extension : instance_extensions)
    {
        list += extension.extensionName;
        list += ' ';
    }
    info_log("Vulkan report: %u instance extensions (%d): %s", count, (int)rc, list.c_str());

    const char *enabled[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MikuPan";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = enabled;
    VkInstance instance = VK_NULL_HANDLE;
    rc = vkCreateInstance(&info, nullptr, &instance);
    info_log("Vulkan report: vkCreateInstance(surface, display) = %d", (int)rc);
    MikuPan_FlushLog();
    if (rc != VK_SUCCESS)
    {
        return;
    }

    PS5_VK(instance, vkDestroyInstance);
    PS5_VK(instance, vkEnumeratePhysicalDevices);
    PS5_VK(instance, vkGetPhysicalDeviceProperties);
    PS5_VK(instance, vkGetPhysicalDeviceFeatures);
    PS5_VK(instance, vkEnumerateDeviceExtensionProperties);
    PS5_VK(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    PS5_VK(instance, vkGetPhysicalDeviceDisplayPropertiesKHR);
    PS5_VK(instance, vkGetDisplayModePropertiesKHR);

    count = 0;
    rc = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    info_log("Vulkan report: vkEnumeratePhysicalDevices = %d, %u GPUs", (int)rc, count);
    std::vector<VkPhysicalDevice> devices(count);
    if (count)
    {
        rc = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    }
    for (VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties properties = {};
        vkGetPhysicalDeviceProperties(device, &properties);
        info_log("Vulkan report: GPU '%s' API %u.%u.%u type %d", properties.deviceName,
                 VK_API_VERSION_MAJOR(properties.apiVersion),
                 VK_API_VERSION_MINOR(properties.apiVersion),
                 VK_API_VERSION_PATCH(properties.apiVersion), (int)properties.deviceType);

        VkPhysicalDeviceFeatures features = {};
        vkGetPhysicalDeviceFeatures(device, &features);
        info_log("Vulkan report: features independentBlend %u imageCubeArray %u depthClamp %u "
                 "shaderClipDistance %u drawIndirectFirstInstance %u sampleRateShading %u "
                 "samplerAnisotropy %u",
                 features.independentBlend, features.imageCubeArray, features.depthClamp,
                 features.shaderClipDistance, features.drawIndirectFirstInstance,
                 features.sampleRateShading, features.samplerAnisotropy);

        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> device_extensions(extension_count);
        if (extension_count)
        {
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count,
                                                 device_extensions.data());
        }
        bool swapchain = false, maintenance1 = false;
        for (const auto &extension : device_extensions)
        {
            swapchain |= std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            maintenance1 |= std::strcmp(extension.extensionName, VK_KHR_MAINTENANCE_1_EXTENSION_NAME) == 0;
        }
        info_log("Vulkan report: %u device extensions, swapchain %d, maintenance1 %d",
                 extension_count, swapchain, maintenance1);

        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        if (families)
        {
            vkGetPhysicalDeviceQueueFamilyProperties(device, &families, queues.data());
        }
        for (uint32_t i = 0; i < families; i++)
        {
            info_log("Vulkan report: queue family %u flags 0x%x count %u", i,
                     (unsigned)queues[i].queueFlags, queues[i].queueCount);
        }

        if (vkGetPhysicalDeviceDisplayPropertiesKHR && vkGetDisplayModePropertiesKHR)
        {
            uint32_t displays = 0;
            rc = vkGetPhysicalDeviceDisplayPropertiesKHR(device, &displays, nullptr);
            info_log("Vulkan report: vkGetPhysicalDeviceDisplayPropertiesKHR = %d, %u displays",
                     (int)rc, displays);
            std::vector<VkDisplayPropertiesKHR> display_list(displays);
            if (displays)
            {
                vkGetPhysicalDeviceDisplayPropertiesKHR(device, &displays, display_list.data());
            }
            for (const auto &display : display_list)
            {
                uint32_t modes = 0;
                rc = vkGetDisplayModePropertiesKHR(device, display.display, &modes, nullptr);
                std::vector<VkDisplayModePropertiesKHR> mode_list(modes);
                if (modes)
                {
                    vkGetDisplayModePropertiesKHR(device, display.display, &modes, mode_list.data());
                }
                for (const auto &mode : mode_list)
                {
                    info_log("Vulkan report: display mode %ux%u %u mHz",
                             mode.parameters.visibleRegion.width,
                             mode.parameters.visibleRegion.height, mode.parameters.refreshRate);
                }
            }
        }
        MikuPan_FlushLog();
    }
    vkDestroyInstance(instance, nullptr);
    info_log("Vulkan report: done");
    MikuPan_FlushLog();
#undef PS5_VK
}

/* ------------------------------------------------------------ crash report */

#include <ps5platform/context.h>

#include <exception>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace
{
char g_crash_path[512];

/* One line to crash.log, opened for each line so nothing is lost if the
 * report itself faults. snprintf is not async-signal-safe, but the title is
 * going down anyway. */
void CrashLine(const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    const int length = std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (length <= 0)
    {
        return;
    }
    const int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
    {
        return;
    }
    (void)write(fd, line, (size_t)(length < (int)sizeof(line) ? length : (int)sizeof(line) - 1));
    (void)write(fd, "\n", 1);
    close(fd);
}

void CrashHeader(const char *what)
{
    CrashLine("MikuPan crashed: %s", what);
    /* Its link-time address is in the ELF's symbol table: the difference is
     * where the console loaded the title. */
    CrashLine("anchor MikuPan_Ps5Notify = %p", (void *)&MikuPan_Ps5Notify);
    CrashLine("thread %p", (void *)pthread_self());
}

void CrashHandler(int signal_number, siginfo_t *info, void *context)
{
    const auto *uc = static_cast<const ucontext_t *>(context);
    const mcontext_t &mc = uc->uc_mcontext;
    char what[96];
    std::snprintf(what, sizeof(what), "signal %d, code %d, address %p", signal_number,
                  info ? info->si_code : 0, info ? info->si_addr : nullptr);
    CrashHeader(what);
    CrashLine("rip %#llx rsp %#llx rbp %#llx", (unsigned long long)mc.mc_rip,
              (unsigned long long)mc.mc_rsp, (unsigned long long)mc.mc_rbp);
    CrashLine("rax %#llx rbx %#llx rcx %#llx rdx %#llx", (unsigned long long)mc.mc_rax,
              (unsigned long long)mc.mc_rbx, (unsigned long long)mc.mc_rcx,
              (unsigned long long)mc.mc_rdx);
    CrashLine("rsi %#llx rdi %#llx r8 %#llx r9 %#llx", (unsigned long long)mc.mc_rsi,
              (unsigned long long)mc.mc_rdi, (unsigned long long)mc.mc_r8,
              (unsigned long long)mc.mc_r9);

    /* The frame-pointer chain, kept to frames that lie above the stack
     * pointer and grow upwards, so a broken chain ends the walk. */
    CrashLine("frame 0 %#llx", (unsigned long long)mc.mc_rip);
    const uintptr_t stack = (uintptr_t)mc.mc_rsp;
    uintptr_t frame = (uintptr_t)mc.mc_rbp;
    for (int i = 1; i < 64; i++)
    {
        if (frame == 0 || (frame & 7) != 0 || frame < stack || frame - stack > (64u << 20))
        {
            break;
        }
        const uintptr_t *slot = reinterpret_cast<const uintptr_t *>(frame);
        const uintptr_t return_address = slot[1];
        if (return_address == 0)
        {
            break;
        }
        CrashLine("frame %d %#llx", i, (unsigned long long)return_address);
        const uintptr_t next = slot[0];
        if (next <= frame)
        {
            break;
        }
        frame = next;
    }
    CrashLine("end");

    /* Let the console report it as usual */
    struct sigaction fallback = {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal_number, &fallback, nullptr);
    raise(signal_number);
}

void TerminateHandler()
{
    const char *what = "std::terminate (no active exception)";
    std::string message;
    if (std::exception_ptr current = std::current_exception())
    {
        try
        {
            std::rethrow_exception(current);
        }
        catch (const std::exception &exception)
        {
            message = std::string("uncaught exception: ") + exception.what();
            what = message.c_str();
        }
        catch (...)
        {
            what = "uncaught exception of an unknown type";
        }
    }
    CrashHeader(what);
    CrashLine("end");
    info_log("MikuPan crashed: %s", what);
    MikuPan_FlushLog();
    std::abort();
}

alignas(16) char g_crash_stack[128 * 1024];
} // namespace

extern "C" void MikuPan_Ps5InstallCrashHandler(void)
{
    std::snprintf(g_crash_path, sizeof(g_crash_path), "%scrash.log",
                  MikuPan_GetPs5UserDirectory());
    unlink(g_crash_path);

    /* Its own stack, so a stack overflow can still be reported */
    stack_t alternate = {};
    alternate.ss_sp = g_crash_stack;
    alternate.ss_size = sizeof(g_crash_stack);
    sigaltstack(&alternate, nullptr);

    struct sigaction action = {};
    action.sa_sigaction = CrashHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int signal_number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP})
    {
        sigaction(signal_number, &action, nullptr);
    }
    std::set_terminate(TerminateHandler);
}

/* ------------------------------------------------------- home screen pause */

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_timer.h>

extern SDL_AudioDeviceID audio_dev; /* sdk/sce/libsd.cpp: the game's sound */
extern "C" void MikuPan_MovieSetAudioPaused(int paused);

namespace
{
void SetGameAudioPaused(bool paused)
{
    if (audio_dev != 0)
    {
        if (paused)
        {
            SDL_PauseAudioDevice(audio_dev);
        }
        else
        {
            SDL_ResumeAudioDevice(audio_dev);
        }
    }
    MikuPan_MovieSetAudioPaused(paused ? 1 : 0);
}
} // namespace

extern "C" bool MikuPan_Ps5ShouldPause(void)
{
    /* The console marks each controller reading it keeps for itself (the
     * home screen, the control centre, a system dialog) with
     * PAD_INTERCEPTED; the latest reading says who has the controller now.
     * SDL's controller driver polled the pads while the events were pumped. */
    static bool paused = false;
    const struct pad_reading *readings = nullptr;
    const int count = pad_player_readings(0, &readings);
    bool shell_has_pad = paused;
    if (count > 0 && readings != nullptr)
    {
        shell_has_pad = (readings[count - 1].buttons & PAD_INTERCEPTED) != 0;
    }

    if (shell_has_pad != paused)
    {
        paused = shell_has_pad;
        SetGameAudioPaused(paused);
        info_log("PS5: %s", paused ? "the system has the controller: game paused"
                                   : "back in the game: resumed");
    }
    if (paused)
    {
        SDL_Delay(50);
    }
    return paused;
}

/* ------------------------------------------------------- sampling profiler */
/* Opt-in (/data/MikuPan/profile.on at launch): a helper thread interrupts the
 * game's main thread about every millisecond; the handler records where it is
 * and the frame-pointer chain above it. At the end of each frame the samples
 * are kept only if the frame was slow, and every few seconds the hottest
 * addresses go to profile.txt, to be matched to the linked ELF offline. */

#include <atomic>
#include <unordered_map>
#include <vector>

namespace
{
constexpr int kProfDepth = 12;
constexpr int kProfFrameSamples = 512;

struct ProfSample
{
    uintptr_t pc[kProfDepth];
    int depth;
};

ProfSample g_prof_frame[kProfFrameSamples];
std::atomic<int> g_prof_count{0};
std::atomic<bool> g_prof_running{false};
pthread_t g_prof_main;
std::unordered_map<uintptr_t, unsigned> g_prof_self;
std::unordered_map<uintptr_t, unsigned> g_prof_incl;
unsigned g_prof_samples = 0;
unsigned g_prof_slow_frames = 0;
double g_prof_slow_ms = 0.0;
Uint64 g_prof_last_write = 0;
unsigned g_prof_written_samples = 0;

void ProfHandler(int, siginfo_t *, void *context)
{
    const int index = g_prof_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= kProfFrameSamples)
    {
        return;
    }
    const auto *uc = static_cast<const ucontext_t *>(context);
    ProfSample &sample = g_prof_frame[index];
    sample.pc[0] = (uintptr_t)uc->uc_mcontext.mc_rip;
    sample.depth = 1;
    const uintptr_t stack = (uintptr_t)uc->uc_mcontext.mc_rsp;
    uintptr_t frame = (uintptr_t)uc->uc_mcontext.mc_rbp;
    while (sample.depth < kProfDepth)
    {
        if (frame == 0 || (frame & 7) != 0 || frame < stack || frame - stack > (8u << 20))
        {
            break;
        }
        const uintptr_t *slot = reinterpret_cast<const uintptr_t *>(frame);
        if (slot[1] == 0)
        {
            break;
        }
        sample.pc[sample.depth++] = slot[1];
        if (slot[0] <= frame)
        {
            break;
        }
        frame = slot[0];
    }
}

void *ProfTicker(void *)
{
    while (g_prof_running.load())
    {
        sceKernelUsleep(1000);
        pthread_kill(g_prof_main, SIGPROF);
    }
    return nullptr;
}

void ProfWrite()
{
    char path[512];
    std::snprintf(path, sizeof(path), "%sprofile.txt", MikuPan_GetPs5UserDirectory());
    FILE *file = std::fopen(path, "w");
    if (file == nullptr)
    {
        return;
    }
    std::fprintf(file, "MikuPan PS5 profile: %u samples from %u slow frames (avg %.1f ms)\n",
                 g_prof_samples, g_prof_slow_frames,
                 g_prof_slow_frames ? g_prof_slow_ms / g_prof_slow_frames : 0.0);
    std::fprintf(file, "anchor MikuPan_Ps5Notify = %p\n", (void *)&MikuPan_Ps5Notify);
    auto dump = [file](const char *title, const std::unordered_map<uintptr_t, unsigned> &map) {
        std::vector<std::pair<unsigned, uintptr_t>> sorted;
        sorted.reserve(map.size());
        for (const auto &entry : map)
        {
            sorted.emplace_back(entry.second, entry.first);
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        std::fprintf(file, "%s\n", title);
        for (size_t i = 0; i < sorted.size() && i < 150; i++)
        {
            std::fprintf(file, "%u %#llx\n", sorted[i].first, (unsigned long long)sorted[i].second);
        }
    };
    dump("self", g_prof_self);
    dump("inclusive", g_prof_incl);
    std::fclose(file);
}
} // namespace

extern "C" void MikuPan_Ps5ProfilerStart(void)
{
    char flag[512];
    std::snprintf(flag, sizeof(flag), "%sprofile.on", MikuPan_GetPs5UserDirectory());
    if (FILE *file = std::fopen(flag, "rb"))
    {
        std::fclose(file);
    }
    else
    {
        return;
    }
    struct sigaction action = {};
    action.sa_sigaction = ProfHandler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, nullptr);
    g_prof_main = pthread_self();
    g_prof_running = true;
    pthread_t ticker;
    if (pthread_create(&ticker, nullptr, ProfTicker, nullptr) == 0)
    {
        pthread_detach(ticker);
        info_log("PS5: sampling profiler on (profile.on found); slow frames go to profile.txt");
    }
    else
    {
        g_prof_running = false;
    }
}

extern "C" void MikuPan_Ps5ProfilerFrame(double frame_ms)
{
    if (!g_prof_running.load())
    {
        return;
    }
    const int count = std::min(g_prof_count.exchange(0), kProfFrameSamples);
    /* Anything under ~52 fps: the doorway dips are ~24 ms frames */
    if (frame_ms > 19.0 && count > 0)
    {
        g_prof_slow_frames++;
        g_prof_slow_ms += frame_ms;
        for (int i = 0; i < count; i++)
        {
            const ProfSample &sample = g_prof_frame[i];
            g_prof_samples++;
            g_prof_self[sample.pc[0]]++;
            for (int d = 0; d < sample.depth; d++)
            {
                bool seen = false;
                for (int e = 0; e < d && !seen; e++)
                {
                    seen = sample.pc[e] == sample.pc[d];
                }
                if (!seen)
                {
                    g_prof_incl[sample.pc[d]]++;
                }
            }
        }
    }
    const Uint64 now = SDL_GetTicks();
    if (g_prof_samples != g_prof_written_samples && now - g_prof_last_write > 5000)
    {
        g_prof_last_write = now;
        g_prof_written_samples = g_prof_samples;
        ProfWrite();
    }
}
