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
    #define GLSL_HEADER "#version 100\nprecision mediump float;\n"
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
    "varying vec2 fragTexCoord;\n"
    "varying vec4 fragColor;\n"
    "varying vec3 fragNormal;\n"
    "void main() {\n"
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
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 lightDir;\n"
    "uniform vec3 lightColor;\n"
    "uniform vec3 ambient;\n"
    "uniform float unlit;\n"
    "void main() {\n"
    "    vec4 c = texture2D(texture0, fragTexCoord) * colDiffuse * fragColor;\n"
    "    if (c.a < 0.02) discard;\n"
    "    vec3 n = normalize(fragNormal);\n"
    "    if (!gl_FrontFacing) n = -n;\n"              // walls are seen from both sides
    "    float d = max(dot(n, lightDir), 0.0);\n"
    "    vec3 lit = mix(ambient + lightColor * d, vec3(1.0), unlit);\n"
    "    gl_FragColor = vec4(c.rgb * lit, c.a);\n"
    "}\n";

// ---------------------------------------------------------------------------
// Level data
// ---------------------------------------------------------------------------
#define MAX_CIRCLES 512
#define MAX_FOG     128

typedef struct { float x, z, r; } Circle;
typedef struct { Vector3 pos; float sx, sz, rot; } FogPatch;

static Circle   circles[MAX_CIRCLES]; static int circleCount = 0;
static FogPatch fog[MAX_FOG];         static int fogCount = 0;
static Vector3  spawn = { 0 };

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
static Model    level, knight;
static Shader   lit;
static int      locUnlit;
static Texture2D sky, fogTex;
static Mesh     fogMesh;
static Material fogMat;
static Camera3D camera = { 0 };

static Vector3 playerPos;
static float   playerVy = 0, facing = 0, walkT = 0;
static bool    onGround = true;
static float   yaw, pitch = 0.32f, camDist = 14.0f;
static bool    paused = true;

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
static void SetupLevelMaterials(bool *walkable)
{
    char names[32][32] = { 0 };
    int count = 0;
    FILE *f = fopen("assets/materials.txt", "r");
    if (f) { while (count < 32 && fscanf(f, "%31s", names[count]) == 1) count++; fclose(f); }

    for (int i = 0; i < level.materialCount; i++) { level.materials[i].shader = lit; walkable[i] = false; }

    for (int g = 0; g < count; g++) {
        int mi = g + 1;                       // raylib keeps a default material at index 0
        if (mi >= level.materialCount) break;
        Material *m = &level.materials[mi];
        Color tint = WHITE;
        Texture2D t = { 0 };
        const char *n = names[g];
        if      (!strcmp(n, "GrassShaded")) { t = Tex("grass.png", false); tint = (Color){ 120, 120, 120, 255 }; walkable[mi] = true; }
        else if (!strcmp(n, "Grass"))       { t = Tex("grass.png", false); tint = (Color){ 110, 110, 110, 255 }; }
        else if (!strcmp(n, "WallTex"))     { t = Tex("wall.png", true); }
        else if (!strcmp(n, "TownStone"))   { t = Tex("town_stone.png", true); walkable[mi] = true; }
        else if (!strcmp(n, "CaveBlack"))   { tint = BLACK; }
        else if (!strcmp(n, "PineTrunk"))   { t = Tex("pine_trunk.png", true); }
        else if (!strcmp(n, "PineLeaves"))  { t = Tex("pine_leaves.png", true); tint = (Color){ 70, 105, 78, 255 }; }
        else if (!strcmp(n, "HouseWall"))   { t = Tex("walls_black.png", true); }
        else if (!strcmp(n, "HouseRoof"))   { t = Tex("roof_gray.png", true); }
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
    SetShaderValue(lit, locUnlit, &zero, SHADER_UNIFORM_FLOAT);

    level = LoadModel("assets/level.glb");
    bool walkable[64] = { 0 };
    SetupLevelMaterials(walkable);
    BuildHeightField(&level, walkable);

    knight = LoadModel("assets/knight.glb");
    for (int i = 0; i < knight.materialCount; i++) {
        knight.materials[i].shader = lit;
        knight.materials[i].maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 235, 225, 200, 255 };   // offsets the lavender moonlight
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

    camera.up = (Vector3){ 0, 1, 0 };
    camera.fovy = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    camera.position = Vector3Add(playerPos, (Vector3){ sinf(yaw)*camDist, 8, cosf(yaw)*camDist });
    camera.target = playerPos;
}

static float WrapAngle(float a) { return atan2f(sinf(a), cosf(a)); }

static void UpdateGame(float dt)
{
    // --- mouse capture / look ---
    bool locked = IsCursorHidden();
    if (paused && (clickedToPlay || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) { paused = false; DisableCursor(); }
    clickedToPlay = false;
    if (!paused && IsKeyPressed(KEY_ESCAPE)) { paused = true; EnableCursor(); }
    if (wasLocked && !locked) paused = true;     // browser released the mouse (Esc)
    wasLocked = locked;
    if (!paused) {
        Vector2 md = GetMouseDelta();
        if (locked || IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            yaw -= md.x*0.0025f;
            pitch = Clamp(pitch + md.y*0.0025f, -0.35f, 1.2f);
        }
    }
    camDist = Clamp(camDist - GetMouseWheelMove()*1.5f, 6.0f, 40.0f);

    // --- movement ---
    bool moving = false, run = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!paused) {
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
    Vector3 off = { sinf(yaw)*cosf(pitch)*camDist, sinf(pitch)*camDist, cosf(yaw)*cosf(pitch)*camDist };
    Vector3 want = Vector3Add(target, off);
    float cg;
    if (GroundHeight(want.x, want.z, &cg) && want.y < cg + 1.5f) want.y = cg + 1.5f;
    camera.position = Vector3Lerp(camera.position, want, fminf(1.0f, dt*10.0f));
    camera.target = target;

    // turn the knight smoothly toward the walking direction
    static float shownFacing = 0;
    shownFacing += WrapAngle(facing - shownFacing)*fminf(1.0f, dt*12.0f);
    knight.transform = MatrixMultiply(MatrixRotateZ(moving && onGround ? sinf(walkT)*0.05f : 0.0f),
                                      MatrixRotateY(shownFacing));
}

static void DrawGame(void)
{
    BeginDrawing();
    ClearBackground((Color){ 11, 8, 24, 255 });

    // sky backdrop, scaled to cover the screen
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    float s = fmaxf(sw/sky.width, sh/sky.height);
    DrawTexturePro(sky, (Rectangle){ 0, 0, (float)sky.width, (float)sky.height },
                   (Rectangle){ (sw - sky.width*s)/2, (sh - sky.height*s)/2, sky.width*s, sky.height*s },
                   (Vector2){ 0 }, 0, WHITE);

    BeginMode3D(camera);
        rlDisableBackfaceCulling();
        DrawModel(level, Vector3Zero(), 1.0f, WHITE);

        float bob = (onGround && (IsKeyDown(KEY_W) || IsKeyDown(KEY_A) || IsKeyDown(KEY_S) || IsKeyDown(KEY_D)) && !paused)
                    ? fabsf(sinf(walkT))*0.18f : 0.0f;
        DrawModel(knight, Vector3Add(playerPos, (Vector3){ 0, bob, 0 }), 1.0f, WHITE);

        // smoky fog: unlit, alpha-blended, no depth writes
        float one = 1.0f, zero = 0.0f;
        SetShaderValue(lit, locUnlit, &one, SHADER_UNIFORM_FLOAT);
        BeginBlendMode(BLEND_ALPHA);
        rlDisableDepthMask();
        for (int i = 0; i < fogCount; i++) {
            Matrix t = MatrixMultiply(MatrixMultiply(MatrixScale(fog[i].sx, 1, fog[i].sz), MatrixRotateY(fog[i].rot)),
                                      MatrixTranslate(fog[i].pos.x, fog[i].pos.y, fog[i].pos.z));
            DrawMesh(fogMesh, fogMat, t);
        }
        rlEnableDepthMask();
        EndBlendMode();
        SetShaderValue(lit, locUnlit, &zero, SHADER_UNIFORM_FLOAT);
        rlEnableBackfaceCulling();
    EndMode3D();

    // HUD
    DrawRectangle(10, (int)sh - 34, 330, 24, (Color){ 14, 10, 28, 180 });
    DrawText("WASD move  Shift run  Space jump  Esc pause", 18, (int)sh - 28, 12, (Color){ 200, 194, 225, 255 });
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
    if (IsWindowResized()) { /* raylib updates the viewport itself */ }
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
