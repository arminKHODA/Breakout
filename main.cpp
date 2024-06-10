#include <SDL.h>
#include <SDL_ttf.h>
#include <iostream>
#include <vector>
#include <sstream>

// Screen dimensions
const int SCREEN_WIDTH = 800;
const int SCREEN_HEIGHT = 600;

// Paddle dimensions
const int PADDLE_WIDTH = 100;
const int PADDLE_HEIGHT = 20;

// Ball dimensions
const int BALL_SIZE = 10;

// Brick dimensions
const int BRICK_WIDTH = 75;
const int BRICK_HEIGHT = 20;

// Paddle speed
const int PADDLE_SPEED = 1; // Changed back to an int for more direct control

// Ball speed
const float BALL_SPEED = 0.2f;

struct Brick {
    int x, y;
    bool active;
};

void RenderText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color) {
    SDL_Surface* surface = TTF_RenderText_Solid(font, text.c_str(), color);
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect destRect = { x, y, surface->w, surface->h };
    SDL_RenderCopy(renderer, texture, nullptr, &destRect);
    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

void ResetGame(int& paddleX, int& paddleY, float& ballX, float& ballY, float& ballVelX, float& ballVelY, int& score, std::vector<Brick>& bricks) {
    paddleX = (SCREEN_WIDTH - PADDLE_WIDTH) / 2;
    paddleY = SCREEN_HEIGHT - PADDLE_HEIGHT - 10;
    ballX = paddleX + PADDLE_WIDTH / 2 - BALL_SIZE / 2;
    ballY = paddleY - BALL_SIZE;
    ballVelX = BALL_SPEED;
    ballVelY = -BALL_SPEED;
    score = 0;
    bricks.clear();
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 5; ++j) {
            bricks.push_back({ i * (BRICK_WIDTH + 10) + 35, j * (BRICK_HEIGHT + 10) + 50, true });
        }
    }
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

    SDL_Window* window = SDL_CreateWindow("Brick Breaker",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "Could not create window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::cerr << "Could not create renderer: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    TTF_Font* font = TTF_OpenFont("font.ttf", 24); // Provide the correct path to a TTF font file
    if (!font) {
        std::cerr << "Could not load font: " << TTF_GetError() << std::endl;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int paddleX, paddleY, score;
    float ballX, ballY, ballVelX, ballVelY;
    std::vector<Brick> bricks;
    ResetGame(paddleX, paddleY, ballX, ballY, ballVelX, ballVelY, score, bricks);

    bool running = true;
    bool gameStarted = false;
    bool gameOver = false;
    SDL_Event event;
    Uint32 lastTime = SDL_GetTicks(), currentTime;

    while (running) {
        currentTime = SDL_GetTicks();
        float deltaTime = (currentTime - lastTime) / 1000.0f; // Calculate delta time in seconds
        lastTime = currentTime;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_RETURN && !gameStarted && !gameOver) {
                    gameStarted = true;
                }
                else if (event.key.keysym.sym == SDLK_RETURN && gameOver) {
                    ResetGame(paddleX, paddleY, ballX, ballY, ballVelX, ballVelY, score, bricks);
                    gameStarted = true;
                    gameOver = false;
                }
                else if (event.key.keysym.sym == SDLK_ESCAPE && gameOver) {
                    running = false;
                }
            }
        }

        if (!gameStarted && !gameOver) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            RenderText(renderer, font, "Press Enter to Start", SCREEN_WIDTH / 2 - 100, SCREEN_HEIGHT / 2, { 255, 255, 255, 255 });
            SDL_RenderPresent(renderer);
            continue;
        }

        if (!gameOver) {
            const Uint8* state = SDL_GetKeyboardState(NULL);
            if (state[SDL_SCANCODE_LEFT] && paddleX > 0) {
                paddleX -= PADDLE_SPEED * deltaTime * 1000; // Use deltaTime for smooth movement
            }
            if (state[SDL_SCANCODE_RIGHT] && paddleX < SCREEN_WIDTH - PADDLE_WIDTH) {
                paddleX += PADDLE_SPEED * deltaTime * 1000; // Use deltaTime for smooth movement
            }

            ballX += ballVelX;
            ballY += ballVelY;

            if (ballY <= 0) {
                ballVelY = -ballVelY;
            }

            if (ballX <= 0 || ballX >= SCREEN_WIDTH - BALL_SIZE) {
                ballVelX = -ballVelX;
            }

            if (ballY + BALL_SIZE >= SCREEN_HEIGHT) {
                gameOver = true;
                gameStarted = false;
            }

            if (ballY + BALL_SIZE >= paddleY && ballX + BALL_SIZE > paddleX && ballX < paddleX + PADDLE_WIDTH) {
                ballVelY = -ballVelY;
            }

            for (auto& brick : bricks) {
                if (brick.active) {
                    SDL_Rect brickRect = { brick.x, brick.y, BRICK_WIDTH, BRICK_HEIGHT };
                    SDL_Rect ballRect = { static_cast<int>(ballX), static_cast<int>(ballY), BALL_SIZE, BALL_SIZE };
                    if (SDL_HasIntersection(&brickRect, &ballRect)) {
                        brick.active = false;
                        ballVelY = -ballVelY;
                        score += 10;
                    }
                }
            }

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);

            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_Rect paddle = { paddleX, paddleY, PADDLE_WIDTH, PADDLE_HEIGHT };
            SDL_Rect ball = { static_cast<int>(ballX), static_cast<int>(ballY), BALL_SIZE, BALL_SIZE };
            SDL_RenderFillRect(renderer, &paddle);
            SDL_RenderFillRect(renderer, &ball);

            for (const auto& brick : bricks) {
                if (brick.active) {
                    SDL_Rect brickRect = { brick.x, brick.y, BRICK_WIDTH, BRICK_HEIGHT };
                    SDL_RenderFillRect(renderer, &brickRect);
                }
            }

            std::stringstream scoreText;
            scoreText << "Score: " << score;
            RenderText(renderer, font, scoreText.str(), 10, 10, { 255, 255, 255, 255 });

            SDL_RenderPresent(renderer);
        }
        else {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            std::stringstream gameOverText;
            gameOverText << "Game Over! Score: " << score;
            RenderText(renderer, font, gameOverText.str(), SCREEN_WIDTH / 2 - 100, SCREEN_HEIGHT / 2 - 20, { 255, 255, 255, 255 });
            RenderText(renderer, font, "Press Enter to Play Again or Esc to Exit", SCREEN_WIDTH / 2 - 150, SCREEN_HEIGHT / 2 + 20, { 255, 255, 255, 255 });
            SDL_RenderPresent(renderer);
        }
    }

    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
