#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#include <sched.h>
#include <pthread.h>
#include <sys/time.h>
#include <algorithm>
#ifndef H_GAPI_SW
#define H_GAPI_SW

#include "core.h"

#define PROFILE_MARKER(title)
#define PROFILE_LABEL(id, name, label)
#define PROFILE_TIMING(time)

//#define DITHER_FILTER

// Miyoo Mini: render straight into the 32-bit screen surface
#if (defined(_OS_LINUX) || defined(_OS_TNS)) && !defined(__MIYOO__)
    #define COLOR_16
#endif

#ifdef COLOR_16
    #if defined(_OS_LINUX) || defined(_OS_TNS)
        #define COLOR_FMT_565
        #define CONV_COLOR(r,g,b) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    #else
        #define COLOR_FMT_555
        #define CONV_COLOR(r,g,b) (((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))
    #endif
#else 
    #define COLOR_FMT_888
    #define CONV_COLOR(r,g,b) ((r << 16) | (g << 8) | b)
#endif

#define SW_MAX_DIST  (20.0f * 1024.0f)
#define SW_FOG_START (12.0f * 1024.0f)

namespace GAPI {

    using namespace Core;

    typedef ::Vertex Vertex;

    #ifdef COLOR_16
        typedef uint16 ColorSW;
    #else
        typedef uint32 ColorSW;
    #endif
    // Depth buffer holds 1/w as a float: its precision is relative at any
    // distance. The original 16-bit z packed most of its precision next to
    // the camera, so close surfaces (Lara's hair and face) z-fought.
    // 16-bit: 1/w * 2^21 (resolution ~0.5 units at 1024 units away). Half
    // the memory traffic of the float buffer, which mattered: this device's
    // memory is slow and both cores share it.
    // Depth: the 24-bit 1/w kept in 16 bits as a tiny float (4-bit exponent,
    // 12-bit mantissa), see depthCodeSW: the same precision (1/4096) at every
    // distance, where the plain top 16 bits lost it far away (z-fighting).
    typedef uint16 DepthSW;

    // monotonic: a larger 1/w (closer) always gives a larger or equal code
    static inline int32 depthCodeSW(int32 z) {
        if (z < 4096) return z;
        const int m = 31 - __builtin_clz(uint32(z));   // 12..23
        return ((m - 11) << 12) | ((z >> (m - 12)) & 0xFFF);
    }

    uint8   *swLightmap;
    uint8   swLightmapNone[32 * 256];
    uint8   swLightmapShade[32 * 256];
    ColorSW *swPalette;
    ColorSW swPaletteColor[256];
    ColorSW swPaletteWater[256];
    ColorSW swPaletteGray[256];
    uint8   swGradient[256];
    Tile8   *curTile;

    uint8 ambient;
    int32 lightsCount;

    struct LightSW {
        uint32 intensity;
        vec3   pos;
        float  radius;
    } lights[MAX_LIGHTS], lightsRel[MAX_LIGHTS];

// Shader
    struct Shader {
        void init(Pass pass, int type, int *def, int defCount) {}
        void deinit() {}
        void bind() {}
        void setParam(UniformType uType, const vec4  &value, int count = 1) {}
        void setParam(UniformType uType, const mat4  &value, int count = 1) {}
    };

// Texture
    void swDrain();
    uint32 swTexVersion = 0;   // unique for every new texture content

    struct Texture {
        uint8      *memory;
        int        width, height, origWidth, origHeight;
        TexFormat  fmt;
        uint32     opt;

        uint32     version;    // changes with the pixels: the background blit cache checks it

        Texture(int width, int height, int depth, uint32 opt) : memory(0), width(width), height(height), origWidth(width), origHeight(height), fmt(FMT_RGBA), opt(opt), version(0) {}

        void init(void *data) {
            ASSERT((opt & OPT_PROXY) == 0);

            opt &= ~(OPT_CUBEMAP | OPT_MIPMAPS);

            memory = new uint8[width * height * 4];
            version = ++swTexVersion;
            if (data) {
                // Two conventions reach this point. Texture::Load creates the
                // texture at its power-of-two size with padded pixels (and fixes
                // origWidth afterwards): copy the whole block. Other callers -
                // the FMV player among them - create it at the image's real size,
                // which the wrapper then rounds up, and pass tightly packed
                // pixels: copy row by row and clear the padding. Copying the
                // whole block there read past the end of the caller's pixels,
                // which crashed on TR2's videos.
                if (origWidth == width && origHeight == height) {
                    memcpy(memory, data, width * height * 4);
                } else {
                    memset(memory, 0, width * height * 4);
                    const uint8 *src = (const uint8*)data;
                    for (int y = 0; y < origHeight; y++) {
                        memcpy(memory + y * width * 4, src + y * origWidth * 4, origWidth * 4);
                    }
                }
            }
        }

        void deinit() {
            swDrain();   // the pending frame may still be reading this texture
            if (memory) {
                delete[] memory;
            }
        }

        void generateMipMap() {}

        void update(void *data) {
            // Match the GL backend (glTexSubImage2D over origWidth x origHeight):
            // callers such as the FMV player pass tightly packed pixels of the
            // image's real size, while the texture itself is rounded up to a
            // power of two. Copying the whole texture in one block skewed every
            // row (and read past the end of the frame), scrambling the videos.
            const uint8 *src = (const uint8*)data;
            for (int y = 0; y < origHeight; y++) {
                memcpy(memory + y * width * 4, src + y * origWidth * 4, origWidth * 4);
            }
            version = ++swTexVersion;
        }

        void bind(int sampler) {
            Core::active.textures[sampler] = this;

            if (!this || (opt & OPT_PROXY)) return;
            ASSERT(memory);

            curTile = NULL;
        }

        void bindTileIndices(Tile8 *tile) {
            curTile = (Tile8*)tile;
        }

        void unbind(int sampler) {}

        void setFilterQuality(int value) {
            if (value > Settings::LOW)
                opt &= ~OPT_NEAREST;
            else
                opt |= OPT_NEAREST;
        }
    };

// Mesh
    struct Mesh {
        Index        *iBuffer;
        GAPI::Vertex *vBuffer;

        int          iCount;
        int          vCount;
        bool         dynamic;

        Mesh(bool dynamic) : iBuffer(NULL), vBuffer(NULL), dynamic(dynamic) {}

        void init(Index *indices, int iCount, ::Vertex *vertices, int vCount, int aCount) {
            this->iCount  = iCount;
            this->vCount  = vCount;

            iBuffer = new Index[iCount];
            vBuffer = new Vertex[vCount];

            update(indices, iCount, vertices, vCount);
        }

        void deinit() {
            delete[] iBuffer;
            delete[] vBuffer;
        }

        void update(Index *indices, int iCount, ::Vertex *vertices, int vCount) {
            if (indices) {
                memcpy(iBuffer, indices, iCount * sizeof(indices[0]));
            }

            if (vertices) {
                memcpy(vBuffer, vertices, vCount * sizeof(vertices[0]));
            }
        }

        void bind(const MeshRange &range) const {}

        void initNextRange(MeshRange &range, int &aIndex) const {
            range.aIndex = -1;
        }
    };


    int cullMode, blendMode;

    ColorSW *swColor;
    DepthSW *swDepth;
    // Mirrors of the core's depth test/write state. The z-buffer below was
    // commented out upstream (likely for speed on weaker targets), which
    // left 3D objects overlapping in draw order instead of by distance.
    bool swDepthTest  = true;
    bool swDepthWrite = true;
    // Set by transform() when every vertex of the batch has w == 1, i.e. an
    // orthographic 2D batch (UI text, bars). Those draw over the scene.
    bool swOrthoBatch = false;
    // Light level and palette folded into one table: the shaded colour for
    // (light level, palette index). Saves a dependent lookup per pixel and
    // fits in the L1 cache (16 KB). Rebuilt once per frame, on clear().
    bool    swShadeDirty = true;
    // Set by the level each frame. The DOS game tinted its palette when the
    // camera was underwater; the GL renderer does it in a shader. Here the
    // tint is baked into swShade, so it costs nothing per pixel.
    bool    swUnderwater = false;
    bool    swShadowBatch = false; // drawing a shadow blob: darken what is below
    bool    swModelInUI   = false; // a 3D model drawn without perspective (picked-up item): not 2D UI
    bool    swBatchWater = false;  // this batch: camera underwater, or geometry in a water room
    bool    swSkyBatch   = false;  // this batch: the TR2 sky (drawn last, only where nothing is)
    float   swWaterTime  = 0.0f;   // seconds, set by the level each frame
    // 2D primitive whose UVs all point at one texel (the white sprite used
    // by frames, backgrounds and bars): fill it with its colour directly.
    bool    swSolidPrim  = false;

    // Frame timing: balances the work between the two cores, and feeds the
    // periodic perf line in the log.
    long long swPerfRaster = 0, swPerfPixels = 0;
    static inline long long swPerfNow() {
        timeval t;
        gettimeofday(&t, NULL);
        return (long long)t.tv_sec * 1000000 + t.tv_usec;
    }
    short4  swClipRect;

    struct VertexSW {
        int32 x, y, z, w;
        int32 u, v, l;
        // u/w, v/w and 1/w: these interpolate linearly across the screen,
        // which is what perspective-correct texturing needs (see drawLine)
        float pu, pv, pq;
        float fx, fy;   // exact projected position, used by the rasterizer
        uint32 color;   // vertex colour RGBA, used by 2D (UI) batches

        inline VertexSW operator + (const VertexSW &p) const {
            VertexSW ret;
            ret.x = x + p.x; ret.y = y; ret.z = z + p.z; ret.w = w + p.w;
            ret.u = u + p.u; ret.v = v + p.v; ret.l = l + p.l;
            ret.pu = pu + p.pu; ret.pv = pv + p.pv; ret.pq = pq + p.pq;
            return ret;
        }

        inline VertexSW operator - (const VertexSW &p) const {
            VertexSW ret;
            ret.x = x - p.x; ret.y = y; ret.z = z - p.z; ret.w = w - p.w;
            ret.u = u - p.u; ret.v = v - p.v; ret.l = l - p.l;
            ret.pu = pu - p.pu; ret.pv = pv - p.pv; ret.pq = pq - p.pq;
            return ret;
        }

        inline VertexSW operator * (const int32 s) const {
            VertexSW ret;
            ret.x = x * s; ret.y = y; ret.z = z * s; ret.w = w * s;
            ret.u = u * s; ret.v = v * s; ret.l = l * s;
            ret.pu = pu * float(s); ret.pv = pv * float(s); ret.pq = pq * float(s);
            return ret;
        }

