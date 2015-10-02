
/*----------------------------------------------------------------------+
 |                                                                      |
 |      moves.c -- move generator and move making                       |
 |                                                                      |
 +----------------------------------------------------------------------*/

/*
 *  Copyright (C) 1998-2015, Marcel van Kervinck
 *  All rights reserved
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *  notice, this list of conditions and the following disclaimer in the
 *  documentation and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/*----------------------------------------------------------------------+
 |      Includes                                                        |
 +----------------------------------------------------------------------*/

// C standard
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// C extension
#include "cplus.h"

// Own interface
#include "Board.h"

// Other modules
#include "zobrist.h"

/*----------------------------------------------------------------------+
 |      Definitions                                                     |
 +----------------------------------------------------------------------*/

/*
 *  Move and attack directions
 */

enum {
        stepN = a2 - a1,          stepE = b1 - a1,
        stepS = -stepN,           stepW = -stepE,
        stepNE = stepN + stepE,   stepSE = stepS + stepE,
        stepSW = stepS + stepW,   stepNW = stepN + stepW,
        jumpNNE = stepN + stepNE, jumpENE = stepE + stepNE,
        jumpESE = stepE + stepSE, jumpSSE = stepS + stepSE,
        jumpSSW = stepS + stepSW, jumpWSW = stepW + stepSW,
        jumpWNW = stepW + stepNW, jumpNNW = stepN + stepNW,
};

enum stepBit { bitN, bitNE, bitE, bitSE, bitS, bitSW, bitW, bitNW };
enum jumpBit { bitNNE, bitENE, bitESE, bitSSE, bitSSW, bitWSW, bitWNW, bitNNW };

enum {
        dirsRook   = (1 << bitN)  | (1 << bitE)  | (1 << bitS)  | (1 << bitW),
        dirsBishop = (1 << bitNE) | (1 << bitSE) | (1 << bitSW) | (1 << bitNW),
        dirsQueen  = dirsRook | dirsBishop
};

// Off-board offsets for use in the undo stack
#define offsetof_castleFlags   offsetof(struct board, castleFlags)
#define offsetof_enPassantPawn offsetof(struct board, enPassantPawn)
#define offsetof_halfmoveClock offsetof(struct board, halfmoveClock)

/*----------------------------------------------------------------------+
 |      Data                                                            |
 +----------------------------------------------------------------------*/

/*
 *  Which castle bits to clear for a move's from and to
 */
static const char castleFlagsClear[boardSize] = {
        [a8] = castleFlagBlackQside,
        [e8] = castleFlagBlackKside | castleFlagBlackQside,
        [h8] = castleFlagBlackKside,
        [a1] = castleFlagWhiteQside,
        [e1] = castleFlagWhiteKside | castleFlagWhiteQside,
        [h1] = castleFlagWhiteKside,
};

static const signed char kingStep[] = { // Offsets for king moves
        [1<<bitN] = stepN, [1<<bitNE] = stepNE,
        [1<<bitE] = stepE, [1<<bitSE] = stepSE,
        [1<<bitS] = stepS, [1<<bitSW] = stepSW,
        [1<<bitW] = stepW, [1<<bitNW] = stepNW,
};

static const signed char knightJump[] = { // Offsets for knight jumps
        [1<<bitNNE] = jumpNNE, [1<<bitENE] = jumpENE,
        [1<<bitESE] = jumpESE, [1<<bitSSE] = jumpSSE,
        [1<<bitSSW] = jumpSSW, [1<<bitWSW] = jumpWSW,
        [1<<bitWNW] = jumpWNW, [1<<bitNNW] = jumpNNW,
};

/*
 *  Some 0x88 preprocessor logic to compute tables for king steps and knight jumps
 */

// Unsigned 0x88 for squares:
#define x88u(square) ((square) + ((square) & ~7))

// Sign-extended 0x88 for vectors:
#define x88s(vector) (x88u(vector) + (((vector) << 1) & 8))

// Move stays inside board?
#define onBoard(square, vector) (((x88u(square) + x88s(vector)) & 0x88) == 0)

// If so, turn it into a flag for in the directions table
#define dir(square, vector, bit) (onBoard(square, vector) << (bit))

// Collect king step flags
#define K(sq) [sq]=(\
        dir(sq,stepNW,bitNW)+  dir(sq,stepN,bitN)+  dir(sq,stepNE,bitNE)+\
                                                                         \
        dir(sq,stepW,bitW)             +              dir(sq,stepE,bitE)+\
                                                                         \
        dir(sq,stepSW,bitSW)+  dir(sq,stepS,bitS)+  dir(sq,stepSE,bitSE))

// Collect knight jump flags
#define N(sq) [sq]=(\
              dir(sq,jumpNNW,bitNNW)   +   dir(sq,jumpNNE,bitNNE)+           \
                                                                             \
  dir(sq,jumpWNW,bitWNW)   +           +           +   dir(sq,jumpENE,bitENE)\
                                                                             \
               +           +           +           +           +             \
                                                                             \
  dir(sq,jumpWSW,bitWSW)   +           +           +   dir(sq,jumpESE,bitESE)\
                                                                             \
             +dir(sq,jumpSSW,bitSSW)   +   dir(sq,jumpSSE,bitSSE))

// 8 bits per square representing which directions a king can step to
static const unsigned char kingDirections[] = {
        K(a1), K(a2), K(a3), K(a4), K(a5), K(a6), K(a7), K(a8),
        K(b1), K(b2), K(b3), K(b4), K(b5), K(b6), K(b7), K(b8),
        K(c1), K(c2), K(c3), K(c4), K(c5), K(c6), K(c7), K(c8),
        K(d1), K(d2), K(d3), K(d4), K(d5), K(d6), K(d7), K(d8),
        K(e1), K(e2), K(e3), K(e4), K(e5), K(e6), K(e7), K(e8),
        K(f1), K(f2), K(f3), K(f4), K(f5), K(f6), K(f7), K(f8),
        K(g1), K(g2), K(g3), K(g4), K(g5), K(g6), K(g7), K(g8),
        K(h1), K(h2), K(h3), K(h4), K(h5), K(h6), K(h7), K(h8),
};

// 8 bits per square representing which directions a knight can jump to
static const unsigned char knightDirections[] = {
        N(a1), N(a2), N(a3), N(a4), N(a5), N(a6), N(a7), N(a8),
        N(b1), N(b2), N(b3), N(b4), N(b5), N(b6), N(b7), N(b8),
        N(c1), N(c2), N(c3), N(c4), N(c5), N(c6), N(c7), N(c8),
        N(d1), N(d2), N(d3), N(d4), N(d5), N(d6), N(d7), N(d8),
        N(e1), N(e2), N(e3), N(e4), N(e5), N(e6), N(e7), N(e8),
        N(f1), N(f2), N(f3), N(f4), N(f5), N(f6), N(f7), N(f8),
        N(g1), N(g2), N(g3), N(g4), N(g5), N(g6), N(g7), N(g8),
        N(h1), N(h2), N(h3), N(h4), N(h5), N(h6), N(h7), N(h8),
};

/*----------------------------------------------------------------------+
 |      Functions                                                       |
 +----------------------------------------------------------------------*/

static uint64_t hashCastleFlags(int flags);
static uint64_t hashEnPassant(int square);

/*----------------------------------------------------------------------+
 |      generateMoves                                                   |
 +----------------------------------------------------------------------*/

// Helper to emit a regular move
static void pushMove(Board_t self, int from, int to)
{
        *self->movePtr++ = move(from, to);
}

// Helper to emit a special move
static void pushSpecialMove(Board_t self, int from, int to)
{
        *self->movePtr++ = specialMove(from, to);
}

// Helper to emit a pawn move
static void pushPawnMove(Board_t self, int from, int to)
{
        if (rank(to) == rank8 || rank(to) == rank1) {
                pushSpecialMove(self, from, to);
                self->movePtr[-1] += queenPromotionFlags;

                pushSpecialMove(self, from, to);
                self->movePtr[-1] += rookPromotionFlags;

                pushSpecialMove(self, from, to);
                self->movePtr[-1] += bishopPromotionFlags;

                pushSpecialMove(self, from, to);
                self->movePtr[-1] += knightPromotionFlags;
        } else
                pushMove(self, from, to); // normal pawn move
}

// Helper to generate slider moves
static void generateSlides(Board_t self, int from, int dirs)
{
        dirs &= kingDirections[from];
        int dir = 0;
        do {
                dir -= dirs; // pick next
                dir &= dirs;
                int vector = kingStep[dir];
                int to = from;
                do {
                        to += vector;
                        if (self->squares[to] != empty) {
                                if (pieceColor(self->squares[to]) != sideToMove(self))
                                        pushMove(self, from, to);
                                break;
                        }
                        pushMove(self, from, to);
                } while (dir & kingDirections[to]);
        } while (dirs -= dir); // remove and go to next
}

