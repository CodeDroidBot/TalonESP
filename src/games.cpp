#include "games.h"
#include "display.h"
#include "buttons.h"
#include <string.h>

// ============================== RGB helper ==============================

// Classic "color wheel" — pos 0-255 sweeps red -> green -> blue -> red.
static uint16_t rgbWheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85)  return tft.color565(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85;  return tft.color565(0, pos * 3, 255 - pos * 3); }
  pos -= 170;      return tft.color565(pos * 3, 255 - pos * 3, 0);
}

uint16_t rgbNow(uint8_t offset) {
  return rgbWheel((uint8_t)((millis() / 8) + offset));
}

// ============================== Snake ==============================

static const int SNAKE_CELL   = 10;
static const int TOP_Y  = 24;
static const int MAX_LEN = 300;
static const unsigned long SNAKE_TICK_MS = 150;

static int cols, rows;
static int16_t snakeX[MAX_LEN];
static int16_t snakeY[MAX_LEN];
static int snakeLen;
static int dirX, dirY;
static int pendingDirX, pendingDirY;
static int foodX, foodY;
static bool snake_gameOver;
static int snake_score;
static unsigned long snake_lastTick;

static void snakeDrawCell(int gx, int gy, uint16_t color) {
  tft.fillRect(gx * SNAKE_CELL, TOP_Y + gy * SNAKE_CELL, SNAKE_CELL - 1, SNAKE_CELL - 1, color);
}

static void placeFood() {
  while (true) {
    int fx = random(0, cols);
    int fy = random(0, rows);
    bool onSnake = false;
    for (int i = 0; i < snakeLen; i++) {
      if (snakeX[i] == fx && snakeY[i] == fy) { onSnake = true; break; }
    }
    if (!onSnake) { foodX = fx; foodY = fy; return; }
  }
}

static void drawScoreLine() {
  tft.fillRect(0, 0, tft.width(), TOP_Y - 2, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(1);
  tft.setCursor(4, 6);
  tft.print("SNAKE  score:" + String(snake_score));
}

// [MOD] Snake game-over overlay (like Tetris)
static void snakeDrawGameOver() {
  int w = min(160, tft.width() - 20);
  int h = 70;
  int x = (tft.width() - w) / 2;
  int y = (TOP_Y + (rows * SNAKE_CELL - h) / 2);

  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.drawRect(x, y, w, h, TFT_RED);
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_RED);

  tft.setTextSize(1);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setCursor(x + (w - 60) / 2, y + 14);
  tft.print("GAME OVER");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(x + (w - 66) / 2, y + 36);
  tft.print("Score: ");
  tft.print(snake_score);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(x + (w - 84) / 2, y + 52);
  tft.print("BACK to exit");
}

void snakeInit() {
  cols = tft.width() / SNAKE_CELL;
  rows = (tft.height() - TOP_Y) / SNAKE_CELL;

  snakeLen = 3;
  int startX = cols / 2, startY = rows / 2;
  for (int i = 0; i < snakeLen; i++) { snakeX[i] = startX - i; snakeY[i] = startY; }
  dirX = 1; dirY = 0;
  pendingDirX = 1; pendingDirY = 0;
  snake_gameOver = false;
  snake_score = 0;
  snake_lastTick = millis();

  tft.fillScreen(TFT_BLACK);
  drawScoreLine();
  for (int i = 0; i < snakeLen; i++) snakeDrawCell(snakeX[i], snakeY[i], rgbNow(i * 12));

  randomSeed(micros());
  placeFood();
  snakeDrawCell(foodX, foodY, TFT_WHITE);
}

void snakeHandleInput(int evt) {
  if (snake_gameOver) return;
  if (evt == EVT_UP    && dirY == 0) { pendingDirX = 0;  pendingDirY = -1; }
  if (evt == EVT_DOWN  && dirY == 0) { pendingDirX = 0;  pendingDirY = 1;  }
  if (evt == EVT_LEFT  && dirX == 0) { pendingDirX = -1; pendingDirY = 0;  }
  if (evt == EVT_RIGHT && dirX == 0) { pendingDirX = 1;  pendingDirY = 0;  }
}

