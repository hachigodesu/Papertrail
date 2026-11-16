// Text set at an angle, drawn from its outlines.
//
// FreeType 2.15.0 drops the transform MuPDF gives it for a glyph turned by about 45 degrees (43 to 45, 133 to 135,
// and so on): FT_Set_Transform finds the matrix degenerate and uses none. MuPDF then draws each such glyph unturned
// at the 1024-point size it works at, so a figure's axis labels set diagonally become black shapes over the page.
// Ubuntu's FreeType 2.13 doesn't, and Arch's and MSYS2's 2.15.0 do. MuPDF's outlines of the same glyphs are right,
// since it asks FreeType for them unscaled and applies the matrix itself.
//
// Only for code that includes MuPDF; the rest of the program never sees these types.

#pragma once

#include <mupdf/fitz.h>

namespace papertrail::engine {

// Has `drawDevice`, made by fz_new_draw_device, fill, stroke and clip with the outlines of any text that's neither
// upright nor turned a quarter, rather than with FreeType's pictures of its glyphs. Text in a Type 3 font, or set
// vertically, is left to the draw device. Rotated text is rare enough, figure labels mostly, that the speed doesn't
// matter.
void drawTurnedTextFromOutlines(fz_device *drawDevice);

} // namespace papertrail::engine
