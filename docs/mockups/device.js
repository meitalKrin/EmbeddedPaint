/* PICO-CADE shared mockup helpers.
   Every screen draws onto a real 128x160 canvas using RGB565 values taken
   straight from the firmware, so the colours match the ST7735 panel exactly.
   (The firmware enables ST7735 INVON/0x21; the colour constants below are the
   values the developer tuned to look right on the inverted panel, so we render
   them nominally.) */

(function (global) {
  'use strict';

  // RGB565 (uint16) -> css rgb() string, matching the panel's 5/6/5 channels.
  function rgb565(v) {
    var r = (v >> 11) & 0x1f;
    var g = (v >> 5) & 0x3f;
    var b = v & 0x1f;
    r = Math.round((r * 255) / 31);
    g = Math.round((g * 255) / 63);
    b = Math.round((b * 255) / 31);
    return 'rgb(' + r + ',' + g + ',' + b + ')';
  }

  // Exact firmware colour constants.
  var COL = {
    SHIP_BG: 0x0010,   // dark navy background (spaceShip LCD_Fill)
    SHIP_PLAYER: 0xFF00, // amber 5x5 player block
    WHITE: 0xFFFF,
    BLACK: 0x0000,
    RED: 0xF800,
    GREEN: 0x07E0,
    BLUE: 0x001F,
    YELLOW: 0xFFE0,
    MAGENTA: 0xF81F,
    CYAN: 0x07FF
  };

  function ctxOf(id) {
    var c = document.getElementById(id || 'screen');
    var ctx = c.getContext('2d');
    ctx.imageSmoothingEnabled = false;
    return ctx;
  }

  // Fill a block of logical pixels with an RGB565 value.
  function fill565(ctx, x, y, w, h, v) {
    ctx.fillStyle = (typeof v === 'number') ? rgb565(v) : v;
    ctx.fillRect(x, y, w, h);
  }

  // Tiny 3x5 pixel font for on-screen HUD text (uppercase + digits + a few marks).
  var FONT = {
    '0':['111','101','101','101','111'], '1':['010','110','010','010','111'],
    '2':['111','001','111','100','111'], '3':['111','001','111','001','111'],
    '4':['101','101','111','001','001'], '5':['111','100','111','001','111'],
    '6':['111','100','111','101','111'], '7':['111','001','010','010','010'],
    '8':['111','101','111','101','111'], '9':['111','101','111','001','111'],
    'A':['111','101','111','101','101'], 'B':['110','101','110','101','110'],
    'C':['111','100','100','100','111'], 'D':['110','101','101','101','110'],
    'E':['111','100','110','100','111'], 'F':['111','100','110','100','100'],
    'G':['111','100','101','101','111'], 'H':['101','101','111','101','101'],
    'I':['111','010','010','010','111'], 'J':['011','001','001','101','010'],
    'K':['101','110','100','110','101'], 'L':['100','100','100','100','111'],
    'M':['101','111','111','101','101'], 'N':['101','111','111','111','101'],
    'O':['111','101','101','101','111'], 'P':['111','101','111','100','100'],
    'Q':['111','101','101','111','011'], 'R':['111','101','111','110','101'],
    'S':['111','100','111','001','111'], 'T':['111','010','010','010','010'],
    'U':['101','101','101','101','111'], 'V':['101','101','101','101','010'],
    'W':['101','101','111','111','101'], 'X':['101','101','010','101','101'],
    'Y':['101','101','010','010','010'], 'Z':['111','001','010','100','111'],
    ' ':['000','000','000','000','000'], ':':['000','010','000','010','000'],
    '-':['000','000','111','000','000'], '.':['000','000','000','000','010'],
    '!':['010','010','010','000','010'], '/':['001','001','010','100','100'],
    '>':['100','010','001','010','100'], '<':['001','010','100','010','001']
  };

  function text(ctx, str, x, y, v, sc) {
    sc = sc || 1;
    ctx.fillStyle = (typeof v === 'number') ? rgb565(v) : v;
    var cx = x;
    str = String(str).toUpperCase();
    for (var i = 0; i < str.length; i++) {
      var g = FONT[str[i]] || FONT[' '];
      for (var r = 0; r < 5; r++) {
        for (var c = 0; c < 3; c++) {
          if (g[r][c] === '1') ctx.fillRect(cx + c * sc, y + r * sc, sc, sc);
        }
      }
      cx += (3 + 1) * sc;
    }
  }

  global.PICO = { rgb565: rgb565, COL: COL, ctxOf: ctxOf, fill565: fill565, text: text };
})(window);