void snakeUpdate() {
  if (snake_gameOver) return;
  unsigned long now = millis();
  if (now - snake_lastTick < SNAKE_TICK_MS) return;
  snake_lastTick = now;

  dirX = pendingDirX;
  dirY = pendingDirY;

  int newX = snakeX[0] + dirX;
  int newY = snakeY[0] + dirY;

  if (newX < 0) newX = cols - 1;
  if (newX >= cols) newX = 0;
  if (newY < 0) newY = rows - 1;
  if (newY >= rows) newY = 0;

  for (int i = 0; i < snakeLen; i++) {
    if (snakeX[i] == newX && snakeY[i] == newY) {
      snake_gameOver = true;
      // [MOD] call overlay instead of just printing on header
      snakeDrawGameOver();
      return;
    }
  }

  bool ate = (newX == foodX && newY == foodY);

  if (!ate) {
    int tailX = snakeX[snakeLen - 1];
    int tailY = snakeY[snakeLen - 1];
    snakeDrawCell(tailX, tailY, TFT_BLACK);
    for (int i = snakeLen - 1; i > 0; i--) {
      snakeX[i] = snakeX[i - 1];
      snakeY[i] = snakeY[i - 1];
    }
  } else {
    if (snakeLen < MAX_LEN) {
      for (int i = snakeLen; i > 0; i--) {
        snakeX[i] = snakeX[i - 1];
        snakeY[i] = snakeY[i - 1];
      }
      snakeLen++;
    }
    snake_score += 10;
    drawScoreLine();
    placeFood();
    snakeDrawCell(foodX, foodY, TFT_WHITE);
  }

  snakeX[0] = newX;
  snakeY[0] = newY;
  snakeDrawCell(snakeX[0], snakeY[0], rgbNow(0));
}

bool snakeIsGameOver() { return snake_gameOver; }
int  snakeGetScore()   { return snake_score; }

// ============================== Tetris ==============================

static const int COLS = 10;
static const int TETRIS_MAX_ROWS = 30;
static const int BOARD_X = 4;
static const int SIDE_PANEL_MIN_W = 84;
static const int BOARD_BOTTOM_MARGIN = 4;
static const unsigned long TETRIS_TICK_MS_BASE = 500;

static int tetris_cell;
static int tetrisRows;
static int tetris_boardY;

static const uint8_t PIECES[7][4] = {
  {0, 15, 0, 0},
  {0, 6,  6, 0},
  {4, 14, 0, 0},
  {6, 12, 0, 0},
  {12, 6, 0, 0},
  {8, 14, 0, 0},
  {2, 14, 0, 0},
};

static const uint16_t PIECE_COLORS[7] = {
  TFT_CYAN, TFT_YELLOW, TFT_MAGENTA, TFT_GREEN, TFT_RED, TFT_BLUE, TFT_ORANGE
};

static uint8_t board[TETRIS_MAX_ROWS][COLS];
static uint16_t boardColor[TETRIS_MAX_ROWS][COLS];
static uint8_t curShape[4];
static int curType, nextType;
static int px, py;
static bool tetris_gameOver;
static int tetris_score;
static int tetris_lines;
static int tetris_level;
static unsigned long tetris_lastTick;

static bool cellSet(const uint8_t shape[4], int c, int r) {
  return (shape[r] >> c) & 1;
}

static bool collides(const uint8_t shape[4], int testPx, int testPy) {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!cellSet(shape, c, r)) continue;
      int bx = testPx + c;
      int by = testPy + r;
      if (bx < 0 || bx >= COLS || by >= tetrisRows) return true;
      if (by >= 0 && board[by][bx]) return true;
    }
  }
  return false;
}

static void spawnPiece() {
  curType = nextType;
  nextType = random(0, 7);
  memcpy(curShape, PIECES[curType], sizeof(curShape));
  px = (COLS - 4) / 2;
  py = -1;
  if (collides(curShape, px, py)) tetris_gameOver = true;
}

static uint16_t tetrisShade(uint16_t color, int delta) {
  int r = (color >> 11) & 0x1F;
  int g = (color >> 5) & 0x3F;
  int b = color & 0x1F;
  r = constrain(r + delta, 0, 31);
  g = constrain(g + delta * 2, 0, 63);
  b = constrain(b + delta, 0, 31);
  return (r << 11) | (g << 5) | b;
}

static void tetrisDrawCell(int bx, int by, uint16_t color) {
  int x = BOARD_X + bx * tetris_cell;
  int y = tetris_boardY + by * tetris_cell;
  int s = tetris_cell - 1;
  tft.fillRect(x, y, s, s, color);
  uint16_t hi = tetrisShade(color, 7);
  uint16_t lo = tetrisShade(color, -7);
  tft.drawFastHLine(x, y, s, hi);
  tft.drawFastVLine(x, y, s, hi);
  tft.drawFastHLine(x, y + s - 1, s, lo);
  tft.drawFastVLine(x + s - 1, y, s, lo);
}

