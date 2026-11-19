#include "common.h"
#include "enums.h"

#include "main/glob.h"
#include "common/ul_math.h"
#include "ingame/map/map_ctrl.h"
#include "ingame/map/map_htck.h"
#include "ingame/map/door_ctl.h"
#include "ingame/plyr/plyr_ctl.h"
#include "ingame/plyr/unit_ctl.h"
#include "graphics/graph3d/sglib.h"
#include "ingame/camera/camera.h"
#include "mikupan/io/mikupan_controller.h"
#include "mikupan/gameplay/mikupan_first_person.h"
#include "mikupan/gameplay/mikupan_over_shoulder.h"
#include "os/key_cnf.h"
#include "sce/libvu0.h"

#include <math.h>
#include <string.h>

#define PI 3.1415928f
#define DEG2RAD(x) ((float)(x) * PI / 180.0f)

/* Camera pitch limits.  Looking too far down lifts the camera into the low
 * ceilings of the mansion; looking too far up drops it to the floor. */
#define OTS_PITCH_MIN_DEG (-40.0f)
#define OTS_PITCH_MAX_DEG (35.0f)

/* Looking straight ahead from the camera this far away keeps the projected
 * interest point stable however short the boom becomes. */
#define OTS_LOOK_DISTANCE 1000.0f

/* Collision probing.  Samples march along the shoulder offset and the boom
 * and the first blocked sample is then refined by bisection, so the free
 * distance changes continuously as Miku moves instead of in steps. */
#define OTS_PROBE_STEP 20.0f
#define OTS_REFINE_ITERATIONS 6
#define OTS_WALL_MARGIN 45.0f
#define OTS_SHOULDER_MARGIN 35.0f

/* Furniture thinner than this along the boom (ropes, poles, hanging cloth)
 * is passed through instead of pulling the camera in. */
#define OTS_THIN_PROP 160.0f

/* Smoothing.  Walls clamp at once so the camera never sees through them;
 * everything else eases.  After any pull-in the camera holds its distance
 * for a moment before easing back out, so narrow corridors don't pump. */
#define OTS_BOOM_PROP_PULL_RATE 0.12f
#define OTS_BOOM_EASE_OUT 0.06f
#define OTS_BOOM_HOLD_FRAMES 20
#define OTS_SHOULDER_PULL_RATE 0.30f
#define OTS_SHOULDER_EASE_OUT 0.05f

/* Keep the camera between these heights above Miku's feet (world Y points
 * down, so "above" is negative). */
#define OTS_MIN_HEIGHT 60.0f
#define OTS_MAX_HEIGHT 1300.0f

/* Below this boom length the camera sits inside Miku's shoulder.  The gap
 * between the two values stops the model flickering at the threshold. */
#define OTS_HIDE_MODEL_BOOM 140.0f
#define OTS_SHOW_MODEL_BOOM 200.0f

/* Blend from the previous camera when it was close enough to be the same
 * shot, e.g. lowering the Camera Obscura. */
#define OTS_BLEND_FRAMES 12
#define OTS_BLEND_MAX_DIST 1800.0f

/* Frames of right-stick silence before the camera drifts back behind Miku. */
#define OTS_AUTO_FOLLOW_DELAY 45
#define OTS_AUTO_FOLLOW_RATE 0.03f
#define OTS_RECENTER_RATE 0.22f

/* Miku turns to face the camera over this many frames when raising the
 * Camera Obscura, well within the 25-frame raise animation. */
#define OTS_FINDER_TURN_FRAMES 8

/* After the camera is handed back (an event, a scripted scare, the Camera
 * Obscura), keep the player's camera angle if Miku is still standing where
 * she was and facing the same way; otherwise start again behind her. */
#define OTS_RESUME_MAX_MOVE 300.0f
#define OTS_RESUME_MAX_TURN_DEG 25.0f

/* plyr_wrk.pr_info.cam_type while the game shows a scripted drama camera. */
#define OTS_CAM_TYPE_DRAMA 2

/* Guide: turn towards what a scripted drama camera frames, unless it is
 * already comfortably on screen.  The player's right stick or mouse takes
 * over at once. */
#define OTS_GUIDE_FRAMES 50
#define OTS_GUIDE_RATE 0.14f
#define OTS_GUIDE_DONE_DEG 1.5f
#define OTS_GUIDE_MIN_DIST 300.0f
#define OTS_GUIDE_VISIBLE_YAW 0.40f   /* fraction of the field of view */
#define OTS_GUIDE_VISIBLE_PITCH 0.30f

#define OTS_MOUSE_RAD_PER_PIXEL 0.0035f
#define OTS_STICK_DEADZONE 24.0f

int mikupan_over_shoulder_enabled = 0;
int mikupan_over_shoulder_side_left = 0;
int mikupan_over_shoulder_collision_enabled = 1;
int mikupan_over_shoulder_auto_follow_enabled = 1;
int mikupan_over_shoulder_scare_camera = MIKUPAN_SCARE_CAMERA_GUIDE;
float mikupan_over_shoulder_distance = 620.0f;
float mikupan_over_shoulder_pivot_height = 560.0f;
float mikupan_over_shoulder_side = 150.0f;
float mikupan_over_shoulder_fov_deg = 62.0f;
float mikupan_over_shoulder_sensitivity = 1.0f;
float mikupan_over_shoulder_stick_yaw_speed_deg = 3.2f;
float mikupan_over_shoulder_stick_pitch_speed_deg = 2.2f;
float mikupan_over_shoulder_default_pitch_deg = -8.0f;
float mikupan_over_shoulder_smoothing = 0.5f;
float mikupan_over_shoulder_turn_speed_deg = 9.0f;

/* Owns the camera this frame / owned it last frame. */
static int ots_active = 0;
static int ots_was_active = 0;
static int ots_ctrl_ran = 0;

/* Orbit state. */
static float ots_yaw = 0.0f;
static float ots_pitch = 0.0f;
static float ots_boom = 0.0f;
static int ots_boom_hold = 0;
static float ots_shoulder_scale = 1.0f;
static sceVu0FVECTOR ots_pivot = {0.0f, 0.0f, 0.0f, 1.0f};
static int ots_pivot_valid = 0;

/* The camera as it was displayed last frame, captured before the stock
 * camera code overwrites it. */
static sceVu0FVECTOR ots_last_p = {0.0f, 0.0f, 0.0f, 1.0f};
static sceVu0FVECTOR ots_last_i = {0.0f, 0.0f, 0.0f, 1.0f};
static int ots_last_valid = 0;

