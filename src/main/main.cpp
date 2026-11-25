#include "typedefs.h"
#include "common.h"
#include "enums.h"
#include "main.h"
#include <string.h>
#include "main/glob.h"
#include "main/gamemain.h"
#include "outgame/title.h"
#include "os/system.h"
#include "os/eeiop/eeiop.h"
#include "os/eeiop/eese.h"
#include "graphics/mov/movie.h"
#include "graphics/graph2d/tim2_new.h"
#include "graphics/graph2d/g2d_main.h"
#include "graphics/graph3d/sgdma.h"
#include "graphics/graph3d/gra3d.h"
#include "os/eeiop/adpcm/ea_cmd.h"
#include "mikupan/rendering/mikupan_baked_texture.h"
#include "mikupan/rendering/mikupan_renderer.h"
#include "mikupan/rendering/mikupan_font_textures.h"

#define SDL_MAIN_USE_CALLBACKS 1  /* use the callbacks instead of main() */
#include "iop/adpcm/iopadpcm.h"
#include "iop/iopmain.h"
#include "mikupan/io/mikupan_controller.h"
#include "mikupan/io/mikupan_finder_gyro.h"
#include "mikupan/io/mikupan_file_c.h"
#include "mikupan/ui/mikupan_ui.h"
#include "os/eeiop/se_cmd.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <mikupan/mikupan_memory.h>
#include <sce/libpad.h>
#include <mikupan/mikupan_framelimiter.h>
#include "mikupan/mikupan_ps5.h"
#ifdef __PROSPERO__
#include "mikupan/debug/mikupan_logging_c.h"
#endif

static int mikupan_game_initialized = 0;
static int mikupan_missing_data_prompted = 0;
static char mikupan_missing_data_file[256] = "";
MikuPan_FrameLimiter frame_limiter;

static int MikuPan_TryInitializeGame(void)
{
    char missing_file[sizeof(mikupan_missing_data_file)] = "";

    if (mikupan_game_initialized)
    {
        return 1;
    }

    if (!MikuPan_HasRequiredDataFiles(missing_file, sizeof(missing_file)))
    {
        if (strcmp(mikupan_missing_data_file, missing_file) != 0)
        {
            strncpy(mikupan_missing_data_file, missing_file,
                    sizeof(mikupan_missing_data_file) - 1);
            mikupan_missing_data_file[sizeof(mikupan_missing_data_file) - 1] =
                '\0';
            mikupan_missing_data_prompted = 0;
        }

        if (!mikupan_missing_data_prompted)
        {
#ifdef __PROSPERO__
            MikuPan_Ps5Notify("MikuPan: %s not found. Put IMG_HD.BIN and "
                              "IMG_BD.BIN in /data/MikuPan/",
                              mikupan_missing_data_file);
#endif
            MikuPan_RequestDataFolderSelection(mikupan_missing_data_file);
            mikupan_missing_data_prompted = 1;
        }

        return 0;
    }

    mikupan_missing_data_file[0] = '\0';
    mikupan_missing_data_prompted = 0;

    /// Model and 2D textures come from the texture pack, baked from the
    /// user's data on first launch.
    if (!MikuPan_BakedTexture_Prepare())
    {
        return 0;
    }

    MikuPanFonts::Prepare();

    /// GAME LOGIC ///
    InitSystem();
    InitGameFirst();
    mikupan_game_initialized = 1;
#ifdef __PROSPERO__
    MikuPan_Ps5Notify("MikuPan: game data loaded");
#endif
    return 1;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
#ifdef __PROSPERO__
    MikuPan_Ps5Notify("%s starting. Log: %smikupan.log", MIKUPAN_PS5_BUILD_LABEL,
                      MikuPan_GetPs5UserDirectory());
#endif

    SDL_AppResult result = MikuPan_Init();
    if (result != SDL_APP_CONTINUE)
    {
#ifdef __PROSPERO__
        MikuPan_Ps5Notify("MikuPan could not start: %s", SDL_GetError());
#endif
        return result;
    }

    MikuPan_InitPs2Memory();

    MikuPan_TryInitializeGame();

    MikuPan_FrameLimiter_Init(&frame_limiter, 60.0f);
#ifdef __PROSPERO__
    MikuPan_Ps5ProfilerStart();
#endif

    return result;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT)
    {
        return SDL_APP_SUCCESS;  /* end the program, reporting success to the OS. */
    }

    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
    {
        MikuPan_UpdateWindowSize(event->window.data1, event->window.data2);
    }

    if (event->type == SDL_EVENT_GAMEPAD_REMOVED)
    {
        scePadPortOpen(0, 0 ,0);
    }

    if (event->type == SDL_EVENT_GAMEPAD_ADDED)
    {
        scePadPortOpen(0, 0 ,0);
    }

    MikuPan_ControllerProcessEvent(event);
    MikuPan_ProcessEventUi(event);

    return SDL_APP_CONTINUE;
}

#ifdef __PROSPERO__
static Uint64 mikupan_ps5_last_frame_ns = 0;
#endif

SDL_AppResult SDL_AppIterate(void *appstate)
{
#ifdef __PROSPERO__
    /* Like a console game at the home screen: no updates, no frames, no
     * sound until the player is back. */
    {
        static bool was_paused = false;
        const bool paused = mikupan_game_initialized && MikuPan_Ps5ShouldPause();
        if (paused)
        {
            was_paused = true;
            return SDL_APP_CONTINUE;
        }
        if (was_paused)
        {
            was_paused = false;
            MikuPan_FrameLimiter_Reset(&frame_limiter);
            mikupan_ps5_last_frame_ns = 0; /* the pause is not a slow frame */
        }
    }
    /* A frame that took much longer than 1/60 s (the limiter's wait
     * included): logged, so the files loaded around it show what it was. */
    {
        Uint64 &last_frame = mikupan_ps5_last_frame_ns;
        const Uint64 now = SDL_GetTicksNS();
        if (last_frame != 0 && mikupan_game_initialized)
        {
            const double ms = (double)(now - last_frame) / 1e6;
            if (ms > 25.0)
            {
                info_log("PS5: slow frame %.1f ms", ms);
            }
            MikuPan_Ps5ProfilerFrame(ms);
        }
        last_frame = now;
    }
#endif
    MikuPan_ServiceMissingDataFolderDialog();

    MikuPan_Clear();
    MikuPan_StartFrameUi();
    MikuPan_LegacyMouseBeginFrame();
    MikuPan_FinderGyroBeginFrame();

    if (!mikupan_game_initialized)
    {
        MikuPan_TryInitializeGame();
        if (!mikupan_game_initialized)
        {
            if (MikuPan_BakedTexture_GetState() == MIKUPAN_BAKE_RUNNING
                || MikuPan_BakedTexture_GetState() == MIKUPAN_BAKE_FAILED)
            {
                MikuPan_DrawTextureBakeUi();
            }
            else
            {
                MikuPan_DrawMissingDataUi(mikupan_missing_data_file);
            }

            MikuPan_EndFrame();
            return SDL_APP_CONTINUE;
        }
    }
    
    if (!SoftResetChk())
    {
        if (!PlayMpegEvent())
        {
            EiMain();
            GameMain();
            CheckDMATrans();
            sceGsSyncPath(0, 0);
            vfunc();
            DrawAll2DMes_P2();
            FlushModel(1);
            ClearTextureCache();
            SeCtrlMain();
        }
        else
        {
            /// Empty the packets for 2D stuff otherwise it crashes
            DrawAll2DMes_P2();
        }
    }
    else
    {
        InitGameFirst();
    }

    /* Resolve finder mouse-look capture once per frame. Runs unconditionally so
     * the cursor is released whenever this frame did not request aiming (movie,
     * menu, soft reset, etc.). */
    MikuPan_FinderMouseUpdate();

    MikuPan_EndFrame();

    MikuPan_FrameLimiter_Wait(&frame_limiter);

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
#ifdef __PROSPERO__
    MikuPan_Ps5Notify("MikuPan closed (%s). Last error: %s",
                      result == SDL_APP_SUCCESS ? "normally" : "after a failure",
                      SDL_GetError());
#endif

    if (mikupan_game_initialized)
    {
        IopShutDown();
    }

    if (result == SDL_APP_SUCCESS)
    {
        MikuPan_ShutDownUi();
    }

    MikuPan_BakedTexture_Shutdown();
    MikuPan_Shutdown();
}

void InitGameFirst()
{
    sys_wrk.reset = 0;
    gra3dInitFirst();
    gra2dInitBG();
    MovieInitWrk();
    realtime_scene_flg = 0;
    sys_wrk.game_mode = GAME_MODE_INIT;
    outgame_wrk.mode = OUTGAME_MODE_INIT;
    outgame_wrk.mode_next = OUTGAME_MODE_TITLE;
    memset(&ingame_wrk, 0, sizeof(INGAME_WRK));
    sys_wrk.sreset_ng = 0;
    opening_movie_type = 0;
}

void CallSoftReset()
{
    int lcount = 0;

    SeStopAll();
    InitSe();
    SetIopCmdSm(IC_SE_INIT, 0, 0, 0);
    EAdpcmCmdStop(0, 0, 0);

    sys_wrk.reset = 0;

    SetSysBackColor(0, 0, 0);

    scene_bg_color = 0;

    while (1)
    {
        EiMain();

        if (EAGetRetStat() == ADPCM_STAT_FULL_STOP || EAGetRetStat() == ADPCM_STAT_LOOPEND_STOP)
        {
            EAdpcmCmdInit(1);
            break;
        }

        lcount++;

        if (lcount > 30)
        {
            EAdpcmCmdStop(0, 0, 0);
            lcount = 0;
        }

        vfunc();
    }
}

int SoftResetChk()
{
    if (
        L1_PRESSED() && L2_PRESSED() && R1_PRESSED() &&
        R2_PRESSED() && START_PRESSED() && SELECT_PRESSED()
    )
    {
        sys_wrk.sreset_count += 1;
    }
    else
    {
        sys_wrk.sreset_count = 0;
    }
    
    if (sys_wrk.sreset_ng != 0)
    {
        sys_wrk.sreset_count = 0;
    }

    if (sys_wrk.sreset_count >= 60)
    {
        sys_wrk.reset = 1;
    }
    
    if (sys_wrk.reset == 1)
    {
        sys_wrk.sreset_count = 0;
        CallSoftReset();
        return 1;
    }
    
    return 0;
}
