#ifndef INGAME_MAP_MAP_HTCK_H
#define INGAME_MAP_MAP_HTCK_H

#include "typedefs.h"

u_char PosInAreaJudgeSub(int *addr, u_short pos_x, u_short pos_y, u_char type);
void Get2DLinearConstFromPos(float *a, float *b, float *c, float x0, float y0, float x1, float y1);
int Get2DLinearCrossPoint(float *cpx, float *cpy, float a0, float b0, float c0, float a1, float b1, float c1);
int ChkLineCross(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3);
int ChkInsideBox(float cpx, float cpy, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3);
u_char HitChkSegment2All(sceVu0FVECTOR f, sceVu0FVECTOR t, float deg);
u_char HitChkSegment2All2D(sceVu0FVECTOR f, sceVu0FVECTOR t, float deg);
u_char FurnCoverCheck(u_short pos_x, short int pos_y, u_short pos_z, u_char room_no);

/// What HitChkSegment2All would report for one point, and why: for the
/// over-the-shoulder camera's collision readout in the debug overlay.
typedef struct {
    u_char kind;        /* 0 clear, 1 room hit area, 2 door, 3 furniture */
    u_char room_id;     /* room the hit belongs to */
    u_char area;        /* room hit area index (kind 1) */
    u_short furn_id;    /* furniture id (kind 3, or furn_xz_only) */
    short furn_top;     /* its height range: hit when -top <= y <= -btm */
    short furn_btm;
    u_char furn_xz_only; /* inside a furniture outline but outside its heights */
} MIKUPAN_HIT_DETAIL;

void MikuPan_HitPointDetail(const sceVu0FVECTOR p, MIKUPAN_HIT_DETAIL* out);
#endif // INGAME_MAP_MAP_HTCK_H
