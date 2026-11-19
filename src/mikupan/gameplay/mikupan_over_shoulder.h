#ifndef MIKUPAN_MIKUPAN_OVER_SHOULDER_H
#define MIKUPAN_MIKUPAN_OVER_SHOULDER_H

#include "typedefs.h"
#include "ingame/enemy/move_types.h"

/* Modern over-the-shoulder follow camera.
 *
 * The original game frames every room with fixed, pre-authored camera angles.
 * This mode replaces them while exploring with a camera that hangs behind and
 * slightly beside Miku, orbits with the right stick or the mouse, and pulls in
 * against walls, doors and furniture so the view is never blocked.  Movement
 * becomes camera-relative: the left stick walks Miku where you push as seen
 * through this camera, and she turns to face that direction.
 *
 * The Camera Obscura keeps its own first-person viewfinder.  Raising it turns
 * Miku towards wherever this camera is looking so she aims where you look, and
 * lowering it pulls the camera back over her shoulder again.  Cutscenes,
 * apparition events, save points and the original fixed cameras they rely on
 * are untouched.
 */

/* What to do when the game switches to one of its scripted "drama" cameras,
 * the authored angles that frame scares and other staged moments. */
#define MIKUPAN_SCARE_CAMERA_GUIDE    (0) /* stay, but turn to face it if unseen */
#define MIKUPAN_SCARE_CAMERA_ORIGINAL (1) /* cut to the authored shot */
#define MIKUPAN_SCARE_CAMERA_OFF      (2) /* ignore it */
#define MIKUPAN_SCARE_CAMERA_COUNT    (3)

extern int mikupan_over_shoulder_enabled;
extern int mikupan_over_shoulder_scare_camera;
extern int mikupan_over_shoulder_side_left;
extern int mikupan_over_shoulder_collision_enabled;
extern int mikupan_over_shoulder_auto_follow_enabled;
extern float mikupan_over_shoulder_distance;
extern float mikupan_over_shoulder_pivot_height;
extern float mikupan_over_shoulder_side;
extern float mikupan_over_shoulder_fov_deg;
extern float mikupan_over_shoulder_sensitivity;
extern float mikupan_over_shoulder_stick_yaw_speed_deg;
extern float mikupan_over_shoulder_stick_pitch_speed_deg;
extern float mikupan_over_shoulder_default_pitch_deg;
extern float mikupan_over_shoulder_smoothing;
extern float mikupan_over_shoulder_turn_speed_deg;

int MikuPan_OverShoulderEnabled(void);
int MikuPan_OverShoulderActive(void);
void MikuPan_OverShoulderSetEnabled(int enabled);
void MikuPan_OverShoulderToggle(void);

/* Called at the top of CameraMain() before anything touches the camera. */
void MikuPan_OverShoulderBeginFrame(void);

/* Builds the camera for this frame.  Returns non-zero when it owns the
 * camera, in which case the caller must not run any other follow camera. */
int MikuPan_OverShoulderCameraCtrl(void);

/* Camera-relative movement.  Returns non-zero when it handled the player's
 * movement for this frame (tv receives the requested world-space move). */
int MikuPan_OverShoulderMoveCtrl(MOVE_BOX* mb, sceVu0FVECTOR tv);

/* Points the flashlight where the camera looks instead of reading the right
 * stick.  Returns non-zero when it handled the flashlight for this frame. */
int MikuPan_OverShoulderSpotMoveCtrl(void);

/* Non-zero while this mode drives movement, so the stock wall-hug turn and
 * other fixed-camera movement helpers must stay out of the way. */
int MikuPan_OverShoulderOwnsMovement(void);

/* Non-zero when the camera has been pulled so close that drawing Miku would
 * only show the inside of her model. */
int MikuPan_OverShoulderShouldHidePlayerModel(void);

/* What limited the camera's distance this frame, for the debug overlay. */
typedef struct {
    int valid;
    float requested;  /* distance asked for */
    float wall;       /* limit from walls, doors and room hit areas */
    float prop;       /* limit from thick furniture */
    float boom;       /* distance used */
    float camera_y;   /* world Y (up is negative) */
    float feet_y;     /* Miku's feet */
    int limited;      /* 0 none, 1 wall, 2 furniture */
    float hit_y;      /* height of the point that limited it */
    unsigned char hit_kind, hit_room, hit_area, hit_xz_only;
    unsigned short hit_furn_id;
    short hit_furn_top, hit_furn_btm;
} MikuPan_OtsCollisionDebug;

const MikuPan_OtsCollisionDebug* MikuPan_OverShoulderCollisionDebug(void);

#endif // MIKUPAN_MIKUPAN_OVER_SHOULDER_H
