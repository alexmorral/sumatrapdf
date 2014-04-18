/* Copyright 2014 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "Mui.h"

using namespace Gdiplus;
#include "GdiPlusUtil.h"
#include "WinUtil.h"

/*
TODO:
 - add transparent rendering to GDI, see:
   http://stackoverflow.com/questions/1340166/transparency-to-text-in-gdi
   http://theartofdev.wordpress.com/2013/10/24/transparent-text-rendering-with-gdi/
 - propagate clipping region from Graphics to HDC in TextRenderGdi, like this:
        var clip = gfx.Clip.GetHrgn(gfx);
         _hdc = gfx.GetHdc();
        SetBkMode(_hdc, 1);
        SelectClipRgn(_hdc, clip);
        DeleteObject(clip);
*/

/* Note: I would prefer this code be in utils but it depends on mui, so it must
be in mui to avoid circular dependency */

namespace mui {

TextRenderGdi *TextRenderGdi::Create(Graphics *gfx) {
    TextRenderGdi *res = new TextRenderGdi();
    res->gfx = gfx;
    // default to red to make mistakes stand out
    res->SetTextColor(Color(0xff, 0xff, 0x0, 0x0));
    res->CreateHdcForTextMeasure(); // could do lazily, but that's more things to track, so not worth it
    return res;
}

void TextRenderGdi::CreateHdcForTextMeasure() {
    HDC hdc = hdcGfxLocked;
    bool unlock = false;
    if (!hdc) {
        hdc = gfx->GetHDC();
        unlock = true;
    }
    hdcForTextMeasure = CreateCompatibleDC(hdc);
    if (unlock) {
        gfx->ReleaseHDC(hdc);
    }
}

TextRenderGdi::~TextRenderGdi() {
    ReleaseDC(NULL, hdcForTextMeasure);
    CrashIf(hdcGfxLocked); // hasn't been Unlock()ed
}

void TextRenderGdi::SetFont(mui::CachedFont *font) {
    // I'm not sure how expensive SelectFont() is so avoid it just in case
    if (currFont == font) {
        return;
    }
    currFont = font;
    HFONT hfont = font->GetHFont();
    if (hdcGfxLocked) {
        SelectFont(hdcGfxLocked, hfont);
    }
    if (hdcForTextMeasure) {
        SelectFont(hdcForTextMeasure, hfont);
    }
}

float TextRenderGdi::GetCurrFontLineSpacing() {
#if 1
    return currFont->font->GetHeight(gfx);
#else
    CrashIf(!currFont);
    TEXTMETRIC tm;
    GetTextMetrics(hdcForTextMeasure, &tm);
    return (float)tm.tmHeight;
#endif
}

RectF TextRenderGdi::Measure(const WCHAR *s, size_t sLen) {
    SIZE txtSize;
    GetTextExtentPoint32W(hdcForTextMeasure, s, (int) sLen, &txtSize);
    RectF res(0.0f, 0.0f, (float) txtSize.cx, (float) txtSize.cy);
    return res;
}

RectF TextRenderGdi::Measure(const char *s, size_t sLen) {
    size_t strLen = str::Utf8ToWcharBuf(s, sLen, txtConvBuf, dimof(txtConvBuf));
    return Measure(txtConvBuf, strLen);
}

void TextRenderGdi::SetTextColor(Gdiplus::Color col) {
    if (textColor.GetValue() == col.GetValue()) {
        return;
    }
    textColor = col;
    if (hdcGfxLocked) {
        ::SetTextColor(hdcGfxLocked, col.ToCOLORREF());
    }
}

void TextRenderGdi::SetTextBgColor(Gdiplus::Color col) {
    if (textBgColor.GetValue() == col.GetValue()) {
        return;
    }
    textBgColor = col;
    if (hdcGfxLocked) {
        ::SetBkColor(hdcGfxLocked, textBgColor.ToCOLORREF());
    }
}

void TextRenderGdi::Lock() {
    CrashIf(hdcGfxLocked);
    hdcGfxLocked = gfx->GetHDC();
    SelectFont(hdcGfxLocked, currFont);
    ::SetTextColor(hdcGfxLocked, textColor.ToCOLORREF());
    ::SetBkColor(hdcGfxLocked, textBgColor.ToCOLORREF());
}

void TextRenderGdi::Unlock() {
    CrashIf(!hdcGfxLocked);
    gfx->ReleaseHDC(hdcGfxLocked);
    hdcGfxLocked = NULL;
}

void TextRenderGdi::Draw(const WCHAR *s, size_t sLen, RectF& bb, bool isRtl) {
#if 0
    DrawTransparent(s, sLen, bb, isRtl);
#else
    CrashIf(!hdcGfxLocked); // hasn't been Lock()ed
    int x = (int) bb.X;
    int y = (int) bb.Y;
    UINT opts = ETO_OPAQUE;
    if (isRtl)
        opts = opts | ETO_RTLREADING;
    ExtTextOutW(hdcGfxLocked, x, y, opts, NULL, s, (int)sLen, NULL);
#endif
}

void TextRenderGdi::Draw(const char *s, size_t sLen, RectF& bb, bool isRtl) {
#if 0
    DrawTransparent(s, sLen, bb, isRtl);
#else
    size_t strLen = str::Utf8ToWcharBuf(s, sLen, txtConvBuf, dimof(txtConvBuf));
    return Draw(txtConvBuf, strLen, bb, isRtl);
#endif
}

// based on http://theartofdev.wordpress.com/2013/10/24/transparent-text-rendering-with-gdi/,
// TODO: look into using http://theartofdev.wordpress.com/2014/01/12/gdi-text-rendering-to-image/
// TODO: doesn't actually look good (i.e. similar to DrawText when using transparent SetBkMode())
// which kind of makes sense, because I'm using transparent mode to draw to in-memory bitmap as well
// TODO: doesn't actually do alpha bf.SourceConstantAlpha > 4 looks the same, values 1-4 produce
// different, but not expected, results
// TODO: the bitmap should be cached so that we don't call CreateDIBSection every time
// TODO: I would like to figure out a way to draw text without the need to Lock()/Unlock()
// maybe draw to in-memory bitmap, convert to Graphics bitmap and blit that bitmap to
// Graphics object
void TextRenderGdi::DrawTransparent(const WCHAR *s, size_t sLen, RectF& bb, bool isRtl) {
    CrashIf(!hdcGfxLocked); // hasn't been Lock()ed
    //SetBkMode(hdcGfxLocked, 1);

    HDC memHdc = CreateCompatibleDC(hdcGfxLocked);
    SetBkMode(memHdc, TRANSPARENT);
    int x = (int) bb.X;
    int y = (int) bb.Y;
    int dx = (int) bb.Width;
    int dy = (int) bb.Height;

    BITMAPINFO bmi = { };
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = dx;
    bmi.bmiHeader.biHeight = dy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = dx * dy * 4; // doesn't seem necessary?

    unsigned char *bmpData = NULL;
    HBITMAP dib = CreateDIBSection(memHdc, &bmi, DIB_RGB_COLORS, (void **)&bmpData, NULL, 0);
    if (!dib)
        return;
    memset(bmpData, 0, bmi.bmiHeader.biSizeImage);
    SelectObject(memHdc, dib);
    //BitBlt(memHdc, 0, 0, dx, dy, hdcGfxLocked, x, y, SRCCOPY);
    SelectObject(memHdc, currFont);
    ::SetTextColor(memHdc, textColor.ToCOLORREF());

#if 0
    TextOut(memHdc, 0, 0, s, sLen);
#else
    UINT opts = 0; //ETO_OPAQUE;
    if (isRtl)
        opts = opts | ETO_RTLREADING;
    ExtTextOutW(memHdc, 0, 0, opts, NULL, s, (int)sLen, NULL);
#endif

    BLENDFUNCTION bf = {};
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.AlphaFormat = 0; // 0 - ignore source alpha, AC_SRC_ALPHA (1) - use source alpha
    bf.SourceConstantAlpha = 0x3; //textColor.GetA();
    AlphaBlend(hdcGfxLocked, x, y, dx, dy, memHdc, 0, 0, dx, dy, bf);
    DeleteObject(dib);
    DeleteDC(memHdc);
}

void TextRenderGdi::DrawTransparent(const char *s, size_t sLen, RectF& bb, bool isRtl) {
    size_t strLen = str::Utf8ToWcharBuf(s, sLen, txtConvBuf, dimof(txtConvBuf));
    return DrawTransparent(txtConvBuf, strLen, bb, isRtl);
}

TextRenderGdiplus *TextRenderGdiplus::Create(Graphics *gfx, RectF (*measureAlgo)(Graphics *g, Font *f, const WCHAR *s, int len)) {
    TextRenderGdiplus *res = new TextRenderGdiplus();
    res->gfx = gfx;
    res->currFont = NULL;
    if (NULL == measureAlgo)
        res->measureAlgo = MeasureTextAccurate;
    else
        res->measureAlgo = measureAlgo;
    // default to red to make mistakes stand out
    res->SetTextColor(Color(0xff, 0xff, 0x0, 0x0));
    return res;
}

void TextRenderGdiplus::SetFont(mui::CachedFont *font) {
    CrashIf(!font->font);
    currFont = font;
}

float TextRenderGdiplus::GetCurrFontLineSpacing() {
    return currFont->font->GetHeight(gfx);
}

RectF TextRenderGdiplus::Measure(const WCHAR *s, size_t sLen) {
    CrashIf(!currFont);
    return MeasureText(gfx, currFont->font, s, sLen, measureAlgo);
}

RectF TextRenderGdiplus::Measure(const char *s, size_t sLen) {
    CrashIf(!currFont);
    size_t strLen = str::Utf8ToWcharBuf(s, sLen, txtConvBuf, dimof(txtConvBuf));
    return MeasureText(gfx, currFont->font, txtConvBuf, strLen, measureAlgo);
}

TextRenderGdiplus::~TextRenderGdiplus() {
    ::delete textColorBrush;
}

void TextRenderGdiplus::SetTextColor(Gdiplus::Color col) {
    if (textColor.GetValue() == col.GetValue()) {
        return;
    }
    textColor = col;
    ::delete textColorBrush;
    textColorBrush = ::new SolidBrush(col);
}

void TextRenderGdiplus::Draw(const WCHAR *s, size_t sLen, RectF& bb, bool isRtl) {
    PointF pos;
    bb.GetLocation(&pos);
    if (!isRtl) {
        gfx->DrawString(s, (INT)sLen, currFont->font, pos, NULL, textColorBrush);
    } else {
        StringFormat rtl;
        rtl.SetFormatFlags(StringFormatFlagsDirectionRightToLeft);
        pos.X += bb.Width;
        gfx->DrawString(s, (INT)sLen, currFont->font, pos, &rtl, textColorBrush);
    }
}

void TextRenderGdiplus::Draw(const char *s, size_t sLen, RectF& bb, bool isRtl) {
    size_t strLen = str::Utf8ToWcharBuf(s, sLen, txtConvBuf, dimof(txtConvBuf));
    Draw(txtConvBuf, strLen, bb, isRtl);
}

ITextRender *CreateTextRender(TextRenderMethod method, Graphics *gfx) {

    if (TextRenderMethodGdiplus == method) {
        return TextRenderGdiplus::Create(gfx);
    }
    if (TextRenderMethodGdiplusQuick == method) {
        return TextRenderGdiplus::Create(gfx, MeasureTextQuick);
    }
    if (TextRenderMethodGdi == method) {
        return TextRenderGdi::Create(gfx);
    }
    CrashIf(true);
    return NULL;
}

// returns number of characters of string s that fits in a given width dx
// note: could be speed up a bit because in our use case we already know
// the width of the whole string so we could supply it to the function, but
// this shouldn't happen often, so that's fine. It's also possible that
// a smarter approach is possible, but this usually only does 3 MeasureText
// calls, so it's not that bad
size_t StringLenForWidth(ITextRender *textMeasure, const WCHAR *s, size_t len, float dx)
{
    RectF r = textMeasure->Measure(s, len);
    if (r.Width <= dx)
        return len;
    // make the best guess of the length that fits
    size_t n = (size_t)((dx / r.Width) * (float)len);
    CrashIf((0 == n) || (n > len));
    r = textMeasure->Measure(s, n);
    // find the length len of s that fits within dx iff width of len+1 exceeds dx
    int dir = 1; // increasing length
    if (r.Width > dx)
        dir = -1; // decreasing length
    for (;;) {
        n += dir;
        r = textMeasure->Measure(s, n);
        if (1 == dir) {
            // if advancing length, we know that previous string did fit, so if
            // the new one doesn't fit, the previous length was the right one
            if (r.Width > dx)
                return n - 1;
        } else {
            // if decreasing length, we know that previous string didn't fit, so if
            // the one one fits, it's of the correct length
            if (r.Width < dx)
                return n;
        }
    }
}

// TODO: not quite sure why spaceDx1 != spaceDx2, using spaceDx2 because
// is smaller and looks as better spacing to me
REAL GetSpaceDx(ITextRender *textMeasure)
{
    RectF bbox;
#if 0
    bbox = textMeasure->Measure(L" ", 1, algo);
    REAL spaceDx1 = bbox.Width;
    return spaceDx1;
#else
    // this method seems to return (much) smaller size that measuring
    // the space itself
    bbox = textMeasure->Measure(L"wa", 2);
    REAL l1 = bbox.Width;
    bbox = textMeasure->Measure(L"w a", 3);
    REAL l2 = bbox.Width;
    REAL spaceDx2 = l2 - l1;
    return spaceDx2;
#endif
}

}  // namespace mui