// teapot.c -- /bin/teapot: the Utah teapot, spinning in an Emacs buffer.
// =====================================================================
// TinyGL (software OpenGL 1.x, vendored under user/tinygl/) renders into
// its ZBuffer; we tessellate the classic Newell Bezier patches ourselves
// (TinyGL has no evaluators) and copy each frame into the shared-memory
// canvas (run-in-buffer hands us the handle in argv). 1975's teapot, on a
// 2026 hobby Lisp machine, lit by hardware floats from the FPU phase.
#include "ulib.h"
#include <GL/gl.h>
#include <zbuffer.h>
#include "teapot_data.h"

#define TESS 6   // segments per patch edge: 32 patches * 6*6*2 = 2304 triangles

static long parse(const char *s)
{ long v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; } return v; }

// Evaluate a bicubic Bezier patch (4x4 control points) at (u, v):
// position via the Bernstein basis, normal via the cross of the partials.
static void bez_point(const float cp[4][4][3], float u, float v,
                      float *out, float *nrm)
{
    float bu[4]  = { (1-u)*(1-u)*(1-u), 3*u*(1-u)*(1-u), 3*u*u*(1-u), u*u*u };
    float bv[4]  = { (1-v)*(1-v)*(1-v), 3*v*(1-v)*(1-v), 3*v*v*(1-v), v*v*v };
    float du[4]  = { -3*(1-u)*(1-u), 3*(1-u)*(1-3*u), 3*u*(2-3*u), 3*u*u };
    float dv[4]  = { -3*(1-v)*(1-v), 3*(1-v)*(1-3*v), 3*v*(2-3*v), 3*v*v };
    float tu[3] = {0,0,0}, tv[3] = {0,0,0};
    for (int k = 0; k < 3; k++) { out[k] = 0; }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            for (int k = 0; k < 3; k++) {
                out[k] += cp[i][j][k] * bu[i] * bv[j];
                tu[k]  += cp[i][j][k] * du[i] * bv[j];
                tv[k]  += cp[i][j][k] * bu[i] * dv[j];
            }
        }
    }
    nrm[0] = tu[1]*tv[2] - tu[2]*tv[1];
    nrm[1] = tu[2]*tv[0] - tu[0]*tv[2];
    nrm[2] = tu[0]*tv[1] - tu[1]*tv[0];
    // Normalize: GL's lighting assumes unit normals, and unnormalized Bezier
    // partials vary wildly in magnitude (hence blown-out highlights).
    float len = __builtin_sqrtf(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
    if (len > 1e-6f) { nrm[0] /= len; nrm[1] /= len; nrm[2] /= len; }
}

static void draw_teapot(void)
{
    glBegin(GL_TRIANGLES);
    for (unsigned p = 0; p < sizeof(patchdata)/sizeof(patchdata[0]); p++) {
        float cp[4][4][3];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                for (int k = 0; k < 3; k++)
                    cp[i][j][k] = cpdata[patchdata[p][i*4+j] - 1][k];
        for (int a = 0; a < TESS; a++) {
            for (int b = 0; b < TESS; b++) {
                float pt[4][3], nm[4][3];
                float u0 = (float)a/TESS, u1 = (float)(a+1)/TESS;
                float v0 = (float)b/TESS, v1 = (float)(b+1)/TESS;
                bez_point(cp, u0, v0, pt[0], nm[0]);
                bez_point(cp, u1, v0, pt[1], nm[1]);
                bez_point(cp, u1, v1, pt[2], nm[2]);
                bez_point(cp, u0, v1, pt[3], nm[3]);
                int idx[6] = { 0, 1, 2, 0, 2, 3 };
                for (int t = 0; t < 6; t++) {
                    glNormal3f(nm[idx[t]][0], nm[idx[t]][1], nm[idx[t]][2]);
                    glVertex3f(pt[idx[t]][0], pt[idx[t]][1], pt[idx[t]][2]);
                }
            }
        }
    }
    glEnd();
}

int umain(int argc, char **argv)
{
    if (argc < 4) { sys_write(1, "teapot: need handle w h\n", 24); return 1; }
    int handle = (int)parse(argv[1]);
    int w = (int)parse(argv[2]), h = (int)parse(argv[3]);
    unsigned int *canvas = shm_map(handle);
    if (!canvas) { sys_write(1, "teapot: shm_map failed\n", 23); return 1; }

    ZBuffer *zb = ZB_open(w, h, ZB_MODE_RGBA, 0);
    if (!zb) { sys_write(1, "teapot: ZB_open failed\n", 23); return 1; }
    glInit(zb);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    static GLfloat pos[4] = { 3.0f, 4.0f, 5.0f, 0.0f };
    static GLfloat amb[4] = { 0.25f, 0.15f, 0.10f, 1.0f };
    static GLfloat dif[4] = { 0.85f, 0.55f, 0.25f, 1.0f };   // brass-ish
    static GLfloat spc[4] = { 1.0f, 1.0f, 0.9f, 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, amb);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, dif);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spc);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 25.0f);
    glEnable(GL_COLOR_MATERIAL);
    glShadeModel(GL_SMOOTH);

    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -0.75, 0.75, 2.0, 60.0);
    glClearColor(0.07f, 0.08f, 0.08f, 1.0f);

    sys_write(1, "teapot: spinning\n", 17);
    float angle = 0.0f;
    for (;;) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, -1.4f, -10.0f);
        glRotatef(angle, 0.0f, 1.0f, 0.0f);
        glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);   // the data is z-up
        draw_teapot();

        // TinyGL's 32-bit buffer and our canvas are both 0x..RRGGBB words.
        unsigned int *src = (unsigned int *)zb->pbuf;
        for (int i = 0; i < w * h; i++) { canvas[i] = src[i]; }

        angle += 3.0f;
        sys_sleep(40);                          // ~25 fps
    }
}
