# Brick Breaker Game

This is a simple Brick Breaker game built using SDL2 and SDL_ttf libraries. The objective of the game is to break all the bricks by bouncing the ball off the paddle.

## Features

- Move the paddle using keyboard inputs.
- Ball bounces off the paddle, walls, and bricks.
- Bricks disappear when hit by the ball.
- Scorekeeping based on the number of bricks broken.
- Display the score on the screen.
- Game over screen with the final score and an option to restart or exit.

## Requirements

- C++ compiler
- SDL2 library
- SDL_ttf library

## Installation

1. **Install SDL2**:
   - On Ubuntu:
     ```bash
     sudo apt-get install libsdl2-dev
     sudo apt-get install libsdl2-ttf-dev
     ```
   - On Windows:
     - Download the SDL2 development libraries from the [SDL website](https://www.libsdl.org/download-2.0.php) and follow the instructions for installation.
     - Download the SDL_ttf library from the [SDL_ttf website](https://www.libsdl.org/projects/SDL_ttf/) and follow the instructions for installation.

2. **Clone the Repository**:
   ```bash
   git clone https://github.com/yourusername/brick-breaker-game.git
   cd brick-breaker-game
