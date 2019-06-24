#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <memory>
#include <random>
#include <utility>

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

struct delete_sdl
{
    void operator()(SDL_Texture *p) const
    {
        SDL_DestroyTexture(p);
    }

    void operator()(SDL_Surface *p) const
    {
        SDL_FreeSurface(p);
    }
};

template<class T>
using sdl_ptr = std::unique_ptr<T, delete_sdl>;

const int INNER_SPREAD = 32;
const int BORDER_SIZE = 16;
const int BAND_SIZE = 32;
const int NBANDS = 7;

const int SIZE = 2 * INNER_SPREAD + 2 * BORDER_SIZE + 2 * NBANDS * BAND_SIZE;
const int WIDTH = SIZE;
const int HEIGHT = SIZE;
const int FONT_HEIGHT = 16;

const int BAND_THICKNESS = 16;

SDL_Window *win = NULL;
TTF_Font *font = NULL;
SDL_Renderer *ren = NULL;
sdl_ptr<SDL_Texture> canvas;

void cleanup()
{
    // Must destroy textures here because global destructors haven't run yet.
    canvas.reset();

    if (ren) SDL_DestroyRenderer(ren);
    if (font) TTF_CloseFont(font);
    if (win) SDL_DestroyWindow(win);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}

void failAny(const char *msg)
{
    std::printf("failed: %s\n", msg);
    exit(1);
}

void failSDL(const char *msg)
{
    std::printf("SDL %s failed: %s\n", msg, SDL_GetError());
    exit(1);
}

void failTTF(const char *msg)
{
    std::printf("TTF %s failed: %s\n", msg, TTF_GetError());
    exit(1);
}

void failIMG(const char *msg)
{
    std::printf("IMG %s failed: %s\n", msg, IMG_GetError());
    exit(1);
}

SDL_Texture * LoadTexture(const char *path)
{
    SDL_Texture *tex = IMG_LoadTexture(ren, path);
    if (!tex) failIMG("LoadTexture");
    return tex;
}

std::minstd_rand rng;
int RandInt(int lo, int hi)
{
    std::uniform_int_distribution<> dis(lo, hi);
    return dis(rng);
}

const int INTRO_LEN = 4;
const int LEVEL_LEN = 300;

const int LANES_MIN = 3;
const int LANES_MAX = 16;

const int BAND_TYPE_NONE = 0;
const int BAND_TYPE_WALL = 1;
const int BAND_TYPE_HURDLE = 2;

int nlanes;
int incoming[LANES_MAX][LEVEL_LEN];
int offset;
int playerLane;
bool playerAlive;
bool playerHurdling;

Uint32 prevFrame_ms;
Uint32 timeSinceAdvance_ms;

double renderAvgTime_ms;
double renderAvgDenom;
const double renderAvg_decay = 0.99;

bool quitRequested;

struct Pattern
{
    std::vector<std::string> rows;
};
std::vector<Pattern> patterns;

void ReadPatterns()
{
    patterns.clear();

    FILE * f = fopen("data/patterns.txt", "r");
    if (!f) failAny("fopen data/patterns.txt");

    if (fscanf(f, " %d", &nlanes) != 1) failAny("could not read number of lanes");
    printf("Geometry has %d lanes\n", nlanes);
    if (nlanes < LANES_MIN || LANES_MAX < nlanes) failAny("number of lanes out of bounds");

    for (int i = 0; ; ++i) {
        Pattern p;
        printf("Pattern %d:\n", i);
        int plen;
        if (fscanf(f, " %d", &plen) != 1) failAny("could not read pattern length");

        if (plen == 0) {
            printf("Read terminating 0\n");
            break;
        }

        for (int j = 0; j < plen; ++j) {
            char buf[256];
            if (fscanf(f, " %255s", buf) != 1) failAny("could not read pattern row");
            std::string row = buf;
            printf("%s\n", row.c_str());
            if (row.size() != nlanes) failAny("incorrect length of pattern row");
            p.rows.push_back(buf);
        }
        patterns.push_back(p);
    }

    if (patterns.empty()) failAny("expected at least one pattern");

    if (fclose(f)) failAny("fclose");
}