/*
 *  Pseudo-legal move generator
 */
extern int generateMoves(Board_t self, int moveList[maxMoves])
{
        updateSideInfo(self);

        self->movePtr = moveList;

        for (int from=0; from<boardSize; from++) {
                int piece = self->squares[from];
                if (piece == empty || pieceColor(piece) != sideToMove(self))
                        continue;

                /*
                 *  Generate moves for this piece
                 */
                int to;
                switch (piece) {
                        int dir, dirs;

                case whiteKing:
                case blackKing:
                        dirs = kingDirections[from];
                        dir = 0;
                        do {
                                dir -= dirs; // pick next
                                dir &= dirs;
                                to = from + kingStep[dir];
                                if (self->squares[to] != empty
                                 && pieceColor(self->squares[to]) == sideToMove(self))
                                        continue;
                                if (self->xside->attacks[to] == 0)
                                        pushMove(self, from, to);
                        } while (dirs -= dir); // remove and go to next
                        break;

                case whiteQueen:
                case blackQueen:
                        generateSlides(self, from, dirsQueen);
                        break;

                case whiteRook:
                case blackRook:
                        generateSlides(self, from, dirsRook);
                        break;

                case whiteBishop:
                case blackBishop:
                        generateSlides(self, from, dirsBishop);
                        break;

                case whiteKnight:
                case blackKnight:
                        dirs = knightDirections[from];
                        dir = 0;
                        do {
                                dir -= dirs; // pick next
                                dir &= dirs;
                                to = from + knightJump[dir];
                                if (self->squares[to] != empty
                                 && pieceColor(self->squares[to]) == sideToMove(self))
                                        continue;
                                pushMove(self, from, to);
                        } while (dirs -= dir); // remove and go to next
                        break;

                case whitePawn:
                        if (file(from) != fileH) {
                                to = from + stepNE;
                                if (self->squares[to] != empty
                                 && pieceColor(self->squares[to]) == black)
                                        pushPawnMove(self, from, to);
                        }
                        if (file(from) != fileA) {
                                to = from + stepNW;
                                if (self->squares[to] != empty
                                 && pieceColor(self->squares[to]) == black)
                                        pushPawnMove(self, from, to);
                        }
                        to = from + stepN;
                        if (self->squares[to] != empty)
                                break;

                        pushPawnMove(self, from, to);
                        if (rank(from) == rank2) {
                                to += stepN;
                                if (self->squares[to] == empty) {
                                        pushMove(self, from, to);
                                        if (self->blackSide.attacks[to+stepS])
                                                self->movePtr[-1] |= specialMoveFlag;
                                }
                        }
                        break;

                case blackPawn:
                        if (file(from) != fileH) {
                                to = from + stepSE;
                                if (self->squares[to] != empty
                                 && pieceColor(self->squares[to]) == white)
                                        pushPawnMove(self, from, to);
                        }
                        if (file(from) != fileA) {
                                to = from + stepSW;
                                if (self->squares[to] != empty
                                 && pieceColor(self->squares[to]) == white)
                                        pushPawnMove(self, from, to);
                        }
                        to = from + stepS;
                        if (self->squares[to] != empty)
                                break;

                        pushPawnMove(self, from, to);
                        if (rank(from) == rank7) {
                                to += stepS;
                                if (self->squares[to] == empty) {
                                        pushMove(self, from, to);
                                        if (self->whiteSide.attacks[to+stepN])
                                                self->movePtr[-1] |= specialMoveFlag;
                                }
                        }
                        break;
                }
        }

        /*
         *  Generate castling moves
         */
        if (self->castleFlags && !inCheck(self)) {
                static const int flags[2][2] = {
                        { castleFlagWhiteKside, castleFlagWhiteQside },
                        { castleFlagBlackKside, castleFlagBlackQside }
                };

                int side = sideToMove(self);
                int sq = self->side->king;

                if ((self->castleFlags & flags[side][0])
                 && self->squares[sq+stepE] == empty
                 && self->squares[sq+2*stepE] == empty
                 && self->xside->attacks[sq+stepE] == 0
                 && self->xside->attacks[sq+2*stepE] == 0)
                        pushSpecialMove(self, sq, sq + 2*stepE);

                if ((self->castleFlags & flags[side][1])
                 && self->squares[sq+stepW] == empty
                 && self->squares[sq+2*stepW] == empty
                 && self->squares[sq+3*stepW] == empty
                 && self->xside->attacks[sq+stepW] == 0
                 && self->xside->attacks[sq+2*stepW] == 0)
                        pushSpecialMove(self, sq, sq + 2*stepW);
        }

        /*
         *  Generate en passant captures
         */
        if (self->enPassantPawn) {
                static const int steps[] = { stepN, stepS };
                static const int pawns[] = { whitePawn, blackPawn };

                int step = steps[sideToMove(self)];
                int pawn = pawns[sideToMove(self)];
                int ep = self->enPassantPawn;

                if (file(ep) != fileA && self->squares[ep+stepW] == pawn)
                        pushSpecialMove(self, ep + stepW, ep + step);

                if (file(ep) != fileH && self->squares[ep+stepE] == pawn)
                        pushSpecialMove(self, ep + stepE, ep + step);
        }

        return self->movePtr - moveList; // nrMoves
}