        inline VertexSW operator / (const int32 s) const {
            VertexSW ret;
            ret.x = x / s; ret.y = y; ret.z = z / s; ret.w = w / s;
            ret.u = u / s; ret.v = v / s; ret.l = l / s;
            ret.pu = pu / float(s); ret.pv = pv / float(s); ret.pq = pq / float(s);
            return ret;
        }
    };

    Array<VertexSW> swVertices;
    Array<Index>    swIndices;
    Array<int32>    swTriangles;
    Array<int32>    swQuads;

    void init() {
        LOG("Renderer : %s\n", "Software");
        LOG("Version  : %s\n", "0.1");
        swDepth = NULL;
    }

    void deinit() {
        delete[] swDepth;
        swVertices.clear();
        swIndices.clear();
        swTriangles.clear();
        swQuads.clear();
    }

    void resize() {
        delete[] swDepth;
        swDepth = new DepthSW[Core::width * Core::height];
    }

    inline mat4::ProjRange getProjRange() {
        return mat4::PROJ_ZERO_POS;
    }

    mat4 ortho(float l, float r, float b, float t, float znear, float zfar) {
        mat4 m;
        m.ortho(getProjRange(), l, r, b, t, znear, zfar);
        return m;
    }

    mat4 perspective(float fov, float aspect, float znear, float zfar, float eye) {
        mat4 m;
        m.perspective(getProjRange(), fov, aspect, znear, zfar, eye);
        return m;
    }

    bool beginFrame() {
        return true;
    }

    void endFrame() {}

    void resetState() {}

    void bindTarget(Texture *texture, int face) {}

    void discardTarget(bool color, bool depth) {}

    void copyTarget(Texture *dst, int xOffset, int yOffset, int x, int y, int width, int height) {}

    void setVSync(bool enable) {}

    void waitVBlank() {}

    void clearImpl(bool color, bool depth);
    void swRecordClear(bool color, bool depth);
    void clear(bool color, bool depth) {
        swShadeDirty = true;
        swRecordClear(color, depth);
    }
    void clearImpl(bool color, bool depth) {
        if (color) {
            memset(swColor, 0x00, Core::width * Core::height * sizeof(ColorSW));
        }

        if (depth) {
            if (swDepth) {
                memset(swDepth, 0, Core::width * Core::height * sizeof(DepthSW)); // 1/w = 0: infinitely far
            }
        }
    }

    void setClearColor(const vec4 &color) {}

    void setViewport(const short4 &v) {}

    void setScissor(const short4 &s) {
        swClipRect.x = s.x;
        swClipRect.y = Core::active.viewport.w - (s.y + s.w);
        swClipRect.z = s.x + s.z;
        swClipRect.w = Core::active.viewport.w - s.y;
    }

    void setDepthTest(bool enable) {
        swDepthTest = enable;
    }

    void setDepthWrite(bool enable) {
        swDepthWrite = enable;
    }

    void setColorWrite(bool r, bool g, bool b, bool a) {}

    void setAlphaTest(bool enable) {}

    void setCullMode(int rsMask) {}

    void setBlendMode(int rsMask) {}

    void setViewProj(const mat4 &mView, const mat4 &mProj) {}

    void updateLights(vec4 *lightPos, vec4 *lightColor, int count) {
        ambient = clamp(int32(active.material.y * 255), 0, 255);

        lightsCount = 0;
        for (int i = 0; i < count; i++) {
            if (lightColor[i].w >= 1.0f) {
                continue;
            }
            LightSW &light = lights[lightsCount++];
            vec4 &c = lightColor[i];
            light.intensity = uint32(((c.x + c.y + c.z) / 3.0f) * 255.0f);
            light.pos    = lightPos[i].xyz();
            light.radius = lightColor[i].w;
        }
    }

    void setFog(const vec4 &params) {}

    bool checkBackface(const VertexSW *a, const VertexSW *b, const VertexSW *c) {
        // Use the exact projected positions. Upstream rounded x and y to whole
        // pixels first, so thin triangles (Lara's braid, fingers, far details)
        // came out with zero or even flipped area and were thrown away.
        return (b->fx - a->fx) * (c->fy - a->fy) -
               (c->fx - a->fx) * (b->fy - a->fy) <= 0.0f;
    }

    inline void sortVertices(VertexSW *&t, VertexSW *&m, VertexSW *&b) {
        if (t->y > m->y) swap(t, m);
        if (t->y > b->y) swap(t, b);
        if (m->y > b->y) swap(m, b);
    }

    inline void sortVertices(VertexSW *&t, VertexSW *&m, VertexSW *&b, VertexSW *&o) {
        if (t->y > m->y) swap(t, m);
        if (o->y > b->y) swap(o, b);
        if (t->y > o->y) swap(t, o);
        if (m->y > b->y) swap(m, b);
        if (m->y > o->y) swap(m, o);
    }

    inline void step(VertexSW &v, const VertexSW &d) {
        //v.w += d.w;
        v.u += d.u;
        v.v += d.v;
        v.l += d.l;
        v.pu += d.pu;
        v.pv += d.pv;
        v.pq += d.pq;
    }

    inline void step(VertexSW &v, const VertexSW &d, int32 count) {
        //v.w += d.w * count;
        v.u += d.u * count;
        v.v += d.v * count;
        v.l += d.l * count;
        v.pu += d.pu * float(count);
        v.pv += d.pv * float(count);
        v.pq += d.pq * float(count);
    }

    // https://www.flipcode.com/archives/Texturing_As_In_Unreal.shtml
    const int uvDither[8] = {
        32768, 16384,           0, 49152,   // (xx yy) for (y & 1 == 0)
        49152,     0,       32768, 16384    // (xx yy) for (y & 1 == 1)
    };

    void drawLine(const VertexSW &L, const VertexSW &R, int32 y) {
        int32 x1 = L.x >> 16;
        int32 x2 = R.x >> 16;

        int32 f = x2 - x1;
        if (f == 0) return;

        VertexSW dS = (R - L) / f;
        VertexSW S  = L;

        if (x1 < swClipRect.x) {
            x1 = swClipRect.x - x1;
            S.z += dS.z * x1;
            step(S, dS, x1);
            x1 = swClipRect.x;
        }
        if (x2 > swClipRect.z) x2 = swClipRect.z;

        int32 i = y * Core::width;

    #ifdef DITHER_FILTER
        const int *dithY = uvDither + ((y & 1) * 4);
    #endif

        // Perspective-correct texturing. Upstream interpolated u/v linearly
        // in screen space (affine, like the PS1), which warped textures on
        // surfaces close to the camera. u/w, v/w and 1/w do interpolate
        // linearly on screen: recover the exact u/v every SW_PERSP_SPAN
        // pixels (one division each) and step linearly in between.
        #define SW_PERSP_SPAN 16
        float curPU = S.pu;
        float curPV = S.pv;
        float curPQ = S.pq;
        int32 endU  = int32(curPU / curPQ);
        int32 endV  = int32(curPV / curPQ);
        int32 texU  = endU, texV = endV;
        int32 texDU = 0, texDV = 0;
        int   spanLeft = 0;

        for (int x = i + x1; x < i + x2; x++) {
            S.z += dS.z;

            if (spanLeft == 0) {
                // Never step past the end of the scanline: extrapolating 1/w
                // beyond the polygon on steep surfaces gave wild u/v values,
                // which landed on transparent texels and left see-through holes.
                int len = (i + x2) - x;
                if (len > SW_PERSP_SPAN) len = SW_PERSP_SPAN;
                texU = endU;
                texV = endV;
                curPU += dS.pu * float(len);
                curPV += dS.pv * float(len);
                curPQ += dS.pq * float(len);
                if (curPQ < 1e-6f) curPQ = 1e-6f;
                endU = int32(curPU / curPQ);
                endV = int32(curPV / curPQ);
                texDU = (endU - texU) / len;
                texDV = (endV - texV) / len;
                spanLeft = len;
            }
            const int32 pixU = texU;
            const int32 pixV = texV;
            texU += texDU;
            texV += texDV;
            spanLeft--;

            DepthSW z = S.pq; // 1/w: larger means closer

            if (!swDepth || !swDepthTest || z >= swDepth[x]) {
            #ifdef DITHER_FILTER
                const int *dithX = dithY + (x & 1);

                int32 u = (pixU + dithX[0]) >> 16;
                int32 v = (pixV + dithX[2]) >> 16;
            #else
                int32 u = pixU >> 16;
                int32 v = pixV >> 16;
            #endif
                // the division can land a hair outside the texture at edges
                if (u < 0) u = 0; else if (u > 255) u = 255;
                if (v < 0) v = 0; else if (v > 255) v = 255;

                uint8 index = curTile->index[(v << 8) + u];

                if (index != 0) {
                    index = swLightmap[((S.l >> (16 + 3)) << 8) + index];

                    swColor[x] = swPalette[index];
                    if (swDepth && swDepthWrite) {
                        swDepth[x] = z;
                    }
                }
            }

            step(S, dS);
        }
    }

    void drawPart(const VertexSW &a, const VertexSW &b, const VertexSW &c, const VertexSW &d) {
        VertexSW L, R, dL, dR;
        int32 minY, maxY;

        int32 f = c.y - a.y;
        dL = (c - a) / f;
        dR = (d - b) / f;

        L = a;
        R = b;

        minY = a.y;
        maxY = c.y;

        if (maxY < swClipRect.y || minY >= swClipRect.w) return;

        if (minY < swClipRect.y) {
            minY = swClipRect.y - minY;
            L.x += dL.x * minY;
            L.z += dL.z * minY;
            R.x += dR.x * minY;
            R.z += dR.z * minY;
            step(L, dL, minY);
            step(R, dR, minY);
            minY = swClipRect.y;
        }

        if (maxY > swClipRect.w) maxY = swClipRect.w;

        for (int y = minY; y < maxY; y++) {
            drawLine(L, R, y);
            L.x += dL.x;
            L.z += dL.z;
            R.x += dR.x;
            R.z += dR.z;
            step(L, dL);
            step(R, dR);
        }
    }

    // ---- Triangle scan conversion ----------------------------------------
    // Upstream walked triangle edges from vertices rounded to whole scanlines,
    // with no sub-pixel correction, and split quads with integer divisions.
    // Neighbouring triangles then disagreed about which pixels they owned:
    // seams opened and closed as the camera moved (PS1-style cracks) and the
    // surfaces behind leaked through them. Here every pixel is sampled at its
    // centre against edges computed from the exact projected positions; each
    // edge is always evaluated top-to-bottom from the same two vertices, so
    // triangles sharing it get bit-identical results; and the interpolants
    // come from per-triangle plane equations instead of accumulated steps.
    #ifndef SW_PERSP_SPAN
    #define SW_PERSP_SPAN 16
    #endif

    struct PlaneSW {
        float dx, dy, c;
    };

