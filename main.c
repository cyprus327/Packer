#include <raylib.h>
#include <raymath.h>

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#define MAX_VERTICES 32
#define MAX_PARTICLES 1000
#define MAX_PACKABLE_SHAPES 20000 // should be very generous

#define SCREEN_WIDTH 1300
#define SCREEN_HEIGHT 800
#define UI_PANEL_WIDTH 340

#define GRID_CELL_SIZE 40
#define GRID_COLS (SCREEN_WIDTH / GRID_CELL_SIZE + 1)
#define GRID_ROWS (SCREEN_HEIGHT / GRID_CELL_SIZE + 1)

#define CLAMP(x, a, b) ((x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef enum state {
    STATE_DRAW_CONTAINER,
    STATE_DRAW_INNER,
    STATE_PACKING,
    STATE_DONE
} State;

typedef struct poly {
    Vector2 vertices[MAX_VERTICES];
    int vertexCount;
    char isClosed;
} Polygon;

typedef struct packedShape {
    Polygon poly;
    float animTimer; // from 0 to 1
    Color color;
} PackedShape;

typedef struct particle {
    Vector2 pos, vel;
    float life;
    Color color;
} Particle;

typedef struct gridCell {
    int* shapeInds;
    int count, cap;
} GridCell;


static void handle_drawing(Polygon *poly, State *currentState, State nextState, Sound addSound, Sound finishSound, float* containerArea);
static int do_lines_intersect(Vector2 A, Vector2 B, Vector2 C, Vector2 D);
static char is_shape_inside_container(const Polygon* shape, const Polygon* container);
static char does_shape_overlap_packed(const Polygon* shape, const PackedShape* packedShapes, int packedCount);
static char check_poly_collisions(const Polygon* p1, const Polygon* p2);
static void project_poly(Vector2 axis, const Vector2* vertices, int vertexCount, float* min, float* max);

static Rectangle get_poly_bounds(const Polygon* poly);
static Vector2 get_poly_center(const Polygon* poly);
static void draw_poly_lines(const Vector2* vertices, int vertexCount, Color color, float thick);
static float poly_area(const Polygon* poly);
static void ensure_winding(Polygon* poly);

static float gui_slider(Rectangle bounds, const char *text, float value, float minValue, float maxValue);
static void draw_ui_panel(State currentState, int packedCount, float* posStep, float* rotationStep, float efficiency);
static void draw_poly_with_handles(Polygon* poly, Color lineColor, Color handleColor);
static void draw_bg_effect(void);

static void particles_spawn(Particle particles[MAX_PARTICLES], Vector2 center, int n, float ld, float sd);
static void particles_update_draw(Particle particles[MAX_PARTICLES]);

static void grid_init(void);
static void grid_clear(void);
static void grid_add_shape(int shapeInd, const Polygon* poly);

static int draggedVert = -1;
static Polygon* draggedPoly = NULL;
static Camera2D camera = {0};
static float screenShakeIntensity = 0.f;
static GridCell spatialGrid[GRID_ROWS][GRID_COLS];
static char checkedInds[MAX_PACKABLE_SHAPES];


