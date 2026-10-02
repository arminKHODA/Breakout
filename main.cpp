#include <SDL.h>
#include <SDL_ttf.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// Screen dimensions
const int SCREEN_WIDTH = 800;
const int SCREEN_HEIGHT = 600;

// Playfield (inside the walls, below the HUD)
const int HUD_HEIGHT = 44;
const int WALL_SIZE = 14;
const float FIELD_LEFT = WALL_SIZE;
const float FIELD_RIGHT = SCREEN_WIDTH - WALL_SIZE;
const float FIELD_TOP = HUD_HEIGHT + WALL_SIZE;

// Paddle
const int PADDLE_WIDTH = 110;
const int PADDLE_HEIGHT = 18;
const float PADDLE_Y = SCREEN_HEIGHT - 50;
const float PADDLE_SPEED = 650.0f; // pixels per second

// Ball
const int BALL_SIZE = 16;
const float BALL_SPEED = 420.0f; // pixels per second
const float BALL_SPEEDUP_PER_LEVEL = 1.08f;
const float MAX_BOUNCE_ANGLE = 1.05f; // radians (~60 degrees) at the paddle edge

// Bricks
const int BRICK_COLS = 10;
const int BRICK_ROWS = 6;
const int BRICK_WIDTH = 68;
const int BRICK_HEIGHT = 24;
const int BRICK_GAP = 6;

const int START_LIVES = 3;

// Fake 3D: shadows are offset down-right because the light comes from the top-left
const float SHADOW_X = 6.0f;
const float SHADOW_Y = 8.0f;
const int SHADOW_BLUR = 6;

const float PI = 3.14159265f;

const SDL_Color ROW_COLORS[BRICK_ROWS] = {
    { 235,  64,  80, 255 }, // red
    { 245, 140,  50, 255 }, // orange
    { 240, 210,  60, 255 }, // yellow
    {  80, 200, 110, 255 }, // green
    {  60, 170, 240, 255 }, // blue
    { 160, 100, 240, 255 }, // purple
};

// ---------------------------------------------------------------------------
// Procedural texture generation (per-pixel lighting)
// ---------------------------------------------------------------------------

struct Vec3 { float x, y, z; };

static float Clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
static float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 Normalize(Vec3 v) {
    float len = std::sqrt(Dot(v, v));
    return { v.x / len, v.y / len, v.z / len };
}

// Light comes from the top-left, slightly above the screen
static const Vec3 LIGHT_DIR = Normalize({ -0.45f, -0.6f, 0.65f });
// Blinn-Phong half vector for a viewer looking straight down (0, 0, 1)
static const Vec3 HALF_DIR = Normalize({ LIGHT_DIR.x, LIGHT_DIR.y, LIGHT_DIR.z + 1.0f });

static Uint32 PackARGB(float r, float g, float b, float a) {
    auto c = [](float v) { return static_cast<Uint32>(std::min(std::max(v, 0.0f), 255.0f) + 0.5f); };
    return (c(a * 255.0f) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

// Diffuse + specular lighting of a surface point with normal n
static Uint32 ShadePixel(SDL_Color base, Vec3 n, float alpha, float specPower, float specStrength, float extra = 0.0f) {
    float diffuse = std::max(Dot(n, LIGHT_DIR), 0.0f);
    float spec = std::pow(std::max(Dot(n, HALF_DIR), 0.0f), specPower) * specStrength;
    float light = 0.30f + 0.85f * diffuse;
    float add = (spec + extra) * 255.0f;
    return PackARGB(base.r * light + add, base.g * light + add, base.b * light + add, alpha);
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

// A rounded box with bevelled edges and a glossy top face
static SDL_Texture* MakeBevelBox(SDL_Renderer* renderer, int w, int h, float radius, float bevel, SDL_Color base, float gloss) {
    std::vector<Uint32> pixels(w * h);
    float halfW = w / 2.0f, halfH = h / 2.0f;
    auto height = [&](float px, float py) {
        float inside = -SdRoundBox(px, py, halfW, halfH, radius);
        return bevel * std::sin(Clamp01(inside / bevel) * PI / 2.0f);
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float px = x + 0.5f - halfW, py = y + 0.5f - halfH;
            float d = SdRoundBox(px, py, halfW, halfH, radius);
            float alpha = Clamp01(0.5f - d);
            if (alpha <= 0.0f) {
                pixels[y * w + x] = 0;
                continue;
            }
            float dhdx = height(px + 0.5f, py) - height(px - 0.5f, py);
            float dhdy = height(px, py + 0.5f) - height(px, py - 0.5f);
            Vec3 n = Normalize({ -dhdx, -dhdy, 1.0f });

            // Sheen on the flat top face, stronger towards the top
            float topFace = Clamp01((-d - bevel) / 2.0f);
            float sheen = gloss * topFace * std::pow(1.0f - static_cast<float>(y) / h, 1.5f) * 0.5f;

            pixels[y * w + x] = ShadePixel(base, n, alpha, 40.0f, 0.55f, sheen);
        }
    }
    return TextureFromPixels(renderer, w, h, pixels);
}

// A shiny sphere with a bluish rim light
static SDL_Texture* MakeSphere(SDL_Renderer* renderer, int size, SDL_Color base) {
    std::vector<Uint32> pixels(size * size);
    float radius = size / 2.0f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = (x + 0.5f - radius) / radius;
            float dy = (y + 0.5f - radius) / radius;
            float dist = std::sqrt(dx * dx + dy * dy);
            float alpha = Clamp01((1.0f - dist) * radius + 0.5f);
            if (alpha <= 0.0f) {
                pixels[y * size + x] = 0;
                continue;
            }
            Vec3 n = Normalize({ dx, dy, std::sqrt(std::max(0.0f, 1.0f - dist * dist)) + 0.001f });
            float diffuse = std::max(Dot(n, LIGHT_DIR), 0.0f);
            float spec = std::pow(std::max(Dot(n, HALF_DIR), 0.0f), 60.0f) * 0.9f;
            float rim = std::pow(1.0f - n.z, 3.0f) * 0.6f;
            float light = 0.25f + 0.85f * diffuse;
            pixels[y * size + x] = PackARGB(
                base.r * light + spec * 255.0f + rim * 80.0f,
                base.g * light + spec * 255.0f + rim * 140.0f,
                base.b * light + spec * 255.0f + rim * 255.0f,
                alpha);
        }
    }
    return TextureFromPixels(renderer, size, size, pixels);
}

// Soft black shadow of a rounded box; the texture is padded by SHADOW_BLUR on each side
static SDL_Texture* MakeShadow(SDL_Renderer* renderer, int w, int h, float radius) {
    int tw = w + SHADOW_BLUR * 2, th = h + SHADOW_BLUR * 2;
    std::vector<Uint32> pixels(tw * th);
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            float d = SdRoundBox(x + 0.5f - tw / 2.0f, y + 0.5f - th / 2.0f, w / 2.0f, h / 2.0f, radius);
            float a = Clamp01((SHADOW_BLUR - d) / (2.0f * SHADOW_BLUR));
            a = a * a * (3.0f - 2.0f * a); // smoothstep
            pixels[y * tw + x] = PackARGB(0, 0, 0, a * 0.55f);
        }
    }
    return TextureFromPixels(renderer, tw, th, pixels);
}

// White radial falloff, drawn with additive blending and tinted with SDL_SetTextureColorMod
static SDL_Texture* MakeGlow(SDL_Renderer* renderer, int size) {
    std::vector<Uint32> pixels(size * size);
    float radius = size / 2.0f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = x + 0.5f - radius, dy = y + 0.5f - radius;
            float a = Clamp01(1.0f - std::sqrt(dx * dx + dy * dy) / radius);
            pixels[y * size + x] = PackARGB(255, 255, 255, a * a);
        }
    }
    SDL_Texture* texture = TextureFromPixels(renderer, size, size, pixels);
    if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
    return texture;
}