static void tetrisDrawBoardFrame() {
  int w = COLS * tetris_cell;
  int h = tetrisRows * tetris_cell;
  tft.drawRect(BOARD_X - 2, tetris_boardY - 2, w + 4, h + 4, rgbNow(60));
  tft.drawRect(BOARD_X - 1, tetris_boardY - 1, w + 2, h + 2, rgbNow(60));
}

static void drawBoard() {
  tft.fillRect(BOARD_X, tetris_boardY, COLS * tetris_cell, tetrisRows * tetris_cell, displayColorBg());
  for (int r = 0; r < tetrisRows; r++)
    for (int c = 0; c < COLS; c++)
      if (board[r][c]) tetrisDrawCell(c, r, boardColor[r][c]);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!cellSet(curShape, c, r)) continue;
      int by = py + r;
      if (by >= 0) tetrisDrawCell(px + c, by, PIECE_COLORS[curType]);
    }
  }
  tetrisDrawBoardFrame();
}

static void tetrisDrawPreview(int x, int y, int boxSize) {
  tft.fillRect(x, y, boxSize, boxSize, displayColorBg());
  tft.drawRect(x, y, boxSize, boxSize, TFT_DARKGREY);

  const uint8_t* shape = PIECES[nextType];
  int minC = 4, maxC = -1, minR = 4, maxR = -1;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (cellSet(shape, c, r)) {
        minC = min(minC, c); maxC = max(maxC, c);
        minR = min(minR, r); maxR = max(maxR, r);
      }
  int shapeW = maxC - minC + 1, shapeH = maxR - minR + 1;
  int cell = min((boxSize - 6) / max(shapeW, 1), (boxSize - 6) / max(shapeH, 1));
  cell = constrain(cell, 4, 14);
  int offX = x + (boxSize - shapeW * cell) / 2;
  int offY = y + (boxSize - shapeH * cell) / 2;

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!cellSet(shape, c, r)) continue;
      int bx = offX + (c - minC) * cell;
      int by = offY + (r - minR) * cell;
      tft.fillRect(bx, by, cell - 1, cell - 1, PIECE_COLORS[nextType]);
    }
  }
}

static void drawSidePanel() {
  int x = BOARD_X + COLS * tetris_cell + 10;
  int w = tft.width() - x - 4;
  if (w < 40) return;

  tft.fillRect(x, tetris_boardY, w, tetrisRows * tetris_cell, displayColorBg());
  tft.setTextSize(1);

  int y = tetris_boardY;
  tft.setTextColor(TFT_DARKGREY, displayColorBg());
  tft.setCursor(x, y); tft.print("SCORE");
  tft.setTextColor(rgbNow(0), displayColorBg());
  tft.setCursor(x, y + 10); tft.print(tetris_score);

  y += 28;
  tft.setTextColor(TFT_DARKGREY, displayColorBg());
  tft.setCursor(x, y); tft.print("LINES");
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setCursor(x, y + 10); tft.print(tetris_lines);

  y += 28;
  tft.setTextColor(TFT_DARKGREY, displayColorBg());
  tft.setCursor(x, y); tft.print("LEVEL");
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setCursor(x, y + 10); tft.print(tetris_level);

  y += 28;
  tft.setTextColor(TFT_DARKGREY, displayColorBg());
  tft.setCursor(x, y); tft.print("NEXT");
  int previewSize = min(w - 4, 44);
  tetrisDrawPreview(x, y + 12, previewSize);
}

static void redrawHeader() {
  displayDrawHeaderBar((String("TETRIS  Score: ") + tetris_score).c_str());
}

static unsigned long tetrisTickInterval() {
  long ms = (long)TETRIS_TICK_MS_BASE - (tetris_level - 1) * 35;
  return (unsigned long)max(100L, ms);
}