// Precompute quantities needed to render quickly
int laneAt[HEIGHT][WIDTH];
double distAt[HEIGHT][WIDTH];
int bandNumAt[HEIGHT][WIDTH];
void Precompute()
{
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            double dx = x - (WIDTH - 1) / 2.0;
            double dy = y - (HEIGHT - 1) / 2.0;

            // Angles are clockwise of straight up
            double theta = atan2(dx, dy) + M_PI;
            int wedge = static_cast<int>(theta / (M_PI / nlanes));
            int lane = ((wedge + 1) % (2 * nlanes)) / 2;
            laneAt[y][x] = lane;

            double rho = lane * (2.0 * M_PI / nlanes);
            double laneDX = -sin(rho);
            double laneDY = -cos(rho);

            // Distance down this lane
            double dist = laneDX * dx + laneDY * dy;
            distAt[y][x] = dist;

            const int INNER_BORDER = INNER_SPREAD + BORDER_SIZE;
            bandNumAt[y][x] = 0;
            if (dist >= INNER_BORDER) {
                double outerDist = dist - INNER_BORDER;
                bandNumAt[y][x] = static_cast<int>(outerDist / BAND_SIZE);
            }
        }
    }
}

void Restart()
{
    ReadPatterns();
    Precompute();

    for (int i = 0; i < LEVEL_LEN; ++i) {
        for (int d = 0; d < nlanes; ++d) {
            incoming[d][i] = BAND_TYPE_NONE;
        }
    }

    int i = INTRO_LEN;
    while (true) {
        // Select random pattern, flip, and rotation
        int type = rng() % patterns.size();
        int lane0 = rng() % nlanes;
        int dlane = -1 + 2 * (rng() % 2);

        const Pattern &p = patterns[type];

        if (i + p.rows.size() >= LEVEL_LEN) break;

        for (auto row : p.rows) {
            for (int k = 0; k < nlanes; ++k) {
                int d = (lane0 + dlane * k + nlanes) % nlanes;
                char c = row[k];
                if (c == '#') {
                    incoming[d][i] = BAND_TYPE_WALL;
                } else if (c == 'o') {
                    incoming[d][i] = BAND_TYPE_HURDLE;
                }
            }
            ++i;
        }
    }

    offset = 0;
    playerLane = 0;
    playerAlive = true;
    playerHurdling = false;
    
    // Anything big is fine.
    timeSinceAdvance_ms = 1000;
}

int GetIncomingBandType(int lane, int bandNum)
{
    bandNum += offset;
    if (bandNum < 0 || LEVEL_LEN <= bandNum) return BAND_TYPE_NONE;
    return incoming[lane][bandNum];
}

bool IsBandPlayer(int lane, int bandNum)
{
    return lane == playerLane && bandNum == 0;
}

bool BandHalfParity(int bandNum)
{
    return ((offset + bandNum) / 2) % 2;
}

void CheckCollision()
{
    int t = GetIncomingBandType(playerLane, 0);
    if (t == BAND_TYPE_WALL ||
            (t == BAND_TYPE_HURDLE && !playerHurdling) ||
            (t == BAND_TYPE_NONE && playerHurdling)) {
        playerAlive = false;
    }
}

void Advance()
{
    timeSinceAdvance_ms = 0;
    ++offset;
    CheckCollision();
    playerHurdling = false;
}

const double ANIM_PER_SEC = 240.0;
const double ANIM_PER_MS = ANIM_PER_SEC / 1000.0;

void DrawText(const char *s, SDL_Color color, int x, int y, int *textW, int *textH, bool center = false)
{
    int tW, tH;
    if (textW == NULL) textW = &tW;
    if (textH == NULL) textH = &tH;

    sdl_ptr<SDL_Surface> textSurf(TTF_RenderText_Solid(font, s, color));
    if (!textSurf) failTTF("TTF_RenderText_Solid");

    sdl_ptr<SDL_Texture> textTex(SDL_CreateTextureFromSurface(ren, textSurf.get()));
    if (!textTex) failSDL("SDL_CreateTextureFromSurface");

    if (SDL_QueryTexture(textTex.get(), NULL, NULL, textW, textH) < 0) failSDL("SDL_QueryTexture");

    if (center) {
        x -= *textW / 2;
        y -= *textH / 2;
    }

    SDL_Rect dst = { x, y, *textW, *textH };
    if (SDL_RenderCopy(ren, textTex.get(), NULL, &dst) < 0) failSDL("SDL_RenderCopy");
}

const uint32_t DARK_RED = 0x471205FF;
const uint32_t MEDIUM_RED = 0x6A1A07FF;
const uint32_t LIGHT_RED = 0xC1161EFF;
const uint32_t VERY_LIGHT_RED = 0xFF7780FF;

const uint32_t LIGHT_GREEN = 0x1fc116FF;

void update()
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            quitRequested = true;
        }
        
        if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_BACKSPACE) {
                Restart();
            }

            if (playerAlive) {
                if (e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_s) {
                    playerLane = (playerLane + 1) % nlanes;
                    Advance();
                }

                if (e.key.keysym.sym == SDLK_RIGHT || e.key.keysym.sym == SDLK_f) {
                    playerLane = (playerLane + nlanes - 1) % nlanes;
                    Advance();
                }

                if (e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_e) {
                    Advance();
                }

                if (e.key.keysym.sym == SDLK_DOWN || e.key.keysym.sym == SDLK_d) {
                    playerHurdling = true;
                    Advance();
                }
            }
        }
    }
}