    static inline void planeSW(PlaneSW &p, const VertexSW *a, const VertexSW *b, const VertexSW *c,
                               float va, float vb, float vc, float invArea) {
        float x1 = b->fx - a->fx;
        float y1 = b->fy - a->fy;
        float x2 = c->fx - a->fx;
        float y2 = c->fy - a->fy;
        float d1 = vb - va;
        float d2 = vc - va;
        p.dx = (d1 * y2 - d2 * y1) * invArea;
        p.dy = (d2 * x1 - d1 * x2) * invArea;
        p.c  = va - p.dx * a->fx - p.dy * a->fy;
    }

    static inline float edgeXSW(const VertexSW *top, const VertexSW *bottom, float y) {
        float dy = bottom->fy - top->fy;
        if (dy <= 0.0f) return top->fx;
        return top->fx + (bottom->fx - top->fx) * ((y - top->fy) / dy);
    }

    static inline ColorSW tintSW(ColorSW c, int kr, int kg, int kb) { // k: 0..256
        int r = (((c >> 16) & 0xFF) * kr) >> 8;
        int g = (((c >>  8) & 0xFF) * kg) >> 8;
        int b = (( c        & 0xFF) * kb) >> 8;
        return ColorSW(0xFF000000u | (r << 16) | (g << 8) | b);
    }

    // ---- Pipelined, two-core rasterization ------------------------------
    // Each DIP call's projected vertices and visible triangles are recorded
    // (with clears and the 2D background blit) into a frame list. As soon as a
    // frame is recorded, the second core starts rasterizing it, while the main
    // core goes on to update the game and record the next frame; then the main
    // core rasterizes its own part of the pending frame. The screen is split
    // into 8-line groups shared between the cores in a ratio that adapts every
    // frame so both finish together. Frames go to two alternating buffers,
    // stored rotated 180 degrees for the upside-down panel (logical pixel
    // (x, y) at end - (y * width + x)), which the platform copies to the
    // screen in the background. Costs one frame of latency.

    #define SW_BAND_SHIFT 3
    #define SW_GROUPS     64

    enum { SW_CMD_CLEAR, SW_CMD_TRIS, SW_CMD_BLIT, SW_CMD_SNAPSHOT, SW_CMD_RESTORE };

    struct CmdSW {
        int          type;
        bool         color, depth;                         // clear
        const uint8 *texels;                               // triangles
        Texture     *tex;                                  // triangles, blit
        bool         affine, ortho, testZ, writeZ;
        bool         tiled;                                // triangles: texels in 8x8 blocks
        uint32       blitKey;                              // blit: which image (for the cache)
        bool         water;                                // triangles: use the underwater palette
        bool         sky;                                  // triangles: the sky, at "infinite" depth
        bool         shadow;                               // triangles: darken instead of texturing
        short4       clip;
        int          triStart, triCount;
        int          sx0, sy0, sw, sh, ox0, oy0, ow, oh;   // blit
    };

    struct TriSW {
        const VertexSW *a, *b, *c;
    };

    #define SW_MAX_CMDS  8192
    #define SW_MAX_TRIS  (96 * 1024)
    #define SW_MAX_VERTS (128 * 1024)

    struct FrameSW {
        CmdSW    *cmds;
        TriSW    *tris;
        VertexSW *verts;
        int       cmdCount, triCount, vertCount;
        bool      overflow;
        bool      snapshot;              // keeps the menu background (SW_CMD_SNAPSHOT)
        ColorSW  *buffer;
        ColorSW  *colorEnd;
        uint8     owner[SW_GROUPS];      // 1: the worker core draws this group
        uint8     lightSel[32 * 256];
        ColorSW   palWorld[256];         // scene palette
        ColorSW   palWater[256];         // scene palette with the underwater tint
        ColorSW   palUI[256];
    };

    FrameSW  swFrames[2];
    FrameSW *swRec  = NULL;   // being recorded
    FrameSW *swPend = NULL;   // recorded, being rasterized
    ColorSW *swBuffers[2] = { NULL, NULL };
    int      swNextBuffer = 0;

    // Drawing straight into the screen's 3 pages (set by the platform when its
    // memory takes the painter's writes as fast as normal memory): frames take
    // them in turn - one on screen, one waiting for the vsync, one being drawn -
    // and no copy of the finished frame is needed.
    ColorSW *swScreenPages[3] = { NULL, NULL, NULL };

    void swUseScreenPages(ColorSW *a, ColorSW *b, ColorSW *c, int first) {
        swScreenPages[0] = a;
        swScreenPages[1] = b;
        swScreenPages[2] = c;
        swNextBuffer     = first % 3;
    }
    void   (*swPresent)(const ColorSW *buffer) = NULL;   // set by the platform
    int      swWorkerGroups = 36;
    long long swLaunchTime  = 0;   // when the worker got the pending frame

    const VertexSW *swBatchBase = NULL;
    CmdSW          *swBatch = NULL;

    static void swInitFrames() {
        if (swRec) return;
        for (int i = 0; i < 2; i++) {
            FrameSW &f = swFrames[i];
            f.cmds  = new CmdSW[SW_MAX_CMDS];
            f.tris  = new TriSW[SW_MAX_TRIS];
            f.verts = new VertexSW[SW_MAX_VERTS];
            f.cmdCount = f.triCount = f.vertCount = 0;
            f.overflow = false;
            f.buffer = f.colorEnd = NULL;
        }
        swRec = &swFrames[0];
    }

    // Texture pages stored in 8x8 blocks of 64 bytes, one cache line each: a
    // span crossing a page vertically or diagonally reads far fewer cache
    // lines than with plain 256-byte rows. Same texels, another order; the
    // level's own pages stay untouched (other code reads them).
    struct TiledPagesSW {
        const Tile8 *orig;
        Tile8       *tiled;
        int          count;
    };
    TiledPagesSW swTiled = { NULL, NULL, 0 };

    // always inside the 256x256 page (a coordinate one step off the edge wraps,
    // as the plain rows' "+ (u >> 16)" stays near it)
    static inline int tiledIndexSW(int tu, int tv) {
        return ((tv & 0xF8) << 8) | ((tu & 0xF8) << 3) | ((tv & 7) << 3) | (tu & 7);
    }

    // division by 16 rounding toward zero, exactly as "/ 16" (a plain ">> 4"
    // rounds negative numbers down: a stretch could end past its last texel)
    static inline int32 div16SW(int32 d) {
        return (d + ((d >> 31) & 15)) >> 4;
    }

    // the same for 32 (the long stretches of the adaptive perspective)
    static inline int32 div32SW(int32 d) {
        return (d + ((d >> 31) & 31)) >> 5;
    }

    void swRegisterTiles(const Tile8 *tiles, int count) {
        swDrain();
        delete[] swTiled.tiled;
        swTiled.orig  = tiles;
        swTiled.count = count;
        swTiled.tiled = new Tile8[count];
        for (int i = 0; i < count; i++) {
            const uint8 *src = tiles[i].index;
            uint8       *dst = swTiled.tiled[i].index;
            for (int tv = 0; tv < 256; tv++)
                for (int tu = 0; tu < 256; tu++)
                    dst[tiledIndexSW(tu, tv)] = src[(tv << 8) + tu];
        }
    }

    void swUnregisterTiles(const Tile8 *tiles) {
        if (swTiled.orig != tiles) return;
        swDrain();
        delete[] swTiled.tiled;
        swTiled.orig  = NULL;
        swTiled.tiled = NULL;
        swTiled.count = 0;
    }

    struct RasterCtxSW {
        const FrameSW *frame;
        ColorSW       *colorEnd;
        const uint8   *owner;
        const uint8   *texels;   // tile indices; NULL for RGBA-textured 2D
        Texture       *tex;
        const ColorSW *pal;      // scene palette for this batch (dry or underwater)
        bool           affine, ortho, testZ, writeZ;
        bool           tiled;
        bool           shadow;
        bool           sky;
        short4         clip;
        int            band;     // 0: main core, 1: worker core
        long long      pixels;
    };

    static inline bool ownsRowSW(const RasterCtxSW &ctx, int y) {
        return ctx.owner[y >> SW_BAND_SHIFT] == ctx.band;
    }

    // 2D spans: texel (or white) times the primitive's vertex colour,
    // alpha-blended over what is on screen - as the GL UI shader does.
    void drawSpanOrthoSW(RasterCtxSW &ctx, int y, int x0, int x1, const PlaneSW &pU, const PlaneSW &pV, const PlaneSW &pL,
                         bool noTex, bool solid, uint32 primColor) {
        ctx.pixels += x1 - x0;
        const float xc = float(x0) + 0.5f;
        const float yc = float(y) + 0.5f;
        float fu = pU.dx * xc + pU.dy * yc + pU.c;
        float fv = pV.dx * xc + pV.dy * yc + pV.c;
        float fl = pL.dx * xc + pL.dy * yc + pL.c;

        ColorSW *color = ctx.colorEnd - y * Core::width;   // mirrored row: pixel x at color[-x]
        const uint8   *texels   = ctx.texels;
        const uint8   *lightSel = ctx.frame->lightSel;
        const ColorSW *palUI    = ctx.frame->palUI;
        const int cr =  primColor        & 0xFF;
        const int cg = (primColor >>  8) & 0xFF;
        const int cb = (primColor >> 16) & 0xFF;
        const int ca = (primColor >> 24) & 0xFF;
        const int32 UV_MAX = (256 << 16) - 1;

        for (int x = x0; x < x1; x++, fu += pU.dx, fv += pV.dx, fl += pL.dx) {
            int r, g, b;
            int pa = 255;
            if (solid) {
                r = g = b = 255;
            } else if (noTex || !texels) {
                Texture *tex = ctx.tex;
                if (tex && tex->memory && tex->width > 0 && tex->height > 0) {
                    int tx = int(((long long)(int32(fu) >> 16) * tex->width)  >> 15);
                    int ty = int(((long long)(int32(fv) >> 16) * tex->height) >> 15);
                    if (tx < 0) tx = 0; else if (tx >= tex->width)  tx = tex->width  - 1;
                    if (ty < 0) ty = 0; else if (ty >= tex->height) ty = tex->height - 1;
                    const uint8 *px = tex->memory + (ty * tex->width + tx) * 4;
                    r = px[0]; g = px[1]; b = px[2]; pa = px[3];
                } else {
                    r = g = b = 255;
                }
            } else {
                int32 u = int32(fu), v = int32(fv);
                if (u < 0) u = 0; else if (u > UV_MAX) u = UV_MAX;
                if (v < 0) v = 0; else if (v > UV_MAX) v = UV_MAX;
                uint8 index = ctx.tiled ? texels[tiledIndexSW(u >> 16, v >> 16)] : texels[((v >> 16) << 8) + (u >> 16)];
                if (index == 0) continue;
                int32 li = int32(fl) >> (16 + 3);
                if ((uint32)li > 31u) li = li < 0 ? 0 : 31;
                ColorSW t = palUI[lightSel[(li << 8) + index]];
                r = (t >> 16) & 0xFF;
                g = (t >>  8) & 0xFF;
                b =  t        & 0xFF;
            }
            // x / 255, exact for 0..65535, without a division
            #define DIV255_SW(x) (((x) + 1 + ((x) >> 8)) >> 8)
            r = DIV255_SW(r * cr);
            g = DIV255_SW(g * cg);
            b = DIV255_SW(b * cb);
            const int ea = DIV255_SW(ca * pa);
            if (ea < 255) {
                if (ea == 0) continue;
                ColorSW d = color[-x];
                r = DIV255_SW(r * ea + ((d >> 16) & 0xFF) * (255 - ea));
                g = DIV255_SW(g * ea + ((d >>  8) & 0xFF) * (255 - ea));
                b = DIV255_SW(b * ea + ( d        & 0xFF) * (255 - ea));
            }
            color[-x] = ColorSW(0xFF000000u | (r << 16) | (g << 8) | b);
        }
    }