static void lockPiece() {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!cellSet(curShape, c, r)) continue;
      int bx = px + c, by = py + r;
      if (by >= 0 && by < tetrisRows && bx >= 0 && bx < COLS) {
        board[by][bx] = 1;
        boardColor[by][bx] = PIECE_COLORS[curType];
      }
    }
  }

  int cleared = 0;
  for (int r = tetrisRows - 1; r >= 0; r--) {
    bool full = true;
    for (int c = 0; c < COLS; c++) {
      if (!board[r][c]) { full = false; break; }
    }
    if (full) {
      cleared++;
      for (int rr = r; rr > 0; rr--) {
        memcpy(board[rr], board[rr - 1], COLS);
        memcpy(boardColor[rr], boardColor[rr - 1], COLS * sizeof(uint16_t));
      }
      memset(board[0], 0, COLS);
      r++;
    }
  }
  if (cleared > 0) {
    tetris_score += cleared * 100 * (cleared > 1 ? cleared : 1);
    tetris_lines += cleared;
    tetris_level = 1 + tetris_lines / 10;
  }

  spawnPiece();
}

static void tetrisGameOverOverlay() {
  int w = min(160, tft.width() - 20);
  int h = 70;
  int x = (tft.width() - w) / 2;
  int y = tetris_boardY + (tetrisRows * tetris_cell - h) / 2;

  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.drawRect(x, y, w, h, TFT_RED);
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_RED);
  tft.setTextSize(1);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setCursor(x + (w - 60) / 2, y + 14);
  tft.print("GAME OVER");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(x + (w - 66) / 2, y + 36);
  tft.print("Score: ");
  tft.print(tetris_score);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(x + (w - 84) / 2, y + 52);
  tft.print("BACK to exit");
}

void tetrisInit() {
  tetris_boardY = displayHeaderHeight() + 4;

  int availW = tft.width() - BOARD_X - SIDE_PANEL_MIN_W - 8;
  int availH = tft.height() - tetris_boardY - BOARD_BOTTOM_MARGIN;
  int cellFromW = availW / COLS;
  int cellFromH = availH / 16;
  tetris_cell = constrain(min(cellFromW, cellFromH), 8, 16);

  tetrisRows = availH / tetris_cell;
  tetrisRows = constrain(tetrisRows, 12, TETRIS_MAX_ROWS);

  memset(board, 0, sizeof(board));
  memset(boardColor, 0, sizeof(boardColor));
  tetris_gameOver = false;
  tetris_score = 0;
  tetris_lines = 0;
  tetris_level = 1;
  randomSeed(micros());
  nextType = random(0, 7);
  spawnPiece();
  tetris_lastTick = millis();

  tft.fillScreen(displayColorBg());
  redrawHeader();
  drawBoard();
  drawSidePanel();
}

void tetrisHandleInput(int evt) {
  if (tetris_gameOver) return;
  bool moved = false;

  if (evt == EVT_LEFT  && !collides(curShape, px - 1, py)) { px--; moved = true; }
  if (evt == EVT_RIGHT && !collides(curShape, px + 1, py)) { px++; moved = true; }

  if (evt == EVT_DOWN) {
    if (!collides(curShape, px, py + 1)) { py++; }
    else { lockPiece(); }
    moved = true;
  }

  if (evt == EVT_UP) {
    uint8_t rotated[4] = {0, 0, 0, 0};
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++)
        if (cellSet(curShape, c, r)) rotated[c] |= (1 << (3 - r));
    if (!collides(rotated, px, py)) {
      memcpy(curShape, rotated, sizeof(rotated));
      moved = true;
    }
  }

  if (evt == EVT_OK) {
    while (!collides(curShape, px, py + 1)) py++;
    lockPiece();
    moved = true;
  }

  if (tetris_gameOver) {
    redrawHeader();
    drawBoard();
    tetrisGameOverOverlay();
  } else if (moved) {
    redrawHeader();
    drawBoard();
    drawSidePanel();
  }
}

void tetrisUpdate() {
  if (tetris_gameOver) return;
  unsigned long now = millis();
  if (now - tetris_lastTick < tetrisTickInterval()) return;
  tetris_lastTick = now;

  if (!collides(curShape, px, py + 1)) {
    py++;
  } else {
    lockPiece();
  }

  redrawHeader();
  if (tetris_gameOver) {
    drawBoard();
    tetrisGameOverOverlay();
    return;
  }

  drawBoard();
  drawSidePanel();
}

bool tetrisIsGameOver() { return tetris_gameOver; }
int  tetrisGetScore()   { return tetris_score; }

// ============================== Pong ==============================

static const int PADDLE_W = 40;
static const int PADDLE_H = 6;
static const int BALL_RADIUS = 8;
static const int PADDLE_STEP = 18;   // [MOD] increased from 12 for faster paddle
static const unsigned long PONG_TICK_MS = 15;