/*----------------------------------------------------------------------+
 |      make/unmake move                                                |
 +----------------------------------------------------------------------*/

extern void undoMove(Board_t self)
{
        self->halfmoveClock--;
        self->plyNumber--;
        self->hash = popList(self->hashHistory);

        assert(self->undoLen > 0);
        signed char *bytes = (signed char *)self;
        int len = self->undoLen;
        for (;;) {
                int offset = self->undoStack[--len];
                if (offset < 0) break; // sentinel
                bytes[offset] = self->undoStack[--len];
        }
        self->undoLen = len;

        if (self->plyNumber < self->sideInfoPlyNumber)
                self->sideInfoPlyNumber = -1; // side info is invalid now
}

extern void makeMove(Board_t self, int move)
{
        signed char *sp = &self->undoStack[self->undoLen];

        //char moveString[maxMoveSize];
        //moveToLongAlgebraic(self, moveString, move);
        //puts(moveString);

        #define push(offset, value) _Statement(                         \
                *sp++ = (value);                                        \
                *sp++ = (offset);                                       \
        )

        #define pg(s) (((s >> 3) | (s << 3)) & 63) // TODO: find a more elegant way

        #define makeSimpleMove(from, to) _Statement(                    \
                int _piece = self->squares[from];                       \
                int _victim = self->squares[to];                        \
                                                                        \
                /* Update the undo stack */                             \
                push(from, _piece);                                     \
                push(to, _victim);                                      \
                                                                        \
                /* Make the simple move */                              \
                self->squares[to] = _piece;                             \
                self->squares[from] = empty;                            \
                                                                        \
                /* Update the incremental hash */                       \
                self->hash ^= zobristPiece[_piece][pg(from)]            \
                            ^ zobristPiece[_piece][pg(to)]              \
                            ^ zobristPiece[_victim][pg(to)];            \
        )

        *sp++ = -1; // sentinel

        pushList(self->hashHistory, self->hash);

        // Always clear en passant info first
        if (self->enPassantPawn) {
                push(offsetof_enPassantPawn, self->enPassantPawn);
                self->hash ^= hashEnPassant(self->enPassantPawn);
                self->enPassantPawn = 0;
        }

        int to = to(move), from = from(move);

        // Handle specials first
        if (move & specialMoveFlag) {
                switch (rank(from)) {
                case rank8:
                        // Black castles. Insert the corresponding rook move
                        if (to == g8)
                                makeSimpleMove(h8, f8);
                        else
                                makeSimpleMove(a8, d8);
                        break;

                case rank7:
                        if (self->squares[from] == blackPawn) { // Set en passant flag
                                push(offsetof_enPassantPawn, 0);
                                self->enPassantPawn = to;
                                self->hash ^= hashEnPassant(to);
                        } else {
                                push(from, self->squares[from]); // White promotes
                                int promoPiece = whiteQueen + (move >> promotionBits);
                                self->squares[from] = promoPiece;
                                self->hash ^= zobristPiece[whitePawn][pg(from)]
                                            ^ zobristPiece[promoPiece][pg(from)];
                        }
                        break;

                case rank5: // White captures en passant
                case rank4: // Black captures en passant
                        ;
                        int square = square(file(to), rank(from));
                        int victim = self->squares[square];
                        push(square, victim);
                        self->squares[square] = empty;
                        self->hash ^= zobristPiece[victim][pg(square)];
                        break;

                case rank2:
                        if (self->squares[from] == whitePawn) { // Set en passant flag
                                push(offsetof_enPassantPawn, 0);
                                self->enPassantPawn = to;
                                self->hash ^= hashEnPassant(to);
                        } else {
                                push(from, self->squares[from]); // Black promotes
                                int promoPiece = blackQueen + (move >> promotionBits);
                                self->squares[from] = promoPiece;
                                self->hash ^= zobristPiece[blackPawn][pg(from)]
                                            ^ zobristPiece[promoPiece][pg(from)];
                        }
                        break;

                case rank1:
                        // White castles. Insert the corresponding rook move
                        if (to == g1)
                                makeSimpleMove(h1, f1);
                        else
                                makeSimpleMove(a1, d1);
                        break;

                default:
                        break;
                }
        }

        self->plyNumber++;
        self->hash ^= zobristTurn[0];

        if (self->squares[to] != empty
         || self->squares[from] == whitePawn
         || self->squares[from] == blackPawn) {
                push(offsetof_halfmoveClock, self->halfmoveClock);
                self->halfmoveClock = 0;
        } else
                self->halfmoveClock++;
        assert(self->halfmoveClock >= 0);

        makeSimpleMove(from, to);

        int flagsToClear = (castleFlagsClear[from] | castleFlagsClear[to]) & self->castleFlags;
        if (flagsToClear) {
                push(offsetof_castleFlags, self->castleFlags);
                self->castleFlags ^= flagsToClear;
                self->hash ^= hashCastleFlags(flagsToClear);
        }

        self->undoLen = sp - self->undoStack;

        // Finalize en passant (only safe after self->undoLen update)
        if (self->enPassantPawn)
                normalizeEnPassantStatus(self);
}