uint32_t * pixels;
int pitch;

void render()
{
    // Draw
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            int lane = laneAt[y][x];
            double dist = distAt[y][x];

            uint32_t color = lane % 2 ? DARK_RED : MEDIUM_RED;

            const int INNER_BORDER = INNER_SPREAD + BORDER_SIZE;
            if (dist < INNER_SPREAD) {
                color = DARK_RED;
            } else if (dist < INNER_BORDER) {
                color = LIGHT_RED;
            } else {
                double outerDist = dist - INNER_BORDER;
                int bandNum = bandNumAt[y][x];
                double inBandDist = outerDist - BAND_SIZE * bandNum;

                for (int dband = 0; dband <= 1; ++dband) {
                    int t = GetIncomingBandType(lane, bandNum - dband);
                    if (t != BAND_TYPE_NONE) {
                        uint32_t bandColor = LIGHT_RED;
                        if (t == BAND_TYPE_HURDLE) bandColor = LIGHT_GREEN;

                        int thickness = GetIncomingBandType(lane, bandNum + 1 - dband) == t ? BAND_SIZE : BAND_THICKNESS;
                        int tween = std::max(BAND_SIZE - static_cast<int>(round(ANIM_PER_MS * timeSinceAdvance_ms)), 0);
                        if (inBandDist + dband * BAND_SIZE < thickness + tween && inBandDist + dband * BAND_SIZE >= tween) color = bandColor;
                    }
                }

                if (IsBandPlayer(lane, bandNum) && inBandDist >= BAND_SIZE - BAND_THICKNESS) {
                    color = VERY_LIGHT_RED;
                }
            }

            pixels[y*WIDTH + x] = color;
        }
    }

    SDL_UpdateTexture(canvas.get(), NULL, pixels, pitch);

    if (SDL_RenderCopy(ren, canvas.get(), NULL, NULL) < 0) failSDL("SDL_RenderCopy canvas");

    if (!playerAlive) {
        DrawText("YOU DIED", { 255, 255, 255, 255 }, WIDTH / 2, HEIGHT / 2, NULL, NULL, true);
    }

    if (renderAvgDenom > 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Render avg: %.2lf ms", renderAvgTime_ms / renderAvgDenom);
        DrawText(buf, { 255, 255, 255, 255 }, 0, 0, NULL, NULL);
    }

    SDL_RenderPresent(ren);
}

void main_loop()
{
    update();

    // Delta time for animation
    Uint32 now_ms = SDL_GetTicks();
    timeSinceAdvance_ms += now_ms - prevFrame_ms;
    prevFrame_ms = now_ms;

    // Render
    Uint32 start_ms = SDL_GetTicks();
    render();
    Uint32 end_ms = SDL_GetTicks();

    renderAvgTime_ms = renderAvg_decay * renderAvgTime_ms + (1-renderAvg_decay) * (end_ms - start_ms);
    renderAvgDenom = renderAvg_decay * renderAvgDenom + (1-renderAvg_decay);
}

int main(int argc, char *argv[])
{
    std::atexit(cleanup);
    std::srand(static_cast<unsigned>(std::time(0)));
    std::random_device rd;
    rng.seed(rd());

    if (SDL_Init(SDL_INIT_VIDEO) < 0) failSDL("SDL_Init");
    if (TTF_Init() == -1) failTTF("TTF_Init");

    int flags = IMG_INIT_PNG;
    if ((IMG_Init(flags) & flags) != flags) failIMG("IMG_Init");

    font = TTF_OpenFont("data/Vera.ttf", FONT_HEIGHT);
    if (!font) failTTF("TTF_OpenFont");

    win = SDL_CreateWindow("Discrete Hexagon", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH, HEIGHT, SDL_WINDOW_SHOWN);
    if (!win) failSDL("SDL_CreateWindow");

    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) failSDL("SDL_CreateRenderer");

    auto format = SDL_PIXELFORMAT_RGBA8888;
    canvas.reset(SDL_CreateTexture(ren, format, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT));
    if (!canvas) failSDL("SDL_CreateTexture canvas");

    pixels = new uint32_t[HEIGHT * WIDTH];
    pitch = SDL_BYTESPERPIXEL(format) * WIDTH;

    Restart();

    prevFrame_ms = SDL_GetTicks();

    renderAvgTime_ms = 0;
    renderAvgDenom = 0;

    quitRequested = false;

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(main_loop, 0, 0);
#else
    while (!quitRequested) {
        main_loop();
    }
#endif

    return 0;
}