static int topY;
static int paddleX, prevPaddleX;
static int ballX, ballY, prevBallX, prevBallY;
static int ballVX, ballVY;
static int lives;
static int pong_score;
static bool pong_gameOver;
static unsigned long pong_lastTick;

static int fieldW() { return tft.width(); }
static int fieldH() { return tft.height(); }
static int paddleY() { return fieldH() - PADDLE_H - 2; }

static void drawHeader() {
  displayDrawHeaderBar((String("PONG s:") + pong_score + " lives:" + lives).c_str());
}

// [MOD] Pong game-over overlay
static void pongDrawGameOver() {
  int w = min(160, tft.width() - 20);
  int h = 70;
  int x = (tft.width() - w) / 2;
  int y = (tft.height() - h) / 2;

  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.drawRect(x, y, w, h, TFT_RED);
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_RED);

  tft.setTextSize(1);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setCursor(x + (w - 60) / 2, y + 14);
  tft.print("GAME OVER");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(x + (w - 66) / 2, y + 36);
  tft.print("Score: ");
  tft.print(pong_score);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(x + (w - 84) / 2, y + 52);
  tft.print("BACK to exit");
}

void pongInit() {
  topY = displayHeaderHeight() + 4;
  paddleX = (fieldW() - PADDLE_W) / 2;
  ballX = fieldW() / 2;
  ballY = topY + 20;
  ballVX = 3;
  ballVY = 3;
  lives = 3;
  pong_score = 0;
  pong_gameOver = false;
  pong_lastTick = millis();

  tft.fillScreen(displayColorBg());
  drawHeader();
  tft.fillRect(paddleX, paddleY(), PADDLE_W, PADDLE_H, displayColorFg());
  tft.fillCircle(ballX, ballY, BALL_RADIUS, rgbNow(0));
  prevPaddleX = paddleX;
  prevBallX = ballX;
  prevBallY = ballY;
}

void pongHandleInput(int evt) {
  if (pong_gameOver) return;
  if (evt == EVT_LEFT)  paddleX -= PADDLE_STEP;
  if (evt == EVT_RIGHT) paddleX += PADDLE_STEP;
  if (paddleX < 0) paddleX = 0;
  if (paddleX > fieldW() - PADDLE_W) paddleX = fieldW() - PADDLE_W;
}

void pongUpdate() {
  if (pong_gameOver) return;
  unsigned long now = millis();
  if (now - pong_lastTick < PONG_TICK_MS) return;
  pong_lastTick = now;

  tft.fillRect(prevPaddleX, paddleY(), PADDLE_W, PADDLE_H, displayColorBg());
  tft.fillCircle(prevBallX, prevBallY, BALL_RADIUS, displayColorBg());

  ballX += ballVX;
  ballY += ballVY;

  if (ballX - BALL_RADIUS <= 0 || ballX + BALL_RADIUS >= fieldW()) ballVX = -ballVX;
  if (ballY - BALL_RADIUS <= topY) ballVY = -ballVY;

  bool hitPaddle = (ballY + BALL_RADIUS >= paddleY()) && (ballY + BALL_RADIUS <= paddleY() + PADDLE_H) &&
                   (ballX + BALL_RADIUS >= paddleX) && (ballX - BALL_RADIUS <= paddleX + PADDLE_W) && (ballVY > 0);
  if (hitPaddle) {
    ballVY = -ballVY;
    pong_score += 1;
  }

  if (ballY - BALL_RADIUS > fieldH()) {
    lives--;
    if (lives <= 0) {
      pong_gameOver = true;
      pongDrawGameOver();          // [MOD] show overlay
    } else {
      ballX = fieldW() / 2;
      ballY = topY + 20;
      ballVY = 3;
    }
  }

  uint16_t paddleColor = (lives >= 3) ? TFT_GREEN : (lives == 2) ? TFT_YELLOW : TFT_RED;
  tft.fillRect(paddleX, paddleY(), PADDLE_W, PADDLE_H, paddleColor);
  tft.fillCircle(ballX, ballY, BALL_RADIUS, rgbNow(0));
  prevPaddleX = paddleX;
  prevBallX = ballX;
  prevBallY = ballY;

  drawHeader();
  // [MOD] removed old header "OVER" text (now using overlay)
}

bool pongIsGameOver() { return pong_gameOver; }
int  pongGetScore()   { return pong_score; }

// ============================== Jumper ==============================