/* Transition blend from the previous shot. */
static int ots_blend_frames = 0;
static sceVu0FVECTOR ots_blend_p = {0.0f, 0.0f, 0.0f, 1.0f};
static sceVu0FVECTOR ots_blend_i = {0.0f, 0.0f, 0.0f, 1.0f};

static int ots_look_idle_frames = 0;
static int ots_recenter = 0;
static int ots_finder_turn_frames = 0;
static float ots_finder_turn_step = 0.0f;
static u_char ots_prev_plyr_mode = 0xff;
static int ots_hide_model = 0;

/* Scripted drama cameras. */
static int ots_drama_prev = 0;
static u_short ots_drama_no = 0xffff;
static int ots_guide_frames = 0;
static float ots_guide_yaw = 0.0f;
static float ots_guide_pitch = 0.0f;

/* Heading that the left stick is relative to.  When the view cuts (to an
 * authored shot and back) it is held until the stick is released, so a
 * cut never flips the direction Miku is walking. */
static float ots_move_ref = 0.0f;
static int ots_move_latched = 0;
static int ots_move_src_prev = -1;

/* Orbit remembered from the last frame this camera was active. */
static int ots_resume_valid = 0;
static float ots_resume_facing = 0.0f;
static sceVu0FVECTOR ots_resume_pos = {0.0f, 0.0f, 0.0f, 1.0f};
static int ots_blend_hide = 0;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static float OtsClamp(float value, float min, float max)
{
    if (value < min)
    {
        return min;
    }

    if (value > max)
    {
        return max;
    }

    return value;
}

static float OtsWrap(float angle)
{
    RotLimitChk(&angle);
    return angle;
}

static float OtsApproach(float value, float target, float step)
{
    if (value < target)
    {
        value += step;

        if (value > target)
        {
            value = target;
        }
    }
    else if (value > target)
    {
        value -= step;

        if (value < target)
        {
            value = target;
        }
    }

    return value;
}

static void OtsCopy(sceVu0FVECTOR dst, const sceVu0FVECTOR src)
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

static void OtsLerp(sceVu0FVECTOR dst, const sceVu0FVECTOR a,
                    const sceVu0FVECTOR b, float t)
{
    dst[0] = a[0] + (b[0] - a[0]) * t;
    dst[1] = a[1] + (b[1] - a[1]) * t;
    dst[2] = a[2] + (b[2] - a[2]) * t;
    dst[3] = 1.0f;
}

static float OtsDist(const sceVu0FVECTOR a, const sceVu0FVECTOR b)
{
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];

    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float OtsPadAxis(u_char value)
{
    float v = (float)value - 128.0f;
    float av = fabsf(v);
    float sign;

    if (av <= OTS_STICK_DEADZONE)
    {
        return 0.0f;
    }

    sign = v < 0.0f ? -1.0f : 1.0f;
    v = (av - OTS_STICK_DEADZONE) / (127.0f - OTS_STICK_DEADZONE);

    return OtsClamp(v, 0.0f, 1.0f) * sign;
}

static float OtsSideSign(void)
{
    return mikupan_over_shoulder_side_left ? -1.0f : 1.0f;
}

static float OtsPitchMin(void)
{
    return DEG2RAD(OTS_PITCH_MIN_DEG);
}

static float OtsPitchMax(void)
{
    return DEG2RAD(OTS_PITCH_MAX_DEG);
}

static void OtsClampPitch(void)
{
    ots_pitch = OtsClamp(ots_pitch, OtsPitchMin(), OtsPitchMax());
}

static int OtsPlayerMoveMotionLocks(void)
{
    return (plyr_wrk.move_mot >= 1 && plyr_wrk.move_mot <= 9)
           || plyr_wrk.move_mot == 12 || plyr_wrk.move_mot == 13;
}

/* ------------------------------------------------------------------------ */
/* Ownership                                                                */
/* ------------------------------------------------------------------------ */

/* The scripted drama camera the stock code picked this frame, if any.  It
 * runs before us every frame, so its choice and authored target are fresh. */
static const CAMERA_DEBUG_PATH* OtsDramaShot(void)
{
    const CAMERA_DEBUG_PATH* path = CameraGetDebugPath();

    if (path == NULL || !path->active || path->kind != OTS_CAM_TYPE_DRAMA)
    {
        return NULL;
    }

    return path;
}

static int OtsModeAllowsCamera(void)
{
    switch (plyr_wrk.mode)
    {
        case PMODE_NORMAL:
        case PMODE_FINDER_IN:
        case PMODE_FINDER_END:
        case PMODE_MSG_DISP:
        case PMODE_ANIME:
        case PMODE_DMG:
        case PMODE_PRE_DMG:
            return 1;
        default:
            return 0;
    }
}

/* Overlays that return to the same spot afterwards: keep the camera
 * where it is behind them so closing them never cuts or swoops. */
static int OtsIngameModeAllowsCamera(void)
{
    switch (ingame_wrk.mode)
    {
        case INGAME_MODE_NOMAL:
        case INGAME_MODE_GET_ITEM:
        case INGAME_MODE_MENU:
        case INGAME_MODE_PAUSE:
        case INGAME_MODE_SPD_MAP:
        case INGAME_MODE_SPD_OPT:
            return 1;
        case INGAME_MODE_EVENT:
            /* Most events (examining an object, a message, picking something
             * up) set no camera of their own: the original simply kept the
             * room's fixed angle, so keep ours instead of cutting to it.
             * Events that do frame something use a drama camera, which the
             * Scare Camera setting handles like any other.  Movies and
             * in-engine scenes lock the camera before we run, and special
             * events have a mode of their own. */
            return 1;
        default:
            return 0;
    }
}

static int OtsCanOwnCamera(void)
{
    if (!mikupan_over_shoulder_enabled || MikuPan_FirstPersonEnabled())
    {
        return 0;
    }

    if (!OtsIngameModeAllowsCamera())
    {
        return 0;
    }

    if (plyr_wrk.cond == 1 || plyr_wrk.cond == 2)
    {
        return 0;
    }

    if (dbg_wrk.cam_mode != 0)
    {
        return 0;
    }

    /* Original: let the authored scare shot play as it was designed. */
    if (mikupan_over_shoulder_scare_camera == MIKUPAN_SCARE_CAMERA_ORIGINAL
        && OtsDramaShot() != NULL)
    {
        return 0;
    }

    return OtsModeAllowsCamera();
}

