#include <SDL.h>
#include <SDL_ttf.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// Screen dimensions
const int SCREEN_WIDTH = 800;
const int SCREEN_HEIGHT = 600;

// Paper frame around the board, with the HUD in the top strip
const int HUD_HEIGHT = 40;
const int FRAME_SIZE = 10;
const float FIELD_LEFT = FRAME_SIZE;
const float FIELD_RIGHT = SCREEN_WIDTH - FRAME_SIZE;
const float FIELD_TOP = HUD_HEIGHT + 2;
const float FIELD_BOTTOM = SCREEN_HEIGHT - FRAME_SIZE;

// Paddle
const int PADDLE_WIDTH = 116;
const int PADDLE_HEIGHT = 18;
const float PADDLE_Y = SCREEN_HEIGHT - 72;
const float PADDLE_SPEED = 650.0f; // pixels per second

// Ball
const int BALL_SIZE = 20;
const float BALL_SPEED = 420.0f; // pixels per second
const float BALL_SPEEDUP_PER_LEVEL = 1.08f;
const float MAX_BOUNCE_ANGLE = 1.05f; // radians (~60 degrees) at the paddle edge

// Bricks
const int BRICK_COLS = 10;
const int BRICK_ROWS = 6;
const int BRICK_WIDTH = 62;
const int BRICK_HEIGHT = 23;
const int BRICK_GAP = 6;
const int BRICK_VARIANTS = 4; // differently worn textures per colour, so bricks don't all look identical

const int START_LIVES = 3;

// Soft shadows under the objects, offset away from the top-left light
const float SHADOW_X = 2.0f;
const float SHADOW_Y = 3.0f;
const int SHADOW_BLUR = 3;

const float PI = 3.14159265f;

// Vintage print palette
const SDL_Color INK = { 24, 40, 28, 255 };     // dark green ink for outlines and HUD text
const SDL_Color CREAM = { 236, 226, 196, 255 }; // paper, ball and paddle
const SDL_Color FELT = { 30, 60, 38, 255 };     // board

const SDL_Color ROW_COLORS[BRICK_ROWS] = {
    { 214,  74,  44, 255 }, // red
    { 228, 132,  62, 255 }, // orange
    { 233, 196,  64, 255 }, // yellow
    {  90, 150,  72, 255 }, // green
    { 112, 184, 196, 255 }, // teal
    { 152,  92, 168, 255 }, // purple
};

// ---------------------------------------------------------------------------
// Procedural texture generation
// ---------------------------------------------------------------------------

struct Vec3 { float x, y, z; };

static float Clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
static float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 Normalize(Vec3 v) {
    float len = std::sqrt(Dot(v, v));
    return { v.x / len, v.y / len, v.z / len };
}

// Light comes from the top-left, slightly above the screen
static const Vec3 LIGHT_DIR = Normalize({ -0.45f, -0.6f, 0.65f });

// Deterministic pseudo-random value in [0, 1] for an integer grid point
static float Hash(int x, int y, int seed) {
    Uint32 h = static_cast<Uint32>(x) * 374761393u + static_cast<Uint32>(y) * 668265263u + static_cast<Uint32>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / 16777215.0f;
}

// Smoothly interpolated noise
static float ValueNoise(float x, float y, int seed) {
    int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float top = Lerp(Hash(xi, yi, seed), Hash(xi + 1, yi, seed), fx);
    float bottom = Lerp(Hash(xi, yi + 1, seed), Hash(xi + 1, yi + 1, seed), fx);
    return Lerp(top, bottom, fy);
}

// Several octaves of noise for blotchy, natural-looking variation
static float Fbm(float x, float y, int seed) {
    float sum = 0, amplitude = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sum += ValueNoise(x, y, seed + i * 31) * amplitude;
        x *= 2.0f;
        y *= 2.0f;
        amplitude *= 0.5f;
    }
    return sum / 0.9375f;
}

struct Rgb { float r, g, b; };
static Rgb ToRgb(SDL_Color c) { return { static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b) }; }
static Rgb Mix(Rgb a, Rgb b, float t) { return { Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t) }; }
static Rgb Scale(Rgb c, float k) { return { c.r * k, c.g * k, c.b * k }; }

static Uint32 PackARGB(Rgb c, float a) {
    auto ch = [](float v) { return static_cast<Uint32>(std::min(std::max(v, 0.0f), 255.0f) + 0.5f); };
    return (ch(a * 255.0f) << 24) | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

static SDL_Texture* TextureFromPixels(SDL_Renderer* renderer, int w, int h, const std::vector<Uint32>& pixels) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!texture) {
        std::cerr << "Could not create texture: " << SDL_GetError() << std::endl;
        return nullptr;
    }
    SDL_UpdateTexture(texture, nullptr, pixels.data(), w * static_cast<int>(sizeof(Uint32)));
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