// Recessed floor: gradient, grid, darker near the walls (ambient occlusion) and a vignette
static SDL_Texture* MakeBackground(SDL_Renderer* renderer) {
    std::vector<Uint32> pixels(SCREEN_WIDTH * SCREEN_HEIGHT);
    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            float r, g, b;
            if (y < HUD_HEIGHT) {
                float t = static_cast<float>(y) / HUD_HEIGHT;
                r = 20 - 6 * t; g = 22 - 6 * t; b = 42 - 10 * t;
            }
            else {
                float t = static_cast<float>(y) / SCREEN_HEIGHT;
                r = 30 - 18 * t; g = 32 - 20 * t; b = 64 - 36 * t;

                int gx = x - static_cast<int>(FIELD_LEFT), gy = y - static_cast<int>(FIELD_TOP);
                if ((gx >= 0 && gx % 40 == 0) || (gy >= 0 && gy % 40 == 0)) {
                    r += 8; g += 10; b += 24;
                }

                // Spotlight from the top centre
                float sx = (x - SCREEN_WIDTH / 2.0f) / SCREEN_WIDTH;
                float sy = (y - FIELD_TOP) / SCREEN_HEIGHT;
                float spot = 1.0f + 0.5f * std::exp(-(sx * sx * 6.0f + sy * sy * 4.0f));

                float edge = std::min({ x - FIELD_LEFT, FIELD_RIGHT - x, y - FIELD_TOP });
                float ao = 0.45f + 0.55f * Clamp01(edge / 60.0f);

                float vx = static_cast<float>(x) / SCREEN_WIDTH - 0.5f;
                float vy = static_cast<float>(y) / SCREEN_HEIGHT - 0.5f;
                float vignette = 1.0f - 0.9f * (vx * vx + vy * vy);

                float k = spot * ao * vignette;
                r *= k; g *= k; b *= k;
            }
            pixels[y * SCREEN_WIDTH + x] = PackARGB(r, g, b, 1.0f);
        }
    }
    return TextureFromPixels(renderer, SCREEN_WIDTH, SCREEN_HEIGHT, pixels);
}

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------

struct Brick {
    float x, y;
    int row;
    bool active;
};

struct Particle {
    float x, y, vx, vy;
    float life, maxLife;
    float size, angle, spin;
    int row;     // brick colour for chips
    bool spark;  // glowing spark instead of a brick chip
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
    std::deque<SDL_FPoint> trail;
    float shake = 0;
    float paddleFlash = 0;
};

struct Assets {
    SDL_Texture* background = nullptr;
    SDL_Texture* wallSide = nullptr;
    SDL_Texture* wallTop = nullptr;
    SDL_Texture* bricks[BRICK_ROWS] = {};
    SDL_Texture* brickShadow = nullptr;
    SDL_Texture* paddle = nullptr;
    SDL_Texture* paddleShadow = nullptr;
    SDL_Texture* ball = nullptr;
    SDL_Texture* ballShadow = nullptr;
    SDL_Texture* glow = nullptr;
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
                                    FIELD_TOP + 40 + row * (BRICK_HEIGHT + BRICK_GAP), row, true });
        }
    }
}