static int OtsCanOwnGameplay(void)
{
    if (!mikupan_over_shoulder_enabled || MikuPan_FirstPersonEnabled())
    {
        return 0;
    }

    if (ingame_wrk.mode != INGAME_MODE_NOMAL)
    {
        return 0;
    }

    /* The pause menu keeps the normal mode but locks gameplay. */
    if ((ingame_wrk.stts & INGAME_STTS_GAMEPLAY_LOCK) != 0)
    {
        return 0;
    }

    if (plyr_wrk.mode != PMODE_NORMAL)
    {
        return 0;
    }

    if ((plyr_wrk.sta & 0x8) != 0)
    {
        return 0;
    }

    if (plyr_wrk.cond == 1 || plyr_wrk.cond == 2)
    {
        return 0;
    }

    if (dbg_wrk.cam_mode != 0)
    {
        return 0;
    }

    return 1;
}

/* ------------------------------------------------------------------------ */
/* Collision                                                                */
/* ------------------------------------------------------------------------ */

#define OTS_FREE 0
#define OTS_SOFT 1
#define OTS_HARD 2

/* The heights of the room's pieces, from the bounding boxes the renderer
 * visits (the walking collision's room hit areas have none). A piece no
 * bigger than this across counts as one object; bigger boxes hold whole
 * walls or rooms and say nothing about a single table. */
#define OTS_BOX_MAX 2048
#define OTS_SMALL_BOX_AREA (1600.0f * 1600.0f)
/* How far above a piece's top the camera must be to pass over it. */
#define OTS_LOW_PIECE_MARGIN 60.0f

typedef struct {
    float x0, x1, y0, y1, z0, z1;
} OtsBox;

static OtsBox ots_boxes[2][OTS_BOX_MAX];
static int ots_box_count[2];
static int ots_box_read = 0;

void MikuPan_OverShoulderRecordUnitBox(sceVu0FMATRIX lwmtx, sceVu0FVECTOR* corners)
{
    OtsBox* box;
    int write = ots_box_read ^ 1;
    int i;

    if (!ots_active || ots_box_count[write] >= OTS_BOX_MAX)
    {
        return;
    }

    box = &ots_boxes[write][ots_box_count[write]++];
    for (i = 0; i < 8; i++)
    {
        sceVu0FVECTOR w;

        sceVu0ApplyMatrix(w, lwmtx, corners[i]);
        if (i == 0)
        {
            box->x0 = box->x1 = w[0];
            box->y0 = box->y1 = w[1];
            box->z0 = box->z1 = w[2];
            continue;
        }
        box->x0 = w[0] < box->x0 ? w[0] : box->x0;
        box->x1 = w[0] > box->x1 ? w[0] : box->x1;
        box->y0 = w[1] < box->y0 ? w[1] : box->y0;
        box->y1 = w[1] > box->y1 ? w[1] : box->y1;
        box->z0 = w[2] < box->z0 ? w[2] : box->z0;
        box->z1 = w[2] > box->z1 ? w[2] : box->z1;
    }
}

/* The boxes recorded while the last frame was drawn become this frame's. */
static void OtsSwapBoxes(void)
{
    ots_box_read ^= 1;
    ots_box_count[ots_box_read ^ 1] = 0;
}

/* The small pieces whose box covers p and that belong to room hit area
 * `area` of room `room_id` (the centre of the box lies inside it: a
 * neighbouring cabinet or wall whose box only overlaps the spot does not),
 * and the top (lowest y: up is negative) of the tallest of them. `covering`
 * receives how many small pieces cover p at all. */
static int OtsSmallPiecesAt(const sceVu0FVECTOR p, u_char room_id, u_char area,
                            float* top, int* covering)
{
    const OtsBox* boxes = ots_boxes[ots_box_read];
    u_char room_no = GetDataRoom(MAP_HIT_CHECK, room_id);
    int count = 0;
    int i;

    *top = 0.0f;
    *covering = 0;
    for (i = 0; i < ots_box_count[ots_box_read]; i++)
    {
        const OtsBox* b = &boxes[i];
        float cx;
        float cz;

        if (p[0] < b->x0 || p[0] > b->x1 || p[2] < b->z0 || p[2] > b->z1)
        {
            continue;
        }
        if ((b->x1 - b->x0) * (b->z1 - b->z0) > OTS_SMALL_BOX_AREA)
        {
            continue;
        }
        (*covering)++;

        cx = (b->x0 + b->x1) * 0.5f;
        cz = (b->z0 + b->z1) * 0.5f;
        if (cx < 0.0f || cz < 0.0f
            || PosInAreaJudge1(MAP_HIT_CHECK, room_no, area, (u_short)cz, (u_short)cx) == 0)
        {
            continue;
        }
        if (count == 0 || b->y0 < *top)
        {
            *top = b->y0;
        }
        count++;
    }
    return count;
}

/* A room hit area has no height: it is a low obstacle the camera may pass
 * over only when the room's own pieces in it are all below the camera.
 * With no piece known in it, it stays a wall, as before. */
static int OtsAboveLowPieces(const sceVu0FVECTOR p, u_char room_id, u_char area)
{
    float top;
    int covering;

    if (OtsSmallPiecesAt(p, room_id, area, &top, &covering) == 0)
    {
        return 0;
    }
    return p[1] < top - OTS_LOW_PIECE_MARGIN;
}

/* Classifies a point against the only collision data the game has: the
 * room outlines and the wall, door and furniture areas Miku cannot walk
 * into.  Outside every room, walls and doors are hard; furniture is soft
 * because it is often thin (a rope, a pole) or low enough to see over. */
static int OtsPointClass(const sceVu0FVECTOR p)
{
    sceVu0FVECTOR q;

    if (p[0] < 0.0f || p[0] > 65535.0f || p[2] < 0.0f || p[2] > 65535.0f)
    {
        return OTS_HARD;
    }

    if (GetPointRoomNo((u_short)p[0], (u_short)p[2]) == 0xff)
    {
        return OTS_HARD;
    }

    q[0] = p[0];
    q[1] = p[1];
    q[2] = p[2];
    q[3] = 1.0f;

    switch (HitChkSegment2All(q, q, 1.0f))
    {
        case 0:
            return OTS_FREE;
        case 1:
        {
            /* a table, a chest: see over it when it is all below the camera */
            MIKUPAN_HIT_DETAIL hit;

            MikuPan_HitPointDetail(q, &hit);
            if (hit.kind != 1)
            {
                return OTS_HARD;
            }
            return OtsAboveLowPieces(q, hit.room_id, hit.area) ? OTS_FREE : OTS_HARD;
        }
        case 2:
            /* a closed door blocks the view; an open or moving one is a thin
             * panel the camera passes, as Miku walks through the doorway */
            return door_cover_hit_stts == 1 ? OTS_HARD : OTS_FREE;
        case 3:
            return OTS_SOFT;
        default:
            return OTS_HARD;
    }
}

