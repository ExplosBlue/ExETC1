// File: Etc1.cpp - Fast, high quality ETC1 block packer/unpacker - Rich Geldreich <richgel99@gmail.com>
// Please see ZLIB license at the end of Etc1.h.
//
// For more information Ericsson Texture Compression (ETC/ETC1), see:
// http://www.khronos.org/registry/gles/extensions/OES/OES_compressed_ETC1_RGB8_texture.txt
//
// v1.04 - 5/15/14 - Fix signed vs. unsigned subtraction problem (noticed when compiled with gcc) in initEtc1Tables().
//         This issue would cause an assert when this func. was called in debug. (Note this module was developed/testing with MSVC,
//         I still need to test it thoroughly when compiled with gcc.)
//
// v1.03 - 5/12/13 - Initial public release
#include "Etc1.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>

#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64) || defined(_M_AMD64)
#include <immintrin.h>
#endif

#ifdef _MSC_VER
#pragma warning(disable : 4201) //  nonstandard extension used : nameless struct/union
#endif

#if defined(_DEBUG) || defined(DEBUG)
constexpr bool sBuildDebug = true;
#else
constexpr bool sBuildDebug = false;
#endif

namespace Etc1 {

constexpr uint32_t sUint32Max = std::numeric_limits<uint32_t>::max();
constexpr uint64_t sUint64Max = std::numeric_limits<uint64_t>::max();

// ---------------------------------------------------------------------------
// ETC1 block format constants.
// ---------------------------------------------------------------------------

constexpr uint32_t sBlockWidth = 4;                                 // block width in pixels
constexpr uint32_t sBlockHeight = 4;                                // block height in pixels
constexpr uint32_t sPixelsPerBlock = sBlockWidth * sBlockHeight;    // 4x4 == 16 pixels
constexpr uint32_t sSubblockPixels = 8;                             // 4x2 pixels per subblock
constexpr uint32_t sBlockBytes = 8;                                 // each block packs to 8 bytes
constexpr uint32_t sBitsPerByte = 8;                                // bits per byte (and per 8-bit channel)
constexpr uint32_t sBitsPerByteLog2 = 3;                            // exponent such that 1 << sBitsPerByteLog2 == sBitsPerByte
constexpr uint32_t sByteMask = 0xFF;                                // low byte mask
constexpr uint32_t sColorChannelMax = 255;                          // largest 8-bit channel value
constexpr uint32_t sBaseColor4Bits = 4;                             // individual-mode base color precision
constexpr uint32_t sBaseColor4Max = 15;                             // 2^sBaseColor4Bits - 1
constexpr uint32_t sBaseColor5Bits = 5;                             // differential-mode base color precision
constexpr uint32_t sBaseColor5Max = 31;                             // 2^sBaseColor5Bits - 1
constexpr uint32_t sDelta3Bits = 3;                                 // differential delta color precision
constexpr uint32_t sDelta3Mask = 7;                                 // 2^sDelta3Bits - 1
constexpr uint32_t sIntenMask = 7;                                  // 3-bit intensity table index mask
constexpr uint32_t sSelectorMask = 3;                               // 2-bit selector mask
constexpr uint32_t sHistogramBins = 256;                            // radix sort: one histogram bin per byte value
constexpr uint16_t sEtc1TableTerminator = 0xFFFF;                   // marks the end of a packed lookup-table row

// ---------------------------------------------------------------------------
// Color channel pack/unpack helpers.
// ---------------------------------------------------------------------------

// A packed 16-bit color stores blue in the least-significant field, then green, then red,
// matching the ETC1 bit layout: b | (g << bits) | (r << (2 * bits)).
struct UnpackedColor {
    uint32_t r;
    uint32_t g;
    uint32_t b;
};

static constexpr uint16_t packColorChannels(uint32_t b, uint32_t g, uint32_t r, uint32_t bits) {
    return static_cast<uint16_t>(b | (g << bits) | (r << (2 * bits)));
}

static constexpr UnpackedColor unpackColorChannels(uint16_t packed, uint32_t bits, uint32_t mask) {
    return {
        (packed >> (2 * bits)) & mask,
        (packed >> bits) & mask,
        packed & mask,
    };
}

// Expands a 5-bit channel (0..31) to the full 8-bit range.
static constexpr uint32_t scaleColor5To8(uint32_t v) {
    return (v << 3) | (v >> 2);
}

// Expands a 4-bit channel (0..15) to the full 8-bit range.
static constexpr uint32_t scaleColor4To8(uint32_t v) {
    return (v << 4) | v;
}

// Round-scales an 8-bit channel down to a base color of the given precision (4 or 5 bits).
static constexpr uint32_t scaleColorToBase(uint32_t channel, uint32_t max, uint32_t bias) {
    return (channel * max + bias) / sColorChannelMax;
}

template <typename T>
constexpr T minimum(T a, T b) {
    return (a < b) ? a : b;
}
template <typename T>
constexpr T minimum(T a, T b, T c) {
    return minimum(minimum(a, b), c);
}
template <typename T>
constexpr T maximum(T a, T b) {
    return (a > b) ? a : b;
}
template <typename T>
constexpr T maximum(T a, T b, T c) {
    return maximum(maximum(a, b), c);
}
template <typename T>
constexpr T clamp(T value, T low, T high) {
    return (value < low) ? low : ((value > high) ? high : value);
}
template <typename T>
constexpr T square(T value) {
    return value * value;
}

template <typename T>
void zero_object(T& obj) {
    std::memset(&obj, 0, sizeof(obj));
}

enum NoClampTag {
    NoClamp
};

struct ColorQuad {
    static inline int clamp(int v) {
        if (v & 0xFFFFFF00U) {
            v = (~(static_cast<int>(v) >> 31)) & 0xFF;
        }
        return v;
    }

    struct ComponentTraits {
        enum {
            Signed  = false,
            Float   = false,
            Min     = 0U,
            Max     = sColorChannelMax
        };
    };

public:
    using Component = unsigned char;
    using Parameter = int;

    enum {
        NumComps = 4
    };

    union {
        struct
        {
            Component r;
            Component g;
            Component b;
            Component a;
        };

        std::array<Component, NumComps> c;

        uint32_t mU32;
    };

    inline ColorQuad() {}

    inline ColorQuad(const ColorQuad& other) = default;

    explicit inline ColorQuad(Parameter y, Parameter alpha = ComponentTraits::Max) {
        set(y, alpha);
    }

    inline ColorQuad(Parameter red, Parameter green, Parameter blue, Parameter alpha = ComponentTraits::Max) {
        set(red, green, blue, alpha);
    }

    explicit inline ColorQuad(NoClampTag, Parameter y, Parameter alpha = ComponentTraits::Max) {
        setNoClampYAlpha(y, alpha);
    }

    inline ColorQuad(NoClampTag, Parameter red, Parameter green, Parameter blue, Parameter alpha = ComponentTraits::Max) {
        setNoClampRgba(red, green, blue, alpha);
    }

    inline void clear() {
        mU32 = 0;
    }

    inline ColorQuad& operator=(const ColorQuad& other) = default;

    inline ColorQuad& setRgb(const ColorQuad& other) {
        r = other.r;
        g = other.g;
        b = other.b;
        return *this;
    }

    inline ColorQuad& operator=(Parameter y) {
        set(y, ComponentTraits::Max);
        return *this;
    }

    inline ColorQuad& set(Parameter y, Parameter alpha = ComponentTraits::Max) {
        y = clamp(y);
        alpha = clamp(alpha);
        r = static_cast<Component>(y);
        g = static_cast<Component>(y);
        b = static_cast<Component>(y);
        a = static_cast<Component>(alpha);
        return *this;
    }

    inline ColorQuad& setNoClampYAlpha(Parameter y, Parameter alpha = ComponentTraits::Max) {
        assert((y >= ComponentTraits::Min) && (y <= ComponentTraits::Max));
        assert((alpha >= ComponentTraits::Min) && (alpha <= ComponentTraits::Max));

        r = static_cast<Component>(y);
        g = static_cast<Component>(y);
        b = static_cast<Component>(y);
        a = static_cast<Component>(alpha);
        return *this;
    }

    inline ColorQuad& set(Parameter red, Parameter green, Parameter blue, Parameter alpha = ComponentTraits::Max) {
        r = static_cast<Component>(clamp(red));
        g = static_cast<Component>(clamp(green));
        b = static_cast<Component>(clamp(blue));
        a = static_cast<Component>(clamp(alpha));
        return *this;
    }

    inline ColorQuad& setNoClampRgba(Parameter red, Parameter green, Parameter blue, Parameter alpha) {
        assert((red >= ComponentTraits::Min) && (red <= ComponentTraits::Max));
        assert((green >= ComponentTraits::Min) && (green <= ComponentTraits::Max));
        assert((blue >= ComponentTraits::Min) && (blue <= ComponentTraits::Max));
        assert((alpha >= ComponentTraits::Min) && (alpha <= ComponentTraits::Max));

        r = static_cast<Component>(red);
        g = static_cast<Component>(green);
        b = static_cast<Component>(blue);
        a = static_cast<Component>(alpha);
        return *this;
    }

    inline ColorQuad& setNoClampRgb(Parameter red, Parameter green, Parameter blue) {
        assert((red >= ComponentTraits::Min) && (red <= ComponentTraits::Max));
        assert((green >= ComponentTraits::Min) && (green <= ComponentTraits::Max));
        assert((blue >= ComponentTraits::Min) && (blue <= ComponentTraits::Max));

        r = static_cast<Component>(red);
        g = static_cast<Component>(green);
        b = static_cast<Component>(blue);
        return *this;
    }

    static inline Parameter getMinComp() { return ComponentTraits::Min; }
    static inline Parameter getMaxComp() { return ComponentTraits::Max; }
    static inline bool getCompsAreSigned() { return ComponentTraits::Signed; }

    inline Component operator[](uint32_t i) const {
        assert(i < NumComps);
        return c[i];
    }
    inline Component& operator[](uint32_t i) {
        assert(i < NumComps);
        return c[i];
    }

    inline ColorQuad& setComponent(uint32_t i, Parameter f) {
        assert(i < NumComps);
        c[i] = static_cast<Component>(clamp(f));
        return *this;
    }

    inline ColorQuad& setGrayscale(Parameter l) {
        auto x = static_cast<Component>(clamp(l));
        c[0] = x;
        c[1] = x;
        c[2] = x;
        return *this;
    }

    inline ColorQuad& clamp(const ColorQuad& l, const ColorQuad& h) {
        for (uint32_t i = 0; i < NumComps; i++) {
            c[i] = static_cast<Component>(Etc1::clamp<Parameter>(c[i], l[i], h[i]));
        }
        return *this;
    }

    inline ColorQuad& clamp(Parameter l, Parameter h) {
        for (unsigned char& i : c) {
            i = static_cast<Component>(Etc1::clamp<Parameter>(i, l, h));
        }
        return *this;
    }

    // Returns CCIR 601 luma.
    inline Parameter getLuma() const {
        return static_cast<Parameter>((19595U * r + 38470U * g + 7471U * b + 32768U) >> 16U);
    }

    // Returns REC 709 luma.
    inline Parameter getLumaRec709() const {
        return static_cast<Parameter>((13938U * r + 46869U * g + 4729U * b + 32768U) >> 16U);
    }

    inline uint32_t squaredDistanceRgb(const ColorQuad& c) const {
        return Etc1::square(r - c.r) + Etc1::square(g - c.g) + Etc1::square(b - c.b);
    }

    inline uint32_t squaredDistanceRgba(const ColorQuad& c) const {
        return Etc1::square(r - c.r) + Etc1::square(g - c.g) + Etc1::square(b - c.b) + Etc1::square(a - c.a);
    }

    inline bool rgbEquals(const ColorQuad& rhs) const {
        return (r == rhs.r) && (g == rhs.g) && (b == rhs.b);
    }

    inline bool operator==(const ColorQuad& rhs) const {
        return mU32 == rhs.mU32;
    }

    ColorQuad& operator+=(const ColorQuad& other) {
        for (uint32_t i = 0; i < 4; i++) {
            c[i] = static_cast<Component>(clamp(c[i] + other.c[i]));
        }
        return *this;
    }

    ColorQuad& operator-=(const ColorQuad& other) {
        for (uint32_t i = 0; i < 4; i++) {
            c[i] = static_cast<Component>(clamp(c[i] - other.c[i]));
        }
        return *this;
    }

    friend ColorQuad operator+(const ColorQuad& lhs, const ColorQuad& rhs) {
        ColorQuad result(lhs);
        result += rhs;
        return result;
    }

    friend ColorQuad operator-(const ColorQuad& lhs, const ColorQuad& rhs) {
        ColorQuad result(lhs);
        result -= rhs;
        return result;
    }
}; // class ColorQuad


struct Vec3F {
    std::array<float, 3> mS;

    inline Vec3F() = default;

    inline Vec3F(float s) {
        mS[0] = s;
        mS[1] = s;
        mS[2] = s;
    }

    inline Vec3F(float x, float y, float z) {
        mS[0] = x;
        mS[1] = y;
        mS[2] = z;
    }

    inline float operator[](uint32_t i) const {
        assert(i < 3);
        return mS[i];
    }

    inline Vec3F& operator+=(const Vec3F& other) {
        for (uint32_t i = 0; i < 3; i++) {
            mS[i] += other.mS[i];
        }
        return *this;
    }

    inline Vec3F& operator*=(float s) {
        for (float& comp : mS) {
            comp *= s;
        }
        return *this;
    }
}; // struct Vec3F


enum EtcConstants {
    BytesPerBlock = 8U,

    SelectorBits   = 2U,
    SelectorValues = 1U << SelectorBits,
    SelectorMask   = SelectorValues - 1U,

    BlockShift = 2U,
    BlockSize  = 1U << BlockShift,

    LSBSelectorIndicesBitOffset = 0,
    MSBSelectorIndicesBitOffset = 16,

    FlipBitOffset = 32,
    DiffBitOffset = 33,

    IntenModifierNumBits             = 3,
    IntenModifierValues              = 1 << IntenModifierNumBits,
    RightIntenModifierTableBitOffset = 34,
    LeftIntenModifierTableBitOffset  = 37,

    // Base+Delta encoding (5 bit bases, 3 bit delta)
    BaseColorCompNumBits = 5,
    BaseColorCompMax     = 1 << BaseColorCompNumBits,

    DeltaColorCompNumBits = 3,
    DeltaColorComp        = 1 << DeltaColorCompNumBits,
    DeltaColorCompMax     = 1 << DeltaColorCompNumBits,

    BaseColor5RBitOffset = 59,
    BaseColor5GBitOffset = 51,
    BaseColor5BBitOffset = 43,

    DeltaColor3RBitOffset = 56,
    DeltaColor3GBitOffset = 48,
    DeltaColor3BBitOffset = 40,

    // Absolute (non-delta) encoding (two 4-bit per component bases)
    AbsColorCompNumBits = 4,
    AbsColorCompMax     = 1 << AbsColorCompNumBits,

    AbsColor4R1BitOffset = 60,
    AbsColor4G1BitOffset = 52,
    AbsColor4B1BitOffset = 44,

    AbsColor4R2BitOffset = 56,
    AbsColor4G2BitOffset = 48,
    AbsColor4B2BitOffset = 40,

    ColorDeltaMin = -4,
    ColorDeltaMax = 3,
}; // EtcConstants


static constexpr std::array<std::array<int32_t, SelectorValues>, IntenModifierValues> sEtc1IntenTables = {{
    {{ -8,  -2,  2,  8}}, {{-17,  -5,  5, 17}}, {{-29,   -9,  9,  29}}, {{-42,  -13, 13,  42}},
    {{-60, -18, 18, 60}}, {{-80, -24, 24, 80}}, {{-106, -33, 33, 106}}, {{-183, -47, 47, 183}}
}};

static const std::array<uint8_t, SelectorValues> sEtc1ToSelectorIndex = {2, 3, 1, 0};
static const std::array<uint8_t, SelectorValues> sSelectorIndexToEtc1 = {3, 2, 0, 1};

// sColor8ToEtcConfig[color][index] = For each 8-bit color value, a 0xFFFF-terminated list of packed
// ETC1 diff/intensity-table/selector/base-color configs that decode to that color.
// To pack: diff | (intenTable << 1) | (selector << 4) | (packedColor << 8).
//
// The lists are generated at compile time: for each (diff, intenTable, selector) the lowest
// packedColor whose decode equals the target is emitted. That makes the boundary values 0/255 keep
// only the first of their clamp-saturated matches, and it reproduces the original hand-generated
// tables bit for bit (order: diff, intenTable, packedColor, selector).
//
// Generation is split into small consteval units so it stays within clang's default constexpr
// step budget: the per-config decode values are materialized once in sEtc1DecodeGrid, then both
// config tables are filled in a single pass and the inverse lookup is built from the monotonic
// grid rows (see makeEtc1InverseLookup).
// Decodes a packed ETC1 color to an 8-bit value; constexpr so it also drives the compile-time table generation.
static constexpr uint32_t etc1DecodeValue(uint32_t diff, uint32_t intensity, uint32_t selector, uint32_t packedC) {
    assert((diff < 2) && (intensity < 8) && (selector < 4) && (packedC < (diff ? 32 : 16)));
    int c = 0;
    if (diff) {
        c = static_cast<int>(packedC >> 2) | static_cast<int>(packedC << 3);
    } else {
        c = static_cast<int>(packedC) | (static_cast<int>(packedC) << 4);
    }
    c += sEtc1IntenTables[intensity][selector];
    c = Etc1::clamp<int>(c, 0, 255);
    return c;
}

// Materializes every (diff, intenTable, selector, packedColor) decode so the table generators
// below avoid re-running the comparatively expensive per-config decode in their own consteval units.
using EtcDecodeGrid = std::array<std::array<std::array<std::array<uint8_t, 32>, SelectorValues>, IntenModifierValues>, 2>;

consteval EtcDecodeGrid makeEtc1DecodeGrid() {
    EtcDecodeGrid grid{};
    for (uint32_t diff = 0; diff < 2; diff++) {
        const uint32_t packedLimit = diff ? 32U : 16U;
        for (uint32_t intensity = 0; intensity < IntenModifierValues; intensity++) {
            for (uint32_t selector = 0; selector < SelectorValues; selector++) {
                for (uint32_t packedColor = 0; packedColor < packedLimit; packedColor++) {
                    grid[diff][intensity][selector][packedColor] = static_cast<uint8_t>(etc1DecodeValue(diff, intensity, selector, packedColor));
                }
            }
        }
    }
    return grid;
}

// One pass over every config; a per-(selector, target) emission guard keeps only the lowest
// packedColor decoding to each target, so the 0/255 rows hold just their first clamp-saturated match.
consteval void fillColor8ToEtcConfigRows(const EtcDecodeGrid& grid, std::array<std::array<uint16_t, 33>, 2>& table0To255, std::array<std::array<uint16_t, 12>, 254>& table1To254) {
    std::array<uint32_t, 256> counts{};
    for (uint32_t diff = 0; diff < 2; diff++) {
        const uint32_t packedLimit = diff ? 32U : 16U;
        for (uint32_t intensity = 0; intensity < IntenModifierValues; intensity++) {
            std::array<std::array<bool, 256>, SelectorValues> emitted{};
            for (uint32_t packedColor = 0; packedColor < packedLimit; packedColor++) {
                for (uint32_t selector = 0; selector < SelectorValues; selector++) {
                    const uint32_t target = grid[diff][intensity][selector][packedColor];
                    if (emitted[selector][target]) {
                        continue;
                    }
                    emitted[selector][target] = true;
                    const auto entry = static_cast<uint16_t>(diff | (intensity << 1) | (selector << 4) | (packedColor << 8));
                    if (target == 0) {
                        table0To255[0][counts[0]++] = entry;
                    } else if (target == sColorChannelMax) {
                        table0To255[1][counts[255]++] = entry;
                    } else {
                        table1To254[target - 1][counts[target]++] = entry;
                    }
                }
            }
        }
    }
    table0To255[0][counts[0]] = sEtc1TableTerminator;
    table0To255[1][counts[255]] = sEtc1TableTerminator;
    for (uint32_t t = 1; t < 255; t++) {
        table1To254[t - 1][counts[t]] = sEtc1TableTerminator;
    }
}

struct EtcColorConfigTables {
    std::array<std::array<uint16_t, 33>, 2> to0To255;
    std::array<std::array<uint16_t, 12>, 254> to1To254;
};

consteval EtcColorConfigTables makeColor8ToEtcConfigTables(const EtcDecodeGrid& grid) {
    EtcColorConfigTables tables{};
    fillColor8ToEtcConfigRows(grid, tables.to0To255, tables.to1To254);
    return tables;
}

static constexpr auto sEtc1DecodeGrid = makeEtc1DecodeGrid();
static constexpr auto sEtcColorConfigTables = makeColor8ToEtcConfigTables(sEtc1DecodeGrid);
static constexpr auto sColor8ToEtcConfig0To255 = sEtcColorConfigTables.to0To255;
static constexpr auto sColor8ToEtcConfig1To254 = sEtcColorConfigTables.to1To254;

// Given an ETC1 diff/intenTable/selector, and an 8-bit desired color, encodes the best packedColor in
// the low byte and its abs error in the high byte. Generated at compile time like the tables above.
// The decode grid rows are monotonic, so each color is assigned to its nearest distinct decode value
// (ties going to the lower packedColor, matching the original ascending scan) without a per-color scan.
consteval std::array<std::array<uint16_t, 256>, static_cast<std::size_t>(2) * IntenModifierValues * SelectorValues> makeEtc1InverseLookup(const EtcDecodeGrid& grid) {
    std::array<std::array<uint16_t, 256>, static_cast<std::size_t>(2) * IntenModifierValues * SelectorValues> table{};
    for (uint32_t diff = 0; diff < 2; diff++) {
        const uint32_t limit = diff ? 32 : 16;
        for (uint32_t intensity = 0; intensity < IntenModifierValues; intensity++) {
            for (uint32_t selector = 0; selector < SelectorValues; selector++) {
                const uint32_t inverseTableIndex = diff + (intensity << 1) + (selector << 4);
                auto& row = table[inverseTableIndex];
                uint32_t rangeStart = 0;
                uint32_t prevPackedC = 0;
                uint32_t prevValue = grid[diff][intensity][selector][0];
                for (uint32_t packedC = 1; packedC < limit; packedC++) {
                    const uint32_t value = grid[diff][intensity][selector][packedC];
                    if (value == prevValue) {
                        continue;
                    }
                    const uint32_t split = (prevValue + value) / 2;
                    for (uint32_t color = rangeStart; color <= split; color++) {
                        const uint32_t err = color > prevValue ? color - prevValue : prevValue - color;
                        row[color] = static_cast<uint16_t>(prevPackedC | (err << 8));
                    }
                    rangeStart = split + 1;
                    prevPackedC = packedC;
                    prevValue = value;
                }
                for (uint32_t color = rangeStart; color < 256; color++) {
                    const uint32_t err = color > prevValue ? color - prevValue : prevValue - color;
                    row[color] = static_cast<uint16_t>(prevPackedC | (err << 8));
                }
            }
        }
    }
    return table;
}

static constexpr auto sEtc1InverseLookup = makeEtc1InverseLookup(sEtc1DecodeGrid); // [diff/intenTable/selector][desired_color]

static constexpr int mul8Bit(int a, int b) {
    const int t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

// Maps a clamped 0..255 value to a 5-bit base color (expanded), offset so index 8 == value 0, for
// 555 dithering. Generated at compile time.
consteval std::array<uint8_t, 256 + 16> makeQuant5Tab() {
    std::array<uint8_t, 256 + 16> tab{};
    for (int i = 0; i < 256 + 16; i++) {
        const int v = Etc1::clamp<int>(i - 8, 0, 255);
        const int q = mul8Bit(v, 31);
        tab[i] = static_cast<uint8_t>(scaleColor5To8(static_cast<uint32_t>(q)));
    }
    return tab;
}

static constexpr auto sQuant5Tab = makeQuant5Tab();

struct Etc1Block {
    // big endian uint64_t:
    // bit ofs:  56  48  40  32  24  16   8   0
    // byte ofs: b0, b1, b2, b3, b4, b5, b6, b7
    union {
        uint64_t mUint64;
        std::array<uint8_t, sBlockBytes> mBytes;
    };

    std::array<uint8_t, 2> mLowColor;
    std::array<uint8_t, 2> mHighColor;

    static constexpr uint32_t NumSelectorBytes = 4;
    std::array<uint8_t, NumSelectorBytes> mSelectors;

    void clear() {
        zero_object(*this);
    }

    inline uint32_t getByteBits(uint32_t ofs, uint32_t num) const {
        assert((ofs + num) <= 64U);
        assert(num && (num <= sBitsPerByte));
        assert((ofs >> sBitsPerByteLog2) == ((ofs + num - 1) >> sBitsPerByteLog2));
        const uint32_t byteOfs = (sBlockBytes - 1) - (ofs >> sBitsPerByteLog2);
        const uint32_t byteBitOfs = ofs & (sBitsPerByte - 1);
        return (mBytes[byteOfs] >> byteBitOfs) & ((1 << num) - 1);
    }

    inline void setByteBits(uint32_t ofs, uint32_t num, uint32_t bits) {
        assert((ofs + num) <= 64U);
        assert(num && (num < 32U));
        assert((ofs >> sBitsPerByteLog2) == ((ofs + num - 1) >> sBitsPerByteLog2));
        assert(bits < (1U << num));
        const uint32_t byteOfs = (sBlockBytes - 1) - (ofs >> sBitsPerByteLog2);
        const uint32_t byteBitOfs = ofs & (sBitsPerByte - 1);
        const uint32_t mask = (1 << num) - 1;
        mBytes[byteOfs] &= ~(mask << byteBitOfs);
        mBytes[byteOfs] |= (bits << byteBitOfs);
    }

    // false = left/right subblocks
    // true = upper/lower subblocks
    inline bool getFlipBit() const {
        return (mBytes[3] & 1) != 0;
    }

    inline void setFlipBit(bool flip) {
        mBytes[3] &= ~1;
        mBytes[3] |= static_cast<uint8_t>(flip);
    }

    inline bool getDiffBit() const {
        return (mBytes[3] & 2) != 0;
    }

    inline void setDiffBit(bool diff) {
        mBytes[3] &= ~2;
        mBytes[3] |= (static_cast<uint32_t>(diff) << 1);
    }

    // Returns intensity modifier table (0-7) used by subblock subblockId.
    // subblockId=0 left/top (CW 1), 1=right/bottom (CW 2)
    inline uint32_t getIntenTable(uint32_t subblockId) const {
        assert(subblockId < 2);
        const uint32_t ofs = subblockId ? 2 : 5;
        return (mBytes[3] >> ofs) & sIntenMask;
    }

    // Sets intensity modifier table (0-7) used by subblock subblockId (0 or 1)
    inline void setIntenTable(uint32_t subblockId, uint32_t t) {
        assert(subblockId < 2);
        assert(t < IntenModifierValues);
        const uint32_t ofs = subblockId ? 2 : 5;
        mBytes[3] &= ~(sIntenMask << ofs);
        mBytes[3] |= (t << ofs);
    }

    // Returned selector value ranges from 0-3 and is a direct index into sEtc1IntenTables.
    inline uint32_t getSelector(uint32_t x, uint32_t y) const {
        assert((x | y) < 4);

        const uint32_t bitIndex = x * sBlockWidth + y;
        const uint32_t byteBitOfs = bitIndex & (sBitsPerByte - 1);
        const uint8_t* p = &mBytes[(sBlockBytes - 1) - (bitIndex >> sBitsPerByteLog2)];
        const uint32_t lsb = (p[0] >> byteBitOfs) & 1;
        const uint32_t msb = (p[-2] >> byteBitOfs) & 1;
        const uint32_t val = lsb | (msb << 1);

        return sEtc1ToSelectorIndex[val];
    }

    // Selector "val" ranges from 0-3 and is a direct index into sEtc1IntenTables.
    inline void setSelector(uint32_t x, uint32_t y, uint32_t val) {
        assert((x | y | val) < 4);
        const uint32_t bitIndex = x * sBlockWidth + y;

        uint8_t* p = &mBytes[(sBlockBytes - 1) - (bitIndex >> sBitsPerByteLog2)];

        const uint32_t byteBitOfs = bitIndex & (sBitsPerByte - 1);
        const uint32_t mask = 1 << byteBitOfs;

        const uint32_t etc1Val = sSelectorIndexToEtc1[val];

        const uint32_t lsb = etc1Val & 1;
        const uint32_t msb = etc1Val >> 1;

        p[0] &= ~mask;
        p[0] |= (lsb << byteBitOfs);

        p[-2] &= ~mask;
        p[-2] |= (msb << byteBitOfs);
    }

    inline void setBase4Color(uint32_t idx, uint16_t c) {
        const UnpackedColor channels = unpackColorChannels(c, sBaseColor4Bits, sBaseColor4Max);
        if (idx) {
            setByteBits(AbsColor4R2BitOffset, sBaseColor4Bits, channels.r);
            setByteBits(AbsColor4G2BitOffset, sBaseColor4Bits, channels.g);
            setByteBits(AbsColor4B2BitOffset, sBaseColor4Bits, channels.b);
        } else {
            setByteBits(AbsColor4R1BitOffset, sBaseColor4Bits, channels.r);
            setByteBits(AbsColor4G1BitOffset, sBaseColor4Bits, channels.g);
            setByteBits(AbsColor4B1BitOffset, sBaseColor4Bits, channels.b);
        }
    }

    inline uint16_t getBase4Color(uint32_t idx) const {
        if (idx) {
            const uint32_t r = getByteBits(AbsColor4R2BitOffset, sBaseColor4Bits);
            const uint32_t g = getByteBits(AbsColor4G2BitOffset, sBaseColor4Bits);
            const uint32_t b = getByteBits(AbsColor4B2BitOffset, sBaseColor4Bits);
            return packColorChannels(b, g, r, sBaseColor4Bits);
        }
        const uint32_t r = getByteBits(AbsColor4R1BitOffset, sBaseColor4Bits);
        const uint32_t g = getByteBits(AbsColor4G1BitOffset, sBaseColor4Bits);
        const uint32_t b = getByteBits(AbsColor4B1BitOffset, sBaseColor4Bits);
        return packColorChannels(b, g, r, sBaseColor4Bits);
    }

    inline void setBase5Color(uint16_t c) {
        const UnpackedColor channels = unpackColorChannels(c, sBaseColor5Bits, sBaseColor5Max);
        setByteBits(BaseColor5RBitOffset, sBaseColor5Bits, channels.r);
        setByteBits(BaseColor5GBitOffset, sBaseColor5Bits, channels.g);
        setByteBits(BaseColor5BBitOffset, sBaseColor5Bits, channels.b);
    }

    inline uint16_t getBase5Color() const {
        const uint32_t r = getByteBits(BaseColor5RBitOffset, sBaseColor5Bits);
        const uint32_t g = getByteBits(BaseColor5GBitOffset, sBaseColor5Bits);
        const uint32_t b = getByteBits(BaseColor5BBitOffset, sBaseColor5Bits);
        return packColorChannels(b, g, r, sBaseColor5Bits);
    }

    void setDelta3Color(uint16_t c) {
        const UnpackedColor channels = unpackColorChannels(c, sDelta3Bits, sDelta3Mask);
        setByteBits(DeltaColor3RBitOffset, sDelta3Bits, channels.r);
        setByteBits(DeltaColor3GBitOffset, sDelta3Bits, channels.g);
        setByteBits(DeltaColor3BBitOffset, sDelta3Bits, channels.b);
    }

    inline uint16_t getDelta3Color() const {
        const uint32_t r = getByteBits(DeltaColor3RBitOffset, sDelta3Bits);
        const uint32_t g = getByteBits(DeltaColor3GBitOffset, sDelta3Bits);
        const uint32_t b = getByteBits(DeltaColor3BBitOffset, sDelta3Bits);
        return packColorChannels(b, g, r, sDelta3Bits);
    }

    // Base color 5
    static uint16_t packColor5(const ColorQuad& color, bool scaled, uint32_t bias = 127U);
    static uint16_t packColor5(uint32_t r, uint32_t g, uint32_t b, bool scaled, uint32_t bias = 127U);

    static ColorQuad unpackColor5(uint16_t packedColor5, bool scaled, uint32_t alpha = sColorChannelMax);
    static void unpackColor5(uint32_t& r, uint32_t& g, uint32_t& b, uint16_t packedColor, bool scaled);

    static bool unpackColor5(ColorQuad& result, uint16_t packedColor5, uint16_t packedDelta3, bool scaled, uint32_t alpha = sColorChannelMax);
    static bool unpackColor5(uint32_t& r, uint32_t& g, uint32_t& b, uint16_t packedColor5, uint16_t packedDelta3, bool scaled, uint32_t alpha = sColorChannelMax);

    // Delta color 3
    // Inputs range from -4 to 3 (ColorDeltaMin to ColorDeltaMax)
    static uint16_t packDelta3(int r, int g, int b);

    // Results range from -4 to 3 (ColorDeltaMin to ColorDeltaMax)
    static void unpackDelta3(int& r, int& g, int& b, uint16_t packedDelta3);

    // Abs color 4
    static uint16_t packColor4(const ColorQuad& color, bool scaled, uint32_t bias = 127U);
    static uint16_t packColor4(uint32_t r, uint32_t g, uint32_t b, bool scaled, uint32_t bias = 127U);

    static ColorQuad unpackColor4(uint16_t packedColor4, bool scaled, uint32_t alpha = sColorChannelMax);
    static void unpackColor4(uint32_t& r, uint32_t& g, uint32_t& b, uint16_t packedColor4, bool scaled);

    // subblock colors
    static void getDiffSubblockColors(ColorQuad* dst, uint16_t packedColor5, uint32_t tableIdx);
    static bool getDiffSubblockColors(ColorQuad* dst, uint16_t packedColor5, uint16_t packedDelta3, uint32_t tableIdx);
    static void getAbsSubblockColors(ColorQuad* dst, uint16_t packedColor4, uint32_t tableIdx);

    static inline void unscaledToScaledColor(ColorQuad& dst, const ColorQuad& src, bool color4) {
        if (color4) {
            dst.r = static_cast<ColorQuad::Component>(scaleColor4To8(src.r));
            dst.g = static_cast<ColorQuad::Component>(scaleColor4To8(src.g));
            dst.b = static_cast<ColorQuad::Component>(scaleColor4To8(src.b));
        } else {
            dst.r = static_cast<ColorQuad::Component>(scaleColor5To8(src.r));
            dst.g = static_cast<ColorQuad::Component>(scaleColor5To8(src.g));
            dst.b = static_cast<ColorQuad::Component>(scaleColor5To8(src.b));
        }
        dst.a = src.a;
    }
};

// Returns pointer to sorted array.
template <typename T, typename Q>
T* indirectRadixSort(uint32_t numIndices, T* indices0, T* indices1, const Q* keys, uint32_t keyOfs, uint32_t keySize, bool initIndices) {
    assert((keyOfs >= 0) && (keyOfs < sizeof(T)));
    assert((keySize >= 1) && (keySize <= 4));

    if (initIndices) {
        T* p = indices0;
        T* q = indices0 + (numIndices >> 1) * 2;
        uint32_t i;
        for (i = 0; p != q; p += 2, i += 2) {
            p[0] = static_cast<T>(i);
            p[1] = static_cast<T>(i + 1);
        }

        if (numIndices & 1) {
            *p = static_cast<T>(i);
        }
    }

    std::array<uint32_t, static_cast<std::size_t>(sHistogramBins) * 4> hist;

    std::memset(hist.data(), 0, sizeof(hist[0]) * sHistogramBins * keySize);

    // Loads the keySize bytes starting at byte offset keyOfs of the key indexed by `index`.
    // The unaligned uint32_t load is expressed with memcpy to stay strictly well-defined.
    const auto keyFromIndex = [keys, keyOfs](uint32_t index) -> uint32_t {
        uint32_t value = 0;
        std::memcpy(&value, static_cast<const void*>(static_cast<const uint8_t*>(static_cast<const void*>(keys + index)) + keyOfs), sizeof(value));
        return value;
    };

    if (keySize == 4) {
        T* p = indices0;
        T* q = indices0 + numIndices;
        for (; p != q; p++) {
            const uint32_t key = keyFromIndex(*p);

            hist[key & sByteMask]++;
            hist[sHistogramBins + ((key >> sBitsPerByte) & sByteMask)]++;
            hist[2 * sHistogramBins + ((key >> (2 * sBitsPerByte)) & sByteMask)]++;
            hist[3 * sHistogramBins + ((key >> (3 * sBitsPerByte)) & sByteMask)]++;
        }
    } else if (keySize == 3) {
        T* p = indices0;
        T* q = indices0 + numIndices;
        for (; p != q; p++) {
            const uint32_t key = keyFromIndex(*p);

            hist[key & sByteMask]++;
            hist[sHistogramBins + ((key >> sBitsPerByte) & sByteMask)]++;
            hist[2 * sHistogramBins + ((key >> (2 * sBitsPerByte)) & sByteMask)]++;
        }
    } else if (keySize == 2) {
        T* p = indices0;
        T* q = indices0 + (numIndices >> 1) * 2;

        for (; p != q; p += 2) {
            const uint32_t key0 = keyFromIndex(*p);
            const uint32_t key1 = keyFromIndex(*(p + 1));

            hist[key0 & sByteMask]++;
            hist[sHistogramBins + ((key0 >> sBitsPerByte) & sByteMask)]++;

            hist[key1 & sByteMask]++;
            hist[sHistogramBins + ((key1 >> sBitsPerByte) & sByteMask)]++;
        }

        if (numIndices & 1) {
            const uint32_t key = keyFromIndex(*p);

            hist[key & sByteMask]++;
            hist[sHistogramBins + ((key >> sBitsPerByte) & sByteMask)]++;
        }
    } else {
        assert(keySize == 1);
        if (keySize != 1) {
            return nullptr;
        }

        T* p = indices0;
        T* q = indices0 + (numIndices >> 1) * 2;

        for (; p != q; p += 2) {
            const uint32_t key0 = keyFromIndex(*p);
            const uint32_t key1 = keyFromIndex(*(p + 1));

            hist[key0 & sByteMask]++;
            hist[key1 & sByteMask]++;
        }

        if (numIndices & 1) {
            const uint32_t key = keyFromIndex(*p);

            hist[key & sByteMask]++;
        }
    }

    T* cur = indices0;
    T* dst = indices1;

    for (uint32_t pass = 0; pass < keySize; pass++) {
        const uint32_t* passHist = hist.data() + (static_cast<size_t>(pass) * sHistogramBins);

        std::array<uint32_t, sHistogramBins> offsets;

        uint32_t curOfs = 0;
        for (uint32_t i = 0; i < sHistogramBins; i += 2) {
            offsets[i] = curOfs;
            curOfs += passHist[i];

            offsets[i + 1] = curOfs;
            curOfs += passHist[i + 1];
        }

        const uint32_t passShift = pass * sBitsPerByte;

        T* p = cur;
        T* q = cur + (numIndices >> 1) * 2;

        for (; p != q; p += 2) {
            uint32_t index0 = p[0];
            uint32_t index1 = p[1];

            uint32_t c0 = (keyFromIndex(index0) >> passShift) & sByteMask;
            uint32_t c1 = (keyFromIndex(index1) >> passShift) & sByteMask;

            if (c0 == c1) {
                uint32_t dstOffset0 = offsets[c0];

                offsets[c0] = dstOffset0 + 2;

                dst[dstOffset0] = static_cast<T>(index0);
                dst[dstOffset0 + 1] = static_cast<T>(index1);
            } else {
                uint32_t dstOffset0 = offsets[c0]++;
                uint32_t dstOffset1 = offsets[c1]++;

                dst[dstOffset0] = static_cast<T>(index0);
                dst[dstOffset1] = static_cast<T>(index1);
            }
        }

        if (numIndices & 1) {
            uint32_t index = *p;
            uint32_t c = (keyFromIndex(index) >> passShift) & sByteMask;

            uint32_t dstOffset = offsets[c];
            offsets[c] = dstOffset + 1;

            dst[dstOffset] = static_cast<T>(index);
        }

        T* t = cur;
        cur = dst;
        dst = t;
    }

    return cur;
}

uint16_t Etc1Block::packColor5(const ColorQuad& color, bool scaled, uint32_t bias) {
    return packColor5(color.r, color.g, color.b, scaled, bias);
}

uint16_t Etc1Block::packColor5(uint32_t r, uint32_t g, uint32_t b, bool scaled, uint32_t bias) {
    if (scaled) {
        r = scaleColorToBase(r, sBaseColor5Max, bias);
        g = scaleColorToBase(g, sBaseColor5Max, bias);
        b = scaleColorToBase(b, sBaseColor5Max, bias);
    }

    r = Etc1::minimum(r, sBaseColor5Max);
    g = Etc1::minimum(g, sBaseColor5Max);
    b = Etc1::minimum(b, sBaseColor5Max);

    return packColorChannels(b, g, r, sBaseColor5Bits);
}

ColorQuad Etc1Block::unpackColor5(uint16_t packedColor5, bool scaled, uint32_t alpha) {
    UnpackedColor channels = unpackColorChannels(packedColor5, sBaseColor5Bits, sBaseColor5Max);

    if (scaled) {
        channels.b = scaleColor5To8(channels.b);
        channels.g = scaleColor5To8(channels.g);
        channels.r = scaleColor5To8(channels.r);
    }

    return {
        NoClamp,
        static_cast<int>(channels.r),
        static_cast<int>(channels.g),
        static_cast<int>(channels.b),
        static_cast<int>(Etc1::minimum(alpha, sColorChannelMax))
    };
}

void Etc1Block::unpackColor5(uint32_t& r, uint32_t& g, uint32_t& b, uint16_t packedColor5, bool scaled) {
    ColorQuad c(unpackColor5(packedColor5, scaled, 0));
    r = c.r;
    g = c.g;
    b = c.b;
}

bool Etc1Block::unpackColor5(ColorQuad& result, uint16_t packedColor5, uint16_t packedDelta3, bool scaled, uint32_t alpha) {
    int dcR, dcG, dcB;
    unpackDelta3(dcR, dcG, dcB, packedDelta3);

    const UnpackedColor channels = unpackColorChannels(packedColor5, sBaseColor5Bits, sBaseColor5Max);
    int b = static_cast<int>(channels.b) + dcB;
    int g = static_cast<int>(channels.g) + dcG;
    int r = static_cast<int>(channels.r) + dcR;

    bool success = true;
    if (static_cast<uint32_t>(r | g | b) > sBaseColor5Max) {
        success = false;
        r = Etc1::clamp<int>(r, 0, sBaseColor5Max);
        g = Etc1::clamp<int>(g, 0, sBaseColor5Max);
        b = Etc1::clamp<int>(b, 0, sBaseColor5Max);
    }

    if (scaled) {
        b = static_cast<int>(scaleColor5To8(b));
        g = static_cast<int>(scaleColor5To8(g));
        r = static_cast<int>(scaleColor5To8(r));
    }

    result.setNoClampRgba(r, g, b, static_cast<int>(Etc1::minimum(alpha, sColorChannelMax)));
    return success;
}

bool Etc1Block::unpackColor5(uint32_t& r, uint32_t& g, uint32_t& b, uint16_t packedColor5, uint16_t packedDelta3, bool scaled, uint32_t alpha) {
    ColorQuad result;
    const bool success = unpackColor5(result, packedColor5, packedDelta3, scaled, alpha);
    r = result.r;
    g = result.g;
    b = result.b;
    return success;
}

uint16_t Etc1Block::packDelta3(int r, int g, int b) {
    assert((r >= ColorDeltaMin) && (r <= ColorDeltaMax));
    assert((g >= ColorDeltaMin) && (g <= ColorDeltaMax));
    assert((b >= ColorDeltaMin) && (b <= ColorDeltaMax));
    if (r < 0) {
        r += 8;
    }
    if (g < 0) {
        g += 8;
    }
    if (b < 0) {
        b += 8;
    }
    return packColorChannels(b, g, r, sDelta3Bits);
}

void Etc1Block::unpackDelta3(int& r, int& g, int& b, uint16_t packedDelta3) {
    const UnpackedColor channels = unpackColorChannels(packedDelta3, sDelta3Bits, sDelta3Mask);
    r = static_cast<int>(channels.r);
    g = static_cast<int>(channels.g);
    b = static_cast<int>(channels.b);
    if (r >= 4) {
        r -= 8;
    }
    if (g >= 4) {
        g -= 8;
    }
    if (b >= 4) {
        b -= 8;
    }
}

uint16_t Etc1Block::packColor4(const ColorQuad& color, bool scaled, uint32_t bias) {
    return packColor4(color.r, color.g, color.b, scaled, bias);
}

uint16_t Etc1Block::packColor4(uint32_t r, uint32_t g, uint32_t b, bool scaled, uint32_t bias) {
    if (scaled) {
        r = scaleColorToBase(r, sBaseColor4Max, bias);
        g = scaleColorToBase(g, sBaseColor4Max, bias);
        b = scaleColorToBase(b, sBaseColor4Max, bias);
    }

    r = Etc1::minimum(r, sBaseColor4Max);
    g = Etc1::minimum(g, sBaseColor4Max);
    b = Etc1::minimum(b, sBaseColor4Max);

    return packColorChannels(b, g, r, sBaseColor4Bits);
}

ColorQuad Etc1Block::unpackColor4(uint16_t packedColor4, bool scaled, uint32_t alpha) {
    UnpackedColor channels = unpackColorChannels(packedColor4, sBaseColor4Bits, sBaseColor4Max);

    if (scaled) {
        channels.b = scaleColor4To8(channels.b);
        channels.g = scaleColor4To8(channels.g);
        channels.r = scaleColor4To8(channels.r);
    }

    return {
        NoClamp,
        static_cast<int>(channels.r),
        static_cast<int>(channels.g),
        static_cast<int>(channels.b),
        static_cast<int>(Etc1::minimum(alpha, sColorChannelMax))
    };
}

void Etc1Block::unpackColor4(uint32_t& r, uint32_t& g, uint32_t& b, uint16_t packedColor4, bool scaled) {
    ColorQuad c(unpackColor4(packedColor4, scaled, 0));
    r = c.r;
    g = c.g;
    b = c.b;
}

void Etc1Block::getDiffSubblockColors(ColorQuad* dst, uint16_t packedColor5, uint32_t tableIdx) {
    assert(tableIdx < IntenModifierValues);
    const int* intenModifierTable = &sEtc1IntenTables[tableIdx][0];

    uint32_t r, g, b;
    unpackColor5(r, g, b, packedColor5, true);

    const int ir = static_cast<int>(r), ig = static_cast<int>(g), ib = static_cast<int>(b);

    const int y0 = intenModifierTable[0];
    dst[0].set(ir + y0, ig + y0, ib + y0);

    const int y1 = intenModifierTable[1];
    dst[1].set(ir + y1, ig + y1, ib + y1);

    const int y2 = intenModifierTable[2];
    dst[2].set(ir + y2, ig + y2, ib + y2);

    const int y3 = intenModifierTable[3];
    dst[3].set(ir + y3, ig + y3, ib + y3);
}

bool Etc1Block::getDiffSubblockColors(ColorQuad* dst, uint16_t packedColor5, uint16_t packedDelta3, uint32_t tableIdx) {
    assert(tableIdx < IntenModifierValues);
    const int* intenModifierTable = &sEtc1IntenTables[tableIdx][0];

    uint32_t r, g, b;
    bool success = unpackColor5(r, g, b, packedColor5, packedDelta3, true);

    const int ir = static_cast<int>(r), ig = static_cast<int>(g), ib = static_cast<int>(b);

    const int y0 = intenModifierTable[0];
    dst[0].set(ir + y0, ig + y0, ib + y0);

    const int y1 = intenModifierTable[1];
    dst[1].set(ir + y1, ig + y1, ib + y1);

    const int y2 = intenModifierTable[2];
    dst[2].set(ir + y2, ig + y2, ib + y2);

    const int y3 = intenModifierTable[3];
    dst[3].set(ir + y3, ig + y3, ib + y3);

    return success;
}

void Etc1Block::getAbsSubblockColors(ColorQuad* dst, uint16_t packedColor4, uint32_t tableIdx) {
    assert(tableIdx < IntenModifierValues);
    const int* intenModifierTable = &sEtc1IntenTables[tableIdx][0];

    uint32_t r, g, b;
    unpackColor4(r, g, b, packedColor4, true);

    const int ir = static_cast<int>(r), ig = static_cast<int>(g), ib = static_cast<int>(b);

    const int y0 = intenModifierTable[0];
    dst[0].set(ir + y0, ig + y0, ib + y0);

    const int y1 = intenModifierTable[1];
    dst[1].set(ir + y1, ig + y1, ib + y1);

    const int y2 = intenModifierTable[2];
    dst[2].set(ir + y2, ig + y2, ib + y2);

    const int y3 = intenModifierTable[3];
    dst[3].set(ir + y3, ig + y3, ib + y3);
}

// Writes one 4-pixel row of a decoded block. When flipped, the top two rows come from subblock 0
// and the bottom two from subblock 1; otherwise the left two pixels come from subblock 0 and the
// right two from subblock 1. preserveAlpha selects setRgb (keep dst alpha) vs. full assignment.
static void writeSubblockRow(ColorQuad* dst, const Etc1Block& block, const std::array<ColorQuad, 4>& subblockColors0, const std::array<ColorQuad, 4>& subblockColors1, uint32_t y, bool flipFlag, bool preserveAlpha) {
    if (preserveAlpha) {
        if (flipFlag) {
            const std::array<ColorQuad, 4>& subblockColors = (y < 2) ? subblockColors0 : subblockColors1;
            dst[0].setRgb(subblockColors[block.getSelector(0, y)]);
            dst[1].setRgb(subblockColors[block.getSelector(1, y)]);
            dst[2].setRgb(subblockColors[block.getSelector(2, y)]);
            dst[3].setRgb(subblockColors[block.getSelector(3, y)]);
        } else {
            dst[0].setRgb(subblockColors0[block.getSelector(0, y)]);
            dst[1].setRgb(subblockColors0[block.getSelector(1, y)]);
            dst[2].setRgb(subblockColors1[block.getSelector(2, y)]);
            dst[3].setRgb(subblockColors1[block.getSelector(3, y)]);
        }
    } else {
        if (flipFlag) {
            const std::array<ColorQuad, 4>& subblockColors = (y < 2) ? subblockColors0 : subblockColors1;
            dst[0] = subblockColors[block.getSelector(0, y)];
            dst[1] = subblockColors[block.getSelector(1, y)];
            dst[2] = subblockColors[block.getSelector(2, y)];
            dst[3] = subblockColors[block.getSelector(3, y)];
        } else {
            dst[0] = subblockColors0[block.getSelector(0, y)];
            dst[1] = subblockColors0[block.getSelector(1, y)];
            dst[2] = subblockColors1[block.getSelector(2, y)];
            dst[3] = subblockColors1[block.getSelector(3, y)];
        }
    }
}

bool unpackEtc1Block(const void* etc1Block, uint32_t* dstPixelsRgba, bool preserveAlpha) {
    auto* dst = std::bit_cast<ColorQuad*>(dstPixelsRgba);
    const Etc1Block& block = *static_cast<const Etc1Block*>(etc1Block);

    const bool diffFlag = block.getDiffBit();
    const bool flipFlag = block.getFlipBit();
    const uint32_t tableIndex0 = block.getIntenTable(0);
    const uint32_t tableIndex1 = block.getIntenTable(1);

    std::array<ColorQuad, 4> subblockColors0;
    std::array<ColorQuad, 4> subblockColors1;
    bool success = true;

    if (diffFlag) {
        const uint16_t baseColor5 = block.getBase5Color();
        const uint16_t deltaColor3 = block.getDelta3Color();
        Etc1Block::getDiffSubblockColors(subblockColors0.data(), baseColor5, tableIndex0);

        if (!Etc1Block::getDiffSubblockColors(subblockColors1.data(), baseColor5, deltaColor3, tableIndex1)) {
            success = false;
        }
    } else {
        const uint16_t baseColor4Subblock0 = block.getBase4Color(0);
        Etc1Block::getAbsSubblockColors(subblockColors0.data(), baseColor4Subblock0, tableIndex0);

        const uint16_t baseColor4Subblock1 = block.getBase4Color(1);
        Etc1Block::getAbsSubblockColors(subblockColors1.data(), baseColor4Subblock1, tableIndex1);
    }

    for (uint32_t y = 0; y < sBlockHeight; y++) {
        writeSubblockRow(dst, block, subblockColors0, subblockColors1, y, flipFlag, preserveAlpha);
        dst += sBlockWidth;
    }

    return success;
}

struct Etc1SolutionCoordinates {
    inline Etc1SolutionCoordinates() : 
        mUnscaledColor(0, 0, 0, 0),
        mIntenTable(0),
        mColor4(false) {}

    inline Etc1SolutionCoordinates(uint32_t r, uint32_t g, uint32_t b, uint32_t intenTable, bool color4) :
        mUnscaledColor(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b), sColorChannelMax),
        mIntenTable(intenTable),
        mColor4(color4) {}

    inline Etc1SolutionCoordinates(const ColorQuad& c, uint32_t intenTable, bool color4) :
        mUnscaledColor(c),
        mIntenTable(intenTable),
        mColor4(color4) {}

    inline Etc1SolutionCoordinates(const Etc1SolutionCoordinates& other) {
        *this = other;
    }

    inline Etc1SolutionCoordinates& operator=(const Etc1SolutionCoordinates& rhs) = default;

    inline void clear() {
        mUnscaledColor.clear();
        mIntenTable = 0;
        mColor4 = false;
    }

    inline ColorQuad getScaledColor() const {
        if (mColor4) {
            return {
                static_cast<int>(scaleColor4To8(mUnscaledColor.r)),
                static_cast<int>(scaleColor4To8(mUnscaledColor.g)),
                static_cast<int>(scaleColor4To8(mUnscaledColor.b))
            };
        }
        return {
            static_cast<int>(scaleColor5To8(mUnscaledColor.r)),
            static_cast<int>(scaleColor5To8(mUnscaledColor.g)),
            static_cast<int>(scaleColor5To8(mUnscaledColor.b))
        };
    }

    inline void getBlockColors(ColorQuad* blockColors) {
        int br, bg, bb;
        if (mColor4) {
            br = static_cast<int>(scaleColor4To8(mUnscaledColor.r));
            bg = static_cast<int>(scaleColor4To8(mUnscaledColor.g));
            bb = static_cast<int>(scaleColor4To8(mUnscaledColor.b));
        } else {
            br = static_cast<int>(scaleColor5To8(mUnscaledColor.r));
            bg = static_cast<int>(scaleColor5To8(mUnscaledColor.g));
            bb = static_cast<int>(scaleColor5To8(mUnscaledColor.b));
        }
        const int* intenTableData = sEtc1IntenTables[mIntenTable].data();
        blockColors[0].set(br + intenTableData[0], bg + intenTableData[0], bb + intenTableData[0]);
        blockColors[1].set(br + intenTableData[1], bg + intenTableData[1], bb + intenTableData[1]);
        blockColors[2].set(br + intenTableData[2], bg + intenTableData[2], bb + intenTableData[2]);
        blockColors[3].set(br + intenTableData[3], bg + intenTableData[3], bb + intenTableData[3]);
    }

    ColorQuad mUnscaledColor;
    uint32_t mIntenTable;
    bool mColor4;
};

class Etc1Optimizer {
public:
    Etc1Optimizer(const Etc1Optimizer&) = delete;
    Etc1Optimizer& operator=(const Etc1Optimizer&) = delete;

    Etc1Optimizer() = default;

    void clear() {
        mParams = nullptr;
        mResult = nullptr;
        mSortedLumaBuf = nullptr;
        mSortedLumaIndices = nullptr;
    }

    struct Params : Etc1PackParams {
        Params() {
            clearOptimizerParams();
        }

        Params(const Etc1PackParams& baseParams) : Etc1PackParams(baseParams) {
            clearOptimizerParams();
        }

        void clearOptimizerParams() {
            mNumSrcPixels = 0;
            mSrcPixels = nullptr;

            mUseColor4 = false;
            static const std::array<int, 1> sDefaultScanDelta = {0};
            mScanDeltas = sDefaultScanDelta.data();
            mScanDeltaSize = 1;

            mBaseColor5.clear();
            mConstrainAgainstBaseColor5 = false;
        }

        uint32_t mNumSrcPixels = 0;
        const ColorQuad* mSrcPixels = nullptr;

        bool mUseColor4 = false;
        const int* mScanDeltas = nullptr;
        uint32_t mScanDeltaSize = 0;

        ColorQuad mBaseColor5;
        bool mConstrainAgainstBaseColor5 = false;
    };

    struct Results {
        uint64_t mError = sUint64Max;
        ColorQuad mBlockColorUnscaled;
        uint32_t mBlockIntenTable = 0;
        uint32_t mN = 0;
        uint8_t* mSelectors = nullptr;
        bool mBlockColor4 = false;

        inline Results& operator=(const Results& rhs) {
            if (this != &rhs) {
                mBlockColorUnscaled = rhs.mBlockColorUnscaled;
                mBlockColor4 = rhs.mBlockColor4;
                mBlockIntenTable = rhs.mBlockIntenTable;
                mError = rhs.mError;
                assert(mN == rhs.mN);
                std::memcpy(mSelectors, rhs.mSelectors, rhs.mN);
            }
            return *this;
        }
    };

    void init(const Params& params, Results& result);
    bool compute();

private:
    struct PotentialSolution {
        PotentialSolution() : 
            mCoords() {}

        Etc1SolutionCoordinates mCoords;
        std::array<uint8_t, sSubblockPixels> mSelectors;
        uint64_t mError{sUint64Max};
        bool mValid{false};

        void clear() {
            mCoords.clear();
            mError = sUint64Max;
            mValid = false;
        }
    };

    const Params* mParams;
    Results* mResult;

    int mLimit;

    Vec3F mAvgColor;
    int mBr, mBg, mBb;
    std::array<uint16_t, sSubblockPixels> mLuma;
    std::array<std::array<uint32_t, sSubblockPixels>, 2> mSortedLuma;
    const uint32_t* mSortedLumaIndices;
    uint32_t* mSortedLumaBuf;

    PotentialSolution mBestSolution;
    PotentialSolution mTrialSolution;
    std::array<uint8_t, sSubblockPixels> mTempSelectors;

    bool passesBaseColor5Constraint(const Etc1SolutionCoordinates& coords) const;
    bool commitTrialSolution(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution);
    bool evaluateSolution(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution);
    bool evaluateSolutionFast(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution);
    void refineSolution(int xd, int yd, int zd, const Etc1SolutionCoordinates& coords);
    bool commitBestSolution();
    void sortLumaForFastMode();
};

// Evaluates all 8 intensity tables (4 candidate block colors each) against the 8 source pixels
// in a single pass, so the pixel loads/setup are amortized and the independent table loops can be
// software-pipelined. Fills errors[8] with the total squared RGB error per table and
// selectors[8*8] with the best selector index (0-3) per pixel per table (table-major:
// selectors[t*8 + c]). All variants must produce bit-identical results: the candidate colors are
// clamp(base + inten_delta, 0, 255), ties resolve to the lowest selector index within a table and
// to the lowest table index between tables.
using EvaluateIntenTablesFunc = void (*)(const ColorQuad*, const ColorQuad&, uint64_t*, uint8_t*);

// Cached dispatch target, lazily resolved once on first use so the inner evaluation has no
// dispatch/CPU-feature cost. getEvaluateIntenTablesFunc() performs the one-time detection.
static EvaluateIntenTablesFunc getEvaluateIntenTablesFunc();

static inline EvaluateIntenTablesFunc getCachedEvalIntenTables() {
    static const EvaluateIntenTablesFunc cached = getEvaluateIntenTablesFunc();
    return cached;
}

static uint64_t evaluateIntenTableScalar(const ColorQuad* srcPixels, const ColorQuad& baseColor, uint32_t intenTable, uint8_t* selectors) {
    const int* intenTableData = sEtc1IntenTables[intenTable].data();

    std::array<ColorQuad, 4> blockColors;
    for (uint32_t s = 0; s < 4; s++) {
        const int yd = intenTableData[s];
        blockColors[s].set(baseColor.r + yd, baseColor.g + yd, baseColor.b + yd, 0);
    }
    uint64_t totalError = 0;
    for (uint32_t c = 0; c < sSubblockPixels; c++) {
        const ColorQuad& srcPixel = srcPixels[c];

        uint32_t bestSelectorIndex = 0;
        uint32_t bestError = blockColors[0].squaredDistanceRgb(srcPixel);
        for (uint32_t s = 1; s < 4; s++) {
            const uint32_t trialError = blockColors[s].squaredDistanceRgb(srcPixel);
            if (trialError < bestError) {
                bestError = trialError;
                bestSelectorIndex = s;
            }
        }

        selectors[c] = static_cast<uint8_t>(bestSelectorIndex);
        totalError += bestError;
    }

    return totalError;
}

static void evaluateIntenTablesScalar(const ColorQuad* srcPixels, const ColorQuad& baseColor, uint64_t* errors, uint8_t* selectors) {
    for (uint32_t t = 0; t < IntenModifierValues; t++) {
        errors[t] = evaluateIntenTableScalar(srcPixels, baseColor, t, selectors + static_cast<size_t>(t) * sSubblockPixels);
    }
}

#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64) || defined(_M_AMD64)

#ifdef RG_ETC1_FORCE_SCALAR
constexpr bool sForceScalar = true;
#else
constexpr bool sForceScalar = false;
#endif

// Processes 2 pixels per 128-bit register. Each int16 lane group is [r,g,b,0] so that
// _mm_madd_epi16(d,d) produces per-pixel partial sums which the 0xB1 shuffle merges into
// per-pixel squared errors [e0,e0,e1,e1]. All errors are non-negative, so the signed
// int32 comparison used for argmin is equivalent to an unsigned one.
[[gnu::target("ssse3")]]
static void evaluateIntenTablesSsse3(const ColorQuad* srcPixels, const ColorQuad& baseColor, uint64_t* errors, uint8_t* selectors) {
    // 8 pixels of RGBA == 32 bytes == two unaligned 128-bit loads.
    const __m128i px01 = _mm_loadu_si128(std::bit_cast<const __m128i*>(&srcPixels[0]));
    const __m128i px23 = _mm_loadu_si128(std::bit_cast<const __m128i*>(&srcPixels[4]));

    // Reorder bytes so each 2-pixel group becomes [r,g,b,0, r,g,b,0] (0x80 zeros the alpha byte).
    const __m128i shuf01 = _mm_setr_epi8(
        0, 1, 2, static_cast<char>(0x80),
        4, 5, 6, static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80)
    );

    const __m128i shuf23 = _mm_setr_epi8(
        8, 9, 10, static_cast<char>(0x80),
        12, 13, 14, static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80)
    );

    const __m128i zero = _mm_setzero_si128();

    std::array<__m128i, 4> pxv;
    pxv[0] = _mm_unpacklo_epi8(_mm_shuffle_epi8(px01, shuf01), zero); // pixels 0,1
    pxv[1] = _mm_unpacklo_epi8(_mm_shuffle_epi8(px01, shuf23), zero); // pixels 2,3
    pxv[2] = _mm_unpacklo_epi8(_mm_shuffle_epi8(px23, shuf01), zero); // pixels 4,5
    pxv[3] = _mm_unpacklo_epi8(_mm_shuffle_epi8(px23, shuf23), zero); // pixels 6,7

    // Int16 lane layout: [r0,g0,b0,a0, r1,g1,b1,a1] (alpha lanes forced to 0).
    const __m128i base16 = _mm_set_epi16(
        0, baseColor.b, baseColor.g, baseColor.r,
        0, baseColor.b, baseColor.g, baseColor.r
    );

    const __m128i kMask = _mm_set_epi16(
        0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF),
        0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF)
    );

    const __m128i k255 = _mm_set1_epi16(255);

    for (uint32_t t = 0; t < IntenModifierValues; t++) {
        const int* inten = sEtc1IntenTables[t].data();

        std::array<__m128i, 4> cvec;
        for (uint32_t s = 0; s < 4; s++) {
            const __m128i ydv = _mm_set1_epi16(static_cast<short>(inten[s]));
            __m128i c = _mm_and_si128(_mm_add_epi16(base16, ydv), kMask);
            c = _mm_max_epi16(c, zero);
            c = _mm_min_epi16(c, k255);
            cvec[s] = c;
        }

        std::array<__m128i, 4> bestErr;
        std::array<__m128i, 4> bestSel;

        // Selector 0 seeds the argmin state.
        for (uint32_t v = 0; v < 4; v++) {
            __m128i d = _mm_sub_epi16(pxv[v], cvec[0]);
            __m128i e = _mm_madd_epi16(d, d);
            e = _mm_add_epi32(e, _mm_shuffle_epi32(e, 0xB1)); // [e0,e0,e1,e1]
            bestErr[v] = e;
            bestSel[v] = zero;
        }

        for (uint32_t s = 1; s < 4; s++) {
            const __m128i seldv = _mm_set1_epi32(static_cast<int>(s));

            for (uint32_t v = 0; v < 4; v++) {
                __m128i d = _mm_sub_epi16(pxv[v], cvec[s]);
                __m128i e = _mm_madd_epi16(d, d);
                e = _mm_add_epi32(e, _mm_shuffle_epi32(e, 0xB1)); // [e0,e0,e1,e1]

                const __m128i lt = _mm_cmpgt_epi32(bestErr[v], e);
                bestErr[v] = _mm_or_si128(_mm_and_si128(lt, e), _mm_andnot_si128(lt, bestErr[v]));
                bestSel[v] = _mm_or_si128(_mm_and_si128(lt, seldv), _mm_andnot_si128(lt, bestSel[v]));
            }
        }

        uint8_t* dst = selectors + static_cast<size_t>(t) * sSubblockPixels;
        for (uint32_t v = 0; v < 4; v++) {
            dst[static_cast<size_t>(v) * 2 + 0] = static_cast<uint8_t>(static_cast<uint32_t>(_mm_cvtsi128_si32(bestSel[v])) & 3U);
            dst[static_cast<size_t>(v) * 2 + 1] = static_cast<uint8_t>(static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(bestSel[v], 0x0E))) & 3U);
        }

        uint64_t totalError = 0;
        for (const auto& e : bestErr) {
            const __m128i sum = _mm_add_epi32(e, _mm_shuffle_epi32(e, 0x0E));
            totalError += static_cast<uint32_t>(_mm_cvtsi128_si32(sum));
        }
        errors[t] = totalError;
    }
}

// Same algorithm, but 4 pixels per 256-bit register. Uses _mm_min_epi32 for the error side of the
// argmin (all errors are non-negative so signed min is exact) which is cheaper than the 3-op select.
#if defined(__GNUC__) || defined(__clang__)
[[gnu::target("avx2")]]
static void evaluateIntenTablesAvx2(const ColorQuad* srcPixels, const ColorQuad& baseColor, uint64_t* errors, uint8_t* selectors) {
    const __m128i px01 = _mm_loadu_si128(std::bit_cast<const __m128i*>(&srcPixels[0]));
    const __m128i px23 = _mm_loadu_si128(std::bit_cast<const __m128i*>(&srcPixels[4]));

    const __m128i shuf4 = _mm_setr_epi8(
        0, 1, 2, static_cast<char>(0x80),
        4, 5, 6, static_cast<char>(0x80),
        8, 9, 10, static_cast<char>(0x80),
        12, 13, 14, static_cast<char>(0x80)
    );

    const __m256i zero = _mm256_setzero_si256();

    std::array<__m256i, 2> pxv;
    pxv[0] = _mm256_cvtepu8_epi16(_mm_shuffle_epi8(px01, shuf4)); // pixels 0..3
    pxv[1] = _mm256_cvtepu8_epi16(_mm_shuffle_epi8(px23, shuf4)); // pixels 4..7

    // Int16 lane layout: [r0,g0,b0,a0, r1,g1,b1,a1, r2,g2,b2,a2, r3,g3,b3,a3] (alpha lanes forced to 0).
    const __m256i base256 = _mm256_set_epi16(
        0, baseColor.b, baseColor.g, baseColor.r,
        0, baseColor.b, baseColor.g, baseColor.r,
        0, baseColor.b, baseColor.g, baseColor.r,
        0, baseColor.b, baseColor.g, baseColor.r
    );

    const __m256i kMask = _mm256_set_epi16(
        0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF),
        0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF),
        0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF),
        0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF)
    );

    const __m256i k255 = _mm256_set1_epi16(255);

    for (uint32_t t = 0; t < IntenModifierValues; t++) {
        const int* inten = sEtc1IntenTables[t].data();

        std::array<__m256i, 4> cvec;
        for (uint32_t s = 0; s < 4; s++) {
            const __m256i ydv = _mm256_set1_epi16(static_cast<short>(inten[s]));
            __m256i c = _mm256_and_si256(_mm256_add_epi16(base256, ydv), kMask);
            c = _mm256_max_epi16(c, zero);
            c = _mm256_min_epi16(c, k255);
            cvec[s] = c;
        }

        std::array<__m256i, 2> bestErr;
        std::array<__m256i, 2> bestSel;

        // Selector 0 seeds the argmin state.
        for (uint32_t v = 0; v < 2; v++) {
            __m256i d = _mm256_sub_epi16(pxv[v], cvec[0]);
            __m256i e = _mm256_madd_epi16(d, d);
            e = _mm256_add_epi32(e, _mm256_shuffle_epi32(e, 0xB1)); // [e0,e0,e1,e1,e2,e2,e3,e3]
            bestErr[v] = e;
            bestSel[v] = zero;
        }

        for (uint32_t s = 1; s < 4; s++) {
            const __m256i seldv = _mm256_set1_epi32(static_cast<int>(s));

            for (uint32_t v = 0; v < 2; v++) {
                __m256i d = _mm256_sub_epi16(pxv[v], cvec[s]);
                __m256i e = _mm256_madd_epi16(d, d);
                e = _mm256_add_epi32(e, _mm256_shuffle_epi32(e, 0xB1));

                const __m256i lt = _mm256_cmpgt_epi32(bestErr[v], e);
                bestErr[v] = _mm256_min_epi32(bestErr[v], e);
                bestSel[v] = _mm256_blendv_epi8(bestSel[v], seldv, lt);
            }
        }

        uint8_t* dst = selectors + static_cast<size_t>(t) * sSubblockPixels;
        for (uint32_t v = 0; v < 2; v++) {
            dst[static_cast<size_t>(v) * 4 + 0] = static_cast<uint8_t>(static_cast<uint32_t>(_mm256_extract_epi32(bestSel[v], 0)) & 3U);
            dst[static_cast<size_t>(v) * 4 + 1] = static_cast<uint8_t>(static_cast<uint32_t>(_mm256_extract_epi32(bestSel[v], 2)) & 3U);
            dst[static_cast<size_t>(v) * 4 + 2] = static_cast<uint8_t>(static_cast<uint32_t>(_mm256_extract_epi32(bestSel[v], 4)) & 3U);
            dst[static_cast<size_t>(v) * 4 + 3] = static_cast<uint8_t>(static_cast<uint32_t>(_mm256_extract_epi32(bestSel[v], 6)) & 3U);
        }

        uint64_t totalError = 0;
        for (const auto& e : bestErr) {
            const __m256i sum = _mm256_add_epi32(e, _mm256_shuffle_epi32(e, 0x0E));
            totalError += static_cast<uint32_t>(_mm256_extract_epi32(sum, 0)) + static_cast<uint32_t>(_mm256_extract_epi32(sum, 4));
        }
        errors[t] = totalError;
    }
}
#endif // __GNUC__ || __clang__

static EvaluateIntenTablesFunc getEvaluateIntenTablesFunc() {
    static const EvaluateIntenTablesFunc sFunc = []() -> EvaluateIntenTablesFunc {
        if (sForceScalar) {
            return &evaluateIntenTablesScalar;
        }
#if defined(__GNUC__) || defined(__clang__)
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2")) {
            return &evaluateIntenTablesAvx2;
        }
        if (__builtin_cpu_supports("ssse3")) {
            return &evaluateIntenTablesSsse3;
        }
#elif defined(_M_X64) || defined(_M_AMD64)
        // All x64 CPUs since 2006 support SSSE3; MSVC has no per-function target attributes,
        // so the SSSE3 kernel is used unconditionally on x64.
        return &evaluateIntenTablesSsse3;
#endif
        return &evaluateIntenTablesScalar;
    }();
    return sFunc;
}

#endif // x86-64

bool Etc1Optimizer::compute() {
    const int scanDeltaSize = static_cast<int>(mParams->mScanDeltaSize);

    // Scan through a subset of the 3D lattice centered around the avg block color trying each 3D (555 or 444) lattice point as a potential block color.
    // Each time a better solution is found try to refine the current solution's block color based of the current selectors and intensity table index.
    for (int zdi = 0; zdi < scanDeltaSize; zdi++) {
        const int zd = mParams->mScanDeltas[zdi];
        const int mbb = mBb + zd;
        if (mbb < 0) {
            continue;
        }
        if (mbb > mLimit) {
            break;
        }

        for (int ydi = 0; ydi < scanDeltaSize; ydi++) {
            const int yd = mParams->mScanDeltas[ydi];
            const int mbg = mBg + yd;
            if (mbg < 0) {
                continue;
            }
            if (mbg > mLimit) {
                break;
            }

            for (int xdi = 0; xdi < scanDeltaSize; xdi++) {
                const int xd = mParams->mScanDeltas[xdi];
                const int mbr = mBr + xd;
                if (mbr < 0) {
                    continue;
                }
                if (mbr > mLimit) {
                    break;
                }

                Etc1SolutionCoordinates coords(mbr, mbg, mbb, 0, mParams->mUseColor4);
                if (mParams->mQuality == Etc1Quality::High) {
                    if (!evaluateSolution(coords, mTrialSolution, &mBestSolution)) {
                        continue;
                    }
                } else {
                    if (!evaluateSolutionFast(coords, mTrialSolution, &mBestSolution)) {
                        continue;
                    }
                }

                refineSolution(xd, yd, zd, coords);

            } // xdi
        } // ydi
    } // zdi

    return commitBestSolution();
}

// Refines the current best solution's block color per component by solving a simple linear equation. For example, for 4 colors:
// The goal is:
// pixel0 - (blockColor+intenTable[selector0]) + pixel1 - (blockColor+intenTable[selector1]) + pixel2 - (blockColor+intenTable[selector2]) + pixel3 - (blockColor+intenTable[selector3]) = 0
// Rearranging this:
// (pixel0 + pixel1 + pixel2 + pixel3) - (blockColor+intenTable[selector0]) - (blockColor+intenTable[selector1]) - (blockColor+intenTable[selector2]) - (blockColor+intenTable[selector3]) = 0
// (pixel0 + pixel1 + pixel2 + pixel3) - blockColor - intenTable[selector0] - blockColor-intenTable[selector1] - blockColor-intenTable[selector2] - blockColor-intenTable[selector3] = 0
// (pixel0 + pixel1 + pixel2 + pixel3) - 4*blockColor - intenTable[selector0] - intenTable[selector1] - intenTable[selector2] - intenTable[selector3] = 0
// (pixel0 + pixel1 + pixel2 + pixel3) - 4*blockColor - (intenTable[selector0] + intenTable[selector1] + intenTable[selector2] + intenTable[selector3]) = 0
// (pixel0 + pixel1 + pixel2 + pixel3)/4 - blockColor - (intenTable[selector0] + intenTable[selector1] + intenTable[selector2] + intenTable[selector3])/4 = 0
// blockColor = (pixel0 + pixel1 + pixel2 + pixel3)/4 - (intenTable[selector0] + intenTable[selector1] + intenTable[selector2] + intenTable[selector3])/4
// So what this means:
// optimal_block_color = avg_input - avg_inten_delta
// So the optimal block color can be computed by taking the average block color and subtracting the current average of the intensity delta.
// Unfortunately, optimal_block_color must then be quantized to 555 or 444 so it's not always possible to improve matters using this formula.
// Also, the above formula is for unclamped intensity deltas. The actual implementation takes into account clamping.
void Etc1Optimizer::refineSolution(int xd, int yd, int zd, const Etc1SolutionCoordinates& coords) {
    const uint32_t n = mParams->mNumSrcPixels;
    const uint32_t maxRefinementTrials = (mParams->mQuality == Etc1Quality::Low) ? 2 : (((xd | yd | zd) == 0) ? 4 : 2);
    for (uint32_t refinementTrial = 0; refinementTrial < maxRefinementTrials; refinementTrial++) {
        const uint8_t* selectors = mBestSolution.mSelectors.data();
        const int* intenTableData = sEtc1IntenTables[mBestSolution.mCoords.mIntenTable].data();

        int deltaSumR = 0, deltaSumG = 0, deltaSumB = 0;
        const ColorQuad baseColor(mBestSolution.mCoords.getScaledColor());
        for (uint32_t r = 0; r < n; r++) {
            const uint32_t s = *selectors++;
            const int yd = intenTableData[s];
            // Compute actual delta being applied to each pixel, taking into account clamping.
            deltaSumR += Etc1::clamp<int>(baseColor.r + yd, 0, sColorChannelMax) - baseColor.r;
            deltaSumG += Etc1::clamp<int>(baseColor.g + yd, 0, sColorChannelMax) - baseColor.g;
            deltaSumB += Etc1::clamp<int>(baseColor.b + yd, 0, sColorChannelMax) - baseColor.b;
        }
        if ((!deltaSumR) && (!deltaSumG) && (!deltaSumB)) {
            break;
        }
        const float avgDeltaRF = static_cast<float>(deltaSumR) / static_cast<float>(n);
        const float avgDeltaGF = static_cast<float>(deltaSumG) / static_cast<float>(n);
        const float avgDeltaBF = static_cast<float>(deltaSumB) / static_cast<float>(n);
        const auto limitF = static_cast<float>(mLimit);
        const auto br1 = Etc1::clamp<int>(static_cast<int>(std::lroundf((mAvgColor[0] - avgDeltaRF) * limitF / static_cast<float>(sColorChannelMax))), 0, mLimit);
        const auto bg1 = Etc1::clamp<int>(static_cast<int>(std::lroundf((mAvgColor[1] - avgDeltaGF) * limitF / static_cast<float>(sColorChannelMax))), 0, mLimit);
        const auto bb1 = Etc1::clamp<int>(static_cast<int>(std::lroundf((mAvgColor[2] - avgDeltaBF) * limitF / static_cast<float>(sColorChannelMax))), 0, mLimit);

        const bool skip =
            ((coords.mUnscaledColor.r == br1) && (coords.mUnscaledColor.g == bg1) && (coords.mUnscaledColor.b == bb1)) ||
            ((br1 == mBestSolution.mCoords.mUnscaledColor.r) && (bg1 == mBestSolution.mCoords.mUnscaledColor.g) && (bb1 == mBestSolution.mCoords.mUnscaledColor.b)) ||
            ((mBr == br1) && (mBg == bg1) && (mBb == bb1));

        if (skip) {
            break;
        }

        Etc1SolutionCoordinates coords1(br1, bg1, bb1, 0, mParams->mUseColor4);
        if (mParams->mQuality == Etc1Quality::High) {
            if (!evaluateSolution(coords1, mTrialSolution, &mBestSolution)) {
                break;
            }
        } else {
            if (!evaluateSolutionFast(coords1, mTrialSolution, &mBestSolution)) {
                break;
            }
        }

    } // refinementTrial
}

bool Etc1Optimizer::commitBestSolution() {
    if (!mBestSolution.mValid) {
        mResult->mError = sUint32Max;
        return false;
    }

    const uint32_t n = mParams->mNumSrcPixels;
    const uint8_t* selectors = mBestSolution.mSelectors.data();

    if (sBuildDebug) {
        std::array<ColorQuad, 4> blockColors;
        mBestSolution.mCoords.getBlockColors(blockColors.data());

        const ColorQuad* srcPixels = mParams->mSrcPixels;
        [[maybe_unused]] uint64_t actualError = 0;
        for (uint32_t i = 0; i < n; i++) {
            actualError += srcPixels[i].squaredDistanceRgb(blockColors[selectors[i]]);
        }

        assert(actualError == mBestSolution.mError);
    }

    mResult->mError = mBestSolution.mError;

    mResult->mBlockColorUnscaled = mBestSolution.mCoords.mUnscaledColor;
    mResult->mBlockColor4 = mBestSolution.mCoords.mColor4;

    mResult->mBlockIntenTable = mBestSolution.mCoords.mIntenTable;
    std::memcpy(mResult->mSelectors, selectors, n);
    mResult->mN = n;

    return true;
}

void Etc1Optimizer::init(const Params& p, Results& r) {
    // This version is hardcoded for 8 pixel subblocks.
    assert(p.mNumSrcPixels == sSubblockPixels);

    mParams = &p;
    mResult = &r;

    const uint32_t n = sSubblockPixels;

    mLimit = static_cast<int>(mParams->mUseColor4 ? sBaseColor4Max : sBaseColor5Max);

    Vec3F avgColor(0.0f);

    for (uint32_t i = 0; i < n; i++) {
        const ColorQuad& c = mParams->mSrcPixels[i];
        const Vec3F fc(c.r, c.g, c.b);

        avgColor += fc;

        mLuma[i] = static_cast<uint16_t>(c.r + c.g + c.b);
        mSortedLuma[0][i] = i;
    }
    avgColor *= (1.0f / static_cast<float>(n));
    mAvgColor = avgColor;

    const auto limitF = static_cast<float>(mLimit);
    mBr = Etc1::clamp<int>(static_cast<int>(std::lroundf(mAvgColor[0] * limitF / static_cast<float>(sColorChannelMax))), 0, mLimit);
    mBg = Etc1::clamp<int>(static_cast<int>(std::lroundf(mAvgColor[1] * limitF / static_cast<float>(sColorChannelMax))), 0, mLimit);
    mBb = Etc1::clamp<int>(static_cast<int>(std::lroundf(mAvgColor[2] * limitF / static_cast<float>(sColorChannelMax))), 0, mLimit);

    if (mParams->mQuality <= Etc1Quality::Medium) {
        sortLumaForFastMode();
    }

    mBestSolution.mCoords.clear();
    mBestSolution.mValid = false;
    mBestSolution.mError = sUint64Max;
}

// Presorts the pixel luma values so evaluateSolutionFast() can classify inputs to selectors with a
// single monotonic walk along the intensity (1,1,1) axis (see evaluateSolutionFast for details).
void Etc1Optimizer::sortLumaForFastMode() {
    const uint32_t n = sSubblockPixels;
    mSortedLumaIndices = indirectRadixSort(n, mSortedLuma[0].data(), mSortedLuma[1].data(), mLuma.data(), 0, sizeof(mLuma[0]), false);
    mSortedLumaBuf = mSortedLuma[0].data();
    if (mSortedLumaIndices == mSortedLuma[0].data()) {
        mSortedLumaBuf = mSortedLuma[1].data();
    }

    for (uint32_t i = 0; i < n; i++) {
        mSortedLumaBuf[i] = mLuma[mSortedLumaIndices[i]];
    }
}

bool Etc1Optimizer::evaluateSolution(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution) {
    trialSolution.mValid = false;
    if (!passesBaseColor5Constraint(coords)) {
        return false;
    }

    const ColorQuad baseColor(coords.getScaledColor());

    trialSolution.mError = sUint64Max;

    const EvaluateIntenTablesFunc evalIntenTables = getCachedEvalIntenTables();

    std::array<uint64_t, IntenModifierValues> tableErrors;
    std::array<uint8_t, static_cast<std::size_t>(IntenModifierValues) * sSubblockPixels> tableSelectors;
    evalIntenTables(mParams->mSrcPixels, baseColor, tableErrors.data(), tableSelectors.data());

    if (sBuildDebug) {
        std::array<uint64_t, IntenModifierValues> scalarErrors;
        std::array<uint8_t, static_cast<std::size_t>(IntenModifierValues) * sSubblockPixels> scalarSelectors;
        evaluateIntenTablesScalar(mParams->mSrcPixels, baseColor, scalarErrors.data(), scalarSelectors.data());
        assert(memcmp(scalarErrors.data(), tableErrors.data(), sizeof(tableErrors)) == 0);
        assert(memcmp(scalarSelectors.data(), tableSelectors.data(), sizeof(tableSelectors)) == 0);
    }

    for (uint32_t intenTable = 0; intenTable < IntenModifierValues; intenTable++) {
        const uint64_t totalError = tableErrors[intenTable];

        if (totalError < trialSolution.mError) {
            trialSolution.mError = totalError;
            trialSolution.mCoords.mIntenTable = intenTable;
            std::memcpy(trialSolution.mSelectors.data(), &tableSelectors[static_cast<size_t>(intenTable) * sSubblockPixels], sSubblockPixels);
            trialSolution.mValid = true;
        }
    }
    trialSolution.mCoords.mUnscaledColor = coords.mUnscaledColor;
    trialSolution.mCoords.mColor4 = mParams->mUseColor4;

    return commitTrialSolution(coords, trialSolution, bestSolution);
}

bool Etc1Optimizer::evaluateSolutionFast(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution) {
    trialSolution.mValid = false;
    if (!passesBaseColor5Constraint(coords)) {
        return false;
    }

    const ColorQuad baseColor(coords.getScaledColor());

    const uint32_t n = sSubblockPixels;

    trialSolution.mError = sUint64Max;

    for (int intenTable = IntenModifierValues - 1; intenTable >= 0; --intenTable) {
        const int* intenTableData = sEtc1IntenTables[intenTable].data();

        std::array<uint32_t, 4> blockInten;
        std::array<ColorQuad, 4> blockColors;
        for (uint32_t s = 0; s < 4; s++) {
            const int yd = intenTableData[s];
            ColorQuad blockColor(baseColor.r + yd, baseColor.g + yd, baseColor.b + yd, 0);
            blockColors[s] = blockColor;
            blockInten[s] = blockColor.r + blockColor.g + blockColor.b;
        }

        // evaluateSolutionFast() enforces/assumes a total ordering of the input colors along the intensity (1,1,1) axis to more quickly classify the inputs to selectors.
        // The inputs colors have been presorted along the projection onto this axis, and ETC1 block colors are always ordered along the intensity axis, so this classification is fast.
        // 0   1   2   3
        //   01  12  23
        const std::array<uint32_t, 3> blockIntenMidpoints = {blockInten[0] + blockInten[1], blockInten[1] + blockInten[2], blockInten[2] + blockInten[3]};

        uint64_t totalError = 0;
        const ColorQuad* srcPixels = mParams->mSrcPixels;
        if ((mSortedLumaBuf[n - 1] * 2) < blockIntenMidpoints[0]) {
            if (blockInten[0] > mSortedLumaBuf[n - 1]) {
                const uint32_t minError = blockInten[0] - mSortedLumaBuf[n - 1];
                if (minError >= trialSolution.mError) {
                    continue;
                }
            }

            std::memset(mTempSelectors.data(), 0, n);

            for (uint32_t c = 0; c < n; c++) {
                totalError += blockColors[0].squaredDistanceRgb(srcPixels[c]);
            }
        } else if ((mSortedLumaBuf[0] * 2) >= blockIntenMidpoints[2]) {
            if (mSortedLumaBuf[0] > blockInten[3]) {
                const uint32_t minError = mSortedLumaBuf[0] - blockInten[3];
                if (minError >= trialSolution.mError) {
                    continue;
                }
            }

            std::memset(mTempSelectors.data(), 3, n);

            for (uint32_t c = 0; c < n; c++) {
                totalError += blockColors[3].squaredDistanceRgb(srcPixels[c]);
            }
        } else {
            uint32_t curSelector = 0, c;
            for (c = 0; c < n; c++) {
                const uint32_t y = mSortedLumaBuf[c];
                while ((y * 2) >= blockIntenMidpoints[curSelector]) {
                    if (++curSelector > 2) {
                        goto done;
                    }
                }
                const uint32_t sortedPixelIndex = mSortedLumaIndices[c];
                mTempSelectors[sortedPixelIndex] = static_cast<uint8_t>(curSelector);
                totalError += blockColors[curSelector].squaredDistanceRgb(srcPixels[sortedPixelIndex]);
            }
        done:
            while (c < n) {
                const uint32_t sortedPixelIndex = mSortedLumaIndices[c];
                mTempSelectors[sortedPixelIndex] = 3;
                totalError += blockColors[3].squaredDistanceRgb(srcPixels[sortedPixelIndex]);
                ++c;
            }
        }

        if (totalError < trialSolution.mError) {
            trialSolution.mError = totalError;
            trialSolution.mCoords.mIntenTable = intenTable;
            std::memcpy(trialSolution.mSelectors.data(), mTempSelectors.data(), n);
            trialSolution.mValid = true;
            if (!totalError) {
                break;
            }
        }
    }
    trialSolution.mCoords.mUnscaledColor = coords.mUnscaledColor;
    trialSolution.mCoords.mColor4 = mParams->mUseColor4;

    return commitTrialSolution(coords, trialSolution, bestSolution);
}

// Returns false when the candidate block color is not allowed to deviate from the reference
// base-color-5 by more than the diff-mode delta range.
bool Etc1Optimizer::passesBaseColor5Constraint(const Etc1SolutionCoordinates& coords) const {
    if (!mParams->mConstrainAgainstBaseColor5) {
        return true;
    }

    const int dr = coords.mUnscaledColor.r - mParams->mBaseColor5.r;
    const int dg = coords.mUnscaledColor.g - mParams->mBaseColor5.g;
    const int db = coords.mUnscaledColor.b - mParams->mBaseColor5.b;

    return (Etc1::minimum(dr, dg, db) >= ColorDeltaMin) && (Etc1::maximum(dr, dg, db) <= ColorDeltaMax);
}

// Stores the evaluated trial solution into the caller-provided best solution slot when it improves
// on it, returning whether the caller should keep searching from this result.
bool Etc1Optimizer::commitTrialSolution(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution) {
    trialSolution.mCoords.mUnscaledColor = coords.mUnscaledColor;
    trialSolution.mCoords.mColor4 = mParams->mUseColor4;

    bool success = false;
    if (bestSolution) {
        if (trialSolution.mError < bestSolution->mError) {
            *bestSolution = trialSolution;
            success = true;
        }
    }

    return success;
}

// The next component index when iterating over the RGB channels in the solid-color packers.
static constexpr std::array<uint32_t, 4> sNextComp = {1, 2, 0, 1};

// Returns the precomputed table of ETC1 configs that decode to the given 8-bit color
// (0 and 255 use the two 0To255 tables, 1..254 use the 1To254 tables).
static const uint16_t* lookupEtcConfigTable(int cPlusDelta) {
    if (!cPlusDelta) {
        return sColor8ToEtcConfig0To255[0].data();
    }
    if (cPlusDelta == sColorChannelMax) {
        return sColor8ToEtcConfig0To255[1].data();
    }
    return sColor8ToEtcConfig1To254[cPlusDelta - 1].data();
}

// A packed config-table entry stores diff | (intenTable << 1) | (selector << 4) | (packedColor << 8).
// The low seven bits combine into the sEtc1InverseLookup index; the base color sits in bits 8-15.
static constexpr uint32_t etcConfigIndex(uint32_t x) {
    return x & sByteMask;
}

static constexpr uint32_t etcConfigBase(uint32_t x) {
    return (x >> sBitsPerByte) & sByteMask;
}

// An sEtc1InverseLookup entry stores the packed color in the low byte and its abs error in the high byte.
static constexpr uint32_t inverseValue(uint16_t comp) {
    return comp & sByteMask;
}

static constexpr uint32_t inverseError(uint16_t comp) {
    return comp >> sBitsPerByte;
}

// Best config found by the solid-color search below.
struct SolidColorSearchResult {
    uint32_t bestError = sUint32Max;
    uint32_t bestI = 0;
    int bestX = 0;
    int bestPackedC1 = 0;
    int bestPackedC2 = 0;
};

// Walks the per-channel config tables (and their inverse decode tables) to find the lowest-error
// diff/inten/selector combination for a solid color. requiredDiff >= 0 restricts the search to a
// single diff mode (0 = individual, 1 = differential); when baseColor5Unscaled is non-null, diff
// mode candidates are further constrained to stay within the base-color-5 delta range.
static SolidColorSearchResult searchSolidColorConfigs(const uint8_t* color, int requiredDiff, const ColorQuad* baseColor5Unscaled) {
    SolidColorSearchResult result;

    // For each possible 8-bit value, there is a precomputed list of diff/inten/selector configurations that allow that 8-bit value to be encoded with no error.
    for (uint32_t i = 0; i < 3; i++) {
        const uint32_t c1 = color[sNextComp[i]], c2 = color[sNextComp[i + 1]];

        const int deltaRange = 1;
        for (int delta = -deltaRange; delta <= deltaRange; delta++) {
            const int cPlusDelta = Etc1::clamp<int>(color[i] + delta, 0, sColorChannelMax);

            const uint16_t* table = lookupEtcConfigTable(cPlusDelta);

            for (;;) {
                const uint32_t x = *table++;
                const uint32_t diff = x & 1;
                if ((requiredDiff >= 0) && (static_cast<int>(diff) != requiredDiff)) {
                    if (*table == sEtc1TableTerminator) {
                        break;
                    }
                    continue;
                }

                if ((diff) && (baseColor5Unscaled)) {
                    const int comp0 = static_cast<int>(etcConfigBase(x));
                    const int delta0 = comp0 - static_cast<int>(baseColor5Unscaled->c[i]);
                    if ((delta0 < ColorDeltaMin) || (delta0 > ColorDeltaMax)) {
                        if (*table == sEtc1TableTerminator) {
                            break;
                        }
                        continue;
                    }
                }

                if (sBuildDebug) {
                    // (x >> 4) & 3 is the selector, (x >> 8) & 255 the base component; the packed
                    // table entry must decode back to cPlusDelta.
                    assert(etc1DecodeValue(diff, (x >> 1) & sIntenMask, (x >> 4) & sSelectorMask, etcConfigBase(x)) == static_cast<uint32_t>(cPlusDelta));
                }

                const uint16_t* inverseTable = sEtc1InverseLookup[etcConfigIndex(x)].data();
                const uint16_t comp1 = inverseTable[c1];
                const uint16_t comp2 = inverseTable[c2];

                if ((diff) && (baseColor5Unscaled)) {
                    const int delta1 = static_cast<int>(inverseValue(comp1)) - static_cast<int>(baseColor5Unscaled->c[sNextComp[i]]);
                    const int delta2 = static_cast<int>(inverseValue(comp2)) - static_cast<int>(baseColor5Unscaled->c[sNextComp[i + 1]]);
                    if ((delta1 < ColorDeltaMin) || (delta1 > ColorDeltaMax) || (delta2 < ColorDeltaMin) || (delta2 > ColorDeltaMax)) {
                        if (*table == sEtc1TableTerminator) {
                            break;
                        }
                        continue;
                    }
                }

                const uint32_t trialError = Etc1::square(cPlusDelta - color[i]) + Etc1::square(inverseError(comp1)) + Etc1::square(inverseError(comp2));
                if (trialError < result.bestError) {
                    result.bestError = trialError;
                    result.bestX = static_cast<int>(x);
                    result.bestPackedC1 = static_cast<int>(inverseValue(comp1));
                    result.bestPackedC2 = static_cast<int>(inverseValue(comp2));
                    result.bestI = i;
                    if (!result.bestError) {
                        goto foundPerfectMatch;
                    }
                }
                if (*table == sEtc1TableTerminator) {
                    break;
                }
            }
        }
    }
foundPerfectMatch:

    return result;
}

// Packs solid color blocks efficiently using a set of small precomputed tables.
// For random 888 inputs, MSE results are better than Ericsson's ETC1 packer in "slow" mode ~9.5% of the time, is slightly worse only ~.01% of the time, and is equal the rest of the time.
static uint64_t packEtc1BlockSolidColor(Etc1Block& block, const uint8_t* color, [[maybe_unused]] Etc1PackParams& packParams) {
    assert(sEtc1InverseLookup[0][sColorChannelMax]);

    const SolidColorSearchResult search = searchSolidColorConfigs(color, -1, nullptr);

    const uint32_t diff = static_cast<uint32_t>(search.bestX) & 1;
    const uint32_t inten = (static_cast<uint32_t>(search.bestX) >> 1) & sIntenMask;

    block.mBytes[3] = static_cast<uint8_t>(((inten | (inten << 3)) << 2) | (diff << 1));

    const uint32_t etc1Selector = sSelectorIndexToEtc1[(static_cast<uint32_t>(search.bestX) >> 4) & sSelectorMask];
    const auto selectorWords0 = static_cast<uint16_t>((etc1Selector & 2) ? 0xFFFF : 0);
    const auto selectorWords1 = static_cast<uint16_t>((etc1Selector & 1) ? 0xFFFF : 0);
    std::memcpy(&block.mBytes[4], &selectorWords0, sizeof(selectorWords0));
    std::memcpy(&block.mBytes[6], &selectorWords1, sizeof(selectorWords1));

    const uint32_t bestPackedC0 = etcConfigBase(static_cast<uint32_t>(search.bestX));
    if (diff) {
        block.mBytes[search.bestI] = static_cast<uint8_t>(bestPackedC0 << 3);
        block.mBytes[sNextComp[search.bestI]] = static_cast<uint8_t>(search.bestPackedC1 << 3);
        block.mBytes[sNextComp[search.bestI + 1]] = static_cast<uint8_t>(search.bestPackedC2 << 3);
    } else {
        block.mBytes[search.bestI] = static_cast<uint8_t>(bestPackedC0 | (bestPackedC0 << 4));
        block.mBytes[sNextComp[search.bestI]] = static_cast<uint8_t>(search.bestPackedC1 | (search.bestPackedC1 << 4));
        block.mBytes[sNextComp[search.bestI + 1]] = static_cast<uint8_t>(search.bestPackedC2 | (search.bestPackedC2 << 4));
    }

    return search.bestError;
}

static uint32_t packEtc1BlockSolidColorConstrained(
    Etc1Optimizer::Results& results,
    uint32_t numColors, const uint8_t* color,
    [[maybe_unused]] Etc1PackParams& packParams,
    bool useDiff,
    const ColorQuad* baseColor5Unscaled) {
    assert(sEtc1InverseLookup[0][sColorChannelMax]);

    const SolidColorSearchResult search = searchSolidColorConfigs(color, useDiff ? 1 : 0, baseColor5Unscaled);

    if (search.bestError == sUint32Max) {
        return search.bestError;
    }

    const uint32_t bestError = search.bestError * numColors;

    results.mN = numColors;
    results.mBlockColor4 = !(search.bestX & 1);
    results.mBlockIntenTable = (static_cast<uint32_t>(search.bestX) >> 1) & sIntenMask;
    std::memset(results.mSelectors, static_cast<int>((static_cast<uint32_t>(search.bestX) >> 4) & sSelectorMask), numColors);

    const uint32_t bestPackedC0 = etcConfigBase(static_cast<uint32_t>(search.bestX));
    results.mBlockColorUnscaled[search.bestI] = static_cast<uint8_t>(bestPackedC0);
    results.mBlockColorUnscaled[sNextComp[search.bestI]] = static_cast<uint8_t>(search.bestPackedC1);
    results.mBlockColorUnscaled[sNextComp[search.bestI + 1]] = static_cast<uint8_t>(search.bestPackedC2);
    results.mError = bestError;

    return bestError;
}

// Function originally from RYG's public domain real-time DXT1 compressor, modified for 555.
static void ditherBlock555(ColorQuad* dest, const ColorQuad* block) {
    std::array<int, 8> err;
    int* ep1 = err.data();
    int* ep2 = err.data() + 4;
    const uint8_t* quant = sQuant5Tab.data() + 8;

    std::memset(dest, sColorChannelMax, sizeof(ColorQuad) * sPixelsPerBlock);

    // process channels seperately
    for (int ch = 0; ch < 3; ch++) {
        const auto* bp = std::bit_cast<const uint8_t*>(block);
        auto* dp = std::bit_cast<uint8_t*>(dest);

        bp += ch;
        dp += ch;

        std::memset(err.data(), 0, sizeof(err));
        for (int y = 0; y < sBlockHeight; y++) {
            // pixel 0
            dp[0] = quant[bp[0] + ((3 * ep2[1] + 5 * ep2[0]) >> 4)];
            ep1[0] = bp[0] - dp[0];

            // pixel 1
            dp[4] = quant[bp[4] + ((7 * ep1[0] + 3 * ep2[2] + 5 * ep2[1] + ep2[0]) >> 4)];
            ep1[1] = bp[4] - dp[4];

            // pixel 2
            dp[8] = quant[bp[8] + ((7 * ep1[1] + 3 * ep2[3] + 5 * ep2[2] + ep2[1]) >> 4)];
            ep1[2] = bp[8] - dp[8];

            // pixel 3
            dp[12] = quant[bp[12] + ((7 * ep1[2] + 5 * ep2[3] + ep2[2]) >> 4)];
            ep1[3] = bp[12] - dp[12];

            // advance to next line
            int* tmp = ep1;
            ep1 = ep2;
            ep2 = tmp;
            bp += 16;
            dp += 16;
        }
    }
}

// Gathers the 8 pixels of one subblock into a contiguous array. Flipped subblocks are two
// horizontal 4x2 strips; non-flipped subblocks are two vertical 2x4 strips.
static void gatherSubblockPixels(ColorQuad* dst, const ColorQuad* srcPixels, uint32_t subblock, uint32_t flip) {
    if (flip) {
        std::memcpy(dst, srcPixels + static_cast<size_t>(subblock) * sSubblockPixels, sizeof(ColorQuad) * sSubblockPixels);
    } else {
        const ColorQuad* srcCol = srcPixels + static_cast<size_t>(subblock) * 2;
        dst[0] = srcCol[0];
        dst[1] = srcCol[sBlockWidth];
        dst[2] = srcCol[static_cast<std::size_t>(2) * sBlockWidth];
        dst[3] = srcCol[static_cast<std::size_t>(3) * sBlockWidth];
        dst[4] = srcCol[1];
        dst[5] = srcCol[1 + sBlockWidth];
        dst[6] = srcCol[1 + 2 * sBlockWidth];
        dst[7] = srcCol[1 + 3 * sBlockWidth];
    }
}

// Selects the initial lattice scan deltas for the current quality setting.
static void setInitialScanDeltas(Etc1Optimizer::Params& params) {
    if (params.mQuality == Etc1Quality::High) {
        static const std::array<int, 9> sScanDelta0To4 = {-4, -3, -2, -1, 0, 1, 2, 3, 4};
        params.mScanDeltaSize = static_cast<uint32_t>(std::size(sScanDelta0To4));
        params.mScanDeltas = sScanDelta0To4.data();
    } else if (params.mQuality == Etc1Quality::Medium) {
        static const std::array<int, 3> sScanDelta0To1 = {-1, 0, 1};
        params.mScanDeltaSize = static_cast<uint32_t>(std::size(sScanDelta0To1));
        params.mScanDeltas = sScanDelta0To1.data();
    } else {
        static const std::array<int, 1> sScanDelta0 = {0};
        params.mScanDeltaSize = static_cast<uint32_t>(std::size(sScanDelta0));
        params.mScanDeltas = sScanDelta0.data();
    }
}

// Widens the lattice scan deltas for a second optimizer pass, driven by the subblock's error.
static void setRefinementScanDeltas(Etc1Optimizer::Params& params, uint32_t subblockError) {
    constexpr uint32_t refinementErrorThresh1 = 6000;
    if (params.mQuality == Etc1Quality::Medium) {
        static const std::array<int, 4> sScanDelta2To3 = {-3, -2, 2, 3};
        params.mScanDeltaSize = static_cast<uint32_t>(std::size(sScanDelta2To3));
        params.mScanDeltas = sScanDelta2To3.data();
    } else {
        static const std::array<int, 2> sScanDelta5To5 = {-5, 5};
        static const std::array<int, 8> sScanDelta5To8 = {-8, -7, -6, -5, 5, 6, 7, 8};
        if (subblockError > refinementErrorThresh1) {
            params.mScanDeltaSize = static_cast<uint32_t>(std::size(sScanDelta5To8));
            params.mScanDeltas = sScanDelta5To8.data();
        } else {
            params.mScanDeltaSize = static_cast<uint32_t>(std::size(sScanDelta5To5));
            params.mScanDeltas = sScanDelta5To5.data();
        }
    }
}

// Attempts the solid-color fast path for a subblock, writing into result (whose mError is reset to
// the "no match" sentinel even when the subblock is not solid or quality is too low).
static void trySolidSubblockColor(Etc1Optimizer::Results& result, const ColorQuad* subblockPixels, Etc1PackParams& packParams, uint32_t useColor4, const ColorQuad* baseColor5Unscaled) {
    result.mError = sUint64Max;
    const uint32_t subblockPixel0U32 = subblockPixels[0].mU32;
    int r;
    for (r = 7; r >= 1; --r) {
        if (subblockPixels[r].mU32 != subblockPixel0U32) {
            break;
        }
    }
    if (!r) {
        packEtc1BlockSolidColorConstrained(result, sSubblockPixels, &subblockPixels[0].r, packParams, !useColor4, baseColor5Unscaled);
    }
}

// Writes the color/intensity bytes (0-3) of the encoded block from the per-subblock optimizer results.
static void encodeBlockColors(Etc1Block& dstBlock, const std::array<Etc1Optimizer::Results, 2>& bestResults, uint32_t useColor4, uint32_t bestFlip) {
    int dr = bestResults[1].mBlockColorUnscaled.r - bestResults[0].mBlockColorUnscaled.r;
    int dg = bestResults[1].mBlockColorUnscaled.g - bestResults[0].mBlockColorUnscaled.g;
    int db = bestResults[1].mBlockColorUnscaled.b - bestResults[0].mBlockColorUnscaled.b;
    assert(useColor4 || ((Etc1::minimum(dr, dg, db) >= ColorDeltaMin) && (Etc1::maximum(dr, dg, db) <= ColorDeltaMax)));

    if (useColor4) {
        dstBlock.mBytes[0] = static_cast<uint8_t>(bestResults[1].mBlockColorUnscaled.r | (bestResults[0].mBlockColorUnscaled.r << 4));
        dstBlock.mBytes[1] = static_cast<uint8_t>(bestResults[1].mBlockColorUnscaled.g | (bestResults[0].mBlockColorUnscaled.g << 4));
        dstBlock.mBytes[2] = static_cast<uint8_t>(bestResults[1].mBlockColorUnscaled.b | (bestResults[0].mBlockColorUnscaled.b << 4));
    } else {
        if (dr < 0) {
            dr += 8;
        }
        dstBlock.mBytes[0] = static_cast<uint8_t>((bestResults[0].mBlockColorUnscaled.r << 3) | dr);
        if (dg < 0) {
            dg += 8;
        }
        dstBlock.mBytes[1] = static_cast<uint8_t>((bestResults[0].mBlockColorUnscaled.g << 3) | dg);
        if (db < 0) {
            db += 8;
        }
        dstBlock.mBytes[2] = static_cast<uint8_t>((bestResults[0].mBlockColorUnscaled.b << 3) | db);
    }

    dstBlock.mBytes[3] = static_cast<uint8_t>((bestResults[1].mBlockIntenTable << 2) | (bestResults[0].mBlockIntenTable << 5) | ((~useColor4 & 1) << 1) | bestFlip);
}

// Writes the selector bytes (4-7) of the encoded block, reordering the per-subblock selector arrays
// into the bit-interleaved ETC1 selector word order for the given flip mode.
static void encodeBlockSelectors(Etc1Block& dstBlock, const std::array<Etc1Optimizer::Results, 2>& bestResults, uint32_t bestFlip) {
    uint32_t selector0 = 0, selector1 = 0;
    if (bestFlip) {
        // flipped:
        // { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 },
        // { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 }
        //
        // { 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 2 },
        // { 0, 3 }, { 1, 3 }, { 2, 3 }, { 3, 3 }
        const uint8_t* selectors0 = bestResults[0].mSelectors;
        const uint8_t* selectors1 = bestResults[1].mSelectors;
        for (int x = 3; x >= 0; --x) {
            uint32_t b;
            b = sSelectorIndexToEtc1[selectors1[4 + x]];
            selector0 = (selector0 << 1) | (b & 1);
            selector1 = (selector1 << 1) | (b >> 1);

            b = sSelectorIndexToEtc1[selectors1[x]];
            selector0 = (selector0 << 1) | (b & 1);
            selector1 = (selector1 << 1) | (b >> 1);

            b = sSelectorIndexToEtc1[selectors0[4 + x]];
            selector0 = (selector0 << 1) | (b & 1);
            selector1 = (selector1 << 1) | (b >> 1);

            b = sSelectorIndexToEtc1[selectors0[x]];
            selector0 = (selector0 << 1) | (b & 1);
            selector1 = (selector1 << 1) | (b >> 1);
        }
    } else {
        // non-flipped:
        // { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 },
        // { 1, 0 }, { 1, 1 }, { 1, 2 }, { 1, 3 }
        //
        // { 2, 0 }, { 2, 1 }, { 2, 2 }, { 2, 3 },
        // { 3, 0 }, { 3, 1 }, { 3, 2 }, { 3, 3 }
        for (int subblock = 1; subblock >= 0; --subblock) {
            const uint8_t* selectors = bestResults[subblock].mSelectors + 4;
            for (uint32_t i = 0; i < 2; i++) {
                uint32_t b;
                b = sSelectorIndexToEtc1[selectors[3]];
                selector0 = (selector0 << 1) | (b & 1);
                selector1 = (selector1 << 1) | (b >> 1);

                b = sSelectorIndexToEtc1[selectors[2]];
                selector0 = (selector0 << 1) | (b & 1);
                selector1 = (selector1 << 1) | (b >> 1);

                b = sSelectorIndexToEtc1[selectors[1]];
                selector0 = (selector0 << 1) | (b & 1);
                selector1 = (selector1 << 1) | (b >> 1);

                b = sSelectorIndexToEtc1[selectors[0]];
                selector0 = (selector0 << 1) | (b & 1);
                selector1 = (selector1 << 1) | (b >> 1);

                selectors -= 4;
            }
        }
    }

    dstBlock.mBytes[4] = static_cast<uint8_t>(selector1 >> sBitsPerByte);
    dstBlock.mBytes[5] = static_cast<uint8_t>(selector1 & sByteMask);
    dstBlock.mBytes[6] = static_cast<uint8_t>(selector0 >> sBitsPerByte);
    dstBlock.mBytes[7] = static_cast<uint8_t>(selector0 & sByteMask);
}

uint32_t packEtc1Block(void* etc1Block, const uint32_t* srcPixelsRgba, Etc1PackParams& packParams) {
    const auto* srcPixels = std::bit_cast<const ColorQuad*>(srcPixelsRgba);
    Etc1Block& dstBlock = *static_cast<Etc1Block*>(etc1Block);

    if (sBuildDebug) {
        // Ensure all alpha values are 0xFF.
        for (uint32_t i = 0; i < sPixelsPerBlock; i++) {
            assert(srcPixels[i].a == sColorChannelMax);
        }
    }

    // Check for solid block.
    const uint32_t firstPixelU32 = srcPixels->mU32;
    int r;
    for (r = 15; r >= 1; --r) {
        if (srcPixels[r].mU32 != firstPixelU32) {
            break;
        }
    }
    if (!r) {
        return static_cast<uint32_t>(16 * packEtc1BlockSolidColor(dstBlock, &srcPixels[0].r, packParams));
    }

    std::array<ColorQuad, 16> ditheredPixels;
    if (packParams.mDithering) {
        ditherBlock555(ditheredPixels.data(), srcPixels);
        srcPixels = ditheredPixels.data();
    }

    Etc1Optimizer optimizer;

    uint64_t bestError = sUint64Max;
    uint32_t bestFlip = false, bestUseColor4 = false;

    std::array<std::array<uint8_t, sSubblockPixels>, 2> bestSelectors;
    std::array<Etc1Optimizer::Results, 2> bestResults;
    for (uint32_t i = 0; i < 2; i++) {
        bestResults[i].mN = sSubblockPixels;
        bestResults[i].mSelectors = bestSelectors[i].data();
    }

    std::array<std::array<uint8_t, sSubblockPixels>, 3> selectorScratch;
    std::array<Etc1Optimizer::Results, 3> results;

    for (uint32_t i = 0; i < 3; i++) {
        results[i].mN = sSubblockPixels;
        results[i].mSelectors = selectorScratch[i].data();
    }

    std::array<ColorQuad, sSubblockPixels> subblockPixels;

    Etc1Optimizer::Params params(packParams);
    params.mNumSrcPixels = sSubblockPixels;
    params.mSrcPixels = subblockPixels.data();

    for (uint32_t flip = 0; flip < 2; flip++) {
        for (uint32_t useColor4 = 0; useColor4 < 2; useColor4++) {
            uint64_t trialError = 0;

            uint32_t subblock;
            for (subblock = 0; subblock < 2; subblock++) {
                gatherSubblockPixels(subblockPixels.data(), srcPixels, subblock, flip);

                results[2].mError = sUint64Max;
                if ((params.mQuality >= Etc1Quality::Medium) && ((subblock) || (useColor4))) {
                    trySolidSubblockColor(results[2], subblockPixels.data(), packParams, useColor4, (subblock && !useColor4) ? &results[0].mBlockColorUnscaled : nullptr);
                }

                params.mUseColor4 = (useColor4 != 0);
                params.mConstrainAgainstBaseColor5 = false;

                if ((!useColor4) && (subblock)) {
                    params.mConstrainAgainstBaseColor5 = true;
                    params.mBaseColor5 = results[0].mBlockColorUnscaled;
                }

                setInitialScanDeltas(params);

                optimizer.init(params, results[subblock]);
                if (!optimizer.compute()) {
                    break;
                }

                if (params.mQuality >= Etc1Quality::Medium) {
                    // TODO: Fix fairly arbitrary/unrefined thresholds that control how far away to scan for potentially better solutions.
                    const uint32_t refinementErrorThresh0 = 3000;
                    if (results[subblock].mError > refinementErrorThresh0) {
                        setRefinementScanDeltas(params, results[subblock].mError);

                        if (!optimizer.compute()) {
                            break;
                        }
                    }

                    if (results[2].mError < results[subblock].mError) {
                        results[subblock] = results[2];
                    }
                }

                trialError += results[subblock].mError;
                if (trialError >= bestError) {
                    break;
                }
            }

            if (subblock < 2) {
                continue;
            }

            bestError = trialError;
            bestResults[0] = results[0];
            bestResults[1] = results[1];
            bestFlip = flip;
            bestUseColor4 = useColor4;

        } // useColor4

    } // flip

    encodeBlockColors(dstBlock, bestResults, bestUseColor4, bestFlip);
    encodeBlockSelectors(dstBlock, bestResults, bestFlip);

    return static_cast<uint32_t>(bestError);
}
} // namespace Etc1