static const int JUMP_TICK_MS_BASE = 20;
static const int JUMP_PLAYER_X = 28;
static const int JUMP_PLAYER_SIZE = 14;
static const int JUMP_GRAVITY = 1;
static const int JUMP_VELOCITY = -11;
static const int JUMP_NUM_OBSTACLES = 3;
static const int JUMP_OBSTACLE_W = 12;
static const int JUMP_MIN_GAP = 90;
static const int JUMP_MAX_GAP = 160;

static int jump_groundY;
static int jump_playerY, jump_velY;
static bool jump_onGround;
static int jump_obstacleX[JUMP_NUM_OBSTACLES];
static int jump_obstacleH[JUMP_NUM_OBSTACLES];
static bool jump_gameOver;
static int jump_score;
static unsigned long jump_lastTick;
static unsigned long jump_startedAt;

static void jumperResetObstacle(int i, int fromX) {
  jump_obstacleX[i] = fromX + JUMP_MIN_GAP + random(0, JUMP_MAX_GAP - JUMP_MIN_GAP);
  jump_obstacleH[i] = 14 + random(0, 22);
}

// [MOD] Jumper game-over overlay
static void jumperDrawGameOver() {
  int w = min(160, tft.width() - 20);
  int h = 70;
  int x = (tft.width() - w) / 2;
  int y = (tft.height() - h) / 2;

  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.drawRect(x, y, w, h, TFT_RED);
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_RED);

  tft.setTextSize(1);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setCursor(x + (w - 60) / 2, y + 14);
  tft.print("GAME OVER");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(x + (w - 66) / 2, y + 36);
  tft.print("Score: ");
  tft.print(jump_score);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(x + (w - 84) / 2, y + 52);
  tft.print("BACK to exit");
}

static void jumperDraw() {
  tft.fillRect(0, displayHeaderHeight(), tft.width(), tft.height() - displayHeaderHeight(), displayColorBg());
  tft.drawFastHLine(0, jump_groundY + JUMP_PLAYER_SIZE, tft.width(), displayColorFg());

  int py = jump_playerY;
  tft.fillRect(JUMP_PLAYER_X, py, JUMP_PLAYER_SIZE, JUMP_PLAYER_SIZE, rgbNow(0));

  for (int i = 0; i < JUMP_NUM_OBSTACLES; i++) {
    int h = jump_obstacleH[i];
    int oy = jump_groundY + JUMP_PLAYER_SIZE - h;
    tft.fillRect(jump_obstacleX[i], oy, JUMP_OBSTACLE_W, h, rgbNow(80 + i * 40));
  }

  displayDrawHeaderBar((String("JUMPER score:") + jump_score).c_str());
}

void jumperInit() {
  jump_groundY = tft.height() - 20;
  jump_playerY = jump_groundY;
  jump_velY = 0;
  jump_onGround = true;
  jump_gameOver = false;
  jump_score = 0;
  jump_lastTick = millis();
  jump_startedAt = millis();

  randomSeed(micros());
  int fromX = tft.width();
  for (int i = 0; i < JUMP_NUM_OBSTACLES; i++) {
    jumperResetObstacle(i, fromX);
    fromX = jump_obstacleX[i];
  }

  tft.fillScreen(displayColorBg());
  jumperDraw();
}

void jumperHandleInput(int evt) {
  if (jump_gameOver) return;
  if ((evt == EVT_UP || evt == EVT_OK) && jump_onGround) {
    jump_velY = JUMP_VELOCITY;
    jump_onGround = false;
  }
}

void jumperUpdate() {
  if (jump_gameOver) return;
  unsigned long now = millis();
  if (now - jump_lastTick < (unsigned long)JUMP_TICK_MS_BASE) return;
  jump_lastTick = now;

  int elapsedSec = (int)((now - jump_startedAt) / 1000);
  int scrollSpeed = 4 + min(elapsedSec / 4, 6);

  jump_velY += JUMP_GRAVITY;
  jump_playerY += jump_velY;
  if (jump_playerY >= jump_groundY) {
    jump_playerY = jump_groundY;
    jump_velY = 0;
    jump_onGround = true;
  }

  for (int i = 0; i < JUMP_NUM_OBSTACLES; i++) {
    jump_obstacleX[i] -= scrollSpeed;
    if (jump_obstacleX[i] + JUMP_OBSTACLE_W < 0) {
      int rightmost = jump_obstacleX[0];
      for (int j = 1; j < JUMP_NUM_OBSTACLES; j++) rightmost = max(rightmost, jump_obstacleX[j]);
      jumperResetObstacle(i, max(rightmost, (int)tft.width()));
      jump_score += 5;
    }

    int h = jump_obstacleH[i];
    int oy = jump_groundY + JUMP_PLAYER_SIZE - h;
    bool overlapX = (JUMP_PLAYER_X + JUMP_PLAYER_SIZE > jump_obstacleX[i]) &&
                    (JUMP_PLAYER_X < jump_obstacleX[i] + JUMP_OBSTACLE_W);
    bool overlapY = (jump_playerY + JUMP_PLAYER_SIZE > oy);
    if (overlapX && overlapY) {
      jump_gameOver = true;
      jumperDrawGameOver();        // [MOD] show overlay
    }
  }

  jumperDraw();
  // [MOD] removed old header "OVER" text
}