static void OtsAlong(sceVu0FVECTOR out, const sceVu0FVECTOR from,
                     const sceVu0FVECTOR dir, float t)
{
    out[0] = from[0] + dir[0] * t;
    out[1] = from[1] + dir[1] * t;
    out[2] = from[2] + dir[2] * t;
    out[3] = 1.0f;
}

/* Bisects between a distance known to be clear and one known to hit
 * something of at least class `min_class`. */
static float OtsRefine(const sceVu0FVECTOR from, const sceVu0FVECTOR dir,
                       float clear, float hit, int min_class)
{
    sceVu0FVECTOR p;
    int k;

    for (k = 0; k < OTS_REFINE_ITERATIONS; k++)
    {
        float mid = (clear + hit) * 0.5f;

        OtsAlong(p, from, dir, mid);

        if (OtsPointClass(p) >= min_class)
        {
            hit = mid;
        }
        else
        {
            clear = mid;
        }
    }

    return clear;
}

/* Distance along `dir` (unit length) from `from` to the first sample of at
 * least class `min_class`, or `length` when the whole way is clear. */
static float OtsFirstHit(const sceVu0FVECTOR from, const sceVu0FVECTOR dir,
                         float length, int min_class)
{
    sceVu0FVECTOR p;
    float travelled = 0.0f;

    while (travelled < length)
    {
        float next = travelled + OTS_PROBE_STEP;

        if (next > length)
        {
            next = length;
        }

        OtsAlong(p, from, dir, next);

        if (OtsPointClass(p) >= min_class)
        {
            return OtsRefine(from, dir, travelled, next, min_class);
        }

        travelled = next;
    }

    return length;
}

/* How far the camera may hang back along `dir` before it would sit in a
 * wall (returned), and before it would sit inside a piece of furniture too
 * thick to look through (`prop_out`, never more than the wall limit). */
static float OtsFreeBoom(const sceVu0FVECTOR from, const sceVu0FVECTOR dir,
                         float length, float* prop_out)
{
    sceVu0FVECTOR p;
    float wall;
    float boom;
    float back;
    float front;

    wall = OtsFirstHit(from, dir, length, OTS_HARD);

    if (wall < length)
    {
        wall -= OTS_WALL_MARGIN;
    }

    boom = wall < 0.0f ? 0.0f : wall;

    /* Thin props along the way are ignored; only care whether the camera
     * itself ends up inside furniture. */
    OtsAlong(p, from, dir, boom);

    *prop_out = boom;

    if (boom <= 0.0f || OtsPointClass(p) != OTS_SOFT)
    {
        return boom;
    }

    /* Measure the prop along the boom: where it starts in front of the
     * camera and where it ends behind it. */
    front = boom;
    while (front > 0.0f)
    {
        front -= OTS_PROBE_STEP;
        OtsAlong(p, from, dir, front < 0.0f ? 0.0f : front);

        if (OtsPointClass(p) == OTS_FREE)
        {
            break;
        }
    }

    back = boom;
    while (back < boom + OTS_THIN_PROP)
    {
        back += OTS_PROBE_STEP;
        OtsAlong(p, from, dir, back);

        if (OtsPointClass(p) == OTS_FREE)
        {
            break;
        }
    }

    if (back - front < OTS_THIN_PROP)
    {
        return boom;
    }

    if (front <= 0.0f)
    {
        *prop_out = 0.0f;
        return boom;
    }

    /* Thick furniture: stop just in front of it. */
    front = OtsRefine(from, dir, front, front + OTS_PROBE_STEP, OTS_SOFT)
            - OTS_WALL_MARGIN * 0.5f;

    *prop_out = front < 0.0f ? 0.0f : front;

    return boom;
}

/* What limited the boom this frame (debug overlay readout). */
static MikuPan_OtsCollisionDebug ots_collision_debug;

static void OtsRecordCollision(const sceVu0FVECTOR from, const sceVu0FVECTOR dir,
                               float wall, float prop)
{
    MikuPan_OtsCollisionDebug* d = &ots_collision_debug;
    float requested = mikupan_over_shoulder_distance;
    float at = -1.0f;

    d->valid = 1;
    d->requested = requested;
    d->wall = wall;
    d->prop = prop;
    d->feet_y = plyr_wrk.move_box.pos[1];
    d->limited = 0;
    d->hit_kind = 0;
    d->hit_boxes = 0;
    d->hit_covering = 0;
    d->boxes = ots_box_count[ots_box_read];

    if (wall < requested - 1.0f)
    {
        d->limited = 1;
        at = wall + OTS_WALL_MARGIN + OTS_PROBE_STEP * 0.5f;
    }
    if (prop < wall - 1.0f)
    {
        d->limited = 2;
        at = prop + OTS_WALL_MARGIN * 0.5f + OTS_PROBE_STEP * 0.5f;
    }
    if (at >= 0.0f)
    {
        sceVu0FVECTOR p;
        MIKUPAN_HIT_DETAIL hit;

        OtsAlong(p, from, dir, at);
        d->hit_y = p[1];
        if (GetPointRoomNo((u_short)p[0], (u_short)p[2]) == 0xff)
        {
            memset(&hit, 0, sizeof(hit));
            hit.kind = 4; /* outside every room */
        }
        else
        {
            MikuPan_HitPointDetail(p, &hit);
        }
        d->hit_kind = hit.kind;
        d->hit_door_state = hit.kind == 2 ? door_cover_hit_stts : 0;
        if (hit.kind == 1)
        {
            float top = 0.0f;

            d->hit_boxes = OtsSmallPiecesAt(p, hit.room_id, hit.area, &top,
                                            &d->hit_covering);
            d->hit_box_top = d->feet_y - top;
        }
        d->hit_room = hit.room_id;
        d->hit_area = hit.area;
        d->hit_xz_only = hit.furn_xz_only;
        d->hit_furn_id = hit.furn_id;
        d->hit_furn_top = hit.furn_top;
        d->hit_furn_btm = hit.furn_btm;
    }
}

const MikuPan_OtsCollisionDebug* MikuPan_OverShoulderCollisionDebug(void)
{
    return &ots_collision_debug;
}

/* Fraction of the shoulder offset that stays clear of walls. */
static float OtsFreeShoulder(const sceVu0FVECTOR from, const sceVu0FVECTOR offset)
{
    sceVu0FVECTOR dir;
    float length = sqrtf(offset[0] * offset[0] + offset[2] * offset[2]);
    float free_len;

    if (length <= 1.0f)
    {
        return 1.0f;
    }

    dir[0] = offset[0] / length;
    dir[1] = 0.0f;
    dir[2] = offset[2] / length;
    dir[3] = 0.0f;

    /* Look a margin past the offset so the camera keeps clear of the wall
     * it is beside, not just out of it. */
    free_len = OtsFirstHit(from, dir, length + OTS_SHOULDER_MARGIN, OTS_HARD);

    if (free_len >= length + OTS_SHOULDER_MARGIN)
    {
        return 1.0f;
    }

    free_len -= OTS_SHOULDER_MARGIN;

    return OtsClamp(free_len / length, 0.0f, 1.0f);
}