int main(void) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Window");
    InitAudioDevice();
    SetTargetFPS(60);
    
    camera.zoom = 1.f;
    camera.target = (Vector2){ (SCREEN_WIDTH - UI_PANEL_WIDTH) / 2.f, SCREEN_HEIGHT / 2.f };
    camera.offset = camera.target;

    const Sound addSound = LoadSound("assets/menuMove.wav");
    const Sound finishSound = LoadSound("assets/levelComplete.wav");
    const Sound packSound = LoadSound("assets/fs1.wav");

    grid_init();

    State currentState = STATE_DRAW_CONTAINER;
    Polygon containerPoly = { .vertexCount = 0, .isClosed = 0 };
    Polygon innerPoly = { .vertexCount = 0, .isClosed = 0 };

    PackedShape* packedShapes = NULL;
    int packedShapesCount = 0, packedShapesCap = 0;

    Rectangle containerBounds = {0};
    Vector2 packingCursor = {0};
    
    float posStep = 3.f;
    float rotationStep = 5.f;
    
    float containerArea = 0.f;
    float packedTotalArea = 0.f;
    float packingEfficiency = 0.f;
    
    Particle particles[MAX_PARTICLES] = {0};

    while (!WindowShouldClose()) {
        if (screenShakeIntensity > 0) {
            camera.offset.x = camera.target.x + ((float)GetRandomValue(-100, 100) / 100.f) * screenShakeIntensity;
            camera.offset.y = camera.target.y + ((float)GetRandomValue(-100, 100) / 100.f) * screenShakeIntensity;
            screenShakeIntensity *= 0.9f;
        } else {
            camera.offset = camera.target;
        }

        switch (currentState) {
            case STATE_DRAW_CONTAINER: {
                handle_drawing(&containerPoly, &currentState, STATE_DRAW_INNER, addSound, finishSound, &containerArea);
            } break;

            case STATE_DRAW_INNER: {
                handle_drawing(&innerPoly, &currentState, STATE_PACKING, addSound, finishSound, NULL);
            } break;

            case STATE_PACKING: {
                if (0 == containerBounds.width) {
                    containerBounds = get_poly_bounds(&containerPoly);
                    packingCursor = (Vector2){ containerBounds.x, containerBounds.y };
                }

                const int attemptsPerFrame = 200;
                for (int c = 0; c < attemptsPerFrame; c += 1) {
                    if (packingCursor.y >= containerBounds.y + containerBounds.height) {
                        currentState = STATE_DONE;
                        
                        const float innerArea = fabsf(poly_area(&innerPoly));
                        packedTotalArea = packedShapesCount * innerArea;
                        if (containerArea > 0) {
                            packingEfficiency = (packedTotalArea / containerArea) * 100.f;
                        }
                        
                        screenShakeIntensity = 8.f;
                        particles_spawn(particles, get_poly_center(&containerPoly), 150, 40.f, 0.4f);
                        PlaySound(finishSound);
                        break;
                    }

                    for (float angle = 0.f; angle < 360.f; angle += rotationStep) {
                        Polygon candidateShape = { .vertexCount = innerPoly.vertexCount };
                        
                        for (int i = 0; i < innerPoly.vertexCount; i += 1) {
                            const Vector2 rotated = Vector2Rotate(innerPoly.vertices[i], angle * DEG2RAD);
                            candidateShape.vertices[i] = Vector2Add(rotated, packingCursor);
                        }

                        if (is_shape_inside_container(&candidateShape, &containerPoly) &&
                            !does_shape_overlap_packed(&candidateShape, packedShapes, packedShapesCount)
                        ) {
                            if (packedShapesCount >= packedShapesCap) {
                                packedShapesCap = (0 == packedShapesCap) ? 16 : packedShapesCap * 2;
                                packedShapes = realloc(packedShapes, packedShapesCap * sizeof(PackedShape));
                            }

                            if (packedShapes) {
                                SetRandomSeed(packedShapesCount * 31415);
                                packedShapes[packedShapesCount] = (PackedShape){
                                    .poly = candidateShape,
                                    .animTimer = 0.f,
                                    .color = { GetRandomValue(40, 120), GetRandomValue(10, 50), GetRandomValue(150, 240), 150 }
                                };
                                grid_add_shape(packedShapesCount, &candidateShape);
                                packedShapesCount += 1;

                                SetSoundPitch(packSound, (float)GetRandomValue(95, 105)/100.f);
                                PlaySound(packSound);
                                
                                particles_spawn(particles, packingCursor, 12, 200.f, 3.f);
                                screenShakeIntensity = 1.f;
                            }
                            
                            break;
                        }
                    }
                    
                    packingCursor.x += posStep;
                    if (packingCursor.x >= containerBounds.x + containerBounds.width) {
                        packingCursor.x = containerBounds.x;
                        packingCursor.y += posStep;
                    }
                }
            } break;

            case STATE_DONE: {
                particles_update_draw(particles);
                if (IsKeyPressed(KEY_A) || IsKeyPressed(KEY_R)) {
                    if (IsKeyPressed(KEY_R)) {
                        containerPoly = (Polygon){0};
                    }
                    innerPoly = (Polygon){0};
                    
                    free(packedShapes);
                    packedShapes = NULL;
                    packedShapesCount = 0;
                    packedShapesCap = 0;

                    containerBounds = (Rectangle){0};
                    packingCursor = (Vector2){0};
                    
                    containerArea = 0.f;
                    packedTotalArea = 0.f;
                    packingEfficiency = 0.f;
                    
                    grid_clear();
                    grid_init();
                    
                    currentState = STATE_DRAW_CONTAINER;
                }
            } break;
        }
        
        for (int i = 0; i < packedShapesCount; i += 1) {
            if (packedShapes[i].animTimer < 1.f) {
                packedShapes[i].animTimer += GetFrameTime() * 2.5f;
                if (packedShapes[i].animTimer > 1.f) {
                    packedShapes[i].animTimer = 1.f;
                }
            }
        }

        BeginDrawing();
        ClearBackground(GetColor(0x181818FF));
        
        BeginMode2D(camera);
        
        draw_bg_effect();

        if (containerPoly.vertexCount > 0) {
            draw_poly_with_handles(&containerPoly, LIGHTGRAY, MAROON);
        }

        if (innerPoly.vertexCount > 0 && STATE_DRAW_INNER == currentState) {
            draw_poly_with_handles(&innerPoly, SKYBLUE, DARKBLUE);
        }

        if (STATE_PACKING == currentState || STATE_DONE == currentState) {
            for (int i = 0; i < packedShapesCount; i += 1) {
                const PackedShape* ps = &packedShapes[i];
                const float scale = sinf(ps->animTimer * PI * 0.5f);
                
                const Vector2 center = get_poly_center(&ps->poly);
                Polygon scaledPoly = { .vertexCount = ps->poly.vertexCount };
                for(int j = 0; j < ps->poly.vertexCount; j += 1) {
                    const Vector2 v = Vector2Scale(Vector2Subtract(ps->poly.vertices[j], center), scale);
                    scaledPoly.vertices[j] = Vector2Add(v, center);
                }

                DrawTriangleFan(scaledPoly.vertices, scaledPoly.vertexCount, Fade(ps->color, scale));
                draw_poly_lines(scaledPoly.vertices, scaledPoly.vertexCount, Fade(DARKGRAY, scale), 1.f);
            }
        }
        
        particles_update_draw(particles);

        EndMode2D();
        
        draw_ui_panel(currentState, packedShapesCount, &posStep, &rotationStep, packingEfficiency);
        EndDrawing();
    }
    
    UnloadSound(addSound);
    UnloadSound(finishSound);
    UnloadSound(packSound);
    CloseAudioDevice();
    
    grid_clear();
    free(packedShapes);
    CloseWindow();
    return 0;
}