/*----------------------------------------------------------------------+
 |      updateSideInfo                                                  |
 +----------------------------------------------------------------------*/

// Helper to update slider attacks
static void updateSliderAttacks(Board_t self, int from, int dirs, struct side *side, int attackValue)
{
        dirs &= kingDirections[from];
        int dir = 0;
        do {
                dir -= dirs; // pick next
                dir &= dirs;
                int to = from;
                do {
                        to += kingStep[dir];
                        side->attacks[to] += attackValue;
                        if (self->squares[to] != empty) break;
                } while (dir & kingDirections[to]);
        } while (dirs -= dir); // remove and go to next
}

extern void updateSideInfo(Board_t self)
{
        if (self->sideInfoPlyNumber == self->plyNumber)
                return;

        memset(&self->whiteSide, 0, sizeof self->whiteSide);
        memset(&self->blackSide, 0, sizeof self->blackSide);

        self->side  = (sideToMove(self) == white) ? &self->whiteSide : &self->blackSide;
        self->xside = (sideToMove(self) == white) ? &self->blackSide : &self->whiteSide;

        for (int from=0; from<boardSize; from++) {
                int piece = self->squares[from];
                if (piece == empty) continue;

                switch (piece) {
                        int dir, dirs;

                case whiteKing:
                        dirs = kingDirections[from];
                        dir = 0;
                        do {
                                dir -= dirs; // pick next
                                dir &= dirs;
                                int to = from + kingStep[dir];
                                self->whiteSide.attacks[to] += attackKing;
                        } while (dirs -= dir); // remove and go to next
                        self->whiteSide.king = from;
                        break;

                case blackKing:
                        dirs = kingDirections[from];
                        dir = 0;
                        do {
                                dir -= dirs; // pick next
                                dir &= dirs;
                                int to = from + kingStep[dir];
                                self->blackSide.attacks[to] += attackKing;
                        } while (dirs -= dir); // remove and go to next
                        self->blackSide.king = from;
                        break;

                case whiteQueen:
                        updateSliderAttacks(self, from, dirsQueen, &self->whiteSide, attackQueen);
                        break;

                case blackQueen:
                        updateSliderAttacks(self, from, dirsQueen, &self->blackSide, attackQueen);
                        break;

                case whiteRook:
                        updateSliderAttacks(self, from, dirsRook, &self->whiteSide, attackRook);
                        break;

                case blackRook:
                        updateSliderAttacks(self, from, dirsRook, &self->blackSide, attackRook);
                        break;

                case whiteBishop:
                        updateSliderAttacks(self, from, dirsBishop, &self->whiteSide, attackMinor);
                        break;

                case blackBishop:
                        updateSliderAttacks(self, from, dirsBishop, &self->blackSide, attackMinor);
                        break;

                case whiteKnight:
                        dirs = knightDirections[from];
                        dir = 0;
                        do {
                                dir -= dirs; // pick next
                                dir &= dirs;
                                int to = from + knightJump[dir];
                                self->whiteSide.attacks[to] += attackMinor;
                        } while (dirs -= dir); // remove and go to next
                        break;

                case blackKnight:
                        dirs = knightDirections[from];
                        dir = 0;
                        do {
                                dir -= dirs; // pick next
                                dir &= dirs;
                                int to = from + knightJump[dir];
                                self->blackSide.attacks[to] += attackMinor;
                        } while (dirs -= dir); // remove and go to next
                        break;

                case whitePawn:
                        if (file(from) != fileH)
                                self->whiteSide.attacks[from+stepNE] += attackPawn;
                        if (file(from) != fileA)
                                self->whiteSide.attacks[from+stepNW] += attackPawn;
                        break;

                case blackPawn:
                        if (file(from) != fileH)
                                self->blackSide.attacks[from+stepSE] = attackPawn;
                        if (file(from) != fileA)
                                self->blackSide.attacks[from+stepSW] = attackPawn;
                        break;
                }
        }

        self->sideInfoPlyNumber = self->plyNumber;
}