/* ------------------------------------------------------------------------ */
/* Camera                                                                   */
/* ------------------------------------------------------------------------ */

static void OtsTargetPivot(sceVu0FVECTOR out)
{
    out[0] = plyr_wrk.move_box.pos[0];
    out[1] = plyr_wrk.move_box.pos[1] - mikupan_over_shoulder_pivot_height;
    out[2] = plyr_wrk.move_box.pos[2];
    out[3] = 1.0f;
}

static void OtsForward(sceVu0FVECTOR out)
{
    sceVu0FVECTOR rot = {ots_pitch, ots_yaw, 0.0f, 0.0f};

    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 1.0f;
    out[3] = 0.0f;

    RotFvector(rot, out);
}

static void OtsShoulder(sceVu0FVECTOR out)
{
    sceVu0FVECTOR rot = {0.0f, ots_yaw, 0.0f, 0.0f};

    out[0] = OtsSideSign() * mikupan_over_shoulder_side;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;

    RotFvector(rot, out);
}

/* The camera pose for the current orbit state without any smoothing or
 * collision, used to decide whether a transition blend makes sense. */
static void OtsIdealCamera(sceVu0FVECTOR out_p)
{
    sceVu0FVECTOR pivot;
    sceVu0FVECTOR shoulder;
    sceVu0FVECTOR forward;

    OtsTargetPivot(pivot);
    OtsShoulder(shoulder);
    OtsForward(forward);

    out_p[0] = pivot[0] + shoulder[0] - forward[0] * mikupan_over_shoulder_distance;
    out_p[1] = pivot[1] + shoulder[1] - forward[1] * mikupan_over_shoulder_distance;
    out_p[2] = pivot[2] + shoulder[2] - forward[2] * mikupan_over_shoulder_distance;
    out_p[3] = 1.0f;
}

static int OtsCanResume(void)
{
    sceVu0FVECTOR pos;
    float turn;

    if (!ots_resume_valid)
    {
        return 0;
    }

    OtsCopy(pos, plyr_wrk.move_box.pos);

    if (OtsDist(pos, ots_resume_pos) > OTS_RESUME_MAX_MOVE)
    {
        return 0;
    }

    turn = OtsWrap(plyr_wrk.move_box.rot[1] - ots_resume_facing);

    return fabsf(turn) <= DEG2RAD(OTS_RESUME_MAX_TURN_DEG);
}

static void OtsRememberOrbit(void)
{
    OtsCopy(ots_resume_pos, plyr_wrk.move_box.pos);
    ots_resume_facing = plyr_wrk.move_box.rot[1];
    ots_resume_valid = 1;
}

static void OtsActivate(void)
{
    sceVu0FVECTOR ideal;

    if (OtsCanResume())
    {
        /* Same spot, same facing: carry on with the player's own angle,
         * distance and shoulder slide.  Only the pivot is re-seated. */
        ots_boom_hold = 0;
    }
    else
    {
        ots_guide_frames = 0;
        ots_yaw = plyr_wrk.move_box.rot[1];
        ots_pitch = DEG2RAD(mikupan_over_shoulder_default_pitch_deg);
        OtsClampPitch();
        ots_boom = mikupan_over_shoulder_distance;
        ots_boom_hold = 0;
        ots_shoulder_scale = 1.0f;
        ots_hide_model = 0;
    }

    OtsTargetPivot(ots_pivot);
    ots_pivot_valid = 1;
    ots_look_idle_frames = OTS_AUTO_FOLLOW_DELAY;
    ots_recenter = 0;
    ots_finder_turn_frames = 0;
    ots_blend_frames = 0;

    OtsIdealCamera(ideal);

    if (ots_last_valid
        && OtsDist(ots_last_p, ideal) < OTS_BLEND_MAX_DIST
        && OtsDist(ots_last_p, plyr_wrk.move_box.pos) < OTS_BLEND_MAX_DIST)
    {
        ots_blend_frames = OTS_BLEND_FRAMES;
        OtsCopy(ots_blend_p, ots_last_p);
        OtsCopy(ots_blend_i, ots_last_i);
    }
}

static void OtsCountIdleFrame(void)
{
    if (ots_look_idle_frames < 100000)
    {
        ots_look_idle_frames++;
    }
}

static int OtsUpdateLook(void)
{
    float yaw_in = 0.0f;
    float pitch_in = 0.0f;
    float sens = mikupan_over_shoulder_sensitivity;
    int moved = 0;

    if (pad[0].id == 0x79)
    {
        yaw_in = OtsPadAxis(pad[0].analog[0]);
        pitch_in = OtsPadAxis(pad[0].analog[1]);
    }

    if (yaw_in != 0.0f)
    {
        ots_yaw += yaw_in * DEG2RAD(mikupan_over_shoulder_stick_yaw_speed_deg) * sens;
        moved = 1;
    }

    if (pitch_in != 0.0f)
    {
        /* Stick up is a value below centre.  Make that look up by default. */
        pitch_in = -pitch_in;

        if (MikuPan_KeyProfileUsesFinderReverseY())
        {
            pitch_in = -pitch_in;
        }

        ots_pitch += pitch_in * DEG2RAD(mikupan_over_shoulder_stick_pitch_speed_deg) * sens;
        moved = 1;
    }

    if (MikuPan_FinderMouseLookEnabled())
    {
        float mdx = 0.0f;
        float mdy = 0.0f;

        MikuPan_FinderMouseRequest();

        if (MikuPan_FinderMouseConsume(&mdx, &mdy))
        {
            float mouse_sens =
                MikuPan_FinderMouseSensitivity() * OTS_MOUSE_RAD_PER_PIXEL * sens;
            float pitch = -mdy * mouse_sens;

            if (MikuPan_KeyProfileUsesFinderReverseY())
            {
                pitch = -pitch;
            }

            ots_yaw += mdx * mouse_sens;
            ots_pitch += pitch;
            moved = 1;
        }
    }

    ots_yaw = OtsWrap(ots_yaw);
    OtsClampPitch();

    if (moved)
    {
        ots_look_idle_frames = 0;
        ots_recenter = 0;
    }
    else
    {
        OtsCountIdleFrame();
    }

    return moved;
}

static void OtsUpdateRecenter(void)
{
    float delta;

    if (R3_PRESSED() == 1)
    {
        ots_recenter = 1;
    }

    if (!ots_recenter)
    {
        return;
    }

    delta = OtsWrap(plyr_wrk.move_box.rot[1] - ots_yaw);
    ots_yaw = OtsWrap(ots_yaw + delta * OTS_RECENTER_RATE);
    ots_pitch += (DEG2RAD(mikupan_over_shoulder_default_pitch_deg) - ots_pitch)
                 * OTS_RECENTER_RATE;
    OtsClampPitch();

    if (fabsf(delta) < DEG2RAD(0.5f))
    {
        ots_recenter = 0;
    }
}

static void OtsAutoFollow(void)
{
    float delta;

    if (!mikupan_over_shoulder_auto_follow_enabled || ots_recenter
        || ots_guide_frames > 0)
    {
        return;
    }

    if (ots_look_idle_frames < OTS_AUTO_FOLLOW_DELAY)
    {
        return;
    }

    /* Only while Miku is actually walking or running, and never while she
     * is spinning round, otherwise the camera would chase her turn. */
    if ((plyr_wrk.mvsta & 0xf) == 0 || (plyr_wrk.mvsta & 0x3000000) != 0)
    {
        return;
    }

    delta = OtsWrap(plyr_wrk.move_box.rot[1] - ots_yaw);

    /* Walking towards the camera is a deliberate choice; leave it alone. */
    if (fabsf(delta) > DEG2RAD(135.0f))
    {
        return;
    }

    ots_yaw = OtsWrap(ots_yaw + delta * OTS_AUTO_FOLLOW_RATE);
}

static void OtsStartGuide(const sceVu0FVECTOR target)
{
    sceVu0FVECTOR from;
    float dx;
    float dy;
    float dz;
    float horiz;
    float yaw;
    float pitch;
    float fov;

    OtsTargetPivot(from);

    dx = target[0] - from[0];
    dy = target[1] - from[1];
    dz = target[2] - from[2];
    horiz = sqrtf(dx * dx + dz * dz);

    /* Shots that simply watch Miku leave nothing to turn towards. */
    if (horiz < OTS_GUIDE_MIN_DIST)
    {
        return;
    }

    /* Positive pitch looks up, and world Y points down. */
    yaw = SgAtan2f(dx, dz);
    pitch = OtsClamp(SgAtan2f(-dy, horiz), OtsPitchMin(), OtsPitchMax());

    fov = DEG2RAD(mikupan_over_shoulder_fov_deg);

    if (fabsf(OtsWrap(yaw - ots_yaw)) < fov * OTS_GUIDE_VISIBLE_YAW
        && fabsf(pitch - ots_pitch) < fov * OTS_GUIDE_VISIBLE_PITCH)
    {
        return;
    }

    ots_guide_yaw = yaw;
    ots_guide_pitch = pitch;
    ots_guide_frames = OTS_GUIDE_FRAMES;
    ots_recenter = 0;
    ots_look_idle_frames = 0;
}

static void OtsUpdateGuide(int player_looked)
{
    float dyaw;
    float dpitch;

    if (ots_guide_frames <= 0)
    {
        return;
    }

    if (player_looked)
    {
        ots_guide_frames = 0;
        return;
    }

    dyaw = OtsWrap(ots_guide_yaw - ots_yaw);
    dpitch = ots_guide_pitch - ots_pitch;

    ots_yaw = OtsWrap(ots_yaw + dyaw * OTS_GUIDE_RATE);
    ots_pitch += dpitch * OTS_GUIDE_RATE;
    OtsClampPitch();

    ots_guide_frames--;

    if (fabsf(dyaw) < DEG2RAD(OTS_GUIDE_DONE_DEG)
        && fabsf(dpitch) < DEG2RAD(OTS_GUIDE_DONE_DEG))
    {
        ots_guide_frames = 0;
    }
}

/* Watches for the game switching to a scripted drama camera and, in Guide
 * mode, turns towards what it frames once per shot. */
static void OtsWatchDrama(void)
{
    const CAMERA_DEBUG_PATH* shot = OtsDramaShot();

    if (shot == NULL)
    {
        ots_drama_prev = 0;
        ots_drama_no = 0xffff;
        return;
    }

    if ((!ots_drama_prev || shot->no != ots_drama_no)
        && mikupan_over_shoulder_scare_camera == MIKUPAN_SCARE_CAMERA_GUIDE)
    {
        OtsStartGuide(shot->target_i);
    }

    ots_drama_prev = 1;
    ots_drama_no = shot->no;
}

/* Raising the Camera Obscura: turn Miku towards where the camera looks so the
 * viewfinder opens on what the player was looking at. */
static void OtsHandleFinderIn(void)
{
    MOVE_BOX* mb = &plyr_wrk.move_box;

    if (plyr_wrk.mode != PMODE_FINDER_IN)
    {
        ots_finder_turn_frames = 0;
        return;
    }

    if (ots_prev_plyr_mode != PMODE_FINDER_IN)
    {
        float delta = OtsWrap(ots_yaw - mb->rot[1]);

        ots_finder_turn_frames = OTS_FINDER_TURN_FRAMES;
        ots_finder_turn_step = delta / (float)OTS_FINDER_TURN_FRAMES;

        /* Cancel the raise animation's own slow turn; we handle it. */
        mb->rspd[1] = 0.0f;

        plyr_wrk.frot_x = OtsClamp(ots_pitch, DEG2RAD(-45.0f), DEG2RAD(45.0f));
    }

    if (ots_finder_turn_frames > 0)
    {
        mb->rot[1] = OtsWrap(mb->rot[1] + ots_finder_turn_step);
        ots_finder_turn_frames--;
    }
}

static void OtsBuildCamera(void)
{
    sceVu0FVECTOR target;
    sceVu0FVECTOR shoulder;
    sceVu0FVECTOR forward;
    sceVu0FVECTOR back;
    sceVu0FVECTOR start;
    float follow;
    float allowed;
    float prop_allowed;
    float min_y;
    float max_y;

    OtsTargetPivot(target);

    /* Ease the pivot so the camera lags a touch behind Miku, but snap when
     * she is moved a long way in one frame (doors, teleports). */
    follow = 1.0f - OtsClamp(mikupan_over_shoulder_smoothing, 0.0f, 0.9f);

    if (!ots_pivot_valid || OtsDist(ots_pivot, target) > 400.0f)
    {
        OtsCopy(ots_pivot, target);
        ots_pivot_valid = 1;
    }
    else
    {
        OtsLerp(ots_pivot, ots_pivot, target, follow);
    }

    OtsShoulder(shoulder);
    OtsForward(forward);

    back[0] = -forward[0];
    back[1] = -forward[1];
    back[2] = -forward[2];
    back[3] = 0.0f;

    {
        float scale_target = 1.0f;

        if (mikupan_over_shoulder_collision_enabled)
        {
            scale_target = OtsFreeShoulder(ots_pivot, shoulder);
        }

        /* The smoothed offset eases towards the clear fraction, quickly in
         * and slowly out.  The camera uses whichever is smaller, so it is
         * always clear of the wall yet never pops back out. */
        if (scale_target < ots_shoulder_scale)
        {
            ots_shoulder_scale +=
                (scale_target - ots_shoulder_scale) * OTS_SHOULDER_PULL_RATE;
        }
        else
        {
            ots_shoulder_scale +=
                (scale_target - ots_shoulder_scale) * OTS_SHOULDER_EASE_OUT;
        }

        ots_shoulder_scale = OtsClamp(ots_shoulder_scale, 0.0f, 1.0f);

        {
            float scale = ots_shoulder_scale < scale_target ? ots_shoulder_scale
                                                            : scale_target;

            shoulder[0] *= scale;
            shoulder[1] *= scale;
            shoulder[2] *= scale;
        }
    }

    sceVu0AddVector(start, ots_pivot, shoulder);
    start[3] = 1.0f;

    allowed = mikupan_over_shoulder_distance;
    prop_allowed = allowed;

    if (mikupan_over_shoulder_collision_enabled)
    {
        allowed = OtsFreeBoom(start, back, allowed, &prop_allowed);
    }
    OtsRecordCollision(start, back, allowed, prop_allowed);

    if (ots_boom > allowed)
    {
        /* A wall: never let the camera end up behind it, or the room beyond
         * would clip into view. */
        ots_boom = allowed;
        ots_boom_hold = OTS_BOOM_HOLD_FRAMES;
    }

    if (prop_allowed < ots_boom)
    {
        /* Furniture: glide in rather than snap. */
        ots_boom += (prop_allowed - ots_boom) * OTS_BOOM_PROP_PULL_RATE;
        ots_boom_hold = OTS_BOOM_HOLD_FRAMES;
    }
    else if (ots_boom_hold > 0)
    {
        ots_boom_hold--;
    }
    else
    {
        ots_boom += (prop_allowed - ots_boom) * OTS_BOOM_EASE_OUT;
    }

    camera.p[0] = start[0] + back[0] * ots_boom;
    camera.p[1] = start[1] + back[1] * ots_boom;
    camera.p[2] = start[2] + back[2] * ots_boom;
    camera.p[3] = 1.0f;
    ots_collision_debug.boom = ots_boom;
    ots_collision_debug.camera_y = camera.p[1];

    /* World Y points down: keep the camera above the floor and under the
     * ceiling whatever the pitch. */
    min_y = plyr_wrk.move_box.pos[1] - OTS_MAX_HEIGHT;
    max_y = plyr_wrk.move_box.pos[1] - OTS_MIN_HEIGHT;
    camera.p[1] = OtsClamp(camera.p[1], min_y, max_y);

    camera.i[0] = camera.p[0] + forward[0] * OTS_LOOK_DISTANCE;
    camera.i[1] = camera.p[1] + forward[1] * OTS_LOOK_DISTANCE;
    camera.i[2] = camera.p[2] + forward[2] * OTS_LOOK_DISTANCE;
    camera.i[3] = 1.0f;

    camera.roll = PI;
    camera.fov = DEG2RAD(mikupan_over_shoulder_fov_deg);

    plyr_wrk.prot = ots_yaw;

    if (ots_boom < OTS_HIDE_MODEL_BOOM)
    {
        ots_hide_model = 1;
    }
    else if (ots_boom > OTS_SHOW_MODEL_BOOM)
    {
        ots_hide_model = 0;
    }
}