bool jumperIsGameOver() { return jump_gameOver; }
int  jumperGetScore()   { return jump_score; }

// ============================== Duel ==============================

static const int DUEL_TICK_MS = 30;
static const int DUEL_MOVE_STEP = 3;
static const int DUEL_HIT_RANGE = 26;
static const int DUEL_FIGHTER_W = 16;
static const int DUEL_FIGHTER_H = 28;
static const int DUEL_PUNCH_DAMAGE = 8;
static const int DUEL_BLOCK_REDUCTION = 5;
static const unsigned long DUEL_PUNCH_COOLDOWN_MS = 350;
static const unsigned long DUEL_AI_THINK_MS = 400;

static int duel_floorY;
static int duel_playerX;
static int duel_enemyX;
static int duel_playerHP, duel_enemyHP;
static bool duel_playerBlocking, duel_enemyBlocking;
static bool duel_gameOver;
static bool duel_playerWon;
static unsigned long duel_lastTick;
static unsigned long duel_lastPlayerPunch;
static unsigned long duel_lastEnemyPunch;
static unsigned long duel_lastAiThink;
static int duel_enemyIntent;

static uint16_t duel_healthColor(int hp, int maxHp) {
  if (hp > maxHp * 2 / 3) return TFT_GREEN;
  if (hp > maxHp / 3)     return TFT_YELLOW;
  return TFT_RED;
}

static void duelDrawHealthBars() {
  const int maxHp = 100;
  const int barW = tft.width() / 2 - 16;
  const int barH = 8;

  tft.fillRect(8, displayHeaderHeight() + 4, barW, barH, TFT_DARKGREY);
  tft.fillRect(8, displayHeaderHeight() + 4, (barW * duel_playerHP) / maxHp, barH,
               duel_healthColor(duel_playerHP, maxHp));

  int rx = tft.width() - 8 - barW;
  tft.fillRect(rx, displayHeaderHeight() + 4, barW, barH, TFT_DARKGREY);
  int enemyFillW = (barW * duel_enemyHP) / maxHp;
  tft.fillRect(rx + (barW - enemyFillW), displayHeaderHeight() + 4, enemyFillW, barH,
               duel_healthColor(duel_enemyHP, maxHp));
}

// [MOD] Duel game-over overlay (shows WIN/LOSE and score)
static void duelDrawGameOver() {
  int w = min(160, tft.width() - 20);
  int h = 70;
  int x = (tft.width() - w) / 2;
  int y = (tft.height() - h) / 2;

  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.drawRect(x, y, w, h, TFT_RED);
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_RED);

  tft.setTextSize(1);
  tft.setTextColor(duel_playerWon ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.setCursor(x + (w - 60) / 2, y + 14);
  tft.print(duel_playerWon ? "YOU WIN!" : "GAME OVER");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(x + (w - 66) / 2, y + 36);
  tft.print("Score: ");
  tft.print(duelGetScore());

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(x + (w - 84) / 2, y + 52);
  tft.print("BACK to exit");
}

static void duelDrawFighters() {
  int y0 = duel_floorY - DUEL_FIGHTER_H;
  tft.fillRect(0, displayHeaderHeight() + 16, tft.width(), tft.height() - displayHeaderHeight() - 16, displayColorBg());
  tft.drawFastHLine(0, duel_floorY, tft.width(), displayColorFg());

  uint16_t playerColor = duel_playerBlocking ? TFT_CYAN : rgbNow(0);
  uint16_t enemyColor  = duel_enemyBlocking  ? TFT_CYAN : rgbNow(140);
  tft.fillRect(duel_playerX, y0, DUEL_FIGHTER_W, DUEL_FIGHTER_H, playerColor);
  tft.fillRect(duel_enemyX,  y0, DUEL_FIGHTER_W, DUEL_FIGHTER_H, enemyColor);

  duelDrawHealthBars();
  displayDrawHeaderBar("DUEL  LEFT/RIGHT move  OK punch  UP block");
}

