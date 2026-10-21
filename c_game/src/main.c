// Forest Starting Level — third-person game in C (raylib), compiled to WebAssembly.
//
// Controls: WASD move (camera-relative), Shift run, Space jump,
//           mouse look (click to capture), wheel zoom, Esc release mouse.

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_WEB)
    #include <emscripten/emscripten.h>
    #include <emscripten/html5.h>
    #define GLSL_HEADER "#version 100\n#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
#else
    #define GLSL_HEADER "#version 100\n"
#endif

// ---------------------------------------------------------------------------
// Lighting shader: texture x material colour x vertex colour, lit by the moon.
// ---------------------------------------------------------------------------
static const char *VS =
    GLSL_HEADER
    "attribute vec3 vertexPosition;\n"
    "attribute vec2 vertexTexCoord;\n"
    "attribute vec3 vertexNormal;\n"
    "attribute vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matNormal;\n"
    "uniform mat4 matModel;\n"
    "varying vec2 fragTexCoord;\n"
    "varying vec4 fragColor;\n"
    "varying vec3 fragNormal;\n"
    "varying vec3 fragPosition;\n"
    "void main() {\n"
    "    fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    fragColor = vertexColor;\n"
    "    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *FS =
    GLSL_HEADER
    "varying vec2 fragTexCoord;\n"
    "varying vec4 fragColor;\n"
    "varying vec3 fragNormal;\n"
    "varying vec3 fragPosition;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 lightDir;\n"
    "uniform vec3 lightColor;\n"
    "uniform vec3 ambient;\n"
    "uniform float unlit;\n"
    "uniform float brightness;\n"
    "uniform float glowOn;\n"
    "uniform vec3 torchPos[16];\n"
    "uniform float torchPower;\n"
    "void main() {\n"
    "    vec4 t = texture2D(texture0, fragTexCoord);\n"
    "    vec4 c = t * colDiffuse * fragColor;\n"
    // texels with alpha ~0.6 are self-lit (house windows, torch flames)
    "    float glow = (t.a > 0.3 && t.a < 0.9) ? glowOn : 0.0;\n"
    "    if (c.a < 0.02) discard;\n"
    "    vec3 n = normalize(fragNormal);\n"
    "    if (!gl_FrontFacing) n = -n;\n"              // walls are seen from both sides
    "    float d = max(dot(n, lightDir), 0.0);\n"
    "    vec3 torch = vec3(0.0);\n"
    "    for (int i = 0; i < 16; i++) {\n"
    "        vec3 L = torchPos[i] - fragPosition;\n"
    "        float dist = length(L);\n"
    "        float a = clamp(1.0 - dist/15.0, 0.0, 1.0);\n"
    "        torch += a*a*(0.6 + 1.5*max(dot(n, L/max(dist, 0.001)), 0.0));\n"
    "    }\n"
    "    vec3 warm = vec3(1.0, 0.52, 0.20) * torch * torchPower;\n"
    "    vec3 lit = mix(ambient + lightColor * d + warm, vec3(1.0), unlit);\n"
    "    vec3 rgb = mix(c.rgb * lit * brightness, t.rgb * colDiffuse.rgb * 1.1, glow);\n"
    "    gl_FragColor = vec4(rgb, mix(c.a, 1.0, glow));\n"
    "}\n";

// ---------------------------------------------------------------------------
// Level data
// ---------------------------------------------------------------------------
#define MAX_CIRCLES 512
#define MAX_FOG     128

typedef struct { float x, z, r; } Circle;
typedef struct { Vector3 pos; float sx, sz, rot; } FogPatch;
typedef struct { Vector3 base; float r, h; bool cone; } Blocker;   // volumes the camera may not enter

static Circle   circles[MAX_CIRCLES]; static int circleCount = 0;
static FogPatch fog[MAX_FOG];         static int fogCount = 0;
static Vector3  spawn = { 0 };
#define MAX_BOXES 256
#define MAX_TORCHES 128
typedef struct { float x, y, z, hx, hz, c, s, h; } Box;     // building footprint, rotated about Y
static Box     boxes[MAX_BOXES];     static int boxCount = 0;
static Vector3 torches[MAX_TORCHES]; static int torchCount = 0;

// Animated set pieces: robed figures (P), the demon (D) and fires (X)
#define MAX_ACTORS 64
typedef struct { Vector3 pos; float rot; } Actor;
static Actor robed[MAX_ACTORS]; static int robedCount = 0;
static Actor demons[8];         static int demonCount = 0;
static Actor fires[MAX_ACTORS]; static int fireCount = 0;     // rot holds the fire's size
static Actor smokes[16];        static int smokeCount = 0;
static int   fireType[MAX_ACTORS];                             // 0 campfire flames, 1 demon-pit flames

// ---------- the robed figure's greeting ----------
static int   talkerIdx = -1;
static bool  talking = false, talkDone = false;
static float talkTime = 0, typed = 0;
static Vector3 talkStand, talkCamPos, talkCamTarget;
static float talkFacing = 0;
static const char *TALK_TEXT = "Greetings, hereby traveler. Take this, for it shall be given to the one down under.";
#define MAX_BLOCKERS 512
static Blocker  blockers[MAX_BLOCKERS]; static int blockerCount = 0;

static void LoadLevelData(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { TraceLog(LOG_ERROR, "missing %s", path); return; }
    char kind;
    float a, b, c, d, e, g;
    while (fscanf(f, " %c", &kind) == 1) {
        if (kind == 'T' || kind == 'C') {               // tree / solid circle (house, rock)
            fscanf(f, "%f %f %f %f", &a, &b, &c, &d);
            if (circleCount < MAX_CIRCLES) circles[circleCount++] = (Circle){ a, c, d };
        } else if (kind == 'F') {                       // fog patch
            fscanf(f, "%f %f %f %f %f %f", &a, &b, &c, &d, &e, &g);
            if (fogCount < MAX_FOG) fog[fogCount++] = (FogPatch){ { a, b, c }, d, e, g };
        } else if (kind == 'B') {                       // camera blocker: x y z radius height cone
            int cone = 0;
            fscanf(f, "%f %f %f %f %f %d", &a, &b, &c, &d, &e, &cone);
            if (blockerCount < MAX_BLOCKERS) blockers[blockerCount++] = (Blocker){ { a, b, c }, d, e, cone != 0 };
        } else if (kind == 'H') {                       // building: x y z halfX halfZ rotation height
            float rot, hgt;
            fscanf(f, "%f %f %f %f %f %f %f", &a, &b, &c, &d, &e, &rot, &hgt);
            if (boxCount < MAX_BOXES) boxes[boxCount++] = (Box){ a, b, c, d, e, cosf(rot), sinf(rot), hgt };
        } else if (kind == 'L') {                       // torch flame (light source)
            fscanf(f, "%f %f %f", &a, &b, &c);
            if (torchCount < MAX_TORCHES) torches[torchCount++] = (Vector3){ a, b, c };
        } else if (kind == 'M') {                       // smoke column
            fscanf(f, "%f %f %f %f", &a, &b, &c, &d);
            if (smokeCount < 16) smokes[smokeCount++] = (Actor){ { a, b, c }, d };
        } else if (kind == 'P' || kind == 'D' || kind == 'X') {   // robed figure / demon / fire
            int extra = 0;
            fscanf(f, "%f %f %f %f", &a, &b, &c, &d);
            if (kind != 'D') fscanf(f, "%d", &extra);       // P: talks to the player, X: fire type
            Actor ac = { { a, b, c }, d };
            if (kind == 'P' && robedCount < MAX_ACTORS) { if (extra) talkerIdx = robedCount; robed[robedCount++] = ac; }
            if (kind == 'D' && demonCount < 8) demons[demonCount++] = ac;
            if (kind == 'X' && fireCount < MAX_ACTORS) { fireType[fireCount] = extra; fires[fireCount++] = ac; }
        } else if (kind == 'S') {                       // player spawn
            fscanf(f, "%f %f %f", &a, &b, &c);
            spawn = (Vector3){ a, b, c };
        } else {
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        }
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// Height field: the walkable meshes rasterised once into a 1 m grid, so ground
// height is a cheap lookup instead of a ray cast every frame.
// Cells with no ground (past the cliff walls) are marked as "no ground".
// ---------------------------------------------------------------------------
#define NO_GROUND -1.0e9f
static float *hf = NULL;
static int    hfW = 0, hfH = 0;
static float  hfX0 = 0, hfZ0 = 0;
static const float HF_CELL = 1.0f;

static Vector3 MeshVertex(const Mesh *m, int i)
{
    return (Vector3){ m->vertices[3*i], m->vertices[3*i + 1], m->vertices[3*i + 2] };
}

static void RasterTriangle(Vector3 a, Vector3 b, Vector3 c)
{
    float minX = fminf(a.x, fminf(b.x, c.x)), maxX = fmaxf(a.x, fmaxf(b.x, c.x));
    float minZ = fminf(a.z, fminf(b.z, c.z)), maxZ = fmaxf(a.z, fmaxf(b.z, c.z));
    int i0 = (int)floorf((minX - hfX0)/HF_CELL), i1 = (int)ceilf((maxX - hfX0)/HF_CELL);
    int j0 = (int)floorf((minZ - hfZ0)/HF_CELL), j1 = (int)ceilf((maxZ - hfZ0)/HF_CELL);
    float den = (b.z - c.z)*(a.x - c.x) + (c.x - b.x)*(a.z - c.z);
    if (fabsf(den) < 1e-6f) return;
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        if (i < 0 || j < 0 || i >= hfW || j >= hfH) continue;
        float px = hfX0 + i*HF_CELL, pz = hfZ0 + j*HF_CELL;
        float w1 = ((b.z - c.z)*(px - c.x) + (c.x - b.x)*(pz - c.z))/den;
        float w2 = ((c.z - a.z)*(px - c.x) + (a.x - c.x)*(pz - c.z))/den;
        float w3 = 1.0f - w1 - w2;
        if (w1 < -1e-4f || w2 < -1e-4f || w3 < -1e-4f) continue;
        float y = w1*a.y + w2*b.y + w3*c.y;
        if (y > hf[j*hfW + i]) hf[j*hfW + i] = y;
    }
}

static void BuildHeightField(const Model *m, const bool *walkable)
{
    BoundingBox bb = { { 1e9f, 1e9f, 1e9f }, { -1e9f, -1e9f, -1e9f } };
    for (int k = 0; k < m->meshCount; k++) if (walkable[m->meshMaterial[k]]) {
        BoundingBox b = GetMeshBoundingBox(m->meshes[k]);
        bb.min = Vector3Min(bb.min, b.min); bb.max = Vector3Max(bb.max, b.max);
    }
    hfX0 = bb.min.x - 2; hfZ0 = bb.min.z - 2;
    hfW = (int)((bb.max.x - hfX0)/HF_CELL) + 4;
    hfH = (int)((bb.max.z - hfZ0)/HF_CELL) + 4;
    hf = malloc(sizeof(float)*hfW*hfH);
    for (int i = 0; i < hfW*hfH; i++) hf[i] = NO_GROUND;

    for (int k = 0; k < m->meshCount; k++) {
        if (!walkable[m->meshMaterial[k]]) continue;
        const Mesh *me = &m->meshes[k];
        int tris = me->triangleCount;
        for (int t = 0; t < tris; t++) {
            int i0 = 3*t, i1 = 3*t + 1, i2 = 3*t + 2;
            if (me->indices) { i0 = me->indices[i0]; i1 = me->indices[i1]; i2 = me->indices[i2]; }
            RasterTriangle(MeshVertex(me, i0), MeshVertex(me, i1), MeshVertex(me, i2));
        }
    }
}

// Returns false where there is no ground (outside the map).
static bool GroundHeight(float x, float z, float *out)
{
    float fx = (x - hfX0)/HF_CELL, fz = (z - hfZ0)/HF_CELL;
    int i = (int)floorf(fx), j = (int)floorf(fz);
    if (i < 0 || j < 0 || i + 1 >= hfW || j + 1 >= hfH) return false;
    float h00 = hf[j*hfW + i], h10 = hf[j*hfW + i + 1];
    float h01 = hf[(j + 1)*hfW + i], h11 = hf[(j + 1)*hfW + i + 1];
    if (h00 == NO_GROUND || h10 == NO_GROUND || h01 == NO_GROUND || h11 == NO_GROUND) return false;
    float tx = fx - i, tz = fz - j;
    *out = Lerp(Lerp(h00, h10, tx), Lerp(h01, h11, tx), tz);
    return true;
}

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------
static Model    level, robedModel, demonModel, fireModels[2], smokeModel;

// The knight is split into body + limbs; limbs swing around their hip/shoulder pivots.
enum { PART_BODY, PART_LEG_L, PART_LEG_R, PART_ARM_L, PART_ARM_R, PART_COUNT };
static const char *PART_NAMES[PART_COUNT] = { "body", "legL", "legR", "armL", "armR" };
static Model   knightPart[PART_COUNT];
static Vector3 knightPivot[PART_COUNT];
static Shader   lit;
static int      locUnlit, locGlowOn, locTorchPos, locTorchPower;
static Texture2D sky, fogTex;
static Mesh     fogMesh;
static Material fogMat;
static Camera3D camera = { 0 };

static Vector3 playerPos;
static float   playerVy = 0, facing = 0, walkT = 0;
static bool    onGround = true;
static float   yaw, pitch = 0.32f, camDist = 14.0f;
static bool    paused = true;
static float   knightFacing = 0, walkBlend = 0;

static const float PLAYER_R = 0.9f;
static bool  clickedToPlay = false;   // set by the browser's click event (never missed, even for very short clicks)
static bool  wasLocked = false;

#if defined(PLATFORM_WEB)
static bool OnCanvasClick(int type, const EmscriptenMouseEvent *e, void *user)
{
    (void)type; (void)e; (void)user;
    clickedToPlay = true;
    return false;
}
#endif

static Texture2D Tex(const char *name, bool pixelated)
{
    Texture2D t = LoadTexture(TextFormat("assets/tex/%s", name));
    SetTextureFilter(t, pixelated ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    return t;
}

// Material setup by name (names come from the Blender export, in glTF order).
// Textures and tints by material name. Names come from the Blender export, in glTF order.
static void SetupModelMaterials(Model *model, const char *namesPath, bool *walkable)
{
    char names[32][32] = { 0 };
    int count = 0;
    FILE *f = fopen(namesPath, "r");
    if (f) { while (count < 32 && fscanf(f, "%31s", names[count]) == 1) count++; fclose(f); }

    for (int i = 0; i < model->materialCount; i++) { model->materials[i].shader = lit; if (walkable) walkable[i] = false; }

    for (int g = 0; g < count; g++) {
        int mi = g + 1;                       // raylib keeps a default material at index 0
        if (mi >= model->materialCount) break;
        Material *m = &model->materials[mi];
        bool walk = false;
        Color tint = WHITE;
        Texture2D t = { 0 };
        const char *n = names[g];
        if      (!strcmp(n, "GrassShaded")) { t = Tex("grass.png", false); tint = (Color){ 120, 120, 120, 255 }; walk = true; }
        else if (!strcmp(n, "Grass"))       { t = Tex("grass.png", false); tint = (Color){ 110, 110, 110, 255 }; }
        else if (!strcmp(n, "WallTex"))     { t = Tex("wall.png", true); }
        else if (!strcmp(n, "TownStone"))   { t = Tex("flagstone.png", true); walk = true; }
        else if (!strcmp(n, "CaveBlack"))   { tint = BLACK; }
        else if (!strcmp(n, "PineTrunk"))   { t = Tex("pine_trunk.png", true); }
        else if (!strcmp(n, "PineLeaves"))  { t = Tex("pine_leaves.png", true); tint = (Color){ 70, 105, 78, 255 }; }
        else if (!strcmp(n, "HouseWall"))   { t = Tex("timber.png", true); }
        else if (!strcmp(n, "HouseRoof"))   { t = Tex("slate.png", true); }
        else if (!strcmp(n, "TowerBrick"))  { t = Tex("brick.png", true); }
        else if (!strcmp(n, "TowerTrim"))   { t = Tex("brick_dark.png", true); }
        else if (!strcmp(n, "Iron"))        { t = Tex("iron.png", true); }
        else if (!strcmp(n, "TorchWood"))   { t = Tex("torchwood.png", true); }
        else if (!strcmp(n, "Flame"))       { t = Tex("flame.png", true); }
        else if (!strcmp(n, "Log"))         { t = Tex("log.png", true); }
        else if (!strcmp(n, "PitStone"))    { t = Tex("pitstone.png", true); }
        else if (!strcmp(n, "Lava"))        { t = Tex("lava.png", true); }
        else if (!strcmp(n, "RobedFront"))  { t = Tex("robed_front.png", false); }   // pixels from the reference image
        else if (!strcmp(n, "RobedBack"))   { t = Tex("robed_back.png", false); }
        else if (!strcmp(n, "DemonFront"))  { t = Tex("demon_front.png", false); }
        else if (!strcmp(n, "DemonBack"))   { t = Tex("demon_back.png", false); }
        else if (!strcmp(n, "HutWall"))     { t = Tex("hut_stone.png", false); }
        else if (!strcmp(n, "HutTeal"))     { t = Tex("hut_teal.png", false); }
        else if (!strcmp(n, "HutBand"))     { t = Tex("hut_band.png", false); }
        else if (!strcmp(n, "HutFloor"))    { t = Tex("hut_stone.png", false); tint = (Color){ 150, 150, 160, 255 }; walk = true; }
        else if (!strcmp(n, "Wood"))        { t = Tex("chest_wood.png", true); }
        else if (!strcmp(n, "Fire"))        { t = Tex("fire.png", true); }
        else if (!strncmp(n, "Img_", 4))    { t = Tex(TextFormat("%s.png", n + 4), false); walk = !strcmp(n, "Img_hut_floor"); }
        if (walkable) walkable[mi] = walk;
        if (t.id) m->maps[MATERIAL_MAP_DIFFUSE].texture = t;
        m->maps[MATERIAL_MAP_DIFFUSE].color = tint;
    }
}

static void InitGame(void)
{
    lit = LoadShaderFromMemory(VS, FS);
    Vector3 lightDir = Vector3Normalize((Vector3){ -0.55f, 1.0f, 0.45f });
    Vector3 lightColor = { 0.80f, 0.74f, 1.00f };
    Vector3 ambient = { 0.36f, 0.32f, 0.52f };
    float zero = 0.0f;
    SetShaderValue(lit, GetShaderLocation(lit, "lightDir"), &lightDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit, GetShaderLocation(lit, "lightColor"), &lightColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit, GetShaderLocation(lit, "ambient"), &ambient, SHADER_UNIFORM_VEC3);
    locUnlit = GetShaderLocation(lit, "unlit");
    locGlowOn = GetShaderLocation(lit, "glowOn");
    locTorchPos = GetShaderLocation(lit, "torchPos");
    locTorchPower = GetShaderLocation(lit, "torchPower");
    float one = 1.0f;
    SetShaderValue(lit, locGlowOn, &one, SHADER_UNIFORM_FLOAT);
    float brightness = 0.7f;                                   // whole scene 30% darker
    SetShaderValue(lit, GetShaderLocation(lit, "brightness"), &brightness, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lit, locUnlit, &zero, SHADER_UNIFORM_FLOAT);

    level = LoadModel("assets/level.glb");
    bool walkable[64] = { 0 };
    SetupModelMaterials(&level, "assets/materials.txt", walkable);
    robedModel = LoadModel("assets/robed.glb"); SetupModelMaterials(&robedModel, "assets/robed.mats", NULL);
    demonModel = LoadModel("assets/demon.glb"); SetupModelMaterials(&demonModel, "assets/demon.mats", NULL);
    fireModels[0] = LoadModel("assets/fire_camp.glb"); SetupModelMaterials(&fireModels[0], "assets/fire_camp.mats", NULL);
    fireModels[1] = LoadModel("assets/fire_pit.glb");  SetupModelMaterials(&fireModels[1], "assets/fire_pit.mats",  NULL);
    smokeModel = LoadModel("assets/smoke.glb"); SetupModelMaterials(&smokeModel, "assets/smoke.mats", NULL);
    BuildHeightField(&level, walkable);

    FILE *pf = fopen("assets/knight_parts.txt", "r");
    char pname[16]; float px, py, pz;
    while (pf && fscanf(pf, "%15s %f %f %f", pname, &px, &py, &pz) == 4)
        for (int p = 0; p < PART_COUNT; p++) if (!strcmp(pname, PART_NAMES[p])) knightPivot[p] = (Vector3){ px, py, pz };
    if (pf) fclose(pf);
    for (int p = 0; p < PART_COUNT; p++) {
        knightPart[p] = LoadModel(TextFormat("assets/knight_%s.glb", PART_NAMES[p]));
        for (int i = 0; i < knightPart[p].materialCount; i++) {
            knightPart[p].materials[i].shader = lit;
            knightPart[p].materials[i].maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 235, 225, 200, 255 };   // offsets the lavender moonlight
        }
    }

    sky = LoadTexture("assets/tex/night_sky.png");
    SetTextureFilter(sky, TEXTURE_FILTER_BILINEAR);

    fogTex = LoadTexture("assets/tex/fog.png");
    SetTextureFilter(fogTex, TEXTURE_FILTER_BILINEAR);
    fogMesh = GenMeshPlane(2.0f, 2.0f, 1, 1);
    fogMat = LoadMaterialDefault();
    fogMat.shader = lit;
    fogMat.maps[MATERIAL_MAP_DIFFUSE].texture = fogTex;

    LoadLevelData("assets/level.txt");
    playerPos = spawn;
    facing = atan2f(-spawn.x, -spawn.z);          // look into the map from the spawn corner
    yaw = facing + PI;
#ifdef TEST_X      // dev builds only: start somewhere else (-DTEST_X=.. -DTEST_Z=.. -DTEST_FACING=..)
    playerPos = (Vector3){ TEST_X, 0, TEST_Z };
    GroundHeight(playerPos.x, playerPos.z, &playerPos.y);
    facing = TEST_FACING; yaw = facing + PI;
#endif

    camera.up = (Vector3){ 0, 1, 0 };
    camera.fovy = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    camera.position = Vector3Add(playerPos, (Vector3){ sinf(yaw)*camDist, 8, cosf(yaw)*camDist });
    camera.target = playerPos;
}

static float WrapAngle(float a) { return atan2f(sinf(a), cosf(a)); }

// point -> box-local XZ (inverse of the box's rotation about Y)
static void BoxLocal(const Box *b, float x, float z, float *lx, float *lz)
{
    float dx = x - b->x, dz = z - b->z;
    *lx = dx*b->c - dz*b->s;
    *lz = dx*b->s + dz*b->c;
}

// Push a circle of radius r out of a building footprint.
static void PushOutOfBox(const Box *b, float *px, float *pz, float r)
{
    float lx, lz; BoxLocal(b, *px, *pz, &lx, &lz);
    float qx = Clamp(lx, -b->hx, b->hx), qz = Clamp(lz, -b->hz, b->hz);
    float ex = lx - qx, ez = lz - qz, dist = sqrtf(ex*ex + ez*ez);
    if (dist >= r) return;
    if (dist > 1e-4f) { lx = qx + ex/dist*r; lz = qz + ez/dist*r; }
    else if (b->hx - fabsf(lx) < b->hz - fabsf(lz)) lx = copysignf(b->hx + r, lx);
    else lz = copysignf(b->hz + r, lz);
    *px = b->x + lx*b->c + lz*b->s;
    *pz = b->z - lx*b->s + lz*b->c;
}

static bool InsideBlocker(Vector3 p)
{
    for (int i = 0; i < boxCount; i++) {
        const Box *b = &boxes[i];
        if (p.y < b->y - 0.5f || p.y > b->y + b->h + 0.5f) continue;
        float lx, lz; BoxLocal(b, p.x, p.z, &lx, &lz);
        if (fabsf(lx) < b->hx + 0.6f && fabsf(lz) < b->hz + 0.6f) return true;
    }
    for (int i = 0; i < blockerCount; i++) {
        const Blocker *b = &blockers[i];
        float up = p.y - b->base.y;
        if (up < -0.5f || up > b->h + 0.5f) continue;
        float r = b->cone ? b->r*(1.0f - up/b->h) + 1.2f : b->r + 0.6f;   // pine canopy narrows toward the tip
        float dx = p.x - b->base.x, dz = p.z - b->base.z;
        if (dx*dx + dz*dz < r*r) return true;
    }
    return false;
}

// How far the camera can sit along target->want before entering a tree, house or rock (0..1).
static float CameraClearance(Vector3 target, Vector3 want)
{
    const int STEPS = 32;
    float last = 0.0f;
    for (int s = 1; s <= STEPS; s++) {
        float t = (float)s/STEPS;
        Vector3 p = Vector3Lerp(target, want, t);
        float g;
        if (InsideBlocker(p) || (GroundHeight(p.x, p.z, &g) && p.y < g + 0.8f)) return last;
        last = t;
    }
    return 1.0f;
}

static void UpdateGame(float dt)
{
    // --- mouse capture / look ---
    bool locked = IsCursorHidden();
    if (paused && (clickedToPlay || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) { paused = false; DisableCursor(); }
    clickedToPlay = false;
    if (!paused && IsKeyPressed(KEY_ESCAPE)) { paused = true; EnableCursor(); }
    if (wasLocked && !locked) paused = true;     // browser released the mouse (Esc)
    wasLocked = locked;
    if (!paused && !talking) {
        Vector2 md = GetMouseDelta();
        if (locked || IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            yaw -= md.x*0.0025f;
            pitch = Clamp(pitch + md.y*0.0025f, -0.9f, 1.2f);
        }
    }
    camDist = Clamp(camDist - GetMouseWheelMove()*1.5f, 6.0f, 40.0f);

    // --- movement ---
    bool moving = false, run = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    // walk up to the robed group -> the front one on the right speaks
    if (!talking && !talkDone && talkerIdx >= 0 && !paused) {
        Vector3 np = robed[talkerIdx].pos;
        if (Vector2Distance((Vector2){ playerPos.x, playerPos.z }, (Vector2){ np.x, np.z }) < 15.0f) {
            float r = robed[talkerIdx].rot;
            Vector3 front = { sinf(r), 0, cosf(r) };                     // the figure's facing direction
            talkStand = Vector3Add(np, Vector3Scale(front, 4.6f));
            GroundHeight(talkStand.x, talkStand.z, &talkStand.y);
            talkFacing = atan2f(-front.x, -front.z);
            talking = true; talkTime = 0; typed = 0;
            talkCamPos = camera.position; talkCamTarget = camera.target;
        }
    }
    if (talking) {
        talkTime += dt;
        Vector3 to = Vector3Subtract(talkStand, playerPos); to.y = 0;
        float dist = Vector3Length(to);
        if (dist > 0.15f) {                                              // walk into place
            Vector3 step = Vector3Scale(to, fminf(1.0f, 8.0f*dt/dist));
            playerPos.x += step.x; playerPos.z += step.z;
            facing = atan2f(to.x, to.z); moving = true;
        } else facing = talkFacing;
        if (talkTime > 0.9f) typed += dt*32.0f;                          // typewriter: 32 letters a second
        int len = (int)strlen(TALK_TEXT);
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            if (typed < len) typed = (float)len;                         // first press finishes the line
            else { talking = false; talkDone = true; }                   // second press is OK
        }
    }
    if (!paused && !talking) {
        float fwd = (float)(IsKeyDown(KEY_W) - IsKeyDown(KEY_S));
        float str = (float)(IsKeyDown(KEY_D) - IsKeyDown(KEY_A));
        if (fwd != 0 || str != 0) {
            Vector3 camF = { -sinf(yaw), 0, -cosf(yaw) };
            Vector3 camR = { -camF.z, 0, camF.x };
            Vector3 dir = Vector3Normalize(Vector3Add(Vector3Scale(camF, fwd), Vector3Scale(camR, str)));
            float speed = run ? 20.0f : 10.0f;
            float px = playerPos.x + dir.x*speed*dt, pz = playerPos.z + dir.z*speed*dt;
            for (int i = 0; i < circleCount; i++) {   // push out of trees / houses / rocks
                float dx = px - circles[i].x, dz = pz - circles[i].z;
                float d = sqrtf(dx*dx + dz*dz), min = circles[i].r + PLAYER_R;
                if (d < min && d > 1e-4f) { px = circles[i].x + dx/d*min; pz = circles[i].z + dz/d*min; }
            }
            for (int i = 0; i < boxCount; i++) PushOutOfBox(&boxes[i], &px, &pz, PLAYER_R);   // houses / towers
            float h;
            if (GroundHeight(px, pz, &h)) { playerPos.x = px; playerPos.z = pz; }   // cliff walls: no ground beyond
            facing = atan2f(dir.x, dir.z);
            moving = true;
        }
        if (onGround && IsKeyDown(KEY_SPACE)) { playerVy = 13.0f; onGround = false; }
    }

    // --- gravity ---
    float gy = playerPos.y;
    GroundHeight(playerPos.x, playerPos.z, &gy);
    playerVy -= 32.0f*dt;
    playerPos.y += playerVy*dt;
    if (playerPos.y <= gy) { playerPos.y = gy; playerVy = 0; onGround = true; }
    else if (playerPos.y - gy > 0.3f) onGround = false;

    walkT += moving ? dt*(run ? 14.0f : 9.0f) : 0.0f;

    // --- third-person camera on a spring arm ---
    Vector3 target = Vector3Add(playerPos, (Vector3){ 0, 3.2f, 0 });
    float armPitch = fmaxf(pitch, -0.30f);     // below this the arm stays low and the view tilts up instead
    Vector3 off = { sinf(yaw)*cosf(armPitch)*camDist, sinf(armPitch)*camDist, cosf(yaw)*cosf(armPitch)*camDist };
    Vector3 want = Vector3Add(target, off);
    float cg;
    if (GroundHeight(want.x, want.z, &cg) && want.y < cg + 1.5f) want.y = cg + 1.5f;

    // keep the camera out of trees, houses and rocks: pull it in front of whatever is in the way
    static float shownT = 1.0f;
    float clearT = fmaxf(CameraClearance(target, want), 2.5f/camDist);
    shownT = (clearT < shownT) ? clearT : shownT + (clearT - shownT)*fminf(1.0f, dt*4.0f);   // snap in, ease out
    camera.position = Vector3Lerp(target, want, shownT);
    camera.target = target;
    if (pitch < -0.30f) camera.target.y += (-0.30f - pitch)*40.0f;   // look up at the towers
    if (talking) {
        Vector3 np = robed[talkerIdx].pos;
        Vector3 dirN = Vector3Subtract(np, talkStand); dirN.y = 0; dirN = Vector3Normalize(dirN);
        Vector3 right = { -dirN.z, 0, dirN.x };
        Vector3 shotPos = Vector3Add(Vector3Add(talkStand, Vector3Scale(dirN, -3.6f)), Vector3Add(Vector3Scale(right, 2.3f), (Vector3){ 0, 3.6f, 0 }));
        Vector3 shotTarget = Vector3Add(Vector3Add(np, Vector3Scale(right, 1.4f)), (Vector3){ 0, 3.0f, 0 });
        float k = fminf(1.0f, dt*3.0f);
        talkCamPos = Vector3Lerp(talkCamPos, shotPos, k);
        talkCamTarget = Vector3Lerp(talkCamTarget, shotTarget, k);
        camera.position = talkCamPos; camera.target = talkCamTarget;
    }

    // turn the knight smoothly toward the walking direction
    static float shownFacing = 0;
    shownFacing += WrapAngle(facing - shownFacing)*fminf(1.0f, dt*12.0f);
    knightFacing = shownFacing;
    float target01 = (moving && onGround) ? 1.0f : 0.0f;
    walkBlend += (target01 - walkBlend)*fminf(1.0f, dt*10.0f);   // ease into / out of the stride
}

// Walk cycle: legs swing from the hips; the torso (body + arms) rides on top of the hips,
// twisting against the stride, rocking side to side over the planted foot, leaning forward
// and bobbing twice per step. Arms counter-swing from the shoulders.
static void DrawKnight(void)
{
    float swing = sinf(walkT)*walkBlend;
    float bob   = fabsf(sinf(walkT))*0.20f*walkBlend;
    float legA  = 0.75f*swing, armA = 0.50f*swing;

    Matrix root = MatrixMultiply(MatrixRotateY(knightFacing),
                                 MatrixTranslate(playerPos.x, playerPos.y + bob, playerPos.z));

    // torso pivots at hip height so it twists/sways around the waist, not the feet
    float hipY = knightPivot[PART_LEG_L].y;
    Matrix torso = MatrixMultiply(MatrixTranslate(0, -hipY, 0),
                   MatrixMultiply(MatrixRotateZ(0.07f*swing),                  // rock over the planted foot
                   MatrixMultiply(MatrixRotateY(-0.22f*swing),                 // twist against the legs
                   MatrixMultiply(MatrixRotateX(0.10f*walkBlend + 0.04f*fabsf(swing)),   // lean into the step
                                  MatrixTranslate(0, hipY, 0)))));

    float angle[PART_COUNT] = { 0, legA, -legA, -armA, armA };
    for (int p = 0; p < PART_COUNT; p++) {
        Matrix local = MatrixMultiply(MatrixRotateX(angle[p]),
                                      MatrixTranslate(knightPivot[p].x, knightPivot[p].y, knightPivot[p].z));
        bool isLeg = (p == PART_LEG_L || p == PART_LEG_R);
        knightPart[p].transform = MatrixMultiply(isLeg ? local : MatrixMultiply(local, torso), root);
        DrawModel(knightPart[p], Vector3Zero(), 1.0f, WHITE);
    }
}

static void UpdateTorchLights(void)
{
    Vector3 near[16];
    float   nd[16];
    for (int k = 0; k < 16; k++) { near[k] = (Vector3){ 0, -10000, 0 }; nd[k] = 1e30f; }
    for (int i = 0; i < torchCount; i++) {
        float dd = Vector3DistanceSqr(torches[i], playerPos);
        int worst = 0;
        for (int k = 1; k < 16; k++) if (nd[k] > nd[worst]) worst = k;
        if (dd < nd[worst]) { nd[worst] = dd; near[worst] = torches[i]; }
    }
    SetShaderValueV(lit, locTorchPos, near, SHADER_UNIFORM_VEC3, 16);
    float t = (float)GetTime();
    float power = 1.0f + 0.08f*sinf(t*13.0f) + 0.05f*sinf(t*7.3f + 1.7f);
    SetShaderValue(lit, locTorchPower, &power, SHADER_UNIFORM_FLOAT);
}

static void DrawActor(Model *m, Matrix local, Vector3 pos, float rot)
{
    m->transform = MatrixMultiply(local, MatrixMultiply(MatrixRotateY(rot), MatrixTranslate(pos.x, pos.y, pos.z)));
    DrawModel(*m, Vector3Zero(), 1.0f, WHITE);
}

// Robed figures sway and breathe; the demon heaves up and down in its pit; fires flicker and turn.
static void DrawActors(void)
{
    float t = (float)GetTime();
    for (int i = 0; i < robedCount; i++) {
        float ph = i*1.7f;
        Matrix local = MatrixMultiply(MatrixScale(1.0f, 1.0f + 0.015f*sinf(t*1.6f + ph), 1.0f),
                                      MatrixRotateZ(0.03f*sinf(t*0.9f + ph)));
        DrawActor(&robedModel, local, robed[i].pos, robed[i].rot);
    }
    for (int i = 0; i < demonCount; i++) {
        float breathe = sinf(t*0.8f);     // small enough that the hands stay on the pit rim
        Matrix local = MatrixMultiply(MatrixRotateX(-0.02f*breathe), MatrixTranslate(0, 0.06f*breathe, 0));
        DrawActor(&demonModel, local, demons[i].pos, demons[i].rot);
    }
    for (int i = 0; i < fireCount; i++) {
        float sz = fires[i].rot;
        float flick = 1.0f + 0.12f*sinf(t*9.0f + i*2.1f) + 0.06f*sinf(t*15.7f + i);
        float sway = (fireType[i] == 1) ? 0.15f*sinf(t*0.9f + i) : t*0.8f + i;   // pit sheet faces out, campfire turns
        Matrix local = MatrixMultiply(MatrixScale(sz*(1.0f + 0.04f*sinf(t*11.0f + i)), sz*flick, sz), MatrixRotateY(sway));
        DrawActor(&fireModels[fireType[i] ? 1 : 0], local, fires[i].pos, fireType[i] == 1 && demonCount ? demons[0].rot : 0);
    }
}

static void DrawGame(void)
{
    UpdateTorchLights();
    BeginDrawing();
    ClearBackground((Color){ 11, 8, 24, 255 });

    // sky backdrop, scaled to cover the screen
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    float s = fmaxf(sw/sky.width, sh/sky.height);
    DrawTexturePro(sky, (Rectangle){ 0, 0, (float)sky.width, (float)sky.height },
                   (Rectangle){ (sw - sky.width*s)/2, (sh - sky.height*s)/2, sky.width*s, sky.height*s },
                   (Vector2){ 0 }, 0, (Color){ 178, 178, 178, 255 });   // sky 30% darker too

    BeginMode3D(camera);
        rlDisableBackfaceCulling();
        DrawModel(level, Vector3Zero(), 1.0f, WHITE);

        DrawKnight();
        DrawActors();

        // smoky fog: unlit, alpha-blended, no depth writes
        float one = 1.0f, zero = 0.0f;
        SetShaderValue(lit, locUnlit, &one, SHADER_UNIFORM_FLOAT);
        SetShaderValue(lit, locGlowOn, &zero, SHADER_UNIFORM_FLOAT);
        BeginBlendMode(BLEND_ALPHA);
        rlDisableDepthMask();
        for (int i = 0; i < fogCount; i++) {
            Matrix t = MatrixMultiply(MatrixMultiply(MatrixScale(fog[i].sx, 1, fog[i].sz), MatrixRotateY(fog[i].rot)),
                                      MatrixTranslate(fog[i].pos.x, fog[i].pos.y, fog[i].pos.z));
            DrawMesh(fogMesh, fogMat, t);
        }
        // campfire smoke: two slowly turning, breathing columns
        float tt = (float)GetTime();
        for (int i = 0; i < smokeCount; i++)
            for (int k = 0; k < 2; k++) {
                float ph = k*2.3f + i;
                Matrix local = MatrixMultiply(MatrixScale(1.0f + 0.12f*sinf(tt*0.7f + ph), 1.0f + 0.08f*sinf(tt*0.5f + ph), 1.0f),
                                              MatrixRotateY(tt*(k ? -0.15f : 0.2f) + ph));
                DrawActor(&smokeModel, local, smokes[i].pos, 0);
            }
        rlEnableDepthMask();
        EndBlendMode();
        SetShaderValue(lit, locUnlit, &zero, SHADER_UNIFORM_FLOAT);
        SetShaderValue(lit, locGlowOn, &one, SHADER_UNIFORM_FLOAT);
        rlEnableBackfaceCulling();
    EndMode3D();

    // dialogue box with typewriter text
    if (talking && talkTime > 0.9f) {
        float bw = sw*0.88f, bx = (sw - bw)/2;
        int fs = (int)fmaxf(18.0f, fminf(sh*0.048f, sw*0.05f)), spacing = fs/10;
        // wrap the whole line first so words never jump while they are being typed
        char lines[6][128] = { 0 }; int nl = 0, start = 0, len = (int)strlen(TALK_TEXT), lineStart[6] = { 0 };
        while (start < len && nl < 6) {
            int end = start, lastSpace = -1;
            while (end < len) {
                char tmp[128] = { 0 }; int n = end - start + 1; if (n > 127) break;
                memcpy(tmp, TALK_TEXT + start, n);
                if (MeasureTextEx(GetFontDefault(), tmp, (float)fs, (float)spacing).x > bw - fs*2.2f) break;
                if (TALK_TEXT[end] == ' ') lastSpace = end;
                end++;
            }
            if (end < len && lastSpace > start) end = lastSpace + 1;
            lineStart[nl] = start; memcpy(lines[nl], TALK_TEXT + start, end - start); nl++; start = end;
        }
        float bh = nl*fs*1.35f + fs*1.9f, by = sh - bh - sh*0.05f;          // box grows to fit the wrapped text
        DrawRectangleRounded((Rectangle){ bx, by, bw, bh }, 0.18f, 8, (Color){ 20, 20, 24, 175 });
        int shown = (int)typed;
        for (int i = 0; i < nl; i++) {
            int visible = shown - lineStart[i]; if (visible <= 0) break;
            char part[128] = { 0 }; int n = (int)strlen(lines[i]); memcpy(part, lines[i], visible < n ? visible : n);
            Vector2 p = { bx + fs*1.1f, by + fs*0.9f + i*fs*1.35f };
            for (int ox = -2; ox <= 2; ox += 2) for (int oy = -2; oy <= 2; oy += 2)          // dark outline
                DrawTextEx(GetFontDefault(), part, (Vector2){ p.x + ox, p.y + oy }, (float)fs, (float)spacing, (Color){ 0, 0, 0, 200 });
            DrawTextEx(GetFontDefault(), part, p, (float)fs, (float)spacing, RAYWHITE);
        }
        if (shown >= len && fmodf((float)GetTime(), 1.0f) < 0.65f) {
            const char *ok = "ENTER  OK";
            int ofs = (int)(fs*0.6f);
            Vector2 sz = MeasureTextEx(GetFontDefault(), ok, (float)ofs, (float)ofs/10);
            float tx = bx + bw - sz.x - fs*1.2f, ty = by + bh - sz.y - fs*0.5f;
            DrawTriangle((Vector2){ tx - ofs*1.4f, ty }, (Vector2){ tx - ofs*0.9f, ty + ofs*0.8f }, (Vector2){ tx - ofs*0.4f, ty }, (Color){ 60, 140, 255, 255 });
            DrawTextEx(GetFontDefault(), ok, (Vector2){ tx, ty }, (float)ofs, (float)ofs/10, (Color){ 210, 225, 255, 255 });
        }
    }

    // HUD (hidden during the conversation)
    if (!talking) {
        DrawRectangle(10, (int)sh - 34, 330, 24, (Color){ 14, 10, 28, 180 });
        DrawText("WASD move  Shift run  Space jump  Esc pause", 18, (int)sh - 28, 12, (Color){ 200, 194, 225, 255 });
    }
    if (paused) {
        int w = 360, h = 150, x = ((int)sw - w)/2, y = ((int)sh - h)/2;
        DrawRectangle(0, 0, (int)sw, (int)sh, (Color){ 8, 5, 20, 120 });
        DrawRectangle(x, y, w, h, (Color){ 14, 10, 28, 220 });
        DrawRectangleLines(x, y, w, h, (Color){ 59, 50, 96, 255 });
        DrawText("Forest Starting Level", x + 50, y + 20, 24, RAYWHITE);
        DrawText("WASD move  -  Shift run  -  Space jump", x + 40, y + 62, 14, (Color){ 169, 162, 196, 255 });
        DrawText("Mouse look  -  Wheel zoom  -  Esc pause", x + 38, y + 82, 14, (Color){ 169, 162, 196, 255 });
        DrawText("Click to play", x + 125, y + 112, 18, RAYWHITE);
    }
    EndDrawing();
}

static void Frame(void)
{
#if defined(PLATFORM_WEB)
    // keep the canvas resolution matched to its on-page size (it can start hidden or change size)
    double cssW, cssH;
    emscripten_get_element_css_size("#canvas", &cssW, &cssH);
    if (cssW > 0 && cssH > 0 && ((int)cssW != GetScreenWidth() || (int)cssH != GetScreenHeight()))
        SetWindowSize((int)cssW, (int)cssH);
#endif
    UpdateGame(fminf(GetFrameTime(), 0.05f));
    DrawGame();
}

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "Forest Starting Level");
    SetExitKey(KEY_NULL);
    InitGame();

#if defined(PLATFORM_WEB)
    emscripten_set_click_callback("#canvas", NULL, false, OnCanvasClick);
    emscripten_set_main_loop(Frame, 0, 1);
#else
    SetTargetFPS(60);
    while (!WindowShouldClose()) Frame();
    CloseWindow();
#endif
    return 0;
}