// Signed distance from point (px, py) to a rounded box centred at the origin (negative = inside)
static float SdRoundBox(float px, float py, float halfW, float halfH, float radius) {
    float qx = std::fabs(px) - (halfW - radius);
    float qy = std::fabs(py) - (halfH - radius);
    float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

// Green felt board: fibres, blotches, chalk specks and a soft vignette
static SDL_Texture* MakeFelt(SDL_Renderer* renderer) {
    std::vector<Uint32> pixels(SCREEN_WIDTH * SCREEN_HEIGHT);
    Rgb felt = ToRgb(FELT), cream = ToRgb(CREAM);
    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            float k = 0.82f + 0.3f * Fbm(x * 0.012f, y * 0.012f, 11);
            k += (Hash(x, y, 12) - 0.5f) * 0.22f;                    // fine grain
            k += (ValueNoise(x * 0.6f, y * 0.05f, 13) - 0.5f) * 0.12f; // vertical fibres
            k += (ValueNoise(x * 0.05f, y * 0.6f, 14) - 0.5f) * 0.08f; // horizontal fibres

            float vx = static_cast<float>(x) / SCREEN_WIDTH - 0.5f;
            float vy = static_cast<float>(y) / SCREEN_HEIGHT - 0.5f;
            k *= 1.0f - 0.6f * (vx * vx + vy * vy);

            Rgb c = Scale(felt, k);
            if (Hash(x, y, 15) > 0.996f) c = Mix(c, cream, 0.25f); // chalk dust
            pixels[y * SCREEN_WIDTH + x] = PackARGB(c, 1.0f);
        }
    }
    return TextureFromPixels(renderer, SCREEN_WIDTH, SCREEN_HEIGHT, pixels);
}

// Aged paper frame with a rough ink edge around the board and the lives box. Transparent over the board.
static SDL_Texture* MakeFrame(SDL_Renderer* renderer) {
    std::vector<Uint32> pixels(SCREEN_WIDTH * SCREEN_HEIGHT);
    Rgb ink = ToRgb(INK), paper = ToRgb(CREAM), stain = { 150, 120, 80 };
    const float boxX = 700, boxY = 6, boxW = 88, boxH = 28;

    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            float jitter = (ValueNoise(x * 0.35f, y * 0.35f, 21) - 0.5f) * 1.8f;

            // Distance outside the board (negative = inside the board)
            float field = std::max({ FIELD_LEFT - x, x - FIELD_RIGHT, FIELD_TOP - y, y - FIELD_BOTTOM }) + jitter;
            if (field < -0.5f) {
                pixels[y * SCREEN_WIDTH + x] = 0;
                continue;
            }

            Rgb c = Mix(paper, stain, Clamp01((Fbm(x * 0.02f, y * 0.02f, 22) - 0.45f) * 0.9f));
            c = Scale(c, 1.0f + (Hash(x, y, 23) - 0.5f) * 0.1f);
            if (Hash(x, y, 24) > 0.992f) c = Mix(c, ink, 0.5f); // ink specks

            // Lives box: dark ink panel in the HUD
            float box = SdRoundBox(x + 0.5f - (boxX + boxW / 2), y + 0.5f - (boxY + boxH / 2), boxW / 2, boxH / 2, 3) + jitter * 0.5f;
            if (box < 0) c = Scale(ink, 1.0f + (Hash(x, y, 25) - 0.5f) * 0.25f);

            // Thin ink line along the board edge, and a worn dark line at the outer edge
            float outer = std::min({ static_cast<float>(x), static_cast<float>(y),
                                     SCREEN_WIDTH - 1.0f - x, SCREEN_HEIGHT - 1.0f - y }) + jitter;
            if (field < 1.6f || outer < 1.5f) c = Mix(c, ink, 0.9f);
            else if (outer < 4.0f) c = Scale(c, 0.85f);

            pixels[y * SCREEN_WIDTH + x] = PackARGB(c, Clamp01(field + 0.5f));
        }
    }
    return TextureFromPixels(renderer, SCREEN_WIDTH, SCREEN_HEIGHT, pixels);
}