/*----------------------------------------------------------------------+
 |      hash                                                            |
 +----------------------------------------------------------------------*/

// Helper for hashing a combination of castling flags
static uint64_t hashCastleFlags(int flags)
{
        uint64_t key = 0;
        if (flags & castleFlagWhiteKside) key ^= zobristCastle[0];
        if (flags & castleFlagWhiteQside) key ^= zobristCastle[1];
        if (flags & castleFlagBlackKside) key ^= zobristCastle[2];
        if (flags & castleFlagBlackQside) key ^= zobristCastle[3];
        return key;
}

static uint64_t hashEnPassant(int square)
{
        return square ? zobristEnPassant[file(square)^fileA] : 0;
}

// Calculate Zobrist hash using Polyglot's definition
uint64_t hash(Board_t self)
{
        uint64_t key = 0;

        // Pieces (mind Polyglot's own square indexing)
        for (int pSquare=0; pSquare<64; pSquare++) {
                int pFile = pSquare & 7;
                int pRank = pSquare >> 3;
                int square = square(pFile ^ fileA, pRank ^ rank1);
                int piece = self->squares[square];
                key ^= zobristPiece[piece][pSquare];
        }

        // Castling
        key ^= hashCastleFlags(self->castleFlags);

        // En passant
        key ^= hashEnPassant(self->enPassantPawn);

        // Turn
        if (sideToMove(self) == white)
                key ^= zobristTurn[0];

        return key;
}

/*----------------------------------------------------------------------+
 |      isPromotion                                                     |
 +----------------------------------------------------------------------*/

bool isPromotion(Board_t self, int from, int to)
{
        return (self->squares[from] == whitePawn && rank(to) == rank8)
            || (self->squares[from] == blackPawn && rank(to) == rank1);
}

/*----------------------------------------------------------------------+
 |      isLegalMove                                                     |
 +----------------------------------------------------------------------*/

bool isLegalMove(Board_t self, int move)
{
        makeMove(self, move);
        bool isLegal = wasLegalMove(self);
        undoMove(self);
        return isLegal;
}

/*----------------------------------------------------------------------+
 |      inCheck                                                         |
 +----------------------------------------------------------------------*/

int inCheck(Board_t self)
{
        updateSideInfo(self);
        return self->xside->attacks[self->side->king] != 0;
}

/*----------------------------------------------------------------------+
 |      normalizeEnPassantStatus                                        |
 +----------------------------------------------------------------------*/

void normalizeEnPassantStatus(Board_t self)
{
        int square = self->enPassantPawn;
        if (!square)
                return;

        int pawn = (sideToMove(self) == white) ? whitePawn : blackPawn;
        int step = (sideToMove(self) == white) ? stepN : stepS;

        if (file(square) != fileA && self->squares[square+stepW] == pawn) {
                int move = specialMove(square + stepW, square + step);
                if (isLegalMove(self, move))
                        return;
        }
        if (file(square) != fileH && self->squares[square+stepE] == pawn) {
                int move = specialMove(square + stepE, square + step);
                if (isLegalMove(self, move))
                        return;
        }

        self->hash ^= hashEnPassant(square);
        self->enPassantPawn = 0;
}

/*----------------------------------------------------------------------+
 |                                                                      |
 +----------------------------------------------------------------------*/