static void OtsApplyBlend(void)
{
    float t;

    ots_blend_hide = 0;

    if (ots_blend_frames <= 0)
    {
        return;
    }

    t = (float)ots_blend_frames / (float)OTS_BLEND_FRAMES;
    t = t * t;

    OtsLerp(camera.p, camera.p, ots_blend_p, t);
    OtsLerp(camera.i, camera.i, ots_blend_i, t);

    ots_blend_hide = OtsDist(camera.p, ots_pivot) < 220.0f;

    ots_blend_frames--;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int MikuPan_OverShoulderEnabled(void)
{
    return mikupan_over_shoulder_enabled;
}

int MikuPan_OverShoulderActive(void)
{
    return ots_active;
}

void MikuPan_OverShoulderSetEnabled(int enabled)
{
    enabled = enabled ? 1 : 0;

    if (enabled && !mikupan_over_shoulder_enabled)
    {
        ots_was_active = 0;
        ots_last_valid = 0;
        ots_resume_valid = 0;
        ots_guide_frames = 0;
        ots_drama_prev = 0;
        ots_move_latched = 0;
        ots_move_src_prev = -1;
    }

    mikupan_over_shoulder_enabled = enabled;

    if (!enabled)
    {
        ots_active = 0;
        ots_was_active = 0;
        ots_hide_model = 0;
        ots_blend_hide = 0;
    }
}

void MikuPan_OverShoulderToggle(void)
{
    MikuPan_OverShoulderSetEnabled(!mikupan_over_shoulder_enabled);
}

void MikuPan_OverShoulderBeginFrame(void)
{
    OtsSwapBoxes();

    /* CameraMain() can hand the frame to a special camera before reaching
     * us; treat that like any other frame we did not own. */
    if (!ots_ctrl_ran)
    {
        ots_was_active = 0;
    }

    ots_ctrl_ran = 0;
    ots_active = 0;

    OtsCopy(ots_last_p, camera.p);
    OtsCopy(ots_last_i, camera.i);
    ots_last_valid = 1;
}

int MikuPan_OverShoulderCameraCtrl(void)
{
    ots_ctrl_ran = 1;

    if (!OtsCanOwnCamera())
    {
        ots_active = 0;
        ots_was_active = 0;
        ots_finder_turn_frames = 0;
        ots_guide_frames = 0;
        ots_prev_plyr_mode = plyr_wrk.mode;

        /* Remember the shot so returning to it does not guide again. */
        OtsWatchDrama();
        return 0;
    }

    ots_active = 1;

    if (!ots_was_active)
    {
        OtsActivate();
    }

    OtsHandleFinderIn();
    OtsWatchDrama();

    if (OtsCanOwnGameplay())
    {
        int looked = OtsUpdateLook();

        OtsUpdateGuide(looked);
        OtsUpdateRecenter();
        OtsAutoFollow();
    }
    else
    {
        /* Events: no player control, but still turn towards a scare. */
        OtsUpdateGuide(0);
        OtsCountIdleFrame();
    }

    OtsBuildCamera();
    OtsApplyBlend();

    ots_was_active = 1;
    ots_prev_plyr_mode = plyr_wrk.mode;
    OtsRememberOrbit();

    return 1;
}

/* The left stick walks Miku relative to the camera on screen: ours, or an
 * authored shot the Scare Camera setting cut to.  Across a cut the old
 * heading is held until the stick is released, as the original game did. */
static void OtsUpdateMoveReference(int stick_held)
{
    int source = ots_active ? 0 : 1;
    float reference;

    if (source == 0)
    {
        reference = ots_yaw;
    }
    else
    {
        reference = GetTrgtRotY(camera.p, camera.i);
    }

    if (ots_move_src_prev >= 0 && source != ots_move_src_prev && stick_held)
    {
        ots_move_latched = 1;
    }

    if (!stick_held)
    {
        ots_move_latched = 0;
    }

    if (!ots_move_latched)
    {
        ots_move_ref = reference;
    }

    ots_move_src_prev = source;
}

int MikuPan_OverShoulderMoveCtrl(MOVE_BOX* mb, sceVu0FVECTOR tv)
{
    float r;
    float desired;
    float delta;
    float turn_step;
    u_char run_chk;
    sceVu0FVECTOR fr = {0.0f, 0.0f, 0.0f, 0.0f};

    tv[0] = 0.0f;
    tv[1] = 0.0f;
    tv[2] = 0.0f;
    tv[3] = 0.0f;

    if (!OtsCanOwnGameplay())
    {
        return 0;
    }

    r = GetMovePad(0);
    OtsUpdateMoveReference(r != 10.0f);
    run_chk = PlyrMoveStaChk(r);
    PlyrCollisionSetInputState(r, run_chk, pad[0].an_dir[0]);
    GetMoveSpeed(tv);

    /* A quick turn is still playing out: finish it before taking new input,
     * exactly like the original control scheme. */
    if (plyr_wrk.mvsta & 0x3000000)
    {
        if (OtsPlayerMoveMotionLocks())
        {
            plyr_wrk.mvsta &= ~(0x8 | 0x4 | 0x2 | 0x1);

            tv[0] = 0.0f;
            tv[1] = 0.0f;
            tv[2] = 0.0f;
            tv[3] = 0.0f;
        }

        if (mb->loop)
        {
            mb->loop -= 1;
            mb->rot[1] = OtsWrap(mb->rot[1] + mb->rspd[1]);
            RotFvector(mb->rot, tv);
        }
        else
        {
            mb->rspd[1] = 0.0f;
            plyr_wrk.mvsta &= ~0x3000000;
        }

        return 1;
    }

    if (r == 10.0f)
    {
        /* No direction held.  Scripted moves (stairs, ledges) and running on
         * the spot with the run button keep going the way Miku faces. */
        if ((tv[0] != 0.0f || tv[1] != 0.0f || tv[2] != 0.0f)
            && (run_chk != 0 || (plyr_wrk.mvsta & 0xfff0) != 0))
        {
            RotFvector(mb->rot, tv);
        }

        return 1;
    }

    if (plyr_wrk.mvsta & 0x200000)
    {
        /* The animation drives the move; GetMovePad() already returned
         * Miku's own heading. */
        RotFvector(mb->rot, tv);
        return 1;
    }

    /* Which way the player pushes, as seen through the camera on screen. */
    desired = OtsWrap(r + ots_move_ref);

    if (plyr_wrk.mvsta & 0x100000)
    {
        if ((PlyrGetStepDirection() & 1) == 0)
        {
            tv[2] = 0.0f;
        }
        else
        {
            tv[0] = 0.0f;
        }
    }

    fr[1] = desired;
    RotFvector(fr, tv);

    delta = OtsWrap(desired - mb->rot[1]);
    turn_step = DEG2RAD(mikupan_over_shoulder_turn_speed_deg);

    if (fabsf(delta) > DEG2RAD(120.0f) && (plyr_wrk.mvsta & 0xfff0) == 0)
    {
        CngPlyrRotRapid(mb, desired);

        if (OtsPlayerMoveMotionLocks())
        {
            tv[0] = 0.0f;
            tv[1] = 0.0f;
            tv[2] = 0.0f;
            tv[3] = 0.0f;

            plyr_wrk.mvsta &= ~(0x8 | 0x4 | 0x2 | 0x1);
        }

        plyr_wrk.mvsta &= ~0x800000;
        plyr_wrk.mvsta |= 0x1000000;
    }
    else if (plyr_wrk.mvsta & 0xfff0)
    {
        plyr_wrk.mvsta &= ~(0x1000000 | 0x800000);
    }
    else if (fabsf(delta) > turn_step)
    {
        mb->rot[1] = OtsWrap(mb->rot[1] + (delta > 0.0f ? turn_step : -turn_step));

        plyr_wrk.mvsta &= ~0x1000000;
        plyr_wrk.mvsta |= 0x800000;
    }
    else
    {
        mb->rot[1] = desired;

        plyr_wrk.mvsta &= ~(0x1000000 | 0x800000);
    }

    return 1;
}

int MikuPan_OverShoulderSpotMoveCtrl(void)
{
    float step = DEG2RAD(2.5f);
    float pitch_target;
    float yaw_target;

    if (!OtsCanOwnGameplay())
    {
        return 0;
    }

    /* The flashlight follows the camera: tilt up with it (the original never
     * tilts it below level) and swing it towards where the camera looks. */
    pitch_target = OtsClamp(ots_pitch, 0.0f, DEG2RAD(45.0f));

    if (plyr_wrk.mvsta & 0x200000)
    {
        yaw_target = 0.0f;
    }
    else
    {
        yaw_target = OtsClamp(OtsWrap(ots_yaw - plyr_wrk.move_box.rot[1]),
                              DEG2RAD(-40.0f), DEG2RAD(40.0f));
    }

    plyr_wrk.spot_rot[0] = OtsApproach(plyr_wrk.spot_rot[0], pitch_target, step);
    plyr_wrk.spot_rot[1] = OtsApproach(plyr_wrk.spot_rot[1], yaw_target, step);

    return 1;
}

int MikuPan_OverShoulderOwnsMovement(void)
{
    return OtsCanOwnGameplay();
}

int MikuPan_OverShoulderShouldHidePlayerModel(void)
{
    return ots_active && (ots_hide_model || ots_blend_hide);
}