    // The innermost loop, generated for each depth test/write combination so
    // the per-pixel code carries no checks whose answer never changes.
    // FLAT_LIGHT: the light level is the same over the whole stretch (both ends
    // fall in the same one of the 32 levels, and it changes linearly), so its
    // row of the light table is picked once - the very same pixels.
    // ZCODE: z is the plain 1/w and the depth code is worked out per pixel
    // (a stretch crossing from one exponent to the next); otherwise z already
    // is the code, in 8.8 fixed point, stepped linearly.
    template <bool TEST_Z, bool WRITE_Z, bool TILED, bool FLAT_LIGHT, bool ZCODE>
    static inline void spanPixelsSW(int &x, int len, ColorSW *color, DepthSW *depth,
                                    const uint8 *texels, const uint8 *lightSel, const ColorSW *palWorld,
                                    int32 u, int32 du, int32 v, int32 dv, int32 lv, int32 dl, int32 z, int32 dz) {
        ColorSW     *c    = color - x;                      // mirrored: walks down
        DepthSW     *d    = (TEST_Z || WRITE_Z) ? depth + x : NULL;
        const uint8 *lrow = lightSel + ((lv >> 19) << 8);
        x += len;
        for (int k = len; k > 0; k--, c--) {
            const int32 zc = (TEST_Z || WRITE_Z) ? (ZCODE ? depthCodeSW(z) : (z >> 8)) : 0;
            if (!TEST_Z || zc >= int32(*d)) {
                const uint8 index = TILED ? texels[tiledIndexSW(u >> 16, v >> 16)] : texels[((v >> 16) << 8) + (u >> 16)];
                if (index != 0) {
                    *c = palWorld[FLAT_LIGHT ? lrow[index] : lightSel[((lv >> 19) << 8) + index]];
                    if (WRITE_Z) *d = DepthSW(zc);
                }
            }
            if (TEST_Z || WRITE_Z) d++;
            u += du;
            v += dv;
            z += dz;
            if (!FLAT_LIGHT) lv += dl;
        }
    }

    // Shadow blobs: halve the brightness of what is already on screen, depth
    // tested and without writing depth; the software counterpart of the
    // multiply blend the GPU renderers use.
    static void drawShadowSpanSW(RasterCtxSW &ctx, int y, int x0, int x1, const PlaneSW &pQ) {
        ctx.pixels += x1 - x0;
        const int32 row   = y * Core::width;
        ColorSW    *color = ctx.colorEnd - row;     // mirrored: pixel x at color[-x]
        DepthSW    *depth = swDepth ? swDepth + row : NULL;
        float       pq    = pQ.dx * (float(x0) + 0.5f) + pQ.dy * (float(y) + 0.5f) + pQ.c;
        for (int x = x0; x < x1; x++, pq += pQ.dx) {
            if (depth) {
                int32 z = int32(pq * 536870912.0f);
                if (z < 0) z = 0; else if (z > (65535 << 8)) z = 65535 << 8;
                if (depthCodeSW(z) < int32(depth[x])) continue;
            }
            const ColorSW c = color[-x];
            color[-x] = (sizeof(ColorSW) == 4) ? ColorSW((c >> 1) & 0x7F7F7F) : ColorSW((c >> 1) & 0x7BEF);
        }
    }

