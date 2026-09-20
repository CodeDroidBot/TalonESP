#pragma once
#include <Arduino.h>

// ---- Shared RGB helper ----
// Cycles through the color wheel over time; pass a per-object offset
// (0-255) to keep multiple things out of phase with each other.
uint16_t rgbNow(uint8_t offset = 0);

// ---- Snake ----
void snakeInit();
void snakeHandleInput(int evt);
void snakeUpdate();
bool snakeIsGameOver();
int  snakeGetScore();

// ---- Tetris ----
void tetrisInit();
void tetrisHandleInput(int evt); // UP=rotate, DOWN=soft drop, LEFT/RIGHT=move, OK=hard drop
void tetrisUpdate();
bool tetrisIsGameOver();
int  tetrisGetScore();

// ---- Pong (solo breakout-style) ----
void pongInit();
void pongHandleInput(int evt);
void pongUpdate();
bool pongIsGameOver();
int  pongGetScore();

// ---- Jumper ----
// Original side-scrolling endless jumper (not a licensed character or
// likeness of any existing game): obstacles scroll in from the right,
// tap jump to clear them, score increases with distance/time survived.
void jumperInit();
void jumperHandleInput(int evt); // UP or OK = jump
void jumperUpdate();
bool jumperIsGameOver();
int  jumperGetScore();

// ---- Duel ----
// Simple two-fighter versus-AI game: move to close distance, punch to
// deal damage, block to reduce incoming damage. First fighter to zero
// HP loses. Not modeled on any specific existing fighting game.
void duelInit();
void duelHandleInput(int evt); // LEFT/RIGHT=move, OK=punch, UP=block
void duelUpdate();
bool duelIsGameOver();
bool duelPlayerWon();
int  duelGetScore();