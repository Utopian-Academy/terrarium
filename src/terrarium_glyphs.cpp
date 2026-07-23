#include "terrarium_render.hpp"

// Raw bitmap tables live here so the higher-level visuals module can stay
// focused on choosing glyphs, colors, and effects instead of storing data.

namespace {

const uint8_t* glyph8_world(unsigned char c) {
  static const uint8_t BLANK[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  static const uint8_t COMMA[8] = {0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x18, 0x18, 0x10};
  static const uint8_t DASH[8] = {0x00, 0x00, 0x00, 0x7E,
                                  0x00, 0x00, 0x00, 0x00};
  static const uint8_t WAVE[8] = {0x00, 0x00, 0x52, 0x2A,
                                  0x15, 0x0A, 0x00, 0x00};
  static const uint8_t EQ[8] = {0x00, 0x00, 0x7E, 0x00,
                                0x7E, 0x00, 0x00, 0x00};
  static const uint8_t PCT[8] = {0x00, 0x62, 0x64, 0x08,
                                 0x10, 0x26, 0x46, 0x00};
  static const uint8_t AT[8] = {0x00, 0x3C, 0x42, 0x5A,
                                0x5A, 0x40, 0x3C, 0x00};

  static const uint8_t TGRASS[8] = {0x24, 0x24, 0x24, 0x00,
                                    0x00, 0x00, 0x00, 0x00};
  static const uint8_t SHRUB[8] = {0x00, 0x24, 0x7E, 0x24,
                                   0x24, 0x7E, 0x24, 0x00};
  static const uint8_t TREE1[8] = {0x10, 0x38, 0x54, 0x10,
                                   0x10, 0x10, 0x38, 0x00};
  static const uint8_t TREE2[8] = {0x10, 0x38, 0x54, 0x10,
                                   0x10, 0x28, 0x44, 0x00};
  static const uint8_t PALM[8] = {0x10, 0x54, 0x38, 0x10,
                                  0x10, 0x10, 0x38, 0x00};
  static const uint8_t MUSH[8] = {0x00, 0x3C, 0x7E, 0x7E,
                                  0x18, 0x18, 0x3C, 0x00};
  static const uint8_t FLOW1[8] = {0x10, 0x54, 0x38, 0x7C,
                                   0x38, 0x54, 0x10, 0x00};
  static const uint8_t FLOW2[8] = {0x00, 0x10, 0x38, 0x7C,
                                   0x38, 0x10, 0x00, 0x00};
  static const uint8_t BIGF[8] = {0x28, 0x7C, 0xFE, 0x7C,
                                  0xFE, 0x7C, 0x28, 0x00};
  static const uint8_t SUPERB[8] = {0x10, 0x7C, 0xFE, 0x7C,
                                    0xFE, 0x7C, 0x10, 0x00};
  static const uint8_t FERN[8] = {0x10, 0x38, 0x10, 0x38,
                                  0x10, 0x28, 0x44, 0x00};
  static const uint8_t REED[8] = {0x10, 0x10, 0x10, 0x10,
                                  0x28, 0x28, 0x00, 0x00};
  static const uint8_t STONE[8] = {0x00, 0x18, 0x3C, 0x7E,
                                   0x7E, 0x3C, 0x18, 0x00};
  static const uint8_t FRUIT[8] = {0x18, 0x24, 0x42, 0x5A,
                                   0x7E, 0x24, 0x18, 0x00};
  static const uint8_t STAR[8] = {0x00, 0x24, 0x18, 0x7E,
                                  0x18, 0x24, 0x00, 0x00};
  static const uint8_t EX[8] = {0x00, 0x42, 0x24, 0x18,
                                0x18, 0x24, 0x42, 0x00};
  static const uint8_t MUD0[8] = {0x00, 0x00, 0x3A, 0x5C,
                                  0x2E, 0x74, 0x5C, 0x2E};
  static const uint8_t MUD1[8] = {0x00, 0x00, 0x3C, 0x6A,
                                  0x5C, 0x3A, 0x6C, 0x00};
  static const uint8_t MUD2[8] = {0x00, 0x00, 0x2C, 0x5A,
                                  0x3C, 0x66, 0x5A, 0x00};

  static const uint8_t A00[8] = {0x00, 0x18, 0x3C, 0x3C,
                                 0x7E, 0x3C, 0x18, 0x00};
  static const uint8_t A01[8] = {0x00, 0x18, 0x3C, 0x7E,
                                 0x3C, 0x3C, 0x18, 0x00};
  static const uint8_t A02[8] = {0x00, 0x18, 0x3C, 0x7E,
                                 0xDB, 0x7E, 0x24, 0x00};
  static const uint8_t A03[8] = {0x00, 0x18, 0x3C, 0x7E,
                                 0xDB, 0x7E, 0x42, 0x00};
  static const uint8_t A04[8] = {0x00, 0x18, 0x3C, 0x18,
                                 0x7E, 0x24, 0x42, 0x00};
  static const uint8_t A05[8] = {0x00, 0x24, 0x3C, 0x18,
                                 0x7E, 0x18, 0x24, 0x00};
  static const uint8_t A06[8] = {0x00, 0x3C, 0x66, 0x5A,
                                 0x66, 0x3C, 0x18, 0x00};
  static const uint8_t A07[8] = {0x00, 0x3C, 0x66, 0x7E,
                                 0x66, 0x3C, 0x18, 0x00};
  static const uint8_t A08[8] = {0x00, 0x18, 0x18, 0x3C,
                                 0x7E, 0x3C, 0x18, 0x00};
  static const uint8_t A09[8] = {0x00, 0x0C, 0x18, 0x3C,
                                 0x7E, 0x3C, 0x30, 0x00};
  static const uint8_t A0A[8] = {0x00, 0x66, 0x7E, 0xDB,
                                 0xFF, 0x7E, 0x24, 0x00};
  static const uint8_t A0B[8] = {0x00, 0x66, 0x7E, 0xBD,
                                 0xFF, 0x7E, 0x24, 0x00};
  static const uint8_t A0C[8] = {0x00, 0x18, 0x5A, 0x3C,
                                 0xFF, 0x3C, 0x5A, 0x18};
  static const uint8_t A0D[8] = {0x00, 0x24, 0x5A, 0x3C,
                                 0xFF, 0x3C, 0x5A, 0x24};
  static const uint8_t A0E[8] = {0x00, 0x3C, 0x66, 0x5A,
                                 0x66, 0x3C, 0x42, 0x00};
  static const uint8_t A0F[8] = {0x00, 0x3C, 0x66, 0x5A,
                                 0x66, 0x3C, 0x24, 0x00};

  static const uint8_t BOUL[8] = {0x00, 0x3C, 0x7E, 0xDB,
                                  0xFF, 0xE7, 0x7E, 0x3C};
  static const uint8_t MOUN[8] = {0x10, 0x38, 0x7C, 0xFE,
                                  0x7C, 0x38, 0x10, 0x00};
  static const uint8_t LILY[8] = {0x00, 0x38, 0x7C, 0xFE,
                                  0xEE, 0x7C, 0x38, 0x00};
  static const uint8_t SAND[8] = {0x00, 0x00, 0x18, 0x3C,
                                  0x7E, 0x3C, 0x18, 0x00};
  static const uint8_t CACT[8] = {0x18, 0x18, 0x5A, 0x7E,
                                  0x5A, 0x18, 0x18, 0x00};
  static const uint8_t LIZ[8] = {0x00, 0x18, 0x3C, 0x66,
                                 0x3C, 0x18, 0x66, 0x00};
  static const uint8_t CAM[8] = {0x00, 0x3C, 0x66, 0x7E,
                                 0x5A, 0x66, 0x24, 0x00};
  static const uint8_t DOLP[8] = {0x00, 0x1C, 0x3E, 0x7C,
                                  0x3E, 0x1C, 0x08, 0x00};
  static const uint8_t WHAL[8] = {0x00, 0x3C, 0x7E, 0xDB,
                                  0xFF, 0x7E, 0x3C, 0x00};
  static const uint8_t SEAM[8] = {0x18, 0x3C, 0x7E, 0xDB,
                                  0x7E, 0x3C, 0x5A, 0x00};
  static const uint8_t DINO[8] = {0x00, 0x1C, 0x3E, 0x3F,
                                  0x1E, 0x3E, 0x2A, 0x22};
  static const uint8_t MONO[8] = {0x18, 0x3C, 0x3C, 0x3C,
                                  0x3C, 0x3C, 0x3C, 0x18};
  static const uint8_t EYE[8] = {0x00, 0x3C, 0x42, 0xA5,
                                 0x81, 0xA5, 0x42, 0x3C};

  static const uint8_t SCOR1[8] = {0x00, 0x10, 0x38, 0x54,
                                   0x38, 0x10, 0x28, 0x00};
  static const uint8_t DRGN1[8] = {0x00, 0x24, 0x18, 0x7E,
                                   0x18, 0x24, 0x42, 0x00};
  static const uint8_t CRAB1[8] = {0x00, 0x24, 0x7E, 0x3C,
                                   0x3C, 0x7E, 0x24, 0x00};
  static const uint8_t JELY1[8] = {0x00, 0x3C, 0x7E, 0x7E,
                                   0x3C, 0x24, 0x24, 0x00};
  static const uint8_t CRAW1[8] = {0x00, 0x3C, 0x5A, 0x3C,
                                   0x5A, 0x3C, 0x24, 0x00};
  static const uint8_t ORB1[8] = {0x00, 0x18, 0x3C, 0x7E,
                                  0x7E, 0x3C, 0x18, 0x00};
  static const uint8_t HIM1[8] = {0x00, 0x18, 0x18, 0x3C,
                                  0x5A, 0x18, 0x24, 0x00};
  static const uint8_t HER1[8] = {0x00, 0x18, 0x18, 0x3C,
                                  0x7E, 0x18, 0x3C, 0x00};

  static const uint8_t WAT1[8] = {0x00, 0x00, 0x00, 0x10,
                                  0x00, 0x00, 0x00, 0x00};
  static const uint8_t WAT2[8] = {0x00, 0x00, 0x10, 0x00,
                                  0x04, 0x00, 0x00, 0x00};
  static const uint8_t WAT3[8] = {0x00, 0x00, 0x28, 0x00,
                                  0x10, 0x00, 0x00, 0x00};
  static const uint8_t WAT4[8] = {0x00, 0x00, 0x28, 0x00,
                                  0x28, 0x00, 0x00, 0x00};
  static const uint8_t WAT5[8] = {0x00, 0x44, 0x28, 0x00,
                                  0x44, 0x28, 0x00, 0x00};
  static const uint8_t WAT6[8] = {0x00, 0x44, 0x28, 0x00,
                                  0x44, 0x28, 0x00, 0x44};
  static const uint8_t WAT7[8] = {0x44, 0x28, 0x00, 0x44,
                                  0x28, 0x00, 0x44, 0x28};

  static const uint8_t W1H[8] = {0x00, 0x00, 0x00, 0x38,
                                 0x00, 0x00, 0x00, 0x00};
  static const uint8_t W2H[8] = {0x00, 0x00, 0x38, 0x00,
                                 0x1C, 0x00, 0x00, 0x00};
  static const uint8_t W3H[8] = {0x00, 0x00, 0x38, 0x00,
                                 0x38, 0x00, 0x00, 0x00};
  static const uint8_t W4H[8] = {0x00, 0x38, 0x00, 0x38,
                                 0x00, 0x38, 0x00, 0x00};
  static const uint8_t W5H[8] = {0x00, 0x7C, 0x00, 0x38,
                                 0x00, 0x7C, 0x00, 0x00};
  static const uint8_t W6H[8] = {0x00, 0x7C, 0x00, 0x7C,
                                 0x00, 0x7C, 0x00, 0x00};
  static const uint8_t W7H[8] = {0x7C, 0x00, 0x7C, 0x00,
                                 0x7C, 0x00, 0x7C, 0x00};

  static const uint8_t W1V[8] = {0x00, 0x00, 0x10, 0x10,
                                 0x10, 0x00, 0x00, 0x00};
  static const uint8_t W2V[8] = {0x00, 0x10, 0x10, 0x00,
                                 0x10, 0x10, 0x00, 0x00};
  static const uint8_t W3V[8] = {0x10, 0x10, 0x00, 0x10,
                                 0x10, 0x00, 0x10, 0x10};
  static const uint8_t W4V[8] = {0x18, 0x18, 0x18, 0x00,
                                 0x18, 0x18, 0x18, 0x00};
  static const uint8_t W5V[8] = {0x1C, 0x1C, 0x00, 0x1C,
                                 0x1C, 0x00, 0x1C, 0x1C};
  static const uint8_t W6V[8] = {0x3C, 0x00, 0x3C, 0x00,
                                 0x3C, 0x00, 0x3C, 0x00};
  static const uint8_t W7V[8] = {0x3C, 0x3C, 0x3C, 0x3C,
                                 0x3C, 0x3C, 0x3C, 0x3C};

  static const uint8_t W1D[8] = {0x00, 0x00, 0x40, 0x03,
                                 0x10, 0x00, 0x00, 0x00};
  static const uint8_t W2D[8] = {0x00, 0x40, 0x03, 0x10,
                                 0x08, 0x00, 0x00, 0x00};
  static const uint8_t W3D[8] = {0x40, 0x03, 0x10, 0x08,
                                 0x04, 0x02, 0x00, 0x00};
  static const uint8_t W4D[8] = {0x40, 0x03, 0x10, 0x08,
                                 0x10, 0x03, 0x40, 0x00};
  static const uint8_t W5D[8] = {0x44, 0x05, 0x11, 0x08,
                                 0x11, 0x05, 0x44, 0x00};
  static const uint8_t W6D[8] = {0x66, 0x33, 0x19, 0x0C,
                                 0x19, 0x33, 0x66, 0x00};
  static const uint8_t W7D[8] = {0x77, 0x3B, 0x1D, 0x0E,
                                 0x1D, 0x3B, 0x77, 0x00};

  static const uint8_t BUG[8] = {0x00, 0x18, 0x3C, 0x5A,
                                 0x3C, 0x18, 0x00, 0x00};
  static const uint8_t BIRD[8] = {0x00, 0x00, 0x42, 0x24,
                                  0x18, 0x00, 0x00, 0x00};
  static const uint8_t RAB[8] = {0x18, 0x3C, 0x66, 0x42,
                                 0x42, 0x66, 0x24, 0x00};
  static const uint8_t SNAKE[8] = {0x00, 0x7E, 0x40, 0x7E,
                                   0x02, 0x7E, 0x00, 0x00};
  static const uint8_t GLOW[8] = {0x00, 0x18, 0x3C, 0x7E,
                                  0x3C, 0x18, 0x00, 0x00};
  static const uint8_t OWL[8] = {0x3C, 0x7E, 0xDB, 0xFF,
                                 0xBD, 0xDB, 0x7E, 0x3C};
  static const uint8_t YETI[8] = {0x3C, 0x7E, 0xFF, 0xDB,
                                  0xFF, 0xDB, 0x7E, 0x3C};
  static const uint8_t AYY[8] = {0x00, 0x18, 0x24, 0x42,
                                 0x7E, 0x42, 0x42, 0x00};

  static const uint8_t SLASH[8] = {0x02, 0x04, 0x08, 0x10,
                                   0x03, 0x40, 0x80, 0x00};
  static const uint8_t BSLASH[8] = {0x80, 0x40, 0x03, 0x10,
                                    0x08, 0x04, 0x02, 0x00};
  static const uint8_t PIPE[8] = {0x18, 0x18, 0x18, 0x18,
                                  0x18, 0x18, 0x18, 0x18};

  switch (c) {
    case ',':
      return COMMA;
    case '-':
      return DASH;
    case '~':
      return WAVE;
    case '=':
      return EQ;
    case '#':
      return SHRUB;
    case '%':
      return PCT;
    case '@':
      return AT;
    case '"':
      return TGRASS;
    case 'T':
      return TREE1;
    case 'Y':
      return TREE2;
    case 'P':
      return PALM;
    case 'm':
      return MUSH;
    case '+':
      return FLOW1;
    case 'f':
      return FLOW2;
    case '&':
      return BIGF;
    case '!':
      return SUPERB;
    case ';':
      return FERN;
    case ':':
      return REED;
    case '^':
      return STONE;
    case '$':
      return FRUIT;
    case '*':
      return STAR;
    case 'x':
      return EX;
    case 'd':
      return MUD0;
    case 'e':
      return MUD1;
    case 'g':
      return MUD2;
    case 'B':
      return BOUL;
    case 'M':
      return MOUN;
    case 'l':
      return LILY;
    case '`':
      return SAND;
    case 'c':
      return CACT;
    case 'L':
      return LIZ;
    case 'C':
      return CAM;
    case 'D':
      return DOLP;
    case 'W':
      return WHAL;
    case 'K':
      return DINO;
    case 'S':
      return SEAM;
    case 'X':
      return MONO;
    case 'E':
      return EYE;
    case '1':
      return WAT1;
    case '2':
      return WAT2;
    case '3':
      return WAT3;
    case '4':
      return WAT4;
    case '5':
      return WAT5;
    case '6':
      return WAT6;
    case '7':
      return WAT7;
    case '\x01':
      return W1H;
    case '\x02':
      return W2H;
    case '\x03':
      return W3H;
    case '\x04':
      return W4H;
    case '\x05':
      return W5H;
    case '\x06':
      return W6H;
    case '\x07':
      return W7H;
    case '\x08':
      return W1V;
    case '\x09':
      return W2V;
    case '\x0A':
      return W3V;
    case '\x0B':
      return W4V;
    case '\x0C':
      return W5V;
    case '\x0D':
      return W6V;
    case '\x0E':
      return W7V;
    case '\x0F':
      return W1D;
    case '\x10':
      return W2D;
    case '\x11':
      return W3D;
    case '\x12':
      return W4D;
    case '\x13':
      return W5D;
    case '\x14':
      return W6D;
    case '\x15':
      return W7D;
    case 'b':
      return BUG;
    case 'v':
      return BIRD;
    case 'r':
      return RAB;
    case 'n':
      return SNAKE;
    case 's':
      return SAND;
    case 'F':
      return GLOW;
    case 'O':
      return OWL;
    case 'H':
      return YETI;
    case 'A':
      return AYY;
    case '/':
      return SLASH;
    case '\\':
      return BSLASH;
    case '|':
      return PIPE;
    case '\x19':
      return HIM1;
    case '\x1A':
      return HER1;
    case '\x16':
      return SCOR1;
    case '\x17':
      return DRGN1;
    case '\x18':
      return CRAB1;
    case '\x1B':
      return JELY1;
    case '\x1C':
      return CRAW1;
    case '\x1D':
      return ORB1;
    case 0x80:
      return A00;
    case 0x81:
      return A01;
    case 0x82:
      return A02;
    case 0x83:
      return A03;
    case 0x84:
      return A04;
    case 0x85:
      return A05;
    case 0x86:
      return A06;
    case 0x87:
      return A07;
    case 0x88:
      return A08;
    case 0x89:
      return A09;
    case 0x8A:
      return A0A;
    case 0x8B:
      return A0B;
    case 0x8C:
      return A0C;
    case 0x8D:
      return A0D;
    case 0x8E:
      return A0E;
    case 0x8F:
      return A0F;
    default:
      return BLANK;
  }
}

const uint8_t* glyph8_text(unsigned char c) {
  static const uint8_t BLANK[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');

#define R(x) (uint8_t)((x) << 2)

  static const uint8_t SPACE[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  static const uint8_t DOT[8] = {0, 0, 0, 0, 0, 0, R(0b00100), 0};
  static const uint8_t COLON[8] = {0, R(0b00100), 0, 0,
                                   R(0b00100), 0, 0, 0};
  static const uint8_t DASH[8] = {0, 0, 0, R(0b11111), 0, 0, 0, 0};
  static const uint8_t PLUS[8] = {0, 0, R(0b00100), R(0b11111),
                                  R(0b00100), 0, 0, 0};
  static const uint8_t SLASH[8] = {R(0b00001), R(0b00010), R(0b00100),
                                   R(0b01000), R(0b10000), 0, 0, 0};
  static const uint8_t PCT[8] = {R(0b11001), R(0b11010), R(0b00100),
                                 R(0b01000), R(0b10110), 0, 0, 0};
  static const uint8_t LBR[8] = {R(0b00110), R(0b00100), R(0b00100),
                                 R(0b00100), R(0b00100), R(0b00100),
                                 R(0b00110), 0};
  static const uint8_t RBR[8] = {R(0b01100), R(0b00100), R(0b00100),
                                 R(0b00100), R(0b00100), R(0b00100),
                                 R(0b01100), 0};
  static const uint8_t LP[8] = {R(0b00010), R(0b00100), R(0b01000),
                                R(0b01000), R(0b01000), R(0b00100),
                                R(0b00010), 0};
  static const uint8_t RP[8] = {R(0b01000), R(0b00100), R(0b00010),
                                R(0b00010), R(0b00010), R(0b00100),
                                R(0b01000), 0};
  static const uint8_t EQ[8] = {0, 0, R(0b11111), 0, R(0b11111), 0, 0, 0};
  static const uint8_t COMMA[8] = {0, 0, 0, 0, 0, R(0b00100),
                                   R(0b00100), R(0b01000)};
  static const uint8_t QUOTE[8] = {R(0b00100), R(0b00100), 0, 0,
                                   0, 0, 0, 0};
  static const uint8_t EXCL[8] = {R(0b00100), R(0b00100), R(0b00100),
                                  R(0b00100), R(0b00100), 0,
                                  R(0b00100), 0};

  static const uint8_t D0[8] = {R(0b01110), R(0b10001), R(0b10011),
                                R(0b10101), R(0b11001), R(0b10001),
                                R(0b01110), 0};
  static const uint8_t D1[8] = {R(0b00100), R(0b01100), R(0b00100),
                                R(0b00100), R(0b00100), R(0b00100),
                                R(0b01110), 0};
  static const uint8_t D2[8] = {R(0b01110), R(0b10001), R(0b00001),
                                R(0b00010), R(0b00100), R(0b01000),
                                R(0b11111), 0};
  static const uint8_t D3[8] = {R(0b11110), R(0b00001), R(0b00001),
                                R(0b01110), R(0b00001), R(0b00001),
                                R(0b11110), 0};
  static const uint8_t D4[8] = {R(0b00010), R(0b00110), R(0b01010),
                                R(0b10010), R(0b11111), R(0b00010),
                                R(0b00010), 0};
  static const uint8_t D5[8] = {R(0b11111), R(0b10000), R(0b10000),
                                R(0b11110), R(0b00001), R(0b00001),
                                R(0b11110), 0};
  static const uint8_t D6[8] = {R(0b01110), R(0b10000), R(0b10000),
                                R(0b11110), R(0b10001), R(0b10001),
                                R(0b01110), 0};
  static const uint8_t D7[8] = {R(0b11111), R(0b00001), R(0b00010),
                                R(0b00100), R(0b01000), R(0b01000),
                                R(0b01000), 0};
  static const uint8_t D8[8] = {R(0b01110), R(0b10001), R(0b10001),
                                R(0b01110), R(0b10001), R(0b10001),
                                R(0b01110), 0};
  static const uint8_t D9[8] = {R(0b01110), R(0b10001), R(0b10001),
                                R(0b01111), R(0b00001), R(0b00001),
                                R(0b01110), 0};

  static const uint8_t A[8] = {R(0b01110), R(0b10001), R(0b10001),
                               R(0b11111), R(0b10001), R(0b10001),
                               R(0b10001), 0};
  static const uint8_t B[8] = {R(0b11110), R(0b10001), R(0b10001),
                               R(0b11110), R(0b10001), R(0b10001),
                               R(0b11110), 0};
  static const uint8_t C[8] = {R(0b01110), R(0b10001), R(0b10000),
                               R(0b10000), R(0b10000), R(0b10001),
                               R(0b01110), 0};
  static const uint8_t D[8] = {R(0b11110), R(0b10001), R(0b10001),
                               R(0b10001), R(0b10001), R(0b10001),
                               R(0b11110), 0};
  static const uint8_t E[8] = {R(0b11111), R(0b10000), R(0b10000),
                               R(0b11110), R(0b10000), R(0b10000),
                               R(0b11111), 0};
  static const uint8_t F[8] = {R(0b11111), R(0b10000), R(0b10000),
                               R(0b11110), R(0b10000), R(0b10000),
                               R(0b10000), 0};
  static const uint8_t G[8] = {R(0b01110), R(0b10001), R(0b10000),
                               R(0b10111), R(0b10001), R(0b10001),
                               R(0b01110), 0};
  static const uint8_t H[8] = {R(0b10001), R(0b10001), R(0b10001),
                               R(0b11111), R(0b10001), R(0b10001),
                               R(0b10001), 0};
  static const uint8_t I[8] = {R(0b01110), R(0b00100), R(0b00100),
                               R(0b00100), R(0b00100), R(0b00100),
                               R(0b01110), 0};
  static const uint8_t J[8] = {R(0b00111), R(0b00010), R(0b00010),
                               R(0b00010), R(0b10010), R(0b10010),
                               R(0b01100), 0};
  static const uint8_t K[8] = {R(0b10001), R(0b10010), R(0b10100),
                               R(0b11000), R(0b10100), R(0b10010),
                               R(0b10001), 0};
  static const uint8_t L[8] = {R(0b10000), R(0b10000), R(0b10000),
                               R(0b10000), R(0b10000), R(0b10000),
                               R(0b11111), 0};
  static const uint8_t M[8] = {R(0b10001), R(0b11011), R(0b10101),
                               R(0b10101), R(0b10001), R(0b10001),
                               R(0b10001), 0};
  static const uint8_t N[8] = {R(0b10001), R(0b11001), R(0b10101),
                               R(0b10011), R(0b10001), R(0b10001),
                               R(0b10001), 0};
  static const uint8_t O[8] = {R(0b01110), R(0b10001), R(0b10001),
                               R(0b10001), R(0b10001), R(0b10001),
                               R(0b01110), 0};
  static const uint8_t P[8] = {R(0b11110), R(0b10001), R(0b10001),
                               R(0b11110), R(0b10000), R(0b10000),
                               R(0b10000), 0};
  static const uint8_t Q[8] = {R(0b01110), R(0b10001), R(0b10001),
                               R(0b10001), R(0b10101), R(0b10010),
                               R(0b01101), 0};
  static const uint8_t Rr[8] = {R(0b11110), R(0b10001), R(0b10001),
                                R(0b11110), R(0b10100), R(0b10010),
                                R(0b10001), 0};
  static const uint8_t S[8] = {R(0b01111), R(0b10000), R(0b10000),
                               R(0b01110), R(0b00001), R(0b00001),
                               R(0b11110), 0};
  static const uint8_t T[8] = {R(0b11111), R(0b00100), R(0b00100),
                               R(0b00100), R(0b00100), R(0b00100),
                               R(0b00100), 0};
  static const uint8_t U[8] = {R(0b10001), R(0b10001), R(0b10001),
                               R(0b10001), R(0b10001), R(0b10001),
                               R(0b01110), 0};
  static const uint8_t V[8] = {R(0b10001), R(0b10001), R(0b10001),
                               R(0b10001), R(0b10001), R(0b01010),
                               R(0b00100), 0};
  static const uint8_t W[8] = {R(0b10001), R(0b10001), R(0b10001),
                               R(0b10101), R(0b10101), R(0b10101),
                               R(0b01010), 0};
  static const uint8_t X[8] = {R(0b10001), R(0b10001), R(0b01010),
                               R(0b00100), R(0b01010), R(0b10001),
                               R(0b10001), 0};
  static const uint8_t Y[8] = {R(0b10001), R(0b10001), R(0b01010),
                               R(0b00100), R(0b00100), R(0b00100),
                               R(0b00100), 0};
  static const uint8_t Z[8] = {R(0b11111), R(0b00001), R(0b00010),
                               R(0b00100), R(0b01000), R(0b10000),
                               R(0b11111), 0};

  static const uint8_t K_MI[8] = {0x00, 0x7C, 0x10, 0x7C,
                                  0x10, 0x7C, 0x00, 0x00};
  static const uint8_t K_ZU[8] = {0x00, 0x44, 0x7C, 0x08,
                                  0x10, 0x20, 0x7C, 0x00};
  static const uint8_t K_O[8] = {0x00, 0x7C, 0x10, 0x7C,
                                 0x12, 0x14, 0x18, 0x00};
  static const uint8_t K_TO[8] = {0x00, 0x10, 0x10, 0x10,
                                  0x10, 0x10, 0x7C, 0x00};
  static const uint8_t K_SU[8] = {0x00, 0x7C, 0x08, 0x10,
                                  0x20, 0x20, 0x7C, 0x00};
  static const uint8_t K_PO[8] = {0x00, 0x7C, 0x10, 0x7C,
                                  0x10, 0x28, 0x44, 0x40};
  static const uint8_t K_N[8] = {0x00, 0x40, 0x20, 0x10,
                                 0x08, 0x08, 0x70, 0x00};
  static const uint8_t K_LONG[8] = {0x00, 0x00, 0x00, 0x7C,
                                    0x00, 0x00, 0x00, 0x00};
  static const uint8_t K_ME[8] = {0x00, 0x44, 0x28, 0x10,
                                  0x28, 0x44, 0x00, 0x00};
  static const uint8_t K_NI[8] = {0x00, 0x7C, 0x00, 0x00,
                                  0x7C, 0x00, 0x00, 0x00};
  static const uint8_t K_SYU[8] = {0x00, 0x00, 0x50, 0x10,
                                   0x7C, 0x00, 0x00, 0x00};

  switch (c) {
    case ' ':
      return SPACE;
    case '.':
      return DOT;
    case ':':
      return COLON;
    case '-':
      return DASH;
    case '+':
      return PLUS;
    case '/':
      return SLASH;
    case '%':
      return PCT;
    case '[':
      return LBR;
    case ']':
      return RBR;
    case '(':
      return LP;
    case ')':
      return RP;
    case '=':
      return EQ;
    case ',':
      return COMMA;
    case '"':
      return QUOTE;
    case '!':
      return EXCL;
    case '0':
      return D0;
    case '1':
      return D1;
    case '2':
      return D2;
    case '3':
      return D3;
    case '4':
      return D4;
    case '5':
      return D5;
    case '6':
      return D6;
    case '7':
      return D7;
    case '8':
      return D8;
    case '9':
      return D9;
    case 'A':
      return A;
    case 'B':
      return B;
    case 'C':
      return C;
    case 'D':
      return D;
    case 'E':
      return E;
    case 'F':
      return F;
    case 'G':
      return G;
    case 'H':
      return H;
    case 'I':
      return I;
    case 'J':
      return J;
    case 'K':
      return K;
    case 'L':
      return L;
    case 'M':
      return M;
    case 'N':
      return N;
    case 'O':
      return O;
    case 'P':
      return P;
    case 'Q':
      return Q;
    case 'R':
      return Rr;
    case 'S':
      return S;
    case 'T':
      return T;
    case 'U':
      return U;
    case 'V':
      return V;
    case 'W':
      return W;
    case 'X':
      return X;
    case 'Y':
      return Y;
    case 'Z':
      return Z;
    case 0x80:
      return K_MI;
    case 0x81:
      return K_ZU;
    case 0x82:
      return K_O;
    case 0x83:
      return K_TO;
    case 0x84:
      return K_SU;
    case 0x85:
      return K_PO;
    case 0x86:
      return K_N;
    case 0x87:
      return K_LONG;
    case 0x88:
      return K_ME;
    case 0x89:
      return K_NI;
    case 0x8A:
      return K_SYU;
    default:
      return BLANK;
  }

#undef R
}

// Hand-drawn 4x4 micro glyphs (top nibble of each row byte). Distinct
// silhouettes at panel scale: tree = mast, mushroom = cap, flower = head on
// a stem, stone = mound. Returns nullptr when a glyph has no hand-drawn
// version (caller OR-downsamples the 8x8 instead).
const uint8_t* glyph4_world(unsigned char c) {
  static const uint8_t G_COMMA[4] = {0x00, 0x00, 0x40, 0x80};
  static const uint8_t G_TGRASS[4] = {0xA0, 0xA0, 0x00, 0x00};
  static const uint8_t G_SEMI[4] = {0x00, 0x40, 0x40, 0x80};
  static const uint8_t G_MOSS[4] = {0xA0, 0x00, 0x50, 0x00};
  static const uint8_t G_SHRUB[4] = {0x00, 0x60, 0xF0, 0x60};
  static const uint8_t G_TREE1[4] = {0x40, 0xE0, 0x40, 0x40};
  static const uint8_t G_TREE2[4] = {0xA0, 0x40, 0x40, 0x40};
  static const uint8_t G_PALM[4] = {0xE0, 0x40, 0x40, 0x60};
  static const uint8_t G_MUSH[4] = {0xF0, 0xF0, 0x60, 0x60};
  static const uint8_t G_FLOW1[4] = {0x40, 0xA0, 0x40, 0x40};
  static const uint8_t G_FLOW2[4] = {0x40, 0xE0, 0x40, 0x00};
  static const uint8_t G_BIGF[4] = {0x60, 0xF0, 0xF0, 0x60};
  static const uint8_t G_SUPERB[4] = {0xA0, 0xF0, 0xF0, 0x40};
  static const uint8_t G_FRUIT[4] = {0x20, 0x60, 0xF0, 0x60};
  static const uint8_t G_STONE[4] = {0x00, 0x40, 0xE0, 0xF0};
  static const uint8_t G_BOULDER[4] = {0x00, 0x60, 0xF0, 0xF0};
  static const uint8_t G_MOUNT[4] = {0x00, 0x40, 0xA0, 0xF0};
  static const uint8_t G_STAR[4] = {0xA0, 0x40, 0xA0, 0x00};
  static const uint8_t G_WAVE[4] = {0x00, 0x50, 0xA0, 0x00};
  static const uint8_t G_FOAM[4] = {0x00, 0xF0, 0x00, 0xF0};
  static const uint8_t G_BURNT[4] = {0x90, 0x60, 0x60, 0x90};
  static const uint8_t G_SAND[4] = {0x00, 0x50, 0x00, 0xA0};
  static const uint8_t G_CLOVER[4] = {0x60, 0x40, 0x60, 0x00};
  static const uint8_t G_MUD0[4] = {0x00, 0x50, 0xA0, 0x50};
  static const uint8_t G_MUD1[4] = {0x00, 0xA0, 0x50, 0xA0};
  static const uint8_t G_MUD2[4] = {0x50, 0xA0, 0x00, 0xA0};
  static const uint8_t G_RAIN_V[4] = {0x40, 0x40, 0x40, 0x40};
  static const uint8_t G_RAIN_S[4] = {0x10, 0x20, 0x40, 0x80};
  static const uint8_t G_RAIN_B[4] = {0x80, 0x40, 0x20, 0x10};

  switch (c) {
    case ',': return G_COMMA;
    case '"': return G_TGRASS;
    case ';': return G_SEMI;
    case ':': return G_MOSS;
    case '#': return G_SHRUB;
    case 'T': return G_TREE1;
    case 'Y': return G_TREE2;
    case 'P': return G_PALM;
    case 'm': return G_MUSH;
    case 'f': return G_FLOW1;
    case '+': return G_FLOW2;
    case '&': return G_BIGF;
    case '!': return G_SUPERB;
    case '$': return G_FRUIT;
    case '^': return G_STONE;
    case 'B': return G_BOULDER;
    case 'M': return G_MOUNT;
    case '*': return G_STAR;
    case '~': return G_WAVE;
    case '=': return G_FOAM;
    case 'x': return G_BURNT;
    case 's': return G_SAND;
    case 'c': return G_CLOVER;
    case 'd': return G_MUD0;
    case 'e': return G_MUD1;
    case 'g': return G_MUD2;
    case '|': return G_RAIN_V;
    case '/': return G_RAIN_S;
    case '\\': return G_RAIN_B;
    default: return nullptr;
  }
}

}  // namespace

void GlyphCache::destroy() {
  for (auto& entry : tex) SDL_DestroyTexture(entry.second);
  tex.clear();
}

SDL_Texture* GlyphCache::makeGlyph(SDL_Renderer* renderer, unsigned char c) {
  const bool micro = (microSize < 8) && !textMode;
  const int size = micro ? microSize : 8;
  SDL_Texture* texture =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING, size, size);
  if (!texture) return nullptr;

  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

  void* pixels = nullptr;
  int pitch = 0;
  if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) != 0) {
    SDL_DestroyTexture(texture);
    return nullptr;
  }

  for (int y = 0; y < size; ++y) {
    uint32_t* row = (uint32_t*)((uint8_t*)pixels + y * pitch);
    for (int x = 0; x < size; ++x) row[x] = 0x00000000;
  }

  uint8_t bitmap[8];
  if (micro) {
    uint8_t m4[4];
    const uint8_t* hand = glyph4_world(c);
    if (hand) {
      for (int y = 0; y < 4; ++y) m4[y] = hand[y];
    } else {
      // OR-downsample the 8x8: a 2x2 block with any ink stays inked, so
      // thin strokes survive (nearest-scaling drops whole rows instead).
      const uint8_t* g8 = glyph8_world(c);
      for (int y = 0; y < 4; ++y) {
        uint8_t merged = (uint8_t)(g8[y * 2] | g8[y * 2 + 1]);
        uint8_t out = 0;
        for (int x = 0; x < 4; ++x) {
          if ((merged & (0xC0u >> (x * 2))) != 0) out |= (uint8_t)(0x80u >> x);
        }
        m4[y] = out;
      }
    }
    if (microSize == 2) {
      // Fold once more to 2x2 marks; color carries most of the identity at
      // this scale, the mark shape adds texture (tree = column, grass = dot).
      for (int y = 0; y < 2; ++y) {
        uint8_t merged = (uint8_t)(m4[y * 2] | m4[y * 2 + 1]);
        uint8_t out = 0;
        if ((merged & 0xC0u) != 0) out |= 0x80u;
        if ((merged & 0x30u) != 0) out |= 0x40u;
        bitmap[y] = out;
      }
    } else {
      for (int y = 0; y < 4; ++y) bitmap[y] = m4[y];
    }
  } else {
    const uint8_t* glyph = textMode ? glyph8_text(c) : glyph8_world(c);
    for (int y = 0; y < 8; ++y) bitmap[y] = glyph[y];
  }

  for (int y = 0; y < size; ++y) {
    uint32_t* row = (uint32_t*)((uint8_t*)pixels + y * pitch);
    uint8_t bits = bitmap[y];
    for (int x = 0; x < size; ++x) {
      if ((bits & (0x80u >> x)) != 0) row[x] = 0xE0FFFFFF;
    }
  }

  SDL_UnlockTexture(texture);
  return texture;
}

SDL_Texture* GlyphCache::get(SDL_Renderer* renderer, unsigned char c) {
  auto it = tex.find(c);
  if (it != tex.end()) return it->second;

  SDL_Texture* texture = makeGlyph(renderer, c);
  if (texture) tex[c] = texture;
  return texture;
}