static void grid_init(void) {
    for (int y = 0; y < GRID_ROWS; y += 1) {
        for (int x = 0; x < GRID_COLS; x += 1) {
            spatialGrid[y][x].shapeInds = NULL;
            spatialGrid[y][x].count = 0;
            spatialGrid[y][x].cap = 0;
        }
    }
}

static void grid_clear(void) {
    for (int y = 0; y < GRID_ROWS; y += 1) {
        for (int x = 0; x < GRID_COLS; x += 1) {
            if (spatialGrid[y][x].shapeInds) {
                free(spatialGrid[y][x].shapeInds);
            }
        }
    }
}

static void grid_add_shape(int shapeInd, const Polygon* poly) {
    Rectangle bounds = get_poly_bounds(poly);
    const int minX = MAX(0, (int)floorf(bounds.x / GRID_CELL_SIZE));
    const int minY = MAX(0, (int)floorf(bounds.y / GRID_CELL_SIZE));
    const int maxX = MIN(GRID_COLS - 1, (int)floorf((bounds.x + bounds.width) / GRID_CELL_SIZE));
    const int maxY = MIN(GRID_ROWS - 1, (int)floorf((bounds.y + bounds.height) / GRID_CELL_SIZE));

    for (int y = minY; y <= maxY; y += 1) {
        for (int x = minX; x <= maxX; x += 1) {
            GridCell* cell = &spatialGrid[y][x];
            if (cell->count >= cell->cap) {
                cell->cap = (0 == cell->cap) ? 8 : cell->cap * 2;
                cell->shapeInds = realloc(cell->shapeInds, cell->cap * sizeof(int));
            }
            if (cell->shapeInds) {
                cell->shapeInds[cell->count] = shapeInd;
                cell->count += 1;
            }
        }
    }
}

static char does_shape_overlap_packed(const Polygon* shape, const PackedShape* packedShapes, int packedCount) {
    if (0 == packedCount) {
        return 0;
    }
    if (packedCount > MAX_PACKABLE_SHAPES) {
        return 1;
    }

    memset(checkedInds, 0, packedCount * sizeof(char));

    Rectangle candidateBox = get_poly_bounds(shape);
    const int minX = MAX(0, (int)floorf(candidateBox.x / GRID_CELL_SIZE));
    const int minY = MAX(0, (int)floorf(candidateBox.y / GRID_CELL_SIZE));
    const int maxX = MIN(GRID_COLS - 1, (int)floorf((candidateBox.x + candidateBox.width) / GRID_CELL_SIZE));
    const int maxY = MIN(GRID_ROWS - 1, (int)floorf((candidateBox.y + candidateBox.height) / GRID_CELL_SIZE));

    for (int y = minY; y <= maxY; y += 1) {
        for (int x = minX; x <= maxX; x += 1) {
            GridCell* cell = &spatialGrid[y][x];
            for (int i = 0; i < cell->count; i += 1) {
                const int shapeInd = cell->shapeInds[i];
                if (checkedInds[shapeInd]) {
                    continue;
                }
                
                checkedInds[shapeInd] = 1;

                const Rectangle packedBox = get_poly_bounds(&packedShapes[shapeInd].poly);
                if (CheckCollisionRecs(candidateBox, packedBox)) {
                    if (check_poly_collisions(shape, &packedShapes[shapeInd].poly)) {
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

static void handle_drawing(Polygon* poly, State* currentState, State nextState, Sound addSound, Sound finishSound, float* containerArea) {
    const Vector2 mousePos = GetScreenToWorld2D(GetMousePosition(), camera);
    if (mousePos.x > SCREEN_WIDTH - UI_PANEL_WIDTH) {
        return;
    }

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        for (int i = 0; i < poly->vertexCount; i += 1) {
            if (CheckCollisionPointCircle(mousePos, poly->vertices[i], 8.f)) {
                draggedVert = i;
                draggedPoly = poly;
                break;
            }
        }
    }
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && -1 != draggedVert && draggedPoly == poly) {
        poly->vertices[draggedVert] = mousePos;
    }
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
        draggedVert = -1;
        draggedPoly = NULL;
    }
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && -1 == draggedVert) {
        if (poly->vertexCount < MAX_VERTICES) {
            poly->vertices[poly->vertexCount] = mousePos;
            poly->vertexCount += 1;
            PlaySound(addSound);
        }
    }
    if (IsKeyPressed(KEY_Z) && poly->vertexCount > 0) {
        poly->vertexCount -= 1;
    }

    if (IsKeyPressed(KEY_SPACE) && poly->vertexCount >= 3) {
        poly->isClosed = 1;
        ensure_winding(poly);
        
        if (containerArea) {
            *containerArea = fabsf(poly_area(poly));
        }

        if (STATE_DRAW_INNER == *currentState) {
            const Vector2 center = get_poly_center(poly);
            for (int i = 0; i < poly->vertexCount; i += 1) {
                poly->vertices[i] = Vector2Subtract(poly->vertices[i], center);
            }
        }

        *currentState = nextState;
        
        PlaySound(finishSound);
    }
}

static void particles_spawn(Particle particles[MAX_PARTICLES], Vector2 center, int n, float ld, float sd) {
    static int i = 0;
    for (int i0 = 0; i0 < n; i0 += 1) {
        i += 1;
        if (i >= MAX_PARTICLES) {
            i = 0;
        }
        const float angle = (float)GetRandomValue(0, 3600) / 10.f * DEG2RAD;
        const float speed = (float)GetRandomValue(50, 250) / sd;
        particles[i] = (Particle){
            .pos = center,
            .vel = { cosf(angle) * speed, sinf(angle) * speed },
            .life = (float)GetRandomValue(50, 150) / ld,
            .color = (Color){ GetRandomValue(100, 255), GetRandomValue(80, 200), GetRandomValue(200, 255), 180 }
        };
    }
}

static void particles_update_draw(Particle particles[MAX_PARTICLES]) {
    for (int i = 0; i < MAX_PARTICLES; i += 1) {
        if (particles[i].life > 0.f) {
            particles[i].pos = Vector2Add(particles[i].pos, Vector2Scale(particles[i].vel, GetFrameTime()));
            particles[i].vel = Vector2Scale(particles[i].vel, 0.98f);
            particles[i].life -= GetFrameTime();
            DrawCircleV(particles[i].pos, particles[i].life * 3.f, Fade(particles[i].color, particles[i].life));
        }
    }
}

static void draw_bg_effect(void) {
    const int gridSize = 40;
    const Color gridColor = GetColor(0x202020FF);

    for (int i = -gridSize; i < SCREEN_WIDTH; i += gridSize) {
        DrawLine(i, 0, i, SCREEN_HEIGHT, gridColor);
    }
    for (int i = -gridSize; i < SCREEN_HEIGHT; i += gridSize) {
        DrawLine(0, i, SCREEN_WIDTH, i, gridColor);
    }
}

static void draw_poly_with_handles(Polygon* poly, Color lineColor, Color handleColor) {
    if (poly->vertexCount < 1) {
        return;
    }

    draw_poly_lines(poly->vertices, poly->vertexCount, lineColor, 2.f);
    
    const Vector2 mousePos = GetScreenToWorld2D(GetMousePosition(), camera);
    for (int i = 0; i < poly->vertexCount; i += 1) {
        float radius = 5.f;
        Color color = handleColor;
        
        char isHovered = CheckCollisionPointCircle(mousePos, poly->vertices[i], 8.f) &&
                         SCREEN_WIDTH - UI_PANEL_WIDTH >= mousePos.x;
        if (draggedVert == i && draggedPoly == poly) {
            radius = 8.f;
            color = SKYBLUE;
        } else if (isHovered) {
            radius = 5.f + sinf(GetTime() * 25.f) * 2.f;
            color = Fade(handleColor, 0.7f);
        }
        
        DrawCircleV(poly->vertices[i], radius, color);
        if (isHovered) {
            DrawCircleLines(poly->vertices[i].x, poly->vertices[i].y, 8.f, WHITE);
        }
    }
}

static Vector2 get_poly_center(const Polygon* poly) {
    if (0 == poly->vertexCount) {
        return (Vector2){0};
    }

    const Rectangle bounds = get_poly_bounds(poly);
    return (Vector2){ bounds.x + bounds.width / 2.f, bounds.y + bounds.height / 2.f };
}

static void draw_ui_panel(State currentState, int packedCount, float* posStep, float* rotationStep, float efficiency) {
    Rectangle panel = { SCREEN_WIDTH - UI_PANEL_WIDTH, 0, UI_PANEL_WIDTH, SCREEN_HEIGHT };
    DrawRectangleRec(panel, GetColor(0x222222DD));
    DrawLine(panel.x, 0, panel.x, SCREEN_HEIGHT, GetColor(0x555555FF));
    int yPos = 20;
    switch (currentState) {
        case STATE_DRAW_CONTAINER: {
            DrawText("DRAW CONTAINER", panel.x + 20, yPos, 22, RAYWHITE); yPos += 42;
            DrawText("LMB: Add Point", panel.x + 20, yPos, 20, LIGHTGRAY); yPos += 26;
            DrawText("SPACE: Finish Shape", panel.x + 20, yPos, 20, LIGHTGRAY); yPos += 26;
            DrawText("Z: Undo Last Point", panel.x + 20, yPos, 20, LIGHTGRAY); yPos += 26;
            DrawText("Drag points to move them.", panel.x + 20, yPos, 16, LIGHTGRAY);
        } break;
        case STATE_DRAW_INNER: {
            DrawText("DRAW SHAPE TO PACK", panel.x + 20, yPos, 22, RAYWHITE); yPos += 42;
            DrawText("LMB: Add Point", panel.x + 20, yPos, 20, LIGHTGRAY); yPos += 26;
            DrawText("SPACE: Finish Shape", panel.x + 20, yPos, 20, LIGHTGRAY); yPos += 26;
            DrawText("Z: Undo Last Point", panel.x + 20, yPos, 20, LIGHTGRAY); yPos += 26;
            DrawText("Drag points to move them.", panel.x + 20, yPos, 16, LIGHTGRAY);
        } break;
        case STATE_PACKING: {
            DrawText("PACKING...", panel.x + 20, yPos, 22, RAYWHITE); yPos += 42;
            DrawText(TextFormat("Shapes Placed: %d", packedCount), panel.x + 20, yPos, 16, LIGHTGRAY);
        } break;
        case STATE_DONE: {
            DrawText("PACKING COMPLETE!", panel.x + 20, yPos, 20, RAYWHITE); yPos += 40;
            
            DrawText("Efficiency:", panel.x + 20, yPos, 20, LIGHTGRAY);
            const char* scoreText = TextFormat("%.2f%%", efficiency);
            int scoreTextWidth = MeasureText(scoreText, 40);
            DrawText(scoreText, panel.x + (panel.width - scoreTextWidth)/2, yPos + 25, 40, SKYBLUE);
            yPos += 80;

            DrawText(TextFormat("Total Shapes: %d", packedCount), panel.x + 20, yPos, 16, LIGHTGRAY);
            yPos += 40;
            DrawText("Press 'R' to restart", panel.x + 20, yPos, 18, SKYBLUE);
            yPos += 30;
            DrawText("Press 'A' to keep container", panel.x + 20, yPos, 18, SKYBLUE);
        } break;
    }

    yPos = 350;
    *posStep = gui_slider((Rectangle){panel.x + 20, yPos, panel.width - 40, 20}, "Position Step", *posStep, 0.2f, 5.f);
    yPos += 70;
    *rotationStep = gui_slider((Rectangle){panel.x + 20, yPos, panel.width - 40, 20}, "Rotation Step", *rotationStep, 0.1f, 15.f);

    yPos += 200;
    static float masterVol = 0.5f;
    masterVol = gui_slider((Rectangle){panel.x + 20, yPos, panel.width - 40, 20}, "Master Volume", masterVol, 0.f, 1.f);
    SetMasterVolume(masterVol);
}

static float gui_slider(Rectangle bounds, const char *text, float value, float minValue, float maxValue) {
    const Vector2 mousePoint = GetMousePosition();

    if (CheckCollisionPointRec(mousePoint, bounds) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        value = ((mousePoint.x - bounds.x) / bounds.width) * (maxValue - minValue) + minValue;
        value = CLAMP(value, minValue, maxValue);
    }
    
    const float fillWidth = ((value - minValue) / (maxValue - minValue)) * bounds.width;
    
    char valueText[32];
    snprintf(valueText, 32, "%.2f", value);
    
    DrawRectangleRec(bounds, GetColor(0x333333FF));
    DrawRectangle(bounds.x, bounds.y, (int)fillWidth, bounds.height, SKYBLUE);
    DrawRectangle(bounds.x + (int)fillWidth - 4, bounds.y - 2, 8, bounds.height + 4, RAYWHITE);
    DrawText(text, bounds.x, bounds.y - 25, 20, LIGHTGRAY);
    DrawText(valueText, bounds.x + bounds.width - MeasureText(valueText, 20), bounds.y - 25, 20, LIGHTGRAY);

    return value;
}

static int do_lines_intersect(Vector2 a, Vector2 b, Vector2 c, Vector2 d) {
    const float s1x = b.x - a.x; const float s1y = b.y - a.y;
    const float s2x = d.x - c.x; const float s2y = d.y - c.y;
    const float s = (-s1y * (a.x - c.x) + s1x * (a.y - c.y)) / (-s2x * s1y + s1x * s2y);
    const float t = ( s2x * (a.y - c.y) - s2y * (a.x - c.x)) / (-s2x * s1y + s1x * s2y);
    return s >= 0.f && s <= 1.f && t >= 0.f && t <= 1.f;
}

static char is_shape_inside_container(const Polygon* shape, const Polygon* container) {
    for (int i = 0; i < shape->vertexCount; i += 1) {
        if (!CheckCollisionPointPoly(shape->vertices[i], container->vertices, container->vertexCount)) {
            return 0;
        }
    }

    for (int i = 0; i < shape->vertexCount; i += 1) {
        const Vector2 a = shape->vertices[i]; const Vector2 b = shape->vertices[(i + 1) % shape->vertexCount];
        for (int j = 0; j < container->vertexCount; j += 1) {
            const Vector2 c = container->vertices[j]; const Vector2 d = container->vertices[(j + 1) % container->vertexCount];
            if (do_lines_intersect(a, b, c, d)) {
                return 0;
            }
        }
    }

    return 1;
}

static char check_poly_collisions(const Polygon* p1, const Polygon* p2) {
    for (int i = 0; i < p1->vertexCount; i += 1) {
        const Vector2 edge = Vector2Subtract(p1->vertices[(i + 1) % p1->vertexCount], p1->vertices[i]);
        const Vector2 axis = { -edge.y, edge.x };
        
        float min1, max1, min2, max2;
        project_poly(axis, p1->vertices, p1->vertexCount, &min1, &max1); project_poly(axis, p2->vertices, p2->vertexCount, &min2, &max2);
        if (max1 < min2 || max2 < min1) {
            return 0;
        }
    }
    for (int i = 0; i < p2->vertexCount; i += 1) {
        const Vector2 edge = Vector2Subtract(p2->vertices[(i + 1) % p2->vertexCount], p2->vertices[i]);
        const Vector2 axis = { -edge.y, edge.x };
        
        float min1, max1, min2, max2;
        project_poly(axis, p1->vertices, p1->vertexCount, &min1, &max1); project_poly(axis, p2->vertices, p2->vertexCount, &min2, &max2);
        if (max1 < min2 || max2 < min1) {
            return 0;
        }
    }

    return 1;
}

static void project_poly(Vector2 axis, const Vector2* vertices, int vertexCount, float* min, float* max) {
    *min = Vector2DotProduct(vertices[0], axis);
    *max = *min;
    
    for (int i = 1; i < vertexCount; i += 1) {
        const float p = Vector2DotProduct(vertices[i], axis);
        if (p < *min) {
            *min = p;
        } else if (p > *max) {
            *max = p;
        }
    }
}

static Rectangle get_poly_bounds(const Polygon* poly) {
    if (0 == poly->vertexCount) {
        return (Rectangle){0};
    }

    Vector2 minV = poly->vertices[0]; Vector2 maxV = poly->vertices[0];
    for (int i = 1; i < poly->vertexCount; i += 1) {
        minV.x = MIN(minV.x, poly->vertices[i].x);
        minV.y = MIN(minV.y, poly->vertices[i].y);
        maxV.x = MAX(maxV.x, poly->vertices[i].x);
        maxV.y = MAX(maxV.y, poly->vertices[i].y);
    }

    return (Rectangle){ minV.x, minV.y, maxV.x - minV.x, maxV.y - minV.y };
}

static void draw_poly_lines(const Vector2* vertices, int vertexCount, Color color, float thick) {
    if (vertexCount < 2) {
        return;
    }

    for (int i = 0; i < vertexCount; i += 1) {
        DrawLineEx(vertices[i], vertices[(i + 1) % vertexCount], thick, color);
    }
}

static float poly_area(const Polygon* poly) {
    float area = 0;
    for (int i = 0; i < poly->vertexCount; i += 1) {
        const Vector2 a = poly->vertices[i];
        const Vector2 b = poly->vertices[(i + 1) % poly->vertexCount];
        area += a.x * b.y - b.x * a.y;
    }
    return area * 0.5f;
}

static void ensure_winding(Polygon* poly) {
    if (poly_area(poly) < 0.f) {
        return;
    }

    for (int i = 0; i < poly->vertexCount / 2; i += 1) {
        const Vector2 temp = poly->vertices[i];
        poly->vertices[i] = poly->vertices[poly->vertexCount - 1 - i];
        poly->vertices[poly->vertexCount - 1 - i] = temp;
    }
}