// Painted wooden brick: matte colour, wood grain streaks, chipped paint and a worn ink outline
static SDL_Texture* MakeBrick(SDL_Renderer* renderer, SDL_Color color, int seed) {
    const int w = BRICK_WIDTH, h = BRICK_HEIGHT;
    std::vector<Uint32> pixels(w * h);
    Rgb base = ToRgb(color), ink = ToRgb(INK), cream = ToRgb(CREAM);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float d = SdRoundBox(x + 0.5f - w / 2.0f, y + 0.5f - h / 2.0f, w / 2.0f, h / 2.0f, 3.0f);
            float alpha = Clamp01(0.5f - d);
            if (alpha <= 0) {
                pixels[y * w + x] = 0;
                continue;
            }

            float k = 1.0f;
            if (y < 3) k += 0.10f;          // light catches the top edge
            if (y >= h - 3) k -= 0.18f;     // darker underside
            float grain = ValueNoise(x * 0.045f + seed * 7.0f, y * 0.75f, seed);
            if (grain > 0.66f) k -= (grain - 0.66f) * 1.1f;
            k += (Hash(x, y, seed + 1) - 0.5f) * 0.08f;

            Rgb c = Scale(base, k);
            if (Hash(x / 2, y, seed + 2) > 0.988f) c = Mix(c, cream, 0.4f); // chipped paint
            c = Mix(c, ink, Clamp01(1.0f + d * 0.8f) * 0.6f);                 // worn outline
            pixels[y * w + x] = PackARGB(c, alpha);
        }
    }
    return TextureFromPixels(renderer, w, h, pixels);
}

// Cream ball with an ink outline and engraved hatching on the shadow side
static SDL_Texture* MakeBall(SDL_Renderer* renderer, int size) {
    std::vector<Uint32> pixels(size * size);
    Rgb ink = ToRgb(INK), cream = ToRgb(CREAM);
    float radius = size / 2.0f, outline = size * 0.1f, inner = radius - outline;
    float spacing = size / 9.0f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = x + 0.5f - radius, dy = y + 0.5f - radius;
            float dist = std::sqrt(dx * dx + dy * dy);
            float alpha = Clamp01(radius - dist + 0.5f);
            if (alpha <= 0) {
                pixels[y * size + x] = 0;
                continue;
            }
            Rgb c = ink;
            if (dist < inner) {
                float nx = dx / inner, ny = dy / inner;
                Vec3 n = { nx, ny, std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny)) };
                float light = std::max(Dot(n, LIGHT_DIR), 0.0f);
                c = Scale(cream, 0.8f + 0.25f * light);
                if (light < 0.5f) {
                    // Diagonal hatch lines that get thicker in darker areas
                    float v = std::fmod(x - y + 1000.0f, spacing);
                    if (v < spacing * (0.5f - light) * 1.4f) c = Mix(c, ink, 0.85f);
                }
                c = Mix(c, ink, Clamp01(1.0f - (inner - dist) / 1.5f)); // anti-aliased inner edge
            }
            pixels[y * size + x] = PackARGB(c, alpha);
        }
    }
    return TextureFromPixels(renderer, size, size, pixels);
}

// Cream pill-shaped paddle with an ink outline, a highlight and hatching underneath. Rendered at 2x.
static SDL_Texture* MakePaddle(SDL_Renderer* renderer) {
    const int w = PADDLE_WIDTH * 2, h = PADDLE_HEIGHT * 2;
    std::vector<Uint32> pixels(w * h);
    Rgb ink = ToRgb(INK), cream = ToRgb(CREAM);
    const float outline = 4.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float d = SdRoundBox(x + 0.5f - w / 2.0f, y + 0.5f - h / 2.0f, w / 2.0f, h / 2.0f, h / 2.0f);
            float alpha = Clamp01(0.5f - d);
            if (alpha <= 0) {
                pixels[y * w + x] = 0;
                continue;
            }
            float v = static_cast<float>(y) / h; // 0 = top, 1 = bottom
            Rgb c = Scale(cream, 1.04f - 0.28f * v * v);
            if (v > 0.22f && v < 0.36f) c = Mix(c, { 255, 250, 235 }, 0.5f); // highlight band
            if (v > 0.68f && std::fmod(x + y * 0.5f, 5.0f) < 1.6f) c = Mix(c, ink, 0.45f);
            c = Mix(c, ink, Clamp01((d + outline) / 1.2f));
            pixels[y * w + x] = PackARGB(c, alpha);
        }
    }
    return TextureFromPixels(renderer, w, h, pixels);
}

// Soft dark shadow of a rounded box; the texture is padded by SHADOW_BLUR on each side
static SDL_Texture* MakeShadow(SDL_Renderer* renderer, int w, int h, float radius) {
    int tw = w + SHADOW_BLUR * 2, th = h + SHADOW_BLUR * 2;
    std::vector<Uint32> pixels(tw * th);
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            float d = SdRoundBox(x + 0.5f - tw / 2.0f, y + 0.5f - th / 2.0f, w / 2.0f, h / 2.0f, radius);
            float a = Clamp01((SHADOW_BLUR - d) / (2.0f * SHADOW_BLUR));
            a = a * a * (3.0f - 2.0f * a); // smoothstep
            pixels[y * tw + x] = PackARGB({ 5, 15, 8 }, a * 0.45f);
        }
    }
    return TextureFromPixels(renderer, tw, th, pixels);
}