// Put the ball back on the paddle
void ResetBall(Game& game) {
    game.ballX = game.paddleX + PADDLE_WIDTH / 2.0f - BALL_SIZE / 2.0f;
    game.ballY = PADDLE_Y - BALL_SIZE;
    game.ballVelX = game.ballVelY = 0;
    game.trail.clear();
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

void SpawnBrickBreak(Game& game, const Brick& brick) {
    float cx = brick.x + BRICK_WIDTH / 2.0f, cy = brick.y + BRICK_HEIGHT / 2.0f;
    for (int i = 0; i < 14; ++i) {
        float angle = RandRange(0, 2 * PI), speed = RandRange(80, 320);
        float life = RandRange(0.5f, 0.9f);
        game.particles.push_back({ cx + RandRange(-25, 25), cy + RandRange(-8, 8),
                                   std::cos(angle) * speed, std::sin(angle) * speed - 60,
                                   life, life, RandRange(5, 10), RandRange(0, 360), RandRange(-600, 600),
                                   brick.row, false, ROW_COLORS[brick.row] });
    }
    for (int i = 0; i < 8; ++i) {
        float angle = RandRange(0, 2 * PI), speed = RandRange(40, 200);
        float life = RandRange(0.3f, 0.5f);
        game.particles.push_back({ cx, cy, std::cos(angle) * speed, std::sin(angle) * speed,
                                   life, life, RandRange(14, 26), 0, 0, brick.row, true, ROW_COLORS[brick.row] });
    }
}

void SpawnSparks(Game& game, float x, float y, SDL_Color color) {
    for (int i = 0; i < 6; ++i) {
        float angle = RandRange(PI, 2 * PI), speed = RandRange(60, 220);
        float life = RandRange(0.2f, 0.4f);
        game.particles.push_back({ x, y, std::cos(angle) * speed, std::sin(angle) * speed,
                                   life, life, RandRange(10, 18), 0, 0, 0, true, color });
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
        game.paddleFlash = 1.0f;
        SpawnSparks(game, ballCenter, PADDLE_Y, { 140, 200, 255, 255 });
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

        if (game.state == State::Playing) {
            game.trail.push_front({ game.ballX, game.ballY });
            if (game.trail.size() > 12) game.trail.pop_back();
        }

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
        if (!p.spark) p.vy += 900.0f * dt; // chips fall
        p.angle += p.spin * dt;
    }
    game.particles.erase(std::remove_if(game.particles.begin(), game.particles.end(),
                                        [](const Particle& p) { return p.life <= 0; }),
                         game.particles.end());

    game.shake = std::max(0.0f, game.shake - 40.0f * dt);
    game.paddleFlash = std::max(0.0f, game.paddleFlash - 4.0f * dt);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Screen-shake offset applied to everything in the playfield
static float g_shakeX = 0, g_shakeY = 0;

void Draw(SDL_Renderer* renderer, SDL_Texture* texture, float x, float y, float w, float h) {
    SDL_FRect dest = { x + g_shakeX, y + g_shakeY, w, h };
    SDL_RenderCopyF(renderer, texture, nullptr, &dest);
}

void DrawShadow(SDL_Renderer* renderer, SDL_Texture* shadow, float x, float y, float w, float h, float lift) {
    Draw(renderer, shadow, x + SHADOW_X * lift - SHADOW_BLUR, y + SHADOW_Y * lift - SHADOW_BLUR,
         w + SHADOW_BLUR * 2, h + SHADOW_BLUR * 2);
}

void DrawGlow(SDL_Renderer* renderer, SDL_Texture* glow, float cx, float cy, float size, SDL_Color color, float alpha) {
    SDL_SetTextureColorMod(glow, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(glow, static_cast<Uint8>(Clamp01(alpha) * 255));
    Draw(renderer, glow, cx - size / 2, cy - size / 2, size, size);
}

void DrawText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, float x, float y, SDL_Color color, bool centered) {
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    float w = static_cast<float>(surface->w), h = static_cast<float>(surface->h);
    SDL_FreeSurface(surface);
    if (!texture) return;
    if (centered) x -= w / 2;

    SDL_FRect shadowRect = { x + 2, y + 3, w, h };
    SDL_SetTextureColorMod(texture, 0, 0, 0);
    SDL_SetTextureAlphaMod(texture, 170);
    SDL_RenderCopyF(renderer, texture, nullptr, &shadowRect);

    SDL_FRect rect = { x, y, w, h };
    SDL_SetTextureColorMod(texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(texture, 255);
    SDL_RenderCopyF(renderer, texture, nullptr, &rect);
    SDL_DestroyTexture(texture);
}

void DrawDim(SDL_Renderer* renderer, Uint8 alpha) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 5, 5, 15, alpha);
    SDL_RenderFillRect(renderer, nullptr);
}

void Render(SDL_Renderer* renderer, const Assets& assets, const Game& game) {
    g_shakeX = game.shake > 0 ? RandRange(-game.shake, game.shake) : 0;
    g_shakeY = game.shake > 0 ? RandRange(-game.shake, game.shake) : 0;

    SDL_SetRenderDrawColor(renderer, 14, 15, 30, 255);
    SDL_RenderClear(renderer);

    Draw(renderer, assets.background, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    // Shadows first so every object casts onto the floor
    for (const auto& brick : game.bricks) {
        if (brick.active) DrawShadow(renderer, assets.brickShadow, brick.x, brick.y, BRICK_WIDTH, BRICK_HEIGHT, 1.0f);
    }
    DrawShadow(renderer, assets.paddleShadow, game.paddleX, PADDLE_Y, PADDLE_WIDTH, PADDLE_HEIGHT, 1.0f);
    DrawShadow(renderer, assets.ballShadow, game.ballX, game.ballY, BALL_SIZE, BALL_SIZE, 1.6f);

    // Walls
    float wallHeight = SCREEN_HEIGHT - HUD_HEIGHT;
    Draw(renderer, assets.wallSide, 0, HUD_HEIGHT, WALL_SIZE, wallHeight);
    Draw(renderer, assets.wallSide, FIELD_RIGHT, HUD_HEIGHT, WALL_SIZE, wallHeight);
    Draw(renderer, assets.wallTop, 0, HUD_HEIGHT, SCREEN_WIDTH, WALL_SIZE);

    for (const auto& brick : game.bricks) {
        if (brick.active) Draw(renderer, assets.bricks[brick.row], brick.x, brick.y, BRICK_WIDTH, BRICK_HEIGHT);
    }

    Draw(renderer, assets.paddle, game.paddleX, PADDLE_Y, PADDLE_WIDTH, PADDLE_HEIGHT);
    if (game.paddleFlash > 0) {
        DrawGlow(renderer, assets.glow, game.paddleX + PADDLE_WIDTH / 2.0f, PADDLE_Y + PADDLE_HEIGHT / 2.0f,
                 PADDLE_WIDTH * 1.4f, { 120, 190, 255, 255 }, game.paddleFlash * 0.8f);
    }

    // Ball trail, glow and the ball itself
    for (size_t i = 0; i < game.trail.size(); ++i) {
        float t = 1.0f - static_cast<float>(i) / game.trail.size();
        float size = BALL_SIZE * (0.4f + 0.6f * t);
        const SDL_FPoint& p = game.trail[i];
        DrawGlow(renderer, assets.glow, p.x + BALL_SIZE / 2.0f, p.y + BALL_SIZE / 2.0f, size * 1.6f, { 90, 150, 255, 255 }, t * 0.5f);
    }
    float ballCX = game.ballX + BALL_SIZE / 2.0f, ballCY = game.ballY + BALL_SIZE / 2.0f;
    DrawGlow(renderer, assets.glow, ballCX, ballCY, 70, { 110, 170, 255, 255 }, 0.55f);
    Draw(renderer, assets.ball, game.ballX, game.ballY, BALL_SIZE, BALL_SIZE);

    // Particles: tumbling brick chips and additive sparks
    for (const auto& p : game.particles) {
        float a = Clamp01(p.life / p.maxLife);
        if (p.spark) {
            DrawGlow(renderer, assets.glow, p.x, p.y, p.size * a, p.color, a);
        }
        else {
            SDL_Texture* chip = assets.bricks[p.row];
            SDL_SetTextureAlphaMod(chip, static_cast<Uint8>(a * 255));
            SDL_FRect dest = { p.x - p.size / 2 + g_shakeX, p.y - p.size / 2 + g_shakeY, p.size, p.size };
            SDL_RenderCopyExF(renderer, chip, nullptr, &dest, p.angle, nullptr, SDL_FLIP_NONE);
            SDL_SetTextureAlphaMod(chip, 255);
        }
    }

    // HUD (not affected by screen shake)
    SDL_Color white = { 235, 240, 255, 255 };
    SDL_Color accent = { 140, 200, 255, 255 };
    DrawText(renderer, assets.font, "SCORE  " + std::to_string(game.score), 20, 9, white, false);
    DrawText(renderer, assets.font, "LEVEL  " + std::to_string(game.level), SCREEN_WIDTH / 2.0f, 9, accent, true);
    for (int i = 0; i < game.lives; ++i) {
        SDL_FRect life = { SCREEN_WIDTH - 30.0f - i * 24.0f, 14, 16, 16 };
        SDL_RenderCopyF(renderer, assets.ball, nullptr, &life);
    }

    float cy = SCREEN_HEIGHT / 2.0f;
    switch (game.state) {
    case State::Title:
        DrawDim(renderer, 150);
        DrawText(renderer, assets.bigFont, "BRICK BREAKER", SCREEN_WIDTH / 2.0f, cy - 80, accent, true);
        DrawText(renderer, assets.font, "Press Enter to Start", SCREEN_WIDTH / 2.0f, cy + 10, white, true);
        DrawText(renderer, assets.font, "Arrows to move   -   F11 fullscreen   -   Esc to quit", SCREEN_WIDTH / 2.0f, cy + 50, { 150, 160, 190, 255 }, true);
        break;
    case State::Serve:
        DrawText(renderer, assets.font, "Press Enter to launch", SCREEN_WIDTH / 2.0f, cy + 90, white, true);
        break;
    case State::GameOver:
        DrawDim(renderer, 170);
        DrawText(renderer, assets.bigFont, "GAME OVER", SCREEN_WIDTH / 2.0f, cy - 80, { 255, 110, 120, 255 }, true);
        DrawText(renderer, assets.font, "Score: " + std::to_string(game.score), SCREEN_WIDTH / 2.0f, cy + 10, white, true);
        DrawText(renderer, assets.font, "Press Enter to Play Again or Esc to Exit", SCREEN_WIDTH / 2.0f, cy + 50, { 150, 160, 190, 255 }, true);
        break;
    case State::Playing:
        break;
    }

    SDL_RenderPresent(renderer);
}

bool LoadAssets(SDL_Renderer* renderer, Assets& assets) {
    // Load the font from the exe's folder so it works no matter where the game is started from
    std::string fontPath = "font.ttf";
    if (char* basePath = SDL_GetBasePath()) {
        fontPath = std::string(basePath) + "font.ttf";
        SDL_free(basePath);
    }
    assets.font = TTF_OpenFont(fontPath.c_str(), 22);
    assets.bigFont = TTF_OpenFont(fontPath.c_str(), 56);
    if (!assets.font || !assets.bigFont) {
        std::cerr << "Could not load font: " << TTF_GetError() << std::endl;
        return false;
    }

    SDL_Color steel = { 95, 105, 140, 255 };
    assets.background = MakeBackground(renderer);
    assets.wallSide = MakeBevelBox(renderer, WALL_SIZE, SCREEN_HEIGHT - HUD_HEIGHT, 3, 6, steel, 0.1f);
    assets.wallTop = MakeBevelBox(renderer, SCREEN_WIDTH, WALL_SIZE, 3, 6, steel, 0.1f);
    for (int row = 0; row < BRICK_ROWS; ++row) {
        assets.bricks[row] = MakeBevelBox(renderer, BRICK_WIDTH, BRICK_HEIGHT, 5, 6, ROW_COLORS[row], 0.35f);
    }
    assets.brickShadow = MakeShadow(renderer, BRICK_WIDTH, BRICK_HEIGHT, 5);
    // Radius = bevel = half the height gives a rounded, cylinder-like paddle
    assets.paddle = MakeBevelBox(renderer, PADDLE_WIDTH, PADDLE_HEIGHT, PADDLE_HEIGHT / 2.0f, PADDLE_HEIGHT / 2.0f, { 190, 200, 225, 255 }, 0.4f);
    assets.paddleShadow = MakeShadow(renderer, PADDLE_WIDTH, PADDLE_HEIGHT, PADDLE_HEIGHT / 2.0f);
    assets.ball = MakeSphere(renderer, BALL_SIZE * 2, { 225, 232, 255, 255 }); // 2x resolution, drawn scaled down
    assets.ballShadow = MakeShadow(renderer, BALL_SIZE, BALL_SIZE, BALL_SIZE / 2.0f);
    assets.glow = MakeGlow(renderer, 64);

    return assets.background && assets.wallSide && assets.wallTop && assets.brickShadow && assets.paddle &&
           assets.paddleShadow && assets.ball && assets.ballShadow && assets.glow &&
           std::all_of(std::begin(assets.bricks), std::end(assets.bricks), [](SDL_Texture* t) { return t != nullptr; });
}

void FreeAssets(Assets& assets) {
    SDL_Texture* textures[] = { assets.background, assets.wallSide, assets.wallTop, assets.brickShadow, assets.paddle,
                                assets.paddleShadow, assets.ball, assets.ballShadow, assets.glow };
    for (SDL_Texture* t : textures) if (t) SDL_DestroyTexture(t);
    for (SDL_Texture* t : assets.bricks) if (t) SDL_DestroyTexture(t);
    if (assets.font) TTF_CloseFont(assets.font);
    if (assets.bigFont) TTF_CloseFont(assets.bigFont);
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