    void drawSpanSW(RasterCtxSW &ctx, int y, int x0, int x1, const PlaneSW &pU, const PlaneSW &pV, const PlaneSW &pQ, const PlaneSW &pL) {
        if (ctx.shadow) {
            drawShadowSpanSW(ctx, y, x0, x1, pQ);
            return;
        }
        const uint8 *texels = ctx.texels;
        if (!texels) return;
        ctx.pixels += x1 - x0;
        const float xc = float(x0) + 0.5f;
        const float yc = float(y) + 0.5f;
        float pu = pU.dx * xc + pU.dy * yc + pU.c;
        float pv = pV.dx * xc + pV.dy * yc + pV.c;
        float pq = pQ.dx * xc + pQ.dy * yc + pQ.c;
        float l  = pL.dx * xc + pL.dy * yc + pL.c;

        const int32    row      = y * Core::width;
        ColorSW       *color    = ctx.colorEnd - row;     // mirrored: pixel x at color[-x]
        DepthSW       *depth    = swDepth ? swDepth + row : NULL;
        const bool     testZ    = depth && ctx.testZ;
        const bool     writeZ   = depth && ctx.writeZ;
        const bool     affine   = ctx.affine;
        const bool     tiled    = ctx.tiled && !affine;
        const bool     sky      = ctx.sky;
        const uint8   *lightSel = ctx.frame->lightSel;
        const ColorSW *palWorld = ctx.pal;
        const int32    UV_MAX   = (256 << 16) - 1;
        const int32    LV_MAX   = (32 << 19) - 1;
        const int32    Z_MAX    = (65535 << 8);

        float invA = 1.0f / (pq < 1e-6f ? 1e-6f : pq);

        // the start of each stretch is the end of the last one: only the first
        // stretch converts its start (float to integer is slow on this CPU)
        int32 ua, va, lvA, zA;
        if (affine) {
            ua = int32(pu);
            va = int32(pv);
        } else {
            ua = int32(pu * invA);
            va = int32(pv * invA);
        }
        if (ua < 0) ua = 0; else if (ua > UV_MAX) ua = UV_MAX;
        if (va < 0) va = 0; else if (va > UV_MAX) va = UV_MAX;
        lvA = int32(l);
        if (lvA < 0) lvA = 0; else if (lvA > LV_MAX) lvA = LV_MAX;
        zA = int32(pq * 536870912.0f);
        if (zA < 0) zA = 0; else if (zA > Z_MAX) zA = Z_MAX;

        bool needA = false;     // the start of the stretch to work out again (after a skip)

        int x = x0;
        while (x < x1) {
            int len = x1 - x;
            if (len > SW_PERSP_SPAN) {
                len = SW_PERSP_SPAN;
                // 1/w barely changes over 32 pixels (far, or facing the camera):
                // one perspective step for all 32 gives the same texture
                if (x1 - x >= 32 && !affine && fabsf(pQ.dx) * 32.0f < pq * (1.0f / 64.0f))
                    len = 32;
            }

            float pu1 = pu + pU.dx * float(len);
            float pv1 = pv + pV.dx * float(len);
            float pq1 = pq + pQ.dx * float(len);
            float l1  = l  + pL.dx * float(len);

            // the whole stretch behind what is already drawn: skipped at once,
            // without its perspective step (none of its pixels would pass the
            // depth test: 1/w is linear, so its nearest point is an end)
            if (testZ) {
                int32 cn = 0;                       // the sky: "infinitely" far
                if (!sky) {
                    float qn = pq > pq1 ? pq : pq1;
                    int32 zn = int32(qn * 536870912.0f);
                    if (zn < 0) zn = 0; else if (zn > Z_MAX) zn = Z_MAX;
                    cn = depthCodeSW(zn);
                }
                const DepthSW *d = depth + x;
                int k = 0;
                while (k < len && int32(d[k]) > cn) k++;
                if (k == len) {
                    x  += len;
                    pu  = pu1;
                    pv  = pv1;
                    pq  = pq1;
                    l   = l1;
                    needA = true;
                    continue;
                }
            }
            if (needA) {                            // after a skip: this stretch's start
                invA = 1.0f / (pq < 1e-6f ? 1e-6f : pq);
                if (affine) {
                    ua = int32(pu);
                    va = int32(pv);
                } else {
                    ua = int32(pu * invA);
                    va = int32(pv * invA);
                }
                if (ua < 0) ua = 0; else if (ua > UV_MAX) ua = UV_MAX;
                if (va < 0) va = 0; else if (va > UV_MAX) va = UV_MAX;
                lvA = int32(l);
                if (lvA < 0) lvA = 0; else if (lvA > LV_MAX) lvA = LV_MAX;
                zA = int32(pq * 536870912.0f);
                if (zA < 0) zA = 0; else if (zA > Z_MAX) zA = Z_MAX;
                needA = false;
            }

            int32 ub, vb;
            if (affine) {
                ub = int32(pu1); vb = int32(pv1);
            } else {
                float qb   = pq1 < 1e-6f ? 1e-6f : pq1;
                float invB = 1.0f / qb;
                ub = int32(pu1 * invB); vb = int32(pv1 * invB);
                invA = invB;
            }
            if (ub < 0) ub = 0; else if (ub > UV_MAX) ub = UV_MAX;
            if (vb < 0) vb = 0; else if (vb > UV_MAX) vb = UV_MAX;
            int32 lvB = int32(l1);
            if (lvB < 0) lvB = 0; else if (lvB > LV_MAX) lvB = LV_MAX;
            int32 zB = int32(pq1 * 536870912.0f);
            if (zB < 0) zB = 0; else if (zB > Z_MAX) zB = Z_MAX;

            // steps: a shift for the usual 16-pixel stretch instead of a division
            int32 u  = ua, v = va;
            int32 du, dv, dl, dz;
            // depth: codes at both ends of the stretch; with the same exponent at
            // both, the code follows 1/w linearly and is just stepped
            const int32 zcA = sky ? 0 : (depthCodeSW(zA) << 8), zcB = sky ? 0 : (depthCodeSW(zB) << 8);   // the sky: "infinitely" far
            const bool  zcode = (zcA >> 20) != (zcB >> 20);   // crosses an exponent
            const int32 zS = zcode ? zA : zcA, zE = zcode ? zB : zcB;
            if (len == 16) {
                du = div16SW(ub - ua);
                dv = div16SW(vb - va);
                dl = div16SW(lvB - lvA);
                dz = div16SW(zE - zS);
            } else if (len == 32) {
                du = div32SW(ub - ua);
                dv = div32SW(vb - va);
                dl = div32SW(lvB - lvA);
                dz = div32SW(zE - zS);
            } else {
                du = (ub - ua) / len;
                dv = (vb - va) / len;
                dl = (lvB - lvA) / len;
                dz = (zE - zS) / len;
            }
            int32 lv = lvA;
            int32 z  = zS;

            const bool flat = (lvA >> 19) == (lvB >> 19);
            #define SW_ARGS x, len, color, depth, texels, lightSel, palWorld, u, du, v, dv, lv, dl, z, dz
            #define SW_SPAN2(TZ, WZ, ZC) { \
                if (tiled) { if (flat) spanPixelsSW<TZ, WZ, true,  true,  ZC>(SW_ARGS); else spanPixelsSW<TZ, WZ, true,  false, ZC>(SW_ARGS); } \
                else       { if (flat) spanPixelsSW<TZ, WZ, false, true,  ZC>(SW_ARGS); else spanPixelsSW<TZ, WZ, false, false, ZC>(SW_ARGS); } }
            #define SW_SPAN(TZ, WZ) { if (zcode) SW_SPAN2(TZ, WZ, true) else SW_SPAN2(TZ, WZ, false) }
            if (testZ) {
                if (writeZ) SW_SPAN(true,  true )
                else        SW_SPAN(true,  false)
            } else {
                if (writeZ) SW_SPAN(false, true )
                else        SW_SPAN(false, false)
            }
            #undef SW_SPAN
            #undef SW_SPAN2
            #undef SW_ARGS
            pu = pu1;
            pv = pv1;
            pq = pq1;
            l  = l1;
            ua  = ub;
            va  = vb;
            lvA = lvB;
            zA  = zB;
        }
    }

    struct EdgeSW {
        float x0, y0, slope;
    };

    static inline void edgeSW(EdgeSW &e, const VertexSW *top, const VertexSW *bottom) {
        float dy = bottom->fy - top->fy;
        e.x0 = top->fx;
        e.y0 = top->fy;
        e.slope = (dy > 0.0f) ? (bottom->fx - top->fx) / dy : 0.0f;
    }

    static inline int ceilSW(float x) {
        int i = int(x);
        return i + (float(i) < x ? 1 : 0);
    }

    void rasterTriangleSW(RasterCtxSW &ctx, const VertexSW *a, const VertexSW *b, const VertexSW *c) {
        float area = (b->fx - a->fx) * (c->fy - a->fy) - (c->fx - a->fx) * (b->fy - a->fy);
        if (area > -1e-4f && area < 1e-4f) return;

        const VertexSW *t = a, *m = b, *d = c, *s;
        if (m->fy < t->fy) { s = t; t = m; m = s; }
        if (d->fy < t->fy) { s = t; t = d; d = s; }
        if (d->fy < m->fy) { s = m; m = d; d = s; }

        int y0 = ceilSW(t->fy - 0.5f);
        int y1 = ceilSW(d->fy - 0.5f);
        if (y0 < ctx.clip.y) y0 = ctx.clip.y;
        if (y1 > ctx.clip.w) y1 = ctx.clip.w;
        if (y0 >= y1) return;

        // first row of this triangle that belongs to this core, before any setup
        int firstOwned = y0;
        while (firstOwned < y1 && !ownsRowSW(ctx, firstOwned)) {
            firstOwned = ((firstOwned >> SW_BAND_SHIFT) + 1) << SW_BAND_SHIFT;
        }
        if (firstOwned >= y1) return;

        const float invArea = 1.0f / area;
        const bool  noTex   = (ctx.texels == NULL);
        const bool  solid   = ctx.ortho && a->u == b->u && b->u == c->u && a->v == b->v && b->v == c->v;
        uint32 primColor = 0;
        if (ctx.ortho) {
            for (int ch = 0; ch < 32; ch += 8) {
                uint32 sum = ((a->color >> ch) & 0xFF) + ((b->color >> ch) & 0xFF) + ((c->color >> ch) & 0xFF);
                primColor |= (sum / 3) << ch;
            }
        }

        PlaneSW pU, pV, pQ, pL;
        if (ctx.affine) {
            planeSW(pU, a, b, c, float(a->u), float(b->u), float(c->u), invArea);
            planeSW(pV, a, b, c, float(a->v), float(b->v), float(c->v), invArea);
        } else {
            planeSW(pU, a, b, c, a->pu, b->pu, c->pu, invArea);
            planeSW(pV, a, b, c, a->pv, b->pv, c->pv, invArea);
        }
        planeSW(pQ, a, b, c, a->pq, b->pq, c->pq, invArea);
        planeSW(pL, a, b, c, float(a->l), float(b->l), float(c->l), invArea);

        EdgeSW eLong, eTop, eBottom;
        edgeSW(eLong,   t, d);
        edgeSW(eTop,    t, m);
        edgeSW(eBottom, m, d);

        for (int y = firstOwned; y < y1; y++) {
            if (!ownsRowSW(ctx, y)) {
                y = (((y >> SW_BAND_SHIFT) + 1) << SW_BAND_SHIFT) - 1;   // on to the next group
                continue;
            }
            float yc = float(y) + 0.5f;
            float xa = eLong.x0 + eLong.slope * (yc - eLong.y0);
            const EdgeSW &eShort = (yc < m->fy) ? eTop : eBottom;
            float xb = eShort.x0 + eShort.slope * (yc - eShort.y0);
            float xl = xa < xb ? xa : xb;
            float xr = xa < xb ? xb : xa;
            int x0 = ceilSW(xl - 0.5f);
            int x1 = ceilSW(xr - 0.5f);
            if (x0 < ctx.clip.x) x0 = ctx.clip.x;
            if (x1 > ctx.clip.z) x1 = ctx.clip.z;
            if (x0 < x1) {
                if (ctx.ortho) drawSpanOrthoSW(ctx, y, x0, x1, pU, pV, pL, noTex, solid, primColor);
                else           drawSpanSW(ctx, y, x0, x1, pU, pV, pQ, pL);
            }
        }
    }

    // ---- recording (main core) ----
    static inline CmdSW* swNewCmd() {
        swInitFrames();
        FrameSW &f = *swRec;
        if (f.cmdCount >= SW_MAX_CMDS) {
            f.overflow = true;
            return NULL;
        }
        return &f.cmds[f.cmdCount++];
    }

    // The in-game menu's background: the software renderer has no render
    // targets, so the game picture (without the HUD) is copied aside when the
    // menu opens and copied back under the ring each frame, instead of drawing
    // the whole level behind it again (the game is paused meanwhile).
    ColorSW *swMenuBg      = NULL;
    bool     swMenuBgReady = false;

    void swRecordSnapshot() {
        CmdSW *cmd = swNewCmd();
        if (!cmd) return;
        cmd->type = SW_CMD_SNAPSHOT;
        swRec->snapshot = true;
    }

    // the kept picture can be put back: ready, or kept earlier in this frame
    bool swMenuBgAvailable() {
        return swMenuBgReady || (swRec && swRec->snapshot);
    }

    void swRecordRestore() {
        CmdSW *cmd = swNewCmd();
        if (!cmd) return;
        cmd->type = SW_CMD_RESTORE;
    }

    void swRecordClear(bool color, bool depth) {
        CmdSW *cmd = swNewCmd();
        if (!cmd) return;
        cmd->type  = SW_CMD_CLEAR;
        cmd->color = color;
        cmd->depth = depth;
    }

    // Background blits (menus, loading screens, videos): the converted, scaled
    // image is kept, row by row, and redone only when it changes.
    ColorSW *swBlitCache  = NULL;
    uint32  *swBlitRowKey = NULL;

    void swRecordBlit(Texture *tex, int sx0, int sy0, int sw, int sh, int ox0, int oy0, int ow, int oh) {
        swInitFrames();
        {   // a blit rewrites every row of the color buffer: earlier color work is lost
            FrameSW &f = *swRec;
            for (int i = 0; i < f.cmdCount; i++) {
                CmdSW &c = f.cmds[i];
                if (c.type == SW_CMD_BLIT) {
                    c.type  = SW_CMD_CLEAR;     // nothing to do
                    c.color = false;
                    c.depth = false;
                } else if (c.type == SW_CMD_CLEAR) {
                    c.color = false;            // the depth part still counts
                }
            }
        }
        CmdSW *cmd = swNewCmd();
        if (!cmd) return;
        cmd->type = SW_CMD_BLIT;
        cmd->tex  = tex;
        cmd->sx0 = sx0; cmd->sy0 = sy0; cmd->sw = sw; cmd->sh = sh;
        cmd->ox0 = ox0; cmd->oy0 = oy0; cmd->ow = ow; cmd->oh = oh;

        static Texture *lastTex = NULL;
        static uint32   lastVersion = 0, lastKey = 0, nextKey = 1;
        static int      lastP[8];
        const int p[8] = { sx0, sy0, sw, sh, ox0, oy0, ow, oh };
        const uint32 version = (tex && tex->memory) ? tex->version : 0;
        if (!lastKey || tex != lastTex || version != lastVersion || memcmp(p, lastP, sizeof(p))) {
            lastKey     = nextKey++;
            lastTex     = tex;
            lastVersion = version;
            memcpy(lastP, p, sizeof(p));
        }
        cmd->blitKey = lastKey;
    }

    void swBeginBatch(bool ortho, int maxTris) {
        swInitFrames();
        FrameSW &f = *swRec;
        const int n = swVertices.length;
        swBatch = NULL;
        if (f.vertCount + n > SW_MAX_VERTS || f.triCount + maxTris > SW_MAX_TRIS || f.cmdCount >= SW_MAX_CMDS) {
            f.overflow = true;   // extremely rare: drop the rest of this frame's geometry
            return;
        }
        memcpy(f.verts + f.vertCount, swVertices.items, n * sizeof(VertexSW));
        swBatchBase  = f.verts + f.vertCount;
        f.vertCount += n;

        swBatch = &f.cmds[f.cmdCount++];
        swBatch->type     = SW_CMD_TRIS;
        const Tile8 *tt = curTile;
        bool tiled = false;
        if (tt && swTiled.tiled && tt >= swTiled.orig && tt < swTiled.orig + swTiled.count) {
            tt    = swTiled.tiled + (tt - swTiled.orig);
            tiled = true;
        }
        swBatch->texels   = tt ? tt->index : NULL;
        swBatch->tiled    = tiled;
        swBatch->tex      = Core::active.textures[0];
        swBatch->affine   = (curTile == (Tile8*)swGradient);
        swBatch->ortho    = ortho;
        swBatch->testZ    = swDepthTest;
        swBatch->writeZ   = swDepthWrite;
        swBatch->water    = swBatchWater;
        swBatch->shadow   = swShadowBatch;
        swBatch->sky      = swSkyBatch;
        swBatch->clip     = swClipRect;
        swBatch->triStart = f.triCount;
        swBatch->triCount = 0;
    }

    // Near faces first, in opaque batches: the depth test then throws away what
    // lies behind them instead of painting it and painting over it. A stable
    // sort by each face's nearest corner (ties keep the level's order).
    struct TriKeySW {
        float key;
        TriSW tri;
    };
    static TriKeySW *swSortBuf = NULL;

    static void swSortNearFirst(TriSW *tris, int n) {
        if (!swSortBuf) swSortBuf = new TriKeySW[SW_MAX_TRIS];
        for (int i = 0; i < n; i++) {
            const TriSW &t = tris[i];
            float k = t.a->pq;
            if (t.b->pq > k) k = t.b->pq;
            if (t.c->pq > k) k = t.c->pq;
            swSortBuf[i].key = k;
            swSortBuf[i].tri = t;
        }
        std::stable_sort(swSortBuf, swSortBuf + n, [](const TriKeySW &a, const TriKeySW &b) { return a.key > b.key; });
        for (int i = 0; i < n; i++)
            tris[i] = swSortBuf[i].tri;
    }

    void swEndBatch() {
        if (!swBatch) return;
        swBatch->triCount = swRec->triCount - swBatch->triStart;
        if (swBatch->triCount > 1 && swBatch->testZ && swBatch->writeZ && !swBatch->ortho && !swBatch->shadow && !swBatch->sky && !swBatch->affine)
            swSortNearFirst(swRec->tris + swBatch->triStart, swBatch->triCount);
    }

    static inline void swAddTri(Index ia, Index ib, Index ic) {
        TriSW &t = swRec->tris[swRec->triCount++];
        t.a = swBatchBase + ia;
        t.b = swBatchBase + ib;
        t.c = swBatchBase + ic;
    }

    void drawTriangle(Index *indices) {
        if (!swBatch) return;
        const VertexSW *t = swVertices.items + indices[0];
        const VertexSW *m = swVertices.items + indices[1];
        const VertexSW *b = swVertices.items + indices[2];
        // the picked-up item: no culling (its winding is reversed with that
        // projection); the depth test sorts its faces
        if (!swModelInUI && checkBackface(t, m, b)) return;
        swAddTri(indices[0], indices[1], indices[2]);
    }

    void drawQuad(Index *indices) {
        if (!swBatch) return;
        const VertexSW *t = swVertices.items + indices[0];
        const VertexSW *m = swVertices.items + indices[1];
        const VertexSW *b = swVertices.items + indices[2];
        // the picked-up item: no culling (its winding is reversed with that
        // projection); the depth test sorts its faces
        if (!swModelInUI && checkBackface(t, m, b)) return;
        swAddTri(indices[0], indices[1], indices[2]);
        swAddTri(indices[0], indices[2], indices[3]);
    }

    // ---- replay (both cores) ----
    static void swBlitRow(const RasterCtxSW &ctx, const CmdSW &cmd, Texture *tex, int y, ColorSW *row);

    static void swExecBlit(const RasterCtxSW &ctx, const CmdSW &cmd) {
        Texture *tex = cmd.tex;
        const int W = Core::width, H = Core::height;
        ColorSW *cacheEnd = swBlitCache ? swBlitCache + W * H - 1 : NULL;
        for (int y = 0; y < H; y++) {
            if (!ownsRowSW(ctx, y)) continue;
            ColorSW *row = ctx.colorEnd - y * W;
            // the row as drawn the last time, if it is the same image
            if (cacheEnd && swBlitRowKey[y] == cmd.blitKey) {
                memcpy(row - (W - 1), cacheEnd - y * W - (W - 1), W * sizeof(ColorSW));
                continue;
            }
            swBlitRow(ctx, cmd, tex, y, row);
            if (cacheEnd) {
                memcpy(cacheEnd - y * W - (W - 1), row - (W - 1), W * sizeof(ColorSW));
                swBlitRowKey[y] = cmd.blitKey;
            }
        }
    }

    static void swBlitRow(const RasterCtxSW &ctx, const CmdSW &cmd, Texture *tex, int y, ColorSW *row) {
        const int W = Core::width;
        {
            const int iy = y - cmd.oy0;
            if (!tex || !tex->memory || iy < 0 || iy >= cmd.oh) {
                memset(row - (W - 1), 0, W * sizeof(ColorSW));
                return;
            }
            const uint8 *src = tex->memory + (size_t)(cmd.sy0 + iy * cmd.sh / cmd.oh) * tex->width * 4;
            if (cmd.ox0 > 0) memset(row - (cmd.ox0 - 1), 0, cmd.ox0 * sizeof(ColorSW));
            const uint32 step = (uint32(cmd.sw) << 16) / uint32(cmd.ow);
            uint32 acc = 0;
            for (int x = 0; x < cmd.ow; x++, acc += step) {
                const uint8 *px = src + (cmd.sx0 + int(acc >> 16)) * 4;
                row[-(cmd.ox0 + x)] = ColorSW(0xFF000000u | (px[0] << 16) | (px[1] << 8) | px[2]);
            }
            const int right = W - cmd.ox0 - cmd.ow;
            if (right > 0) memset(row - (W - 1), 0, right * sizeof(ColorSW));
        }
    }

    static void swExecute(FrameSW &f, int band, long long &pixels) {
        RasterCtxSW ctx;
        ctx.frame    = &f;
        ctx.colorEnd = f.colorEnd;
        ctx.owner    = f.owner;
        ctx.band     = band;
        ctx.pixels   = 0;
        const int W = Core::width, H = Core::height;
        for (int i = 0; i < f.cmdCount; i++) {
            const CmdSW &cmd = f.cmds[i];
            if (cmd.type == SW_CMD_CLEAR) {
                for (int y = 0; y < H; y++) {
                    if (!ownsRowSW(ctx, y)) continue;
                    if (cmd.color) memset(ctx.colorEnd - y * W - (W - 1), 0, W * sizeof(ColorSW));
                    if (cmd.depth && swDepth) memset(swDepth + y * W, 0, W * sizeof(DepthSW));
                }
            } else if (cmd.type == SW_CMD_SNAPSHOT || cmd.type == SW_CMD_RESTORE) {
                ColorSW *bgEnd = swMenuBg + W * H - 1;
                for (int y = 0; y < H; y++) {
                    if (!ownsRowSW(ctx, y)) continue;
                    ColorSW *row = ctx.colorEnd - y * W - (W - 1);
                    ColorSW *bg  = bgEnd - y * W - (W - 1);
                    if (cmd.type == SW_CMD_SNAPSHOT) {
                        memcpy(bg, row, W * sizeof(ColorSW));
                        // darkened by half, as the original's menu background
                        for (int i = 0; i < W; i++) {
                        #ifdef COLOR_16
                            bg[i] = ColorSW((bg[i] >> 1) & 0x7BEF);
                        #else
                            bg[i] = ColorSW((bg[i] >> 1) & 0x7F7F7F7F);
                        #endif
                        }
                    } else {
                        memcpy(row, bg, W * sizeof(ColorSW));
                    }
                }
            } else if (cmd.type == SW_CMD_BLIT) {
                swExecBlit(ctx, cmd);
            } else {
                ctx.texels = cmd.texels;
                ctx.tex    = cmd.tex;
                ctx.affine = cmd.affine;
                ctx.tiled  = cmd.tiled;
                ctx.ortho  = cmd.ortho;
                ctx.testZ  = cmd.testZ;
                ctx.writeZ = cmd.writeZ;
                ctx.clip   = cmd.clip;
                ctx.pal    = cmd.water ? f.palWater : f.palWorld;
                ctx.shadow = cmd.shadow;
                ctx.sky    = cmd.sky;
                const TriSW *tri = f.tris + cmd.triStart;
                for (int k = 0; k < cmd.triCount; k++, tri++) {
                    rasterTriangleSW(ctx, tri->a, tri->b, tri->c);
                }
            }
        }
        pixels += ctx.pixels;
    }

    static void swBuildShadeFrame(FrameSW &f) {
        // Always from the real light map and the normal palette (the core may
        // have switched to the unshaded ones for the UI by now). Shade 0 is
        // taken as the brightest; if the map says otherwise, read it reversed.
        int lum0 = 0, lum31 = 0;
        for (int i = 1; i < 256; i++) {
            ColorSW a = swPaletteColor[swLightmapShade[i]];
            ColorSW b = swPaletteColor[swLightmapShade[(31 << 8) + i]];
            lum0  += ((a >> 16) & 0xFF) + ((a >> 7) & 0x1FE) + (a & 0xFF);
            lum31 += ((b >> 16) & 0xFF) + ((b >> 7) & 0x1FE) + (b & 0xFF);
        }
        const bool flip = lum0 < lum31;
        for (int li = 0; li < 32; li++) {
            const int src = flip ? 31 - li : li;
            memcpy(f.lightSel + (li << 8), swLightmapShade + (src << 8), 256);
        }
        for (int i = 0; i < 256; i++) {
            ColorSW col = swPaletteColor[i] | 0xFF000000u;
            f.palUI[i]    = col;
            f.palWorld[i] = col;
            f.palWater[i] = tintSW(col, 154, 230, 230);   // the level's underwater colour: 0.6, 0.9, 0.9
        }
    }

    static void swBuildOwner(FrameSW &f, int k) {
        const int groups = (Core::height + (1 << SW_BAND_SHIFT) - 1) >> SW_BAND_SHIFT;
        for (int g = 0; g < SW_GROUPS; g++) f.owner[g] = 0;
        for (int g = 0; g < groups && g < SW_GROUPS; g++) {   // spread the worker's groups evenly
            f.owner[g] = uint8(((g + 1) * k) / groups - (g * k) / groups);
        }
    }

    struct WorkerSW {
        pthread_t       thread;
        pthread_mutex_t mutex;
        pthread_cond_t  cond;
        FrameSW        *job;
        int             seq, done;
        bool            started;
        long long       pixels, busy, endTime;
    };
    WorkerSW swWorker;

    static void* swWorkerProc(void *arg) {
        int seen = 0;
        for (;;) {
            pthread_mutex_lock(&swWorker.mutex);
            while (swWorker.seq == seen) {
                pthread_cond_wait(&swWorker.cond, &swWorker.mutex);
            }
            seen = swWorker.seq;
            FrameSW *job = swWorker.job;
            pthread_mutex_unlock(&swWorker.mutex);

            const long long tStart = swPerfNow();
            long long px = 0;
            swExecute(*job, 1, px);
            const long long tEnd = swPerfNow();

            pthread_mutex_lock(&swWorker.mutex);
            swWorker.busy    = tEnd - tStart;
            swWorker.endTime = tEnd;
            swWorker.pixels  = px;
            swWorker.done    = seen;
            pthread_cond_broadcast(&swWorker.cond);
            pthread_mutex_unlock(&swWorker.mutex);
        }
        return NULL;
    }

    static void swStartWorker(FrameSW *f) {
        if (!swWorker.started) {
            pthread_mutex_init(&swWorker.mutex, NULL);
            pthread_cond_init(&swWorker.cond, NULL);
            swWorker.seq = swWorker.done = 0;
            swWorker.started = true;
            pthread_create(&swWorker.thread, NULL, swWorkerProc, NULL);
            pthread_setname_np(swWorker.thread, "sw-worker");

            // pin the two threads to different cores (the scheduler kept both on core 0)
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(1, &set);
            int rw = pthread_setaffinity_np(swWorker.thread, sizeof(set), &set);
            CPU_ZERO(&set);
            CPU_SET(0, &set);
            int rm = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
            fprintf(stderr, "mt: pin worker->core1 %s, main->core0 %s\n",
                rw == 0 ? "ok" : "FAILED", rm == 0 ? "ok" : "FAILED");
        }
        pthread_mutex_lock(&swWorker.mutex);
        swWorker.job = f;
        swWorker.seq++;
        pthread_cond_broadcast(&swWorker.cond);
        pthread_mutex_unlock(&swWorker.mutex);
    }

    // Finish the pending frame: the main core draws its share, waits for the
    // worker, rebalances the split, and the frame goes to the screen.
    static void swFinishPending() {
        if (!swPend) return;
        FrameSW &f = *swPend;

        const long long t0 = swPerfNow();
        long long px = 0;
        swExecute(f, 0, px);
        const long long t1 = swPerfNow();

        pthread_mutex_lock(&swWorker.mutex);
        while (swWorker.done != swWorker.seq) {
            pthread_cond_wait(&swWorker.cond, &swWorker.mutex);
        }
        px += swWorker.pixels;
        const long long workerEnd  = swWorker.endTime;
        const long long workerBusy = swWorker.busy;
        pthread_mutex_unlock(&swWorker.mutex);
        const long long t2 = swPerfNow();

        // Balance: whoever finished first gets more of the next frame.
        const int groups = (Core::height + (1 << SW_BAND_SHIFT) - 1) >> SW_BAND_SHIFT;
        const long long mainWait   = t2 - t1;          // main idle, waiting for the worker
        const long long workerIdle = t1 - workerEnd;   // worker done before the main core
        {
            // While the main core recorded the next frame, the worker had the
            // pending one to itself; split the rest so both finish together,
            // using this frame's cost per 8-line group.
            const double rec  = double(t0 - swLaunchTime);
            const double cost = double(workerBusy + (t1 - t0)) / groups;
            if (cost > 1.0) {
                double target = (rec / cost + groups) * 0.5;
                if (target < 2) target = 2;
                if (target > groups) target = groups;
                swWorkerGroups = int((swWorkerGroups * 3 + target) / 4.0 + 0.5);
            }
        }


        swPerfPixels += px;
        swPerfRaster += t2 - t0;

        if (f.snapshot) {                     // the menu background is in place
            swMenuBgReady = true;
            f.snapshot    = false;
        }

        if (swPresent) swPresent(f.buffer);   // waits for the other buffer's copy, then starts this one

        f.cmdCount = f.triCount = f.vertCount = 0;
        f.overflow = false;
        swPend = NULL;
    }

    // End of the main loop's frame: finish the previous frame and hand this
    // one to the worker core, which starts on it right away.
    void swFrameEnd() {
        swInitFrames();
        swFinishPending();

        FrameSW &f = *swRec;
        if (f.overflow) {
            static int warned = 0;
            if (warned++ < 5) fprintf(stderr, "sw: frame lists full, some geometry dropped\n");
        }
        if (f.cmdCount == 0) return;

        const int W = Core::width, H = Core::height;
        if (!swBlitCache) {
            swBlitCache  = new ColorSW[W * H];
            swMenuBg     = new ColorSW[W * H];
            swBlitRowKey = new uint32[H];
            memset(swBlitRowKey, 0, H * sizeof(uint32));
        }
        if (swScreenPages[0]) {                 // straight into the screen pages, in turn
            f.buffer     = swScreenPages[swNextBuffer];
            swNextBuffer = (swNextBuffer + 1) % 3;
        } else {
            if (!swBuffers[0]) {
                for (int i = 0; i < 2; i++) {
                    swBuffers[i] = new ColorSW[W * H];
                    memset(swBuffers[i], 0, W * H * sizeof(ColorSW));
                }
            }
            f.buffer     = swBuffers[swNextBuffer & 1];
            swNextBuffer = (swNextBuffer + 1) & 1;
        }
        f.colorEnd = f.buffer + W * H - 1;

        swBuildShadeFrame(f);
        swBuildOwner(f, swWorkerGroups);

        swPend = &f;
        swLaunchTime = swPerfNow();
        swStartWorker(&f);
        swRec = (swRec == &swFrames[0]) ? &swFrames[1] : &swFrames[0];
    }

    // Finish any frame in flight. Called before textures or palettes change,
    // so the rasterizer never reads anything that is being freed.
    void swDrain() {
        swFinishPending();
    }

    void applyLighting(VertexSW &result, const Vertex &vertex, float depth) {
        float lighting = 0.0f;
        if (lightsCount > 0) {   // most vertices have no dynamic light: skip the normalize
        vec3 coord  = vec3(float(vertex.coord.x), float(vertex.coord.y), float(vertex.coord.z));
        vec3 normal = vec3(float(vertex.normal.x), float(vertex.normal.y), float(vertex.normal.z)).normal();
        for (int i = 0; i < lightsCount; i++) {
            LightSW &light = lightsRel[i];
            vec3 dir = (light.pos - coord) * light.radius;
            float att = dir.length2();
            float lum = normal.dot(dir / sqrtf(att));
            lighting += (max(0.0f, lum) * max(0.0f, 1.0f - att)) * light.intensity;
        }
        }

        lighting += result.l;

        depth -= SW_FOG_START;
        if (depth > 0.0f) {
            lighting *= clamp(1.0f - depth / (SW_MAX_DIST - SW_FOG_START), 0.0f, 1.0f);
        }

        result.l = (255 - min(255, int32(lighting))) << 16;
    }

    // Near-plane clipping. Upstream dropped any primitive with a vertex
    // behind the camera, so large floor/wall polygons close to the camera
    // vanished whole (the floor behind Lara turned black as she walked).
    // Primitives crossing the near plane are now cut at it and drawn as a
    // triangle fan, the way a hardware rasterizer would.
    #define SW_NEAR_W 32.0f

    struct ClipVertexSW {
        vec4  c;
        int32 u, v, l;
        uint32 color;
    };

    int32 projectVertexSW(const ClipVertexSW &cv) {
        vec4 c = cv.c;
        const float invW = 1.0f / c.w;
        c.x *= invW;
        c.y *= invW;
        c.z *= invW;
        c.x = clamp(c.x, -16384.0f, 16384.0f);
        c.y = clamp(c.y, -16384.0f, 16384.0f);

        VertexSW result;
        // Keep x with sub-pixel precision: rounding it to whole pixels before
        // rasterizing made vertices snap and edges wobble as the camera moved.
        result.x = int32(c.x * 65536.0f);
        result.y = int32(floorf(c.y + 0.5f));
        result.z = uint32(clamp(c.z, 0.0f, 1.0f) * 65535.0f) << 16;
        result.w = int32(cv.c.w) << 16;
        result.u = cv.u;
        result.v = cv.v;
        result.l = cv.l;
        result.pq = invW;
        if (swModelInUI)   // no perspective (w == 1): the depth comes from z, in a band
                           // just below the depth buffer's maximum (1/w = 1/32)
            result.pq = 1.0f / (32.5f - 0.5f * clamp(c.z, -1.0f, 1.0f));   // the UI camera's z runs
                                                                             // the other way: nearer is larger
        result.pu = float(cv.u) * result.pq;
        result.pv = float(cv.v) * result.pq;
        result.fx = c.x;
        result.fy = c.y;
        result.color = cv.color;
        return swVertices.push(result);
    }

    static inline int32 lerpFixedSW(int32 a, int32 b, double t) {
        return int32(double(a) + (double(b) - double(a)) * t);
    }

    ClipVertexSW clipEdgeSW(const ClipVertexSW &a, const ClipVertexSW &b) {
        double t = (double(SW_NEAR_W) - double(a.c.w)) / (double(b.c.w) - double(a.c.w));
        ClipVertexSW r;
        r.c.x = float(a.c.x + (b.c.x - a.c.x) * t);
        r.c.y = float(a.c.y + (b.c.y - a.c.y) * t);
        r.c.z = float(a.c.z + (b.c.z - a.c.z) * t);
        r.c.w = SW_NEAR_W;
        r.u = lerpFixedSW(a.u, b.u, t);
        r.v = lerpFixedSW(a.v, b.v, t);
        r.l = lerpFixedSW(a.l, b.l, t);
        r.color = a.color;
        return r;
    }

    // This rasterizer maps textures affinely (u/v interpolated linearly in
    // screen space, like the PS1), which warps them badly on polygons whose
    // depth varies a lot across them: walls right next to the camera, or the
    // pieces left by near-plane clipping. Split such polygons into smaller
    // ones - the classic fix on hardware without perspective-correct
    // texturing. Far polygons have nearly uniform depth and are left alone.
    #define SW_SUBDIV_RATIO 1.25f
    #define SW_SUBDIV_DEPTH 0  // disabled: perspective-correct texturing replaced it

    static inline ClipVertexSW midVertexSW(const ClipVertexSW &a, const ClipVertexSW &b) {
        ClipVertexSW r;
        r.c.x = (a.c.x + b.c.x) * 0.5f;
        r.c.y = (a.c.y + b.c.y) * 0.5f;
        r.c.z = (a.c.z + b.c.z) * 0.5f;
        r.c.w = (a.c.w + b.c.w) * 0.5f;
        r.u = (a.u >> 1) + (b.u >> 1);
        r.v = (a.v >> 1) + (b.v >> 1);
        r.l = (a.l >> 1) + (b.l >> 1);
        r.color = a.color;
        return r;
    }

    void emitPolygonSW(const ClipVertexSW *v, int n, int depth) {
        float minW = v[0].c.w;
        float maxW = v[0].c.w;
        for (int k = 1; k < n; k++) {
            if (v[k].c.w < minW) minW = v[k].c.w;
            if (v[k].c.w > maxW) maxW = v[k].c.w;
        }

        if (depth < SW_SUBDIV_DEPTH && maxW > minW * SW_SUBDIV_RATIO) {
            if (n == 3) {
                ClipVertexSW m01 = midVertexSW(v[0], v[1]);
                ClipVertexSW m12 = midVertexSW(v[1], v[2]);
                ClipVertexSW m20 = midVertexSW(v[2], v[0]);
                ClipVertexSW t0[3] = { v[0], m01,  m20  };
                ClipVertexSW t1[3] = { m01,  v[1], m12  };
                ClipVertexSW t2[3] = { m20,  m12,  v[2] };
                ClipVertexSW t3[3] = { m01,  m12,  m20  };
                emitPolygonSW(t0, 3, depth + 1);
                emitPolygonSW(t1, 3, depth + 1);
                emitPolygonSW(t2, 3, depth + 1);
                emitPolygonSW(t3, 3, depth + 1);
            } else {
                ClipVertexSW m01 = midVertexSW(v[0], v[1]);
                ClipVertexSW m12 = midVertexSW(v[1], v[2]);
                ClipVertexSW m23 = midVertexSW(v[2], v[3]);
                ClipVertexSW m30 = midVertexSW(v[3], v[0]);
                ClipVertexSW mc  = midVertexSW(m01, m23);
                ClipVertexSW q0[4] = { v[0], m01,  mc,   m30  };
                ClipVertexSW q1[4] = { m01,  v[1], m12,  mc   };
                ClipVertexSW q2[4] = { mc,   m12,  v[2], m23  };
                ClipVertexSW q3[4] = { m30,  mc,   m23,  v[3] };
                emitPolygonSW(q0, 4, depth + 1);
                emitPolygonSW(q1, 4, depth + 1);
                emitPolygonSW(q2, 4, depth + 1);
                emitPolygonSW(q3, 4, depth + 1);
            }
            return;
        }

        int32 first = swIndices.length;
        for (int k = 0; k < n; k++) {
            swIndices.push(projectVertexSW(v[k]));
        }
        if (n == 3) {
            swTriangles.push(first);
        } else {
            swQuads.push(first);
        }
    }

    bool transform(const Index *indices, const Vertex *vertices, int iStart, int iCount, int vStart) {
        // What is in the water is tinted and shimmers even seen from dry
        // land, as in the DOS game; with the camera underwater, everything.
        // The level switches to the water palette for water rooms (and for
        // objects in them) in setRoomParams.
        swBatchWater = swUnderwater || swPalette == swPaletteWater;
        swVertices.reset();
        swIndices.reset();
        swTriangles.reset();
        swQuads.reset();

        mat4 swMatrix;
        swMatrix.viewport(0.0f, (float)Core::height, (float)Core::width, -(float)Core::height, 0.0f, 1.0f);
        swMatrix = swMatrix * mViewProj * mModel;

        const bool colored = vertices[vStart + indices[iStart]].color.w == 142;

        swOrthoBatch = true;

        int i = 0;
        while (i < iCount) {
            const bool isTriangle = vertices[vStart + indices[iStart + i]].normal.w == 1;

            // the loader splits quads into two triangles with indices 012[02]3;
            // take positions 0, 1, 2 and 5 to rebuild the quad
            int pos[4];
            int n;
            if (isTriangle) {
                n = 3;
                pos[0] = i; pos[1] = i + 1; pos[2] = i + 2;
                i += 3;
            } else {
                n = 4;
                pos[0] = i; pos[1] = i + 1; pos[2] = i + 2; pos[3] = i + 5;
                i += 6;
            }
            if (pos[n - 1] >= iCount) break;

            ClipVertexSW poly[4];
            bool tooFar    = false;
            bool allFront  = true;
            bool allBehind = true;

            for (int k = 0; k < n; k++) {
                const Vertex &vertex = vertices[vStart + indices[iStart + pos[k]]];
                ClipVertexSW &cv = poly[k];
                {
                cv.c = swMatrix * vec4(vertex.coord.x, vertex.coord.y, vertex.coord.z, 1.0f);

                // Underwater, as the DOS game did it: vertices sway a little and
                // the light shimmers in slow waves. 2D batches (w == 1) are
                // left alone. Shared vertices move identically, so no seams.
                float waterPhase = 0.0f;
                const bool waterFx = swBatchWater && cv.c.w != 1.0f;
                if (waterFx) {
                    waterPhase = swWaterTime * 2.0f + (float(vertex.coord.x) + float(vertex.coord.z)) * (1.0f / 256.0f);
                }
                if (waterFx && swUnderwater) {   // the sway: only when looking through the water
                    vec4 pos = vec4(float(vertex.coord.x) + sinf(waterPhase * 1.3f) * 6.0f,
                                    float(vertex.coord.y) + cosf(waterPhase) * 6.0f,
                                    float(vertex.coord.z), 1.0f);
                    cv.c = swMatrix * pos;
                }

                if (colored) {
                    cv.u = vertex.color.x << 16;
                    cv.v = 0;
                } else {
                    cv.u = (vertex.texCoord.x << 16);
                    cv.v = (vertex.texCoord.y << 16);
                }

                VertexSW lit;
                lit.x = lit.y = lit.z = lit.w = 0;
                lit.u = cv.u;
                lit.v = cv.v;
                // TR3 lights vertices in colour; use the luminance (identical
                // to the old red-channel read for TR1/TR2's grey light)
                lit.l = (((vertex.light.x * 77 + vertex.light.y * 150 + vertex.light.z * 29) >> 8) * ambient) >> 8;
                if (swSkyBatch)
                    lit.l = 0;      // the sky: its own colours, no room light, lamps or fog (as the original)
                else
                    applyLighting(lit, vertex, cv.c.w);
                cv.l = lit.l;
                if (waterFx) {
                    cv.l += int32(sinf(waterPhase * 2.1f + swWaterTime) * 22.0f * 65536.0f);
                }
                // UI geometry (MeshBuilder::addDynBar / addDynFrame) stores its
                // colour in the vertex light field only; the colour field is
                // left unset. Byte order is R, G, B, A.
                cv.color = uint32(vertex.light.x) | (uint32(vertex.light.y) << 8) |
                           (uint32(vertex.light.z) << 16) | (uint32(vertex.light.w) << 24);

                }
                if (cv.c.w > SW_MAX_DIST && !swSkyBatch) tooFar = true;   // the sky is always far: never cut
                // Orthographic (2D) geometry has w == 1 exactly: it is never
                // behind the camera and must not be near-clipped.
                const bool isOrtho = (cv.c.w == 1.0f);
                if (!isOrtho) swOrthoBatch = false;
                if (cv.c.w < SW_NEAR_W && !isOrtho) allFront = false; else allBehind = false;
            }

            if (tooFar || allBehind) continue;

            if (allFront) {
                int32 first = swIndices.length;
                for (int k = 0; k < n; k++) {
                    swIndices.push(projectVertexSW(poly[k]));
                }
                if (n == 3) {
                    swTriangles.push(first);
                } else {
                    swQuads.push(first);
                }
                continue;
            }

            // crosses the near plane: Sutherland-Hodgman against w = SW_NEAR_W
            ClipVertexSW clipped[5];
            int count = 0;
            for (int k = 0; k < n; k++) {
                const ClipVertexSW &a = poly[k];
                const ClipVertexSW &b = poly[(k + 1) % n];
                bool aIn = a.c.w >= SW_NEAR_W;
                bool bIn = b.c.w >= SW_NEAR_W;
                if (aIn) clipped[count++] = a;
                if (aIn != bIn) clipped[count++] = clipEdgeSW(a, b);
            }
            if (count < 3) continue;

            for (int k = 1; k + 1 < count; k++) {
                ClipVertexSW tri[3] = { clipped[0], clipped[k], clipped[k + 1] };
                emitPolygonSW(tri, 3, 0);
            }
        }

        return colored;
    }

    void transformLights() {
        memcpy(lightsRel, lights, sizeof(LightSW) * lightsCount);

        mat4 mModelInv = mModel.inverseOrtho();
        for (int i = 0; i < lightsCount; i++) {
            lightsRel[i].pos = mModelInv * lights[i].pos;
        }
    }

    void DIP(Mesh *mesh, const MeshRange &range) {

        transformLights();

        bool colored = transform(mesh->iBuffer, mesh->vBuffer, range.iStart, range.iCount, range.vStart);

        const bool oldDepthTest  = swDepthTest;
        const bool oldDepthWrite = swDepthWrite;
        if (swOrthoBatch && !swModelInUI) {
            swDepthTest  = false;
            swDepthWrite = false;
        }
        if (curTile == NULL && !swOrthoBatch && !swShadowBatch) {
            // untextured 3D batches stay unsupported, as upstream
            swDepthTest  = oldDepthTest;
            swDepthWrite = oldDepthWrite;
            return;
        }

        Tile8 *oldTile = curTile;

        if (colored) {
            curTile = (Tile8*)swGradient;
        }

        // the picked-up item in the corner is a 3D model drawn without
        // perspective: flagged by the UI, it stays a model (not 2D UI)
        swBeginBatch(swOrthoBatch && !swModelInUI, swQuads.length * 2 + swTriangles.length);

        for (int i = 0; i < swQuads.length; i++) {
            drawQuad(&swIndices[swQuads[i]]);
        }

        for (int i = 0; i < swTriangles.length; i++) {
            drawTriangle(&swIndices[swTriangles[i]]);
        }

        swEndBatch();

        curTile = oldTile;

        swDepthTest  = oldDepthTest;
        swDepthWrite = oldDepthWrite;
    }

    void initPalette(Color24 *palette, uint8 *lightmap) {
        swDrain();   // the pending frame still uses the current palette
        for (uint32 i = 0; i < 256; i++) {
            const Color24 &p = palette[i];
            swPaletteColor[i] = CONV_COLOR(p.r, p.g, p.b);
            swPaletteWater[i] = CONV_COLOR((uint32(p.r) * 150) >> 8, (uint32(p.g) * 230) >> 8, (uint32(p.b) * 230) >> 8);
            swPaletteGray[i]  = CONV_COLOR((i * 57) >> 8, (i * 29) >> 8, (i * 112) >> 8);
            swGradient[i]     = i;
        }

        for (uint32 i = 0; i < 256 * 32; i++) {
            swLightmapNone[i]  = i % 256;
            swLightmapShade[i] = lightmap[i];
        }

        swLightmap = swLightmapShade;
        swPalette  = swPaletteColor;
    }

    void setPalette(ColorSW *palette) {
        swPalette = palette;
    }

    void setShading(bool enabled) {
        swLightmap = enabled ? swLightmapShade : swLightmapNone;
    }

    vec4 copyPixel(int x, int y) {
        return vec4(0.0f); // TODO: read from framebuffer
    }
}

#endif