void duelInit() {
  duel_floorY = tft.height() - 12;
  duel_playerX = 30;
  duel_enemyX = tft.width() - 30 - DUEL_FIGHTER_W;
  duel_playerHP = 100;
  duel_enemyHP = 100;
  duel_playerBlocking = false;
  duel_enemyBlocking = false;
  duel_gameOver = false;
  duel_playerWon = false;
  duel_lastTick = millis();
  duel_lastPlayerPunch = 0;
  duel_lastEnemyPunch = 0;
  duel_lastAiThink = 0;
  duel_enemyIntent = -1;

  randomSeed(micros());
  tft.fillScreen(displayColorBg());
  duelDrawFighters();
}

static int duelGap() { return duel_enemyX - (duel_playerX + DUEL_FIGHTER_W); }

void duelHandleInput(int evt) {
  if (duel_gameOver) return;

  duel_playerBlocking = (evt == EVT_UP);

  if (evt == EVT_LEFT)  duel_playerX -= DUEL_MOVE_STEP * 3;
  if (evt == EVT_RIGHT) duel_playerX += DUEL_MOVE_STEP * 3;
  if (duel_playerX < 4) duel_playerX = 4;
  if (duel_playerX > duel_enemyX - DUEL_FIGHTER_W - 4) duel_playerX = duel_enemyX - DUEL_FIGHTER_W - 4;

  if (evt == EVT_OK) {
    unsigned long now = millis();
    if (now - duel_lastPlayerPunch >= DUEL_PUNCH_COOLDOWN_MS && duelGap() <= DUEL_HIT_RANGE) {
      duel_lastPlayerPunch = now;
      int dmg = duel_enemyBlocking ? max(1, DUEL_PUNCH_DAMAGE - DUEL_BLOCK_REDUCTION) : DUEL_PUNCH_DAMAGE;
      duel_enemyHP = max(0, duel_enemyHP - dmg);
    }
  }
}

static void duelAiThink() {
  int gap = duelGap();
  duel_enemyBlocking = false;

  if (gap > DUEL_HIT_RANGE) {
    duel_enemyIntent = -1;
  } else {
    int roll = random(0, 100);
    if (roll < 55) {
      duel_enemyIntent = 1;
    } else {
      duel_enemyIntent = 0;
      duel_enemyBlocking = true;
    }
  }
}

void duelUpdate() {
  if (duel_gameOver) return;
  unsigned long now = millis();
  if (now - duel_lastTick < (unsigned long)DUEL_TICK_MS) return;
  duel_lastTick = now;

  if (now - duel_lastAiThink >= DUEL_AI_THINK_MS) {
    duel_lastAiThink = now;
    duelAiThink();
  }

  if (duel_enemyIntent == -1) {
    duel_enemyX -= DUEL_MOVE_STEP;
    if (duel_enemyX < duel_playerX + DUEL_FIGHTER_W + 4) duel_enemyX = duel_playerX + DUEL_FIGHTER_W + 4;
  } else if (duel_enemyIntent == 1) {
    if (now - duel_lastEnemyPunch >= DUEL_PUNCH_COOLDOWN_MS && duelGap() <= DUEL_HIT_RANGE) {
      duel_lastEnemyPunch = now;
      int dmg = duel_playerBlocking ? max(1, DUEL_PUNCH_DAMAGE - DUEL_BLOCK_REDUCTION) : DUEL_PUNCH_DAMAGE;
      duel_playerHP = max(0, duel_playerHP - dmg);
    }
  }
  if (duel_enemyX > tft.width() - DUEL_FIGHTER_W - 4) duel_enemyX = tft.width() - DUEL_FIGHTER_W - 4;

  if (duel_playerHP <= 0 || duel_enemyHP <= 0) {
    duel_gameOver = true;
    duel_playerWon = (duel_enemyHP <= 0 && duel_playerHP > 0);
    duelDrawGameOver();          // [MOD] show overlay
  }

  duelDrawFighters();
  // [MOD] removed old WIN/LOSE text on header
}

bool duelIsGameOver() { return duel_gameOver; }
bool duelPlayerWon()  { return duel_playerWon; }
int  duelGetScore()   { return duel_playerWon ? duel_enemyHP + 100 : duel_playerHP; }