// NEOTKO_COLORSCI_TAG_START — P0 (Fase A)
// Implementación. Ports anotados contra el original de Tab.cpp para auditar
// paridad durante la migración de call sites (sesión CS-1).
#include "ColorSci.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace ColorSci {

// --- Espacios de color -----------------------------------------------------

float srgb_to_linear(float c)
{
    // port: lambda `lin`/`to_lin` (Tab.cpp:1329 y 1356)
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float c)
{
    // port: lambda `to_srgb` (Tab.cpp:1359)
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.0031308f ? 12.92f * c
                           : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

Lab rgb_to_lab(const float rgb[3])
{
    // port exacto de mp_rgb_to_lab (Tab.cpp:1327-1339), float en vez de double.
    const float rl = srgb_to_linear(rgb[0]);
    const float gl = srgb_to_linear(rgb[1]);
    const float bl = srgb_to_linear(rgb[2]);
    const float X = rl * 0.4124564f + gl * 0.3575761f + bl * 0.1804375f;
    const float Y = rl * 0.2126729f + gl * 0.7151522f + bl * 0.0721750f;
    const float Z = rl * 0.0193339f + gl * 0.1191920f + bl * 0.9503041f;
    const float xn = X / 0.95047f, yn = Y, zn = Z / 1.08883f;
    auto f = [](float t) {
        return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f;
    };
    return { 116.f * f(yn) - 16.f,
             500.f * (f(xn) - f(yn)),
             200.f * (f(yn) - f(zn)) };
}

float delta_e76(const Lab& x, const Lab& y)
{
    // port exacto de mp_delta_e (Tab.cpp:1341)
    return std::sqrt((x.L - y.L) * (x.L - y.L) +
                     (x.a - y.a) * (x.a - y.a) +
                     (x.b - y.b) * (x.b - y.b));
}

float delta_e2000(const Lab& x, const Lab& y)
{
    // CIEDE2000, implementación estándar (Sharma, Wu & Dalal 2005).
    // kL = kC = kH = 1. Verificar contra el vector de test oficial del paper
    // (34 pares) en la sesión CS-1 antes de migrar mp_suggest.
    const double L1 = x.L, a1 = x.a, b1 = x.b;
    const double L2 = y.L, a2 = y.a, b2 = y.b;
    const double PI = 3.14159265358979323846;

    const double C1 = std::sqrt(a1 * a1 + b1 * b1);
    const double C2 = std::sqrt(a2 * a2 + b2 * b2);
    const double Cbar = 0.5 * (C1 + C2);
    const double Cbar7 = std::pow(Cbar, 7.0);
    const double G = 0.5 * (1.0 - std::sqrt(Cbar7 / (Cbar7 + std::pow(25.0, 7.0))));

    const double a1p = (1.0 + G) * a1;
    const double a2p = (1.0 + G) * a2;
    const double C1p = std::sqrt(a1p * a1p + b1 * b1);
    const double C2p = std::sqrt(a2p * a2p + b2 * b2);

    auto hp = [&](double ap, double b) -> double {
        if (ap == 0.0 && b == 0.0) return 0.0;
        double h = std::atan2(b, ap);
        if (h < 0.0) h += 2.0 * PI;
        return h * 180.0 / PI;          // grados [0, 360)
    };
    const double h1p = hp(a1p, b1);
    const double h2p = hp(a2p, b2);

    const double dLp = L2 - L1;
    const double dCp = C2p - C1p;

    double dhp = 0.0;
    if (C1p * C2p != 0.0) {
        dhp = h2p - h1p;
        if (dhp > 180.0)       dhp -= 360.0;
        else if (dhp < -180.0) dhp += 360.0;
    }
    const double dHp = 2.0 * std::sqrt(C1p * C2p)
                     * std::sin(dhp * PI / 360.0);   // sin(Δh'/2) en rad

    const double Lbp = 0.5 * (L1 + L2);
    const double Cbp = 0.5 * (C1p + C2p);

    double hbp = h1p + h2p;             // media de ángulos h'
    if (C1p * C2p != 0.0) {
        if (std::abs(h1p - h2p) > 180.0)
            hbp += (hbp < 360.0) ? 360.0 : -360.0;
        hbp *= 0.5;
    }

    const double T = 1.0
        - 0.17 * std::cos((hbp - 30.0) * PI / 180.0)
        + 0.24 * std::cos((2.0 * hbp) * PI / 180.0)
        + 0.32 * std::cos((3.0 * hbp + 6.0) * PI / 180.0)
        - 0.20 * std::cos((4.0 * hbp - 63.0) * PI / 180.0);

    const double dTheta = 30.0 * std::exp(-((hbp - 275.0) / 25.0) * ((hbp - 275.0) / 25.0));
    const double Cbp7 = std::pow(Cbp, 7.0);
    const double RC = 2.0 * std::sqrt(Cbp7 / (Cbp7 + std::pow(25.0, 7.0)));
    const double Lbp2 = (Lbp - 50.0) * (Lbp - 50.0);
    const double SL = 1.0 + 0.015 * Lbp2 / std::sqrt(20.0 + Lbp2);
    const double SC = 1.0 + 0.045 * Cbp;
    const double SH = 1.0 + 0.015 * Cbp * T;
    const double RT = -std::sin(2.0 * dTheta * PI / 180.0) * RC;

    const double tL = dLp / SL;
    const double tC = dCp / SC;
    const double tH = dHp / SH;
    return (float)std::sqrt(tL * tL + tC * tC + tH * tH + RT * tC * tH);
}

// --- Composición -----------------------------------------------------------

void blend_stacked(const std::vector<Layer>& layers,
                   const float bg_rgb[3],
                   float out_rgb[3],
                   float* out_transmit)
{
    // port de mp_beer_blend (Tab.cpp:1352-1374), TD per-channel:
    //   t = 0.1^(ratio/td[c]) por canal; td≈0 → capa opaca (pisa el fondo).
    float acc[3], transmit[3] = { 1.f, 1.f, 1.f };
    for (int c = 0; c < 3; ++c)
        acc[c] = srgb_to_linear(bg_rgb[c]);
    for (const Layer& lyr : layers) {
        for (int c = 0; c < 3; ++c) {
            const float lc = srgb_to_linear(lyr.rgb[c]);
            if (lyr.td[c] < 1e-6f) {
                acc[c] = lc;            // opaco: reemplaza (mismo branch que el original)
                transmit[c] = 0.f;
                continue;
            }
            const float t  = std::pow(0.1f, lyr.ratio / lyr.td[c]);
            acc[c]      = lc * (1.f - t) + acc[c] * t;
            transmit[c] = transmit[c] * t;
        }
    }
    for (int c = 0; c < 3; ++c)
        out_rgb[c] = linear_to_srgb(acc[c]);
    if (out_transmit)
        *out_transmit = (transmit[0] + transmit[1] + transmit[2]) / 3.f;
}

void slice_opacity(const Material& m, float ratio, float out_op[3])
{
    for (int c = 0; c < 3; ++c)
        out_op[c] = (m.td[c] < 1e-6f)
            ? 1.f
            : 1.f - std::pow(0.1f, ratio / m.td[c]);
}

void blend_parallel(const std::vector<Slice>& slices,
                    const Material mats[4],
                    float out_rgb[3],
                    float* out_weight)
{
    // port de la agregación de blend_preview_zone (Tab.cpp:5431-5452).
    // El original pondera con UNA opacidad escalar; aquí el color mezcla por
    // canal y el peso escalar es la media de los 3 canales — con TD r=g=b el
    // resultado es bit-a-bit equivalente al legacy (criterio de paridad CS-1).
    float tr = 0.f, tg = 0.f, tb = 0.f, tw = 0.f;
    for (const Slice& s : slices) {
        const Material& m = mats[std::clamp(s.tool, 0, 3)];
        float op[3];
        slice_opacity(m, std::max(0.f, s.ratio), op);
        const float ow = (op[0] + op[1] + op[2]) / 3.f;
        tr += m.rgb[0] * op[0];
        tg += m.rgb[1] * op[1];
        tb += m.rgb[2] * op[2];
        tw += ow;
    }
    if (tw < 1e-6f) {
        // mismo fallback gris 180 que el original (Tab.cpp:5444-5446)
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 180.f / 255.f;
        if (out_weight) *out_weight = 0.f;
        return;
    }
    out_rgb[0] = std::min(1.f, tr / tw);
    out_rgb[1] = std::min(1.f, tg / tw);
    out_rgb[2] = std::min(1.f, tb / tw);
    if (out_weight) *out_weight = tw;
}

// --- Construcción ----------------------------------------------------------

Material material_from_hex(const std::string& hex_rgb,
                           float td_r, float td_g, float td_b)
{
    Material m;
    m.td = { td_r, td_g, td_b };
    std::string s = hex_rgb;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() >= 6) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        bool ok = true;
        int v[3];
        for (int i = 0; i < 3; ++i) {
            const int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
            if (hi < 0 || lo < 0) { ok = false; break; }
            v[i] = hi * 16 + lo;
        }
        if (ok) {
            m.rgb = { v[0] / 255.f, v[1] / 255.f, v[2] / 255.f };
            return m;
        }
    }
    m.rgb = { 0.5f, 0.5f, 0.5f };   // fallback gris (paridad tool_colour)
    return m;
}

Material material_from_hex(const std::string& hex_rgb, float td_scalar)
{
    return material_from_hex(hex_rgb, td_scalar, td_scalar, td_scalar);
}

} // namespace ColorSci
} // namespace Slic3r
// NEOTKO_COLORSCI_TAG_END