// Print grain drawn over everything: ink and paper specks plus faint worn patches
static SDL_Texture* MakeGrain(SDL_Renderer* renderer) {
    std::vector<Uint32> pixels(SCREEN_WIDTH * SCREEN_HEIGHT);
    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            float h = Hash(x, y, 31);
            float wear = Clamp01((Fbm(x * 0.03f, y * 0.03f, 32) - 0.55f) * 2.0f) * 0.08f;
            if (h < 0.015f) pixels[y * SCREEN_WIDTH + x] = PackARGB({ 10, 20, 12 }, 0.18f);
            else if (h > 0.988f) pixels[y * SCREEN_WIDTH + x] = PackARGB(ToRgb(CREAM), 0.12f);
            else pixels[y * SCREEN_WIDTH + x] = PackARGB(ToRgb(CREAM), wear);
        }
    }
    return TextureFromPixels(renderer, SCREEN_WIDTH, SCREEN_HEIGHT, pixels);
}

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------

struct Brick {
    float x, y;
    int row, variant;
    bool active;
};

enum class ParticleKind { Chip, Dust };

struct Particle {
    ParticleKind kind;
    float x, y, vx, vy;
    float ox, oy; // offset from (x, y) that shrinks as the particle fades (used for the trail's cone)
    float life, maxLife;
    float size, angle, spin;
    int row, variant;
    SDL_Color color;
};

enum class State { Title, Serve, Playing, GameOver };

struct Game {
    State state = State::Title;
    float paddleX = 0;
    float ballX = 0, ballY = 0, ballVelX = 0, ballVelY = 0;
    float ballSpeed = BALL_SPEED;
    int score = 0, lives = START_LIVES, level = 1;
    std::vector<Brick> bricks;
    std::vector<Particle> particles;
    float shake = 0;
};

struct Assets {
    SDL_Texture* felt = nullptr;
    SDL_Texture* frame = nullptr;
    SDL_Texture* grain = nullptr;
    SDL_Texture* bricks[BRICK_ROWS][BRICK_VARIANTS] = {};
    SDL_Texture* brickShadow = nullptr;
    SDL_Texture* paddle = nullptr;
    SDL_Texture* paddleShadow = nullptr;
    SDL_Texture* ball = nullptr;
    SDL_Texture* ballShadow = nullptr;
    TTF_Font* hudFont = nullptr;
    TTF_Font* font = nullptr;
    TTF_Font* bigFont = nullptr;
};

static std::mt19937 rng{ std::random_device{}() };
static float RandRange(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

void BuildBricks(Game& game) {
    game.bricks.clear();
    float totalWidth = BRICK_COLS * (BRICK_WIDTH + BRICK_GAP) - BRICK_GAP;
    float startX = (SCREEN_WIDTH - totalWidth) / 2.0f;
    for (int row = 0; row < BRICK_ROWS; ++row) {
        for (int col = 0; col < BRICK_COLS; ++col) {
            game.bricks.push_back({ startX + col * (BRICK_WIDTH + BRICK_GAP),
                                    FIELD_TOP + 46 + row * (BRICK_HEIGHT + BRICK_GAP),
                                    row, (row * 3 + col * 7) % BRICK_VARIANTS, true });
        }
    }
}

// Put the ball back on the paddle
void ResetBall(Game& game) {
    game.ballX = game.paddleX + PADDLE_WIDTH / 2.0f - BALL_SIZE / 2.0f;
    game.ballY = PADDLE_Y - BALL_SIZE;
    game.ballVelX = game.ballVelY = 0;
}

void NewGame(Game& game) {
    game.paddleX = (SCREEN_WIDTH - PADDLE_WIDTH) / 2.0f;
    game.ballSpeed = BALL_SPEED;
    game.score = 0;
    game.lives = START_LIVES;
    game.level = 1;
    game.particles.clear();
    BuildBricks(game);
    ResetBall(game);
}

void LaunchBall(Game& game) {
    float angle = RandRange(-0.35f, 0.35f);
    game.ballVelX = game.ballSpeed * std::sin(angle);
    game.ballVelY = -game.ballSpeed * std::cos(angle);
    game.state = State::Playing;
}

void SpawnDust(Game& game, float x, float y, int count, float minSpeed, float maxSpeed, SDL_Color color) {
    for (int i = 0; i < count; ++i) {
        float angle = RandRange(0, 2 * PI), speed = RandRange(minSpeed, maxSpeed);
        float life = RandRange(0.3f, 0.6f);
        game.particles.push_back({ ParticleKind::Dust, x, y, std::cos(angle) * speed, std::sin(angle) * speed,
                                   0, 0, life, life, RandRange(1.0f, 2.5f), 0, 0, 0, 0, color });
    }
}

void SpawnBrickBreak(Game& game, const Brick& brick) {
    float cx = brick.x + BRICK_WIDTH / 2.0f, cy = brick.y + BRICK_HEIGHT / 2.0f;
    for (int i = 0; i < 12; ++i) {
        float angle = RandRange(0, 2 * PI), speed = RandRange(80, 300);
        float life = RandRange(0.5f, 0.9f);
        game.particles.push_back({ ParticleKind::Chip, cx + RandRange(-25, 25), cy + RandRange(-8, 8),
                                   std::cos(angle) * speed, std::sin(angle) * speed - 60, 0, 0,
                                   life, life, RandRange(5, 10), RandRange(0, 360), RandRange(-600, 600),
                                   brick.row, brick.variant, ROW_COLORS[brick.row] });
    }
    SpawnDust(game, cx, cy, 14, 30, 160, CREAM);
}

// Stippled trail: specks left behind the ball that pull in towards its path as they fade
void SpawnTrail(Game& game) {
    float cx = game.ballX + BALL_SIZE / 2.0f, cy = game.ballY + BALL_SIZE / 2.0f;
    for (int i = 0; i < 9; ++i) {
        float angle = RandRange(0, 2 * PI), r = RandRange(0, BALL_SIZE * 0.5f);
        float life = RandRange(0.2f, 0.4f);
        game.particles.push_back({ ParticleKind::Dust, cx, cy, 0, 0, std::cos(angle) * r, std::sin(angle) * r,
                                   life, life, RandRange(1.0f, 2.2f), 0, 0, 0, 0, CREAM });
    }
}

void LoseLife(Game& game) {
    game.lives--;
    game.shake = 10;
    game.state = game.lives > 0 ? State::Serve : State::GameOver;
    ResetBall(game);
}

// Moves the ball one small step and resolves collisions. Returns false if the ball was lost.
bool StepBall(Game& game, float dt) {
    game.ballX += game.ballVelX * dt;
    game.ballY += game.ballVelY * dt;

    if (game.ballX < FIELD_LEFT) { game.ballX = FIELD_LEFT; game.ballVelX = std::fabs(game.ballVelX); }
    if (game.ballX + BALL_SIZE > FIELD_RIGHT) { game.ballX = FIELD_RIGHT - BALL_SIZE; game.ballVelX = -std::fabs(game.ballVelX); }
    if (game.ballY < FIELD_TOP) { game.ballY = FIELD_TOP; game.ballVelY = std::fabs(game.ballVelY); }

    if (game.ballY > SCREEN_HEIGHT) {
        LoseLife(game);
        return false;
    }

    // Paddle: the bounce angle depends on where the ball hits it
    if (game.ballVelY > 0 &&
        game.ballY + BALL_SIZE >= PADDLE_Y && game.ballY + BALL_SIZE <= PADDLE_Y + PADDLE_HEIGHT &&
        game.ballX + BALL_SIZE >= game.paddleX && game.ballX <= game.paddleX + PADDLE_WIDTH) {
        float ballCenter = game.ballX + BALL_SIZE / 2.0f;
        float hit = (ballCenter - (game.paddleX + PADDLE_WIDTH / 2.0f)) / (PADDLE_WIDTH / 2.0f);
        float angle = std::min(std::max(hit, -1.0f), 1.0f) * MAX_BOUNCE_ANGLE;
        game.ballVelX = game.ballSpeed * std::sin(angle);
        game.ballVelY = -game.ballSpeed * std::cos(angle);
        game.ballY = PADDLE_Y - BALL_SIZE;
        SpawnDust(game, ballCenter, PADDLE_Y, 8, 20, 120, CREAM);
    }

    // Bricks: bounce off the side with the smallest overlap
    for (auto& brick : game.bricks) {
        if (!brick.active) continue;
        float overlapX = std::min(game.ballX + BALL_SIZE - brick.x, brick.x + BRICK_WIDTH - game.ballX);
        float overlapY = std::min(game.ballY + BALL_SIZE - brick.y, brick.y + BRICK_HEIGHT - game.ballY);
        if (overlapX <= 0 || overlapY <= 0) continue;

        brick.active = false;
        game.score += 10 * (BRICK_ROWS - brick.row);
        game.shake = std::max(game.shake, 3.0f);
        SpawnBrickBreak(game, brick);

        if (overlapX < overlapY) {
            game.ballVelX = (game.ballX + BALL_SIZE / 2.0f < brick.x + BRICK_WIDTH / 2.0f) ? -std::fabs(game.ballVelX) : std::fabs(game.ballVelX);
        }
        else {
            game.ballVelY = (game.ballY + BALL_SIZE / 2.0f < brick.y + BRICK_HEIGHT / 2.0f) ? -std::fabs(game.ballVelY) : std::fabs(game.ballVelY);
        }
        break; // one brick per step
    }
    return true;
}

void Update(Game& game, float dt) {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (game.state != State::GameOver) {
        if (keys[SDL_SCANCODE_LEFT]) game.paddleX -= PADDLE_SPEED * dt;
        if (keys[SDL_SCANCODE_RIGHT]) game.paddleX += PADDLE_SPEED * dt;
        game.paddleX = std::min(std::max(game.paddleX, FIELD_LEFT), FIELD_RIGHT - PADDLE_WIDTH);
    }

    if (game.state == State::Title || game.state == State::Serve) {
        ResetBall(game);
    }
    else if (game.state == State::Playing) {
        // Sub-steps so the fast ball can't tunnel through bricks
        int steps = std::max(1, static_cast<int>(std::ceil(game.ballSpeed * dt / 4.0f)));
        for (int i = 0; i < steps; ++i) {
            if (!StepBall(game, dt / steps)) break;
        }

        if (game.state == State::Playing) SpawnTrail(game);

        bool cleared = std::none_of(game.bricks.begin(), game.bricks.end(), [](const Brick& b) { return b.active; });
        if (cleared) {
            game.level++;
            game.ballSpeed *= BALL_SPEEDUP_PER_LEVEL;
            BuildBricks(game);
            ResetBall(game);
            game.state = State::Serve;
        }
    }

    for (auto& p : game.particles) {
        p.life -= dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        if (p.kind == ParticleKind::Chip) p.vy += 900.0f * dt; // chips fall
        else { p.vx *= 1.0f - 3.0f * dt; p.vy *= 1.0f - 3.0f * dt; } // dust slows down
        p.angle += p.spin * dt;
    }
    game.particles.erase(std::remove_if(game.particles.begin(), game.particles.end(),
                                        [](const Particle& p) { return p.life <= 0; }),
                         game.particles.end());

    game.shake = std::max(0.0f, game.shake - 40.0f * dt);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Screen-shake offset applied to everything except the print grain
static float g_shakeX = 0, g_shakeY = 0;

void Draw(SDL_Renderer* renderer, SDL_Texture* texture, float x, float y, float w, float h) {
    SDL_FRect dest = { x + g_shakeX, y + g_shakeY, w, h };
    SDL_RenderCopyF(renderer, texture, nullptr, &dest);
}

void DrawShadow(SDL_Renderer* renderer, SDL_Texture* shadow, float x, float y, float w, float h, float lift) {
    Draw(renderer, shadow, x + SHADOW_X * lift - SHADOW_BLUR, y + SHADOW_Y * lift - SHADOW_BLUR,
         w + SHADOW_BLUR * 2, h + SHADOW_BLUR * 2);
}

void DrawText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, float x, float y,
              SDL_Color color, bool centered, bool shadow = true) {
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    float w = static_cast<float>(surface->w), h = static_cast<float>(surface->h);
    SDL_FreeSurface(surface);
    if (!texture) return;
    if (centered) x -= w / 2;
    x += g_shakeX;
    y += g_shakeY;

    if (shadow) {
        SDL_FRect shadowRect = { x + 2, y + 3, w, h };
        SDL_SetTextureColorMod(texture, 0, 0, 0);
        SDL_SetTextureAlphaMod(texture, 170);
        SDL_RenderCopyF(renderer, texture, nullptr, &shadowRect);
        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture, 255);
    }

    SDL_FRect rect = { x, y, w, h };
    SDL_RenderCopyF(renderer, texture, nullptr, &rect);
    SDL_DestroyTexture(texture);
}

void DrawDim(SDL_Renderer* renderer, Uint8 alpha) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 10, 24, 14, alpha);
    SDL_RenderFillRect(renderer, nullptr);
}

void Render(SDL_Renderer* renderer, const Assets& assets, const Game& game) {
    g_shakeX = game.shake > 0 ? RandRange(-game.shake, game.shake) : 0;
    g_shakeY = game.shake > 0 ? RandRange(-game.shake, game.shake) : 0;

    SDL_SetRenderDrawColor(renderer, INK.r, INK.g, INK.b, 255);
    SDL_RenderClear(renderer);

    Draw(renderer, assets.felt, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    // Shadows first so every object casts onto the felt
    for (const auto& brick : game.bricks) {
        if (brick.active) DrawShadow(renderer, assets.brickShadow, brick.x, brick.y, BRICK_WIDTH, BRICK_HEIGHT, 1.0f);
    }
    DrawShadow(renderer, assets.paddleShadow, game.paddleX, PADDLE_Y, PADDLE_WIDTH, PADDLE_HEIGHT, 1.0f);
    DrawShadow(renderer, assets.ballShadow, game.ballX, game.ballY, BALL_SIZE, BALL_SIZE, 1.5f);

    for (const auto& brick : game.bricks) {
        if (brick.active) Draw(renderer, assets.bricks[brick.row][brick.variant], brick.x, brick.y, BRICK_WIDTH, BRICK_HEIGHT);
    }

    // Particles: tumbling brick chips and stippled dust (including the ball trail)
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (const auto& p : game.particles) {
        float a = Clamp01(p.life / p.maxLife);
        if (p.kind == ParticleKind::Chip) {
            SDL_Texture* chip = assets.bricks[p.row][p.variant];
            SDL_SetTextureAlphaMod(chip, static_cast<Uint8>(a * 255));
            SDL_FRect dest = { p.x - p.size / 2 + g_shakeX, p.y - p.size / 2 + g_shakeY, p.size, p.size };
            SDL_RenderCopyExF(renderer, chip, nullptr, &dest, p.angle, nullptr, SDL_FLIP_NONE);
            SDL_SetTextureAlphaMod(chip, 255);
        }
        else {
            SDL_SetRenderDrawColor(renderer, p.color.r, p.color.g, p.color.b, static_cast<Uint8>(a * 230));
            SDL_FRect speck = { p.x + p.ox * a - p.size / 2 + g_shakeX, p.y + p.oy * a - p.size / 2 + g_shakeY, p.size, p.size };
            SDL_RenderFillRectF(renderer, &speck);
        }
    }

    Draw(renderer, assets.paddle, game.paddleX, PADDLE_Y, PADDLE_WIDTH, PADDLE_HEIGHT);
    Draw(renderer, assets.ball, game.ballX, game.ballY, BALL_SIZE, BALL_SIZE);

    // Paper frame on top, so the ball disappears under it at the bottom edge
    Draw(renderer, assets.frame, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    // HUD printed in ink on the paper strip
    DrawText(renderer, assets.hudFont, "SCORE " + std::to_string(game.score), 22, 6, INK, false, false);
    DrawText(renderer, assets.hudFont, "LEVEL " + std::to_string(game.level), SCREEN_WIDTH / 2.0f, 6, INK, true, false);
    for (int i = 0; i < game.lives; ++i) {
        Draw(renderer, assets.ball, 708.0f + i * 26.0f, 11, 18, 18);
    }

    SDL_Color light = { 240, 228, 196, 255 };
    SDL_Color muted = { 190, 186, 160, 255 };
    float cy = SCREEN_HEIGHT / 2.0f;
    switch (game.state) {
    case State::Title:
        DrawDim(renderer, 150);
        DrawText(renderer, assets.bigFont, "BRICK BREAKER", SCREEN_WIDTH / 2.0f, cy - 80, light, true);
        DrawText(renderer, assets.font, "Press Enter to Start", SCREEN_WIDTH / 2.0f, cy + 10, light, true);
        DrawText(renderer, assets.font, "Arrows to move   -   F11 fullscreen   -   Esc to quit", SCREEN_WIDTH / 2.0f, cy + 50, muted, true);
        break;
    case State::Serve:
        DrawText(renderer, assets.font, "Press Enter to launch", SCREEN_WIDTH / 2.0f, cy + 90, light, true);
        break;
    case State::GameOver:
        DrawDim(renderer, 170);
        DrawText(renderer, assets.bigFont, "GAME OVER", SCREEN_WIDTH / 2.0f, cy - 80, ROW_COLORS[0], true);
        DrawText(renderer, assets.font, "Score: " + std::to_string(game.score), SCREEN_WIDTH / 2.0f, cy + 10, light, true);
        DrawText(renderer, assets.font, "Press Enter to Play Again or Esc to Exit", SCREEN_WIDTH / 2.0f, cy + 50, muted, true);
        break;
    case State::Playing:
        break;
    }

    // Print grain over the whole picture (not shaken, like the paper itself)
    SDL_RenderCopy(renderer, assets.grain, nullptr, nullptr);

    SDL_RenderPresent(renderer);
}

// Prefers a bold serif from Windows for the vintage look, falling back to the bundled font
TTF_Font* OpenFont(int size) {
    std::vector<std::string> candidates;
    if (const char* windir = SDL_getenv("WINDIR")) {
        candidates.push_back(std::string(windir) + "\\Fonts\\BOOKOSB.TTF"); // Bookman Old Style Bold
        candidates.push_back(std::string(windir) + "\\Fonts\\georgiab.ttf");
    }
    if (char* basePath = SDL_GetBasePath()) {
        candidates.push_back(std::string(basePath) + "font.ttf");
        SDL_free(basePath);
    }
    candidates.push_back("font.ttf");

    for (const auto& path : candidates) {
        if (TTF_Font* font = TTF_OpenFont(path.c_str(), size)) return font;
    }
    std::cerr << "Could not load font: " << TTF_GetError() << std::endl;
    return nullptr;
}

bool LoadAssets(SDL_Renderer* renderer, Assets& assets) {
    assets.hudFont = OpenFont(26);
    assets.font = OpenFont(22);
    assets.bigFont = OpenFont(56);
    if (!assets.hudFont || !assets.font || !assets.bigFont) return false;

    assets.felt = MakeFelt(renderer);
    assets.frame = MakeFrame(renderer);
    assets.grain = MakeGrain(renderer);
    for (int row = 0; row < BRICK_ROWS; ++row) {
        for (int v = 0; v < BRICK_VARIANTS; ++v) {
            assets.bricks[row][v] = MakeBrick(renderer, ROW_COLORS[row], 100 + row * BRICK_VARIANTS + v);
        }
    }
    assets.brickShadow = MakeShadow(renderer, BRICK_WIDTH, BRICK_HEIGHT, 3);
    assets.paddle = MakePaddle(renderer);
    assets.paddleShadow = MakeShadow(renderer, PADDLE_WIDTH, PADDLE_HEIGHT, PADDLE_HEIGHT / 2.0f);
    assets.ball = MakeBall(renderer, BALL_SIZE * 3); // 3x resolution so the hatching stays crisp
    assets.ballShadow = MakeShadow(renderer, BALL_SIZE, BALL_SIZE, BALL_SIZE / 2.0f);

    bool bricksOk = true;
    for (auto& row : assets.bricks)
        for (SDL_Texture* t : row) bricksOk = bricksOk && t != nullptr;
    return bricksOk && assets.felt && assets.frame && assets.grain && assets.brickShadow && assets.paddle &&
           assets.paddleShadow && assets.ball && assets.ballShadow;
}

void FreeAssets(Assets& assets) {
    SDL_Texture* textures[] = { assets.felt, assets.frame, assets.grain, assets.brickShadow, assets.paddle,
                                assets.paddleShadow, assets.ball, assets.ballShadow };
    for (SDL_Texture* t : textures) if (t) SDL_DestroyTexture(t);
    for (auto& row : assets.bricks)
        for (SDL_Texture* t : row) if (t) SDL_DestroyTexture(t);
    for (TTF_Font* f : { assets.hudFont, assets.font, assets.bigFont }) if (f) TTF_CloseFont(f);
}

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "Could not initialize SDL: " << SDL_GetError() << std::endl;
        return 1;
    }

    if (TTF_Init() < 0) {
        std::cerr << "Could not initialize SDL_ttf: " << TTF_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    // Smooth (linear) filtering when textures are scaled or drawn at sub-pixel positions
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    SDL_Window* window = SDL_CreateWindow("Brick Breaker",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    if (!window) {
        std::cerr << "Could not create window: " << SDL_GetError() << std::endl;
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    // VSync keeps the frame rate at the monitor's refresh rate instead of burning a CPU core
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::cerr << "Could not create renderer: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    // Draw at 800x600 and let SDL scale it to the window (needed for fullscreen)
    SDL_RenderSetLogicalSize(renderer, SCREEN_WIDTH, SCREEN_HEIGHT);

    // No title bar, so the HUD strip at the top acts as the drag handle for moving the window
    SDL_SetWindowHitTest(window, [](SDL_Window* win, const SDL_Point* area, void*) {
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) return SDL_HITTEST_NORMAL;
        return area->y < HUD_HEIGHT ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
    }, nullptr);

    Assets assets;
    if (!LoadAssets(renderer, assets)) {
        FreeAssets(assets);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    Game game;
    NewGame(game);

    bool running = true;
    SDL_Event event;
    Uint64 lastCounter = SDL_GetPerformanceCounter();
    const double counterFrequency = static_cast<double>(SDL_GetPerformanceFrequency());

    while (running) {
        Uint64 counter = SDL_GetPerformanceCounter();
        // Clamp so a window drag or breakpoint doesn't make the ball jump
        float deltaTime = std::min(static_cast<float>((counter - lastCounter) / counterFrequency), 1.0f / 30.0f);
        lastCounter = counter;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE) {
                    running = false;
                }
                else if (key == SDLK_F11) {
                    // Toggle borderless fullscreen
                    bool fullscreen = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                else if (key == SDLK_RETURN || key == SDLK_SPACE) {
                    if (game.state == State::Title || game.state == State::Serve) {
                        LaunchBall(game);
                    }
                    else if (game.state == State::GameOver) {
                        NewGame(game);
                        game.state = State::Serve;
                    }
                }
            }
        }

        Update(game, deltaTime);
        Render(renderer, assets, game);
    }

    FreeAssets(assets);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
