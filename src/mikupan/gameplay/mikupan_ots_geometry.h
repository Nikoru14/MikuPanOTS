#ifndef MIKUPAN_MIKUPAN_OTS_GEOMETRY_H
#define MIKUPAN_MIKUPAN_OTS_GEOMETRY_H

/*
 * The room's own triangles, for the over-the-shoulder camera's collision.
 *
 * The game's walking collision is a floor plan: outlines without heights,
 * padded around furniture for Miku's size. A camera that hangs above her
 * shoulder needs the shapes that are drawn instead, so the renderer hands
 * every room and furniture mesh it draws in the main scene to this module
 * (MikuPan_OtsGeomRecordMesh), which keeps each mesh's triangles once, in
 * the mesh's local space, and the frame's draws of them with their world
 * matrices. The camera then casts segments against them.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* A mesh the renderer is about to draw: `key` identifies it (with `top`,
 * the loaded data block it belongs to), `positions` are its vertex
 * positions, `stride` floats apart, and `strips` holds the vertex count of
 * each of its `strip_count` triangle strips, laid out one after another.
 * `model` is the local-to-world matrix (PS2 layout: rows 0-2 are the axes,
 * row 3 the translation). `source` and `detail` say what is drawn (the
 * draw-source kind and its first detail, e.g. a furniture or door number).
 * Only called for scenery in the main scene, not for reflections, shadows
 * or characters. */
void MikuPan_OtsGeomRecordMesh(const void *key, const void *top,
                               const float *positions, int stride,
                               const int *strips, int strip_count,
                               const float model[4][4], int source, int detail);

/* Non-zero when the renderer should hand the mesh it is drawing over:
 * collection is on and this mesh is not already recorded for this frame. */
int MikuPan_OtsGeomCollecting(void);

/* Turns collection on or off (the camera turns it on while it is active). */
void MikuPan_OtsGeomSetCollecting(int on);

/* The draws recorded while the last frame was drawn become the ones
 * MikuPan_OtsGeomCast tests; call once per frame before casting. */
void MikuPan_OtsGeomNextFrame(void);

/* Number of drawn pieces known for casting this frame. */
int MikuPan_OtsGeomPieceCount(void);

/* What a cast hit. */
typedef struct {
    float distance;     /* along the segment, or the length when clear */
    int hit;            /* non-zero when something was hit */
    int source;         /* draw-source kind of what was hit */
    int detail;         /* its detail (furniture / door number, room...) */
    int pieces_tested;  /* pieces whose bounds the segment crossed */
    int triangles_tested;
} MikuPan_OtsGeomHit;

/* Casts a sphere of `radius` (approximated by five parallel rays) from
 * `from` along unit vector `dir` for `length`, against the recorded
 * scenery. Pieces thinner than a rope or a pole across are ignored. */
void MikuPan_OtsGeomCast(const float from[3], const float dir[3], float length,
                         float radius, MikuPan_OtsGeomHit *out);

#ifdef __cplusplus
}
#endif

#endif // MIKUPAN_MIKUPAN_OTS_GEOMETRY_H
