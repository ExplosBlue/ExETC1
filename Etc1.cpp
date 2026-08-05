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
// NOLINTBEGIN(cert-dcl03-c) // clang-tidy 22.1.8 fires on runtime asserts (argc>0, loop guards) and its static_assert autofix would not compile; every assert() here is a runtime invariant.
constexpr uint32_t sUint32Max = std::numeric_limits<uint32_t>::max();
constexpr uint64_t sUint64Max = std::numeric_limits<uint64_t>::max();

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
            Signed = false,
            Float = false,
            Min = 0U,
            Max = 255U
        };
    };

    public:
    using Component = unsigned char;
    using Parameter = int;

    enum {
        NumComps = 4
    };

    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access,clang-diagnostic-gnu-anonymous-struct,clang-diagnostic-nested-anon-types,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    union {
        struct
        {
            Component r;
            Component g;
            Component b;
            Component a;
        };

        Component c[NumComps];

        uint32_t mU32;
    };
    // NOLINTEND(cppcoreguidelines-pro-type-union-access,clang-diagnostic-gnu-anonymous-struct,clang-diagnostic-nested-anon-types,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)

    inline ColorQuad() {
    }

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

    // Returns CCIR 601 luma (consistent with color_utils::RGB_To_Y).
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
};

enum EtcConstants {
    BytesPerBlock = 8U,

    SelectorBits = 2U,
    SelectorValues = 1U << SelectorBits,
    SelectorMask = SelectorValues - 1U,

    BlockShift = 2U,
    BlockSize = 1U << BlockShift,

    LSBSelectorIndicesBitOffset = 0,
    MSBSelectorIndicesBitOffset = 16,

    FlipBitOffset = 32,
    DiffBitOffset = 33,

    IntenModifierNumBits = 3,
    IntenModifierValues = 1 << IntenModifierNumBits,
    RightIntenModifierTableBitOffset = 34,
    LeftIntenModifierTableBitOffset = 37,

    // Base+Delta encoding (5 bit bases, 3 bit delta)
    BaseColorCompNumBits = 5,
    BaseColorCompMax = 1 << BaseColorCompNumBits,

    DeltaColorCompNumBits = 3,
    DeltaColorComp = 1 << DeltaColorCompNumBits,
    DeltaColorCompMax = 1 << DeltaColorCompNumBits,

    BaseColor5RBitOffset = 59,
    BaseColor5GBitOffset = 51,
    BaseColor5BBitOffset = 43,

    DeltaColor3RBitOffset = 56,
    DeltaColor3GBitOffset = 48,
    DeltaColor3BBitOffset = 40,

    // Absolute (non-delta) encoding (two 4-bit per component bases)
    AbsColorCompNumBits = 4,
    AbsColorCompMax = 1 << AbsColorCompNumBits,

    AbsColor4R1BitOffset = 60,
    AbsColor4G1BitOffset = 52,
    AbsColor4B1BitOffset = 44,

    AbsColor4R2BitOffset = 56,
    AbsColor4G2BitOffset = 48,
    AbsColor4B2BitOffset = 40,

    ColorDeltaMin = -4,
    ColorDeltaMax = 3,

    // Delta3:
    // 0   1   2   3   4   5   6   7
    // 000 001 010 011 100 101 110 111
    // 0   1   2   3   -4  -3  -2  -1
};

static constexpr std::array<std::array<int32_t, SelectorValues>, IntenModifierValues> sEtc1IntenTables = {{{{-8, -2, 2, 8}}, {{-17, -5, 5, 17}}, {{-29, -9, 9, 29}}, {{-42, -13, 13, 42}}, {{-60, -18, 18, 60}}, {{-80, -24, 24, 80}}, {{-106, -33, 33, 106}}, {{-183, -47, 47, 183}}}};

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
// Decodes a packed ETC1 color to an 8-bit value; constexpr so it also drives the compile-time table generation.
static constexpr uint32_t etc1DecodeValue(uint32_t diff, uint32_t inten, uint32_t selector, uint32_t packedC) {
    assert((diff < 2) && (inten < 8) && (selector < 4) && (packedC < (diff ? 32 : 16)));
    int c;
    if (diff) {
        c = static_cast<int>(packedC >> 2) | static_cast<int>(packedC << 3);
    } else {
        c = static_cast<int>(packedC) | (static_cast<int>(packedC) << 4);
    }
    c += sEtc1IntenTables[inten][selector];
    c = Etc1::clamp<int>(c, 0, 255);
    return c;
}

template <std::size_t N>
consteval void fillColor8ToEtcConfigRow(uint32_t target, std::array<uint16_t, N>& row) {
    uint32_t index = 0;
    for (uint32_t diff = 0; diff < 2; diff++) {
        const uint32_t packedLimit = diff ? 32U : 16U;
        for (uint32_t inten = 0; inten < IntenModifierValues; inten++) {
            std::array<bool, SelectorValues> emitted{};
            for (uint32_t packedColor = 0; packedColor < packedLimit; packedColor++) {
                for (uint32_t selector = 0; selector < SelectorValues; selector++) {
                    if (!emitted[selector] && etc1DecodeValue(diff, inten, selector, packedColor) == target) {
                        row[index++] = static_cast<uint16_t>(diff | (inten << 1) | (selector << 4) | (packedColor << 8));
                        emitted[selector] = true;
                    }
                }
            }
        }
    }
    row[index] = 0xFFFF;
}

consteval std::array<std::array<uint16_t, 33>, 2> makeColor8ToEtcConfig0To255() {
    std::array<std::array<uint16_t, 33>, 2> table{};
    fillColor8ToEtcConfigRow(0, table[0]);
    fillColor8ToEtcConfigRow(255, table[1]);
    return table;
}

consteval std::array<std::array<uint16_t, 12>, 254> makeColor8ToEtcConfig1To254() {
    std::array<std::array<uint16_t, 12>, 254> table{};
    for (uint32_t t = 1; t < 255; t++) {
        fillColor8ToEtcConfigRow(t, table[t - 1]);
    }
    return table;
}

static constexpr auto sColor8ToEtcConfig0To255 = makeColor8ToEtcConfig0To255();
static constexpr auto sColor8ToEtcConfig1To254 = makeColor8ToEtcConfig1To254();

// Given an ETC1 diff/intenTable/selector, and an 8-bit desired color, encodes the best packedColor in
// the low byte and its abs error in the high byte. Generated at compile time like the tables above.
consteval std::array<std::array<uint16_t, 256>, static_cast<std::size_t>(2) * 8 * 4> makeEtc1InverseLookup() {
    std::array<std::array<uint16_t, 256>, static_cast<std::size_t>(2) * 8 * 4> table{};
    for (uint32_t diff = 0; diff < 2; diff++) {
        const uint32_t limit = diff ? 32 : 16;
        for (uint32_t inten = 0; inten < IntenModifierValues; inten++) {
            for (uint32_t selector = 0; selector < SelectorValues; selector++) {
                const uint32_t inverseTableIndex = diff + (inten << 1) + (selector << 4);
                for (uint32_t color = 0; color < 256; color++) {
                    uint32_t bestError = sUint32Max, bestPackedC = 0;
                    for (uint32_t packedC = 0; packedC < limit; packedC++) {
                        const uint32_t v = etc1DecodeValue(diff, inten, selector, packedC);
                        const uint32_t err = v > color ? v - color : color - v;
                        if (err < bestError) {
                            bestError = err;
                            bestPackedC = packedC;
                            if (!bestError) {
                                break;
                            }
                        }
                    }
                    assert(bestError <= 255);
                    table[inverseTableIndex][color] = static_cast<uint16_t>(bestPackedC | (bestError << 8));
                }
            }
        }
    }
    return table;
}

static constexpr auto sEtc1InverseLookup = makeEtc1InverseLookup(); // [diff/intenTable/selector][desired_color]

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
        tab[i] = static_cast<uint8_t>((q << 3) | (q >> 2));
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
        std::array<uint8_t, 8> mBytes;
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
        assert(num && (num <= 8U));
        assert((ofs >> 3) == ((ofs + num - 1) >> 3));
        const uint32_t byteOfs = 7 - (ofs >> 3);
        const uint32_t byteBitOfs = ofs & 7;
        return (mBytes[byteOfs] >> byteBitOfs) & ((1 << num) - 1);
    }

    inline void setByteBits(uint32_t ofs, uint32_t num, uint32_t bits) {
        assert((ofs + num) <= 64U);
        assert(num && (num < 32U));
        assert((ofs >> 3) == ((ofs + num - 1) >> 3));
        assert(bits < (1U << num));
        const uint32_t byteOfs = 7 - (ofs >> 3);
        const uint32_t byteBitOfs = ofs & 7;
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
        return (mBytes[3] >> ofs) & 7;
    }

    // Sets intensity modifier table (0-7) used by subblock subblockId (0 or 1)
    inline void setIntenTable(uint32_t subblockId, uint32_t t) {
        assert(subblockId < 2);
        assert(t < 8);
        const uint32_t ofs = subblockId ? 2 : 5;
        mBytes[3] &= ~(7 << ofs);
        mBytes[3] |= (t << ofs);
    }

    // Returned selector value ranges from 0-3 and is a direct index into sEtc1IntenTables.
    inline uint32_t getSelector(uint32_t x, uint32_t y) const {
        assert((x | y) < 4);

        const uint32_t bitIndex = x * 4 + y;
        const uint32_t byteBitOfs = bitIndex & 7;
        const uint8_t* p = &mBytes[7 - (bitIndex >> 3)];
        const uint32_t lsb = (p[0] >> byteBitOfs) & 1;
        const uint32_t msb = (p[-2] >> byteBitOfs) & 1;
        const uint32_t val = lsb | (msb << 1);

        return sEtc1ToSelectorIndex[val];
    }

    // Selector "val" ranges from 0-3 and is a direct index into sEtc1IntenTables.
    inline void setSelector(uint32_t x, uint32_t y, uint32_t val) {
        assert((x | y | val) < 4);
        const uint32_t bitIndex = x * 4 + y;

        uint8_t* p = &mBytes[7 - (bitIndex >> 3)];

        const uint32_t byteBitOfs = bitIndex & 7;
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
        if (idx) {
            setByteBits(AbsColor4R2BitOffset, 4, (c >> 8) & 15);
            setByteBits(AbsColor4G2BitOffset, 4, (c >> 4) & 15);
            setByteBits(AbsColor4B2BitOffset, 4, c & 15);
        } else {
            setByteBits(AbsColor4R1BitOffset, 4, (c >> 8) & 15);
            setByteBits(AbsColor4G1BitOffset, 4, (c >> 4) & 15);
            setByteBits(AbsColor4B1BitOffset, 4, c & 15);
        }
    }

    inline uint16_t getBase4Color(uint32_t idx) const {
        uint32_t r, g, b;
        if (idx) {
            r = getByteBits(AbsColor4R2BitOffset, 4);
            g = getByteBits(AbsColor4G2BitOffset, 4);
            b = getByteBits(AbsColor4B2BitOffset, 4);
        } else {
            r = getByteBits(AbsColor4R1BitOffset, 4);
            g = getByteBits(AbsColor4G1BitOffset, 4);
            b = getByteBits(AbsColor4B1BitOffset, 4);
        }
        return static_cast<uint16_t>(b | (g << 4U) | (r << 8U));
    }

    inline void setBase5Color(uint16_t c) {
        setByteBits(BaseColor5RBitOffset, 5, (c >> 10) & 31);
        setByteBits(BaseColor5GBitOffset, 5, (c >> 5) & 31);
        setByteBits(BaseColor5BBitOffset, 5, c & 31);
    }

    inline uint16_t getBase5Color() const {
        const uint32_t r = getByteBits(BaseColor5RBitOffset, 5);
        const uint32_t g = getByteBits(BaseColor5GBitOffset, 5);
        const uint32_t b = getByteBits(BaseColor5BBitOffset, 5);
        return static_cast<uint16_t>(b | (g << 5U) | (r << 10U));
    }

    void setDelta3Color(uint16_t c) {
        setByteBits(DeltaColor3RBitOffset, 3, (c >> 6) & 7);
        setByteBits(DeltaColor3GBitOffset, 3, (c >> 3) & 7);
        setByteBits(DeltaColor3BBitOffset, 3, c & 7);
    }

    inline uint16_t getDelta3Color() const {
        const uint32_t r = getByteBits(DeltaColor3RBitOffset, 3);
        const uint32_t g = getByteBits(DeltaColor3GBitOffset, 3);
        const uint32_t b = getByteBits(DeltaColor3BBitOffset, 3);
        return static_cast<uint16_t>(b | (g << 3U) | (r << 6U));
    }

    // Base color 5
    static uint16_t packColor5(const ColorQuad& color, bool scaled, uint32_t bias = 127U);
    static uint16_t packColor5(uint32_t r, uint32_t g, uint32_t b, bool scaled, uint32_t bias = 127U);

    static ColorQuad unpackColor5(uint16_t packedColor5, bool scaled, uint32_t alpha = 255U);
    static void unpackColor5(uint32_t& r, uint32_t& g, uint32_t& b, uint16_t packedColor, bool scaled);

    static bool unpackColor5(ColorQuad& result, uint16_t packedColor5, uint16_t packedDelta3, bool scaled, uint32_t alpha = 255U);
    static bool unpackColor5(uint32_t& r, uint32_t& g, uint32_t& b, uint16_t packedColor5, uint16_t packedDelta3, bool scaled, uint32_t alpha = 255U);

    // Delta color 3
    // Inputs range from -4 to 3 (ColorDeltaMin to ColorDeltaMax)
    static uint16_t packDelta3(int r, int g, int b);

    // Results range from -4 to 3 (ColorDeltaMin to ColorDeltaMax)
    static void unpackDelta3(int& r, int& g, int& b, uint16_t packedDelta3);

    // Abs color 4
    static uint16_t packColor4(const ColorQuad& color, bool scaled, uint32_t bias = 127U);
    static uint16_t packColor4(uint32_t r, uint32_t g, uint32_t b, bool scaled, uint32_t bias = 127U);

    static ColorQuad unpackColor4(uint16_t packedColor4, bool scaled, uint32_t alpha = 255U);
    static void unpackColor4(uint32_t& r, uint32_t& g, uint32_t& b, uint16_t packedColor4, bool scaled);

    // subblock colors
    static void getDiffSubblockColors(ColorQuad* dst, uint16_t packedColor5, uint32_t tableIdx);
    static bool getDiffSubblockColors(ColorQuad* dst, uint16_t packedColor5, uint16_t packedDelta3, uint32_t tableIdx);
    static void getAbsSubblockColors(ColorQuad* dst, uint16_t packedColor4, uint32_t tableIdx);

    static inline void unscaledToScaledColor(ColorQuad& dst, const ColorQuad& src, bool color4) {
        if (color4) {
            dst.r = src.r | (src.r << 4);
            dst.g = src.g | (src.g << 4);
            dst.b = src.b | (src.b << 4);
        } else {
            dst.r = (src.r >> 2) | (src.r << 3);
            dst.g = (src.g >> 2) | (src.g << 3);
            dst.b = (src.b >> 2) | (src.b << 3);
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

    std::array<uint32_t, 256 * 4> hist;

    std::memset(hist.data(), 0, sizeof(hist[0]) * 256 * keySize);

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

            hist[key & 0xFF]++;
            hist[256 + ((key >> 8) & 0xFF)]++;
            hist[512 + ((key >> 16) & 0xFF)]++;
            hist[768 + ((key >> 24) & 0xFF)]++;
        }
    } else if (keySize == 3) {
        T* p = indices0;
        T* q = indices0 + numIndices;
        for (; p != q; p++) {
            const uint32_t key = keyFromIndex(*p);

            hist[key & 0xFF]++;
            hist[256 + ((key >> 8) & 0xFF)]++;
            hist[512 + ((key >> 16) & 0xFF)]++;
        }
    } else if (keySize == 2) {
        T* p = indices0;
        T* q = indices0 + (numIndices >> 1) * 2;

        for (; p != q; p += 2) {
            const uint32_t key0 = keyFromIndex(*p);
            const uint32_t key1 = keyFromIndex(*(p + 1));

            hist[key0 & 0xFF]++;
            hist[256 + ((key0 >> 8) & 0xFF)]++;

            hist[key1 & 0xFF]++;
            hist[256 + ((key1 >> 8) & 0xFF)]++;
        }

        if (numIndices & 1) {
            const uint32_t key = keyFromIndex(*p);

            hist[key & 0xFF]++;
            hist[256 + ((key >> 8) & 0xFF)]++;
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

            hist[key0 & 0xFF]++;
            hist[key1 & 0xFF]++;
        }

        if (numIndices & 1) {
            const uint32_t key = keyFromIndex(*p);

            hist[key & 0xFF]++;
        }
    }

    T* cur = indices0;
    T* dst = indices1;

    for (uint32_t pass = 0; pass < keySize; pass++) {
        const uint32_t* passHist = hist.data() + (static_cast<size_t>(pass) << 8);

        std::array<uint32_t, 256> offsets;

        uint32_t curOfs = 0;
        for (uint32_t i = 0; i < 256; i += 2) {
            offsets[i] = curOfs;
            curOfs += passHist[i];

            offsets[i + 1] = curOfs;
            curOfs += passHist[i + 1];
        }

        const uint32_t passShift = pass << 3;

        T* p = cur;
        T* q = cur + (numIndices >> 1) * 2;

        for (; p != q; p += 2) {
            uint32_t index0 = p[0];
            uint32_t index1 = p[1];

            uint32_t c0 = (keyFromIndex(index0) >> passShift) & 0xFF;
            uint32_t c1 = (keyFromIndex(index1) >> passShift) & 0xFF;

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
            uint32_t c = (keyFromIndex(index) >> passShift) & 0xFF;

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
        r = (r * 31U + bias) / 255U;
        g = (g * 31U + bias) / 255U;
        b = (b * 31U + bias) / 255U;
    }

    r = Etc1::minimum(r, 31U);
    g = Etc1::minimum(g, 31U);
    b = Etc1::minimum(b, 31U);

    return static_cast<uint16_t>(b | (g << 5U) | (r << 10U));
}

ColorQuad Etc1Block::unpackColor5(uint16_t packedColor5, bool scaled, uint32_t alpha) {
    uint32_t b = packedColor5 & 31U;
    uint32_t g = (packedColor5 >> 5U) & 31U;
    uint32_t r = (packedColor5 >> 10U) & 31U;

    if (scaled) {
        b = (b << 3U) | (b >> 2U);
        g = (g << 3U) | (g >> 2U);
        r = (r << 3U) | (r >> 2U);
    }

    return {NoClamp, static_cast<int>(r), static_cast<int>(g), static_cast<int>(b), static_cast<int>(Etc1::minimum(alpha, 255U))};
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

    int b = static_cast<int>(packedColor5 & 31U) + dcB;
    int g = static_cast<int>((packedColor5 >> 5U) & 31U) + dcG;
    int r = static_cast<int>((packedColor5 >> 10U) & 31U) + dcR;

    bool success = true;
    if (static_cast<uint32_t>(r | g | b) > 31U) {
        success = false;
        r = Etc1::clamp<int>(r, 0, 31);
        g = Etc1::clamp<int>(g, 0, 31);
        b = Etc1::clamp<int>(b, 0, 31);
    }

    if (scaled) {
        b = (b << 3U) | (b >> 2U);
        g = (g << 3U) | (g >> 2U);
        r = (r << 3U) | (r >> 2U);
    }

    result.setNoClampRgba(r, g, b, static_cast<int>(Etc1::minimum(alpha, 255U)));
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
    return static_cast<uint16_t>(b | (g << 3) | (r << 6));
}

void Etc1Block::unpackDelta3(int& r, int& g, int& b, uint16_t packedDelta3) {
    r = (packedDelta3 >> 6) & 7;
    g = (packedDelta3 >> 3) & 7;
    b = packedDelta3 & 7;
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
        r = (r * 15U + bias) / 255U;
        g = (g * 15U + bias) / 255U;
        b = (b * 15U + bias) / 255U;
    }

    r = Etc1::minimum(r, 15U);
    g = Etc1::minimum(g, 15U);
    b = Etc1::minimum(b, 15U);

    return static_cast<uint16_t>(b | (g << 4U) | (r << 8U));
}

ColorQuad Etc1Block::unpackColor4(uint16_t packedColor4, bool scaled, uint32_t alpha) {
    uint32_t b = packedColor4 & 15U;
    uint32_t g = (packedColor4 >> 4U) & 15U;
    uint32_t r = (packedColor4 >> 8U) & 15U;

    if (scaled) {
        b = (b << 4U) | b;
        g = (g << 4U) | g;
        r = (r << 4U) | r;
    }

    return {NoClamp, static_cast<int>(r), static_cast<int>(g), static_cast<int>(b), static_cast<int>(Etc1::minimum(alpha, 255U))};
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

    if (preserveAlpha) {
        if (flipFlag) {
            for (uint32_t y = 0; y < 2; y++) {
                dst[0].setRgb(subblockColors0[block.getSelector(0, y)]);
                dst[1].setRgb(subblockColors0[block.getSelector(1, y)]);
                dst[2].setRgb(subblockColors0[block.getSelector(2, y)]);
                dst[3].setRgb(subblockColors0[block.getSelector(3, y)]);
                dst += 4;
            }

            for (uint32_t y = 2; y < 4; y++) {
                dst[0].setRgb(subblockColors1[block.getSelector(0, y)]);
                dst[1].setRgb(subblockColors1[block.getSelector(1, y)]);
                dst[2].setRgb(subblockColors1[block.getSelector(2, y)]);
                dst[3].setRgb(subblockColors1[block.getSelector(3, y)]);
                dst += 4;
            }
        } else {
            for (uint32_t y = 0; y < 4; y++) {
                dst[0].setRgb(subblockColors0[block.getSelector(0, y)]);
                dst[1].setRgb(subblockColors0[block.getSelector(1, y)]);
                dst[2].setRgb(subblockColors1[block.getSelector(2, y)]);
                dst[3].setRgb(subblockColors1[block.getSelector(3, y)]);
                dst += 4;
            }
        }
    } else {
        if (flipFlag) {
            // 0000
            // 0000
            // 1111
            // 1111
            for (uint32_t y = 0; y < 2; y++) {
                dst[0] = subblockColors0[block.getSelector(0, y)];
                dst[1] = subblockColors0[block.getSelector(1, y)];
                dst[2] = subblockColors0[block.getSelector(2, y)];
                dst[3] = subblockColors0[block.getSelector(3, y)];
                dst += 4;
            }

            for (uint32_t y = 2; y < 4; y++) {
                dst[0] = subblockColors1[block.getSelector(0, y)];
                dst[1] = subblockColors1[block.getSelector(1, y)];
                dst[2] = subblockColors1[block.getSelector(2, y)];
                dst[3] = subblockColors1[block.getSelector(3, y)];
                dst += 4;
            }
        } else {
            // 0011
            // 0011
            // 0011
            // 0011
            for (uint32_t y = 0; y < 4; y++) {
                dst[0] = subblockColors0[block.getSelector(0, y)];
                dst[1] = subblockColors0[block.getSelector(1, y)];
                dst[2] = subblockColors1[block.getSelector(2, y)];
                dst[3] = subblockColors1[block.getSelector(3, y)];
                dst += 4;
            }
        }
    }

    return success;
}

struct Etc1SolutionCoordinates {
    inline Etc1SolutionCoordinates() : mUnscaledColor(0, 0, 0, 0),
                                       mIntenTable(0),
                                       mColor4(false) {
    }

    inline Etc1SolutionCoordinates(uint32_t r, uint32_t g, uint32_t b, uint32_t intenTable, bool color4) : mUnscaledColor(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b), 255),
                                                                                       mIntenTable(intenTable),
                                                                                       mColor4(color4) {
    }

    inline Etc1SolutionCoordinates(const ColorQuad& c, uint32_t intenTable, bool color4) : mUnscaledColor(c),
                                                                                      mIntenTable(intenTable),
                                                                                      mColor4(color4) {
    }

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
        int br, bg, bb;
        if (mColor4) {
            br = mUnscaledColor.r | (mUnscaledColor.r << 4);
            bg = mUnscaledColor.g | (mUnscaledColor.g << 4);
            bb = mUnscaledColor.b | (mUnscaledColor.b << 4);
        } else {
            br = (mUnscaledColor.r >> 2) | (mUnscaledColor.r << 3);
            bg = (mUnscaledColor.g >> 2) | (mUnscaledColor.g << 3);
            bb = (mUnscaledColor.b >> 2) | (mUnscaledColor.b << 3);
        }
        return {br, bg, bb};
    }

    inline void getBlockColors(ColorQuad* blockColors) {
        int br, bg, bb;
        if (mColor4) {
            br = mUnscaledColor.r | (mUnscaledColor.r << 4);
            bg = mUnscaledColor.g | (mUnscaledColor.g << 4);
            bb = mUnscaledColor.b | (mUnscaledColor.b << 4);
        } else {
            br = (mUnscaledColor.r >> 2) | (mUnscaledColor.r << 3);
            bg = (mUnscaledColor.g >> 2) | (mUnscaledColor.g << 3);
            bb = (mUnscaledColor.b >> 2) | (mUnscaledColor.b << 3);
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
        PotentialSolution() : mCoords() {
        }

        Etc1SolutionCoordinates mCoords;
        std::array<uint8_t, 8> mSelectors;
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
    std::array<uint16_t, 8> mLuma;
    std::array<std::array<uint32_t, 8>, 2> mSortedLuma;
    const uint32_t* mSortedLumaIndices;
    uint32_t* mSortedLumaBuf;

    PotentialSolution mBestSolution;
    PotentialSolution mTrialSolution;
    std::array<uint8_t, 8> mTempSelectors;

    bool evaluateSolution(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution);
    bool evaluateSolutionFast(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution);
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
    for (uint32_t c = 0; c < 8; c++) {
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
        errors[t] = evaluateIntenTableScalar(srcPixels, baseColor, t, selectors + static_cast<size_t>(t) * 8);
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
    const __m128i shuf01 = _mm_setr_epi8(0, 1, 2, static_cast<char>(0x80), 4, 5, 6, static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80));
    const __m128i shuf23 = _mm_setr_epi8(8, 9, 10, static_cast<char>(0x80), 12, 13, 14, static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80));

    const __m128i zero = _mm_setzero_si128();

    // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) // __m128i in std::array triggers GCC -Wignored-attributes
    __m128i pxv[4];
    pxv[0] = _mm_unpacklo_epi8(_mm_shuffle_epi8(px01, shuf01), zero); // pixels 0,1
    pxv[1] = _mm_unpacklo_epi8(_mm_shuffle_epi8(px01, shuf23), zero); // pixels 2,3
    pxv[2] = _mm_unpacklo_epi8(_mm_shuffle_epi8(px23, shuf01), zero); // pixels 4,5
    pxv[3] = _mm_unpacklo_epi8(_mm_shuffle_epi8(px23, shuf23), zero); // pixels 6,7

    // Int16 lane layout: [r0,g0,b0,a0, r1,g1,b1,a1] (alpha lanes forced to 0).
    const __m128i base16 = _mm_set_epi16(0, baseColor.b, baseColor.g, baseColor.r, 0, baseColor.b, baseColor.g, baseColor.r);
    const __m128i kMask = _mm_set_epi16(0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), 0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF));
    const __m128i k255 = _mm_set1_epi16(255);

    for (uint32_t t = 0; t < IntenModifierValues; t++) {
        const int* inten = sEtc1IntenTables[t].data();

        __m128i cvec[4];
        for (uint32_t s = 0; s < 4; s++) {
            const __m128i ydv = _mm_set1_epi16(static_cast<short>(inten[s]));
            __m128i c = _mm_and_si128(_mm_add_epi16(base16, ydv), kMask);
            c = _mm_max_epi16(c, zero);
            c = _mm_min_epi16(c, k255);
            cvec[s] = c;
        }

        __m128i bestErr[4];
        __m128i bestSel[4];

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

        uint8_t* dst = selectors + static_cast<size_t>(t) * 8;
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
    // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
}

// Same algorithm, but 4 pixels per 256-bit register. Uses _mm_min_epi32 for the error side of the
// argmin (all errors are non-negative so signed min is exact) which is cheaper than the 3-op select.
#if defined(__GNUC__) || defined(__clang__)
[[gnu::target("avx2")]]
static void evaluateIntenTablesAvx2(const ColorQuad* srcPixels, const ColorQuad& baseColor, uint64_t* errors, uint8_t* selectors) {
    const __m128i px01 = _mm_loadu_si128(std::bit_cast<const __m128i*>(&srcPixels[0]));
    const __m128i px23 = _mm_loadu_si128(std::bit_cast<const __m128i*>(&srcPixels[4]));

    const __m128i shuf4 = _mm_setr_epi8(0, 1, 2, static_cast<char>(0x80), 4, 5, 6, static_cast<char>(0x80), 8, 9, 10, static_cast<char>(0x80), 12, 13, 14, static_cast<char>(0x80));
    const __m256i zero = _mm256_setzero_si256();

    // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) // __m256i in std::array triggers GCC -Wignored-attributes
    __m256i pxv[2];
    pxv[0] = _mm256_cvtepu8_epi16(_mm_shuffle_epi8(px01, shuf4)); // pixels 0..3
    pxv[1] = _mm256_cvtepu8_epi16(_mm_shuffle_epi8(px23, shuf4)); // pixels 4..7

    // Int16 lane layout: [r0,g0,b0,a0, r1,g1,b1,a1, r2,g2,b2,a2, r3,g3,b3,a3] (alpha lanes forced to 0).
    const __m256i base256 = _mm256_set_epi16(0, baseColor.b, baseColor.g, baseColor.r,
                                             0, baseColor.b, baseColor.g, baseColor.r,
                                             0, baseColor.b, baseColor.g, baseColor.r,
                                             0, baseColor.b, baseColor.g, baseColor.r);
    const __m256i kMask = _mm256_set_epi16(0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), 0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), 0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), 0, static_cast<short>(0xFFFF), static_cast<short>(0xFFFF), static_cast<short>(0xFFFF));
    const __m256i k255 = _mm256_set1_epi16(255);

    for (uint32_t t = 0; t < IntenModifierValues; t++) {
        const int* inten = sEtc1IntenTables[t].data();

        __m256i cvec[4];
        for (uint32_t s = 0; s < 4; s++) {
            const __m256i ydv = _mm256_set1_epi16(static_cast<short>(inten[s]));
            __m256i c = _mm256_and_si256(_mm256_add_epi16(base256, ydv), kMask);
            c = _mm256_max_epi16(c, zero);
            c = _mm256_min_epi16(c, k255);
            cvec[s] = c;
        }

        __m256i bestErr[2];
        __m256i bestSel[2];

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

        uint8_t* dst = selectors + static_cast<size_t>(t) * 8;
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
    // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
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
    const uint32_t n = mParams->mNumSrcPixels;
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

                // Now we have the input block, the avg. color of the input pixels, a set of trial selector indices, and the block color+intensity index.
                // Now, for each component, attempt to refine the current solution by solving a simple linear equation. For example, for 4 colors:
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
                        deltaSumR += Etc1::clamp<int>(baseColor.r + yd, 0, 255) - baseColor.r;
                        deltaSumG += Etc1::clamp<int>(baseColor.g + yd, 0, 255) - baseColor.g;
                        deltaSumB += Etc1::clamp<int>(baseColor.b + yd, 0, 255) - baseColor.b;
                    }
                    if ((!deltaSumR) && (!deltaSumG) && (!deltaSumB)) {
                        break;
                    }
                    const float avgDeltaRF = static_cast<float>(deltaSumR) / static_cast<float>(n);
                    const float avgDeltaGF = static_cast<float>(deltaSumG) / static_cast<float>(n);
                    const float avgDeltaBF = static_cast<float>(deltaSumB) / static_cast<float>(n);
                    const auto limitF = static_cast<float>(mLimit);
                    const auto br1 = Etc1::clamp<int>(static_cast<int>(std::lroundf((mAvgColor[0] - avgDeltaRF) * limitF / 255.0f)), 0, mLimit);
                    const auto bg1 = Etc1::clamp<int>(static_cast<int>(std::lroundf((mAvgColor[1] - avgDeltaGF) * limitF / 255.0f)), 0, mLimit);
                    const auto bb1 = Etc1::clamp<int>(static_cast<int>(std::lroundf((mAvgColor[2] - avgDeltaBF) * limitF / 255.0f)), 0, mLimit);

                    const bool skip = ((mbr == br1) && (mbg == bg1) && (mbb == bb1)) || ((br1 == mBestSolution.mCoords.mUnscaledColor.r) && (bg1 == mBestSolution.mCoords.mUnscaledColor.g) && (bb1 == mBestSolution.mCoords.mUnscaledColor.b)) || ((mBr == br1) && (mBg == bg1) && (mBb == bb1));

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

            } // xdi
        } // ydi
    } // zdi

    if (!mBestSolution.mValid) {
        mResult->mError = sUint32Max;
        return false;
    }

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
    assert(p.mNumSrcPixels == 8);

    mParams = &p;
    mResult = &r;

    const uint32_t n = 8;

    mLimit = mParams->mUseColor4 ? 15 : 31;

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
    mBr = Etc1::clamp<int>(static_cast<int>(std::lroundf(mAvgColor[0] * limitF / 255.0f)), 0, mLimit);
    mBg = Etc1::clamp<int>(static_cast<int>(std::lroundf(mAvgColor[1] * limitF / 255.0f)), 0, mLimit);
    mBb = Etc1::clamp<int>(static_cast<int>(std::lroundf(mAvgColor[2] * limitF / 255.0f)), 0, mLimit);

    if (mParams->mQuality <= Etc1Quality::Medium) {
        mSortedLumaIndices = indirectRadixSort(n, mSortedLuma[0].data(), mSortedLuma[1].data(), mLuma.data(), 0, sizeof(mLuma[0]), false);
        mSortedLumaBuf = mSortedLuma[0].data();
        if (mSortedLumaIndices == mSortedLuma[0].data()) {
            mSortedLumaBuf = mSortedLuma[1].data();
        }

        for (uint32_t i = 0; i < n; i++) {
            mSortedLumaBuf[i] = mLuma[mSortedLumaIndices[i]];
        }
    }

    mBestSolution.mCoords.clear();
    mBestSolution.mValid = false;
    mBestSolution.mError = sUint64Max;
}

bool Etc1Optimizer::evaluateSolution(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution) {
    trialSolution.mValid = false;

    if (mParams->mConstrainAgainstBaseColor5) {
        const int dr = coords.mUnscaledColor.r - mParams->mBaseColor5.r;
        const int dg = coords.mUnscaledColor.g - mParams->mBaseColor5.g;
        const int db = coords.mUnscaledColor.b - mParams->mBaseColor5.b;

        if ((Etc1::minimum(dr, dg, db) < ColorDeltaMin) || (Etc1::maximum(dr, dg, db) > ColorDeltaMax)) {
            return false;
        }
    }

    const ColorQuad baseColor(coords.getScaledColor());

    trialSolution.mError = sUint64Max;

    const EvaluateIntenTablesFunc evalIntenTables = getCachedEvalIntenTables();

    std::array<uint64_t, IntenModifierValues> tableErrors;
    std::array<uint8_t, static_cast<std::size_t>(IntenModifierValues) * 8> tableSelectors;
    evalIntenTables(mParams->mSrcPixels, baseColor, tableErrors.data(), tableSelectors.data());

    if (sBuildDebug) {
        std::array<uint64_t, IntenModifierValues> scalarErrors;
        std::array<uint8_t, static_cast<std::size_t>(IntenModifierValues) * 8> scalarSelectors;
        evaluateIntenTablesScalar(mParams->mSrcPixels, baseColor, scalarErrors.data(), scalarSelectors.data());
        assert(memcmp(scalarErrors.data(), tableErrors.data(), sizeof(tableErrors)) == 0);
        assert(memcmp(scalarSelectors.data(), tableSelectors.data(), sizeof(tableSelectors)) == 0);
    }

    for (uint32_t intenTable = 0; intenTable < IntenModifierValues; intenTable++) {
        const uint64_t totalError = tableErrors[intenTable];

        if (totalError < trialSolution.mError) {
            trialSolution.mError = totalError;
            trialSolution.mCoords.mIntenTable = intenTable;
            std::memcpy(trialSolution.mSelectors.data(), &tableSelectors[static_cast<size_t>(intenTable) * 8], 8);
            trialSolution.mValid = true;
        }
    }
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

bool Etc1Optimizer::evaluateSolutionFast(const Etc1SolutionCoordinates& coords, PotentialSolution& trialSolution, PotentialSolution* bestSolution) {
    if (mParams->mConstrainAgainstBaseColor5) {
        const int dr = coords.mUnscaledColor.r - mParams->mBaseColor5.r;
        const int dg = coords.mUnscaledColor.g - mParams->mBaseColor5.g;
        const int db = coords.mUnscaledColor.b - mParams->mBaseColor5.b;

        if ((Etc1::minimum(dr, dg, db) < ColorDeltaMin) || (Etc1::maximum(dr, dg, db) > ColorDeltaMax)) {
            trialSolution.mValid = false;
            return false;
        }
    }

    const ColorQuad baseColor(coords.getScaledColor());

    const uint32_t n = 8;

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

        // evaluateSolutionFast() enforces/assumesd a total ordering of the input colors along the intensity (1,1,1) axis to more quickly classify the inputs to selectors.
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

    bool success = false;
    if (bestSolution) {
        if (trialSolution.mError < bestSolution->mError) {
            *bestSolution = trialSolution;
            success = true;
        }
    }

    return success;
}

// Packs solid color blocks efficiently using a set of small precomputed tables.
// For random 888 inputs, MSE results are better than Erricson's ETC1 packer in "slow" mode ~9.5% of the time, is slightly worse only ~.01% of the time, and is equal the rest of the time.
static uint64_t packEtc1BlockSolidColor(Etc1Block& block, const uint8_t* color, [[maybe_unused]] Etc1PackParams& packParams) {
    assert(sEtc1InverseLookup[0][255]);

    static const std::array<uint32_t, 4> sNextComp = {1, 2, 0, 1};

    uint32_t bestError = sUint32Max, bestI = 0;
    int bestX = 0, bestPackedC1 = 0, bestPackedC2 = 0;

    // For each possible 8-bit value, there is a precomputed list of diff/inten/selector configurations that allow that 8-bit value to be encoded with no error.
    for (uint32_t i = 0; i < 3; i++) {
        const uint32_t c1 = color[sNextComp[i]], c2 = color[sNextComp[i + 1]];

        const int deltaRange = 1;
        for (int delta = -deltaRange; delta <= deltaRange; delta++) {
            const int cPlusDelta = Etc1::clamp<int>(color[i] + delta, 0, 255);

            const uint16_t* table;
            if (!cPlusDelta) {
                table = sColor8ToEtcConfig0To255[0].data();
            } else if (cPlusDelta == 255) {
                table = sColor8ToEtcConfig0To255[1].data();
            } else {
                table = sColor8ToEtcConfig1To254[cPlusDelta - 1].data();
            }

            for (;;) {
                const uint32_t x = *table++;

                if (sBuildDebug) {
                    // (x >> 4) & 3 is the selector, (x >> 8) & 255 the base component; the packed
                    // table entry must decode back to cPlusDelta.
                    assert(etc1DecodeValue(x & 1, (x >> 1) & 7, (x >> 4) & 3, (x >> 8) & 255) == static_cast<uint32_t>(cPlusDelta));
                }

                const uint16_t* inverseTable = sEtc1InverseLookup[x & 0xFF].data();
                uint16_t comp1 = inverseTable[c1];
                uint16_t comp2 = inverseTable[c2];
                const uint32_t trialError = Etc1::square(cPlusDelta - color[i]) + Etc1::square(comp1 >> 8) + Etc1::square(comp2 >> 8);
                if (trialError < bestError) {
                    bestError = trialError;
                    bestX = static_cast<int>(x);
                    bestPackedC1 = comp1 & 0xFF;
                    bestPackedC2 = comp2 & 0xFF;
                    bestI = i;
                    if (!bestError) {
                        goto foundPerfectMatch;
                    }
                }
                if (*table == 0xFFFF) {
                    break;
                }
            }
        }
    }
foundPerfectMatch:

    const uint32_t diff = bestX & 1;
    const uint32_t inten = (bestX >> 1) & 7;

    block.mBytes[3] = static_cast<uint8_t>(((inten | (inten << 3)) << 2) | (diff << 1));

    const uint32_t etc1Selector = sSelectorIndexToEtc1[(bestX >> 4) & 3];
    const auto selectorWords0 = static_cast<uint16_t>((etc1Selector & 2) ? 0xFFFF : 0);
    const auto selectorWords1 = static_cast<uint16_t>((etc1Selector & 1) ? 0xFFFF : 0);
    std::memcpy(&block.mBytes[4], &selectorWords0, sizeof(selectorWords0));
    std::memcpy(&block.mBytes[6], &selectorWords1, sizeof(selectorWords1));

    const uint32_t bestPackedC0 = (bestX >> 8) & 255;
    if (diff) {
        block.mBytes[bestI] = static_cast<uint8_t>(bestPackedC0 << 3);
        block.mBytes[sNextComp[bestI]] = static_cast<uint8_t>(bestPackedC1 << 3);
        block.mBytes[sNextComp[bestI + 1]] = static_cast<uint8_t>(bestPackedC2 << 3);
    } else {
        block.mBytes[bestI] = static_cast<uint8_t>(bestPackedC0 | (bestPackedC0 << 4));
        block.mBytes[sNextComp[bestI]] = static_cast<uint8_t>(bestPackedC1 | (bestPackedC1 << 4));
        block.mBytes[sNextComp[bestI + 1]] = static_cast<uint8_t>(bestPackedC2 | (bestPackedC2 << 4));
    }

    return bestError;
}

static uint32_t packEtc1BlockSolidColorConstrained(
    Etc1Optimizer::Results& results,
    uint32_t numColors, const uint8_t* color,
    [[maybe_unused]] Etc1PackParams& packParams,
    bool useDiff,
    const ColorQuad* baseColor5Unscaled) {
    assert(sEtc1InverseLookup[0][255]);

    static const std::array<uint32_t, 4> sNextComp = {1, 2, 0, 1};

    uint32_t bestError = sUint32Max, bestI = 0;
    int bestX = 0, bestPackedC1 = 0, bestPackedC2 = 0;

    // For each possible 8-bit value, there is a precomputed list of diff/inten/selector configurations that allow that 8-bit value to be encoded with no error.
    for (uint32_t i = 0; i < 3; i++) {
        const uint32_t c1 = color[sNextComp[i]], c2 = color[sNextComp[i + 1]];

        const int deltaRange = 1;
        for (int delta = -deltaRange; delta <= deltaRange; delta++) {
            const int cPlusDelta = Etc1::clamp<int>(color[i] + delta, 0, 255);

            const uint16_t* table;
            if (!cPlusDelta) {
                table = sColor8ToEtcConfig0To255[0].data();
            } else if (cPlusDelta == 255) {
                table = sColor8ToEtcConfig0To255[1].data();
            } else {
                table = sColor8ToEtcConfig1To254[cPlusDelta - 1].data();
            }

            for (;;) {
                const uint32_t x = *table++;
                const uint32_t diff = x & 1;
                if (static_cast<uint32_t>(useDiff) != diff) {
                    if (*table == 0xFFFF) {
                        break;
                    }
                    continue;
                }

                if ((diff) && (baseColor5Unscaled)) {
                    const int comp0 = static_cast<int>((x >> 8) & 255);
                    int delta = comp0 - static_cast<int>(baseColor5Unscaled->c[i]);
                    if ((delta < ColorDeltaMin) || (delta > ColorDeltaMax)) {
                        if (*table == 0xFFFF) {
                            break;
                        }
                        continue;
                    }
                }

                if (sBuildDebug) {
                    // (x >> 4) & 3 is the selector, (x >> 8) & 255 the base component; the packed
                    // table entry must decode back to cPlusDelta.
                    assert(etc1DecodeValue(diff, (x >> 1) & 7, (x >> 4) & 3, (x >> 8) & 255) == static_cast<uint32_t>(cPlusDelta));
                }

                const uint16_t* inverseTable = sEtc1InverseLookup[x & 0xFF].data();
                uint16_t comp1 = inverseTable[c1];
                uint16_t comp2 = inverseTable[c2];

                if ((diff) && (baseColor5Unscaled)) {
                    int delta1 = (comp1 & 0xFF) - static_cast<int>(baseColor5Unscaled->c[sNextComp[i]]);
                    int delta2 = (comp2 & 0xFF) - static_cast<int>(baseColor5Unscaled->c[sNextComp[i + 1]]);
                    if ((delta1 < ColorDeltaMin) || (delta1 > ColorDeltaMax) || (delta2 < ColorDeltaMin) || (delta2 > ColorDeltaMax)) {
                        if (*table == 0xFFFF) {
                            break;
                        }
                        continue;
                    }
                }

                const uint32_t trialError = Etc1::square(cPlusDelta - color[i]) + Etc1::square(comp1 >> 8) + Etc1::square(comp2 >> 8);
                if (trialError < bestError) {
                    bestError = trialError;
                    bestX = static_cast<int>(x);
                    bestPackedC1 = comp1 & 0xFF;
                    bestPackedC2 = comp2 & 0xFF;
                    bestI = i;
                    if (!bestError) {
                        goto foundPerfectMatch;
                    }
                }
                if (*table == 0xFFFF) {
                    break;
                }
            }
        }
    }
foundPerfectMatch:

    if (bestError == sUint32Max) {
        return bestError;
    }

    bestError *= numColors;

    results.mN = numColors;
    results.mBlockColor4 = !(bestX & 1);
    results.mBlockIntenTable = (bestX >> 1) & 7;
    std::memset(results.mSelectors, (bestX >> 4) & 3, numColors);

    const uint32_t bestPackedC0 = (bestX >> 8) & 255;
    results.mBlockColorUnscaled[bestI] = static_cast<uint8_t>(bestPackedC0);
    results.mBlockColorUnscaled[sNextComp[bestI]] = static_cast<uint8_t>(bestPackedC1);
    results.mBlockColorUnscaled[sNextComp[bestI + 1]] = static_cast<uint8_t>(bestPackedC2);
    results.mError = bestError;

    return bestError;
}

// Function originally from RYG's public domain real-time DXT1 compressor, modified for 555.
static void ditherBlock555(ColorQuad* dest, const ColorQuad* block) {
    std::array<int, 8> err;
    int* ep1 = err.data();
    int* ep2 = err.data() + 4;
    const uint8_t* quant = sQuant5Tab.data() + 8;

    std::memset(dest, 0xFF, sizeof(ColorQuad) * 16);

    // process channels seperately
    for (int ch = 0; ch < 3; ch++) {
        const auto* bp = std::bit_cast<const uint8_t*>(block);
        auto* dp = std::bit_cast<uint8_t*>(dest);

        bp += ch;
        dp += ch;

        std::memset(err.data(), 0, sizeof(err));
        for (int y = 0; y < 4; y++) {
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

uint32_t packEtc1Block(void* etc1Block, const uint32_t* srcPixelsRgba, Etc1PackParams& packParams) {
    const auto* srcPixels = std::bit_cast<const ColorQuad*>(srcPixelsRgba);
    Etc1Block& dstBlock = *static_cast<Etc1Block*>(etc1Block);

    if (sBuildDebug) {
        // Ensure all alpha values are 0xFF.
        for (uint32_t i = 0; i < 16; i++) {
            assert(srcPixels[i].a == 255);
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

    std::array<std::array<uint8_t, 8>, 2> bestSelectors;
    std::array<Etc1Optimizer::Results, 2> bestResults;
    for (uint32_t i = 0; i < 2; i++) {
        bestResults[i].mN = 8;
        bestResults[i].mSelectors = bestSelectors[i].data();
    }

    std::array<std::array<uint8_t, 8>, 3> selectorScratch;
    std::array<Etc1Optimizer::Results, 3> results;

    for (uint32_t i = 0; i < 3; i++) {
        results[i].mN = 8;
        results[i].mSelectors = selectorScratch[i].data();
    }

    std::array<ColorQuad, 8> subblockPixels;

    Etc1Optimizer::Params params(packParams);
    params.mNumSrcPixels = 8;
    params.mSrcPixels = subblockPixels.data();

    for (uint32_t flip = 0; flip < 2; flip++) {
        for (uint32_t useColor4 = 0; useColor4 < 2; useColor4++) {
            uint64_t trialError = 0;

            uint32_t subblock;
            for (subblock = 0; subblock < 2; subblock++) {
                if (flip) {
                    std::memcpy(subblockPixels.data(), srcPixels + static_cast<size_t>(subblock) * 8, sizeof(ColorQuad) * 8);
                } else {
                    const ColorQuad* srcCol = srcPixels + static_cast<size_t>(subblock) * 2;
                    subblockPixels[0] = srcCol[0];
                    subblockPixels[1] = srcCol[4];
                    subblockPixels[2] = srcCol[8];
                    subblockPixels[3] = srcCol[12];
                    subblockPixels[4] = srcCol[1];
                    subblockPixels[5] = srcCol[5];
                    subblockPixels[6] = srcCol[9];
                    subblockPixels[7] = srcCol[13];
                }

                results[2].mError = sUint64Max;
                if ((params.mQuality >= Etc1Quality::Medium) && ((subblock) || (useColor4))) {
                    const uint32_t subblockPixel0U32 = subblockPixels[0].mU32;
                    for (r = 7; r >= 1; --r) {
                        if (subblockPixels[r].mU32 != subblockPixel0U32) {
                            break;
                        }
                    }
                    if (!r) {
                        packEtc1BlockSolidColorConstrained(results[2], 8, &subblockPixels[0].r, packParams, !useColor4, (subblock && !useColor4) ? &results[0].mBlockColorUnscaled : nullptr);
                    }
                }

                params.mUseColor4 = (useColor4 != 0);
                params.mConstrainAgainstBaseColor5 = false;

                if ((!useColor4) && (subblock)) {
                    params.mConstrainAgainstBaseColor5 = true;
                    params.mBaseColor5 = results[0].mBlockColorUnscaled;
                }

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

                optimizer.init(params, results[subblock]);
                if (!optimizer.compute()) {
                    break;
                }

                if (params.mQuality >= Etc1Quality::Medium) {
                    // TODO: Fix fairly arbitrary/unrefined thresholds that control how far away to scan for potentially better solutions.
                    const uint32_t refinementErrorThresh0 = 3000;
                    const uint32_t refinementErrorThresh1 = 6000;
                    if (results[subblock].mError > refinementErrorThresh0) {
                        if (params.mQuality == Etc1Quality::Medium) {
                            static const std::array<int, 4> sScanDelta2To3 = {-3, -2, 2, 3};
                            params.mScanDeltaSize = static_cast<uint32_t>(std::size(sScanDelta2To3));
                            params.mScanDeltas = sScanDelta2To3.data();
                        } else {
                            static const std::array<int, 2> sScanDelta5To5 = {-5, 5};
                            static const std::array<int, 8> sScanDelta5To8 = {-8, -7, -6, -5, 5, 6, 7, 8};
                            if (results[subblock].mError > refinementErrorThresh1) {
                                params.mScanDeltaSize = static_cast<uint32_t>(std::size(sScanDelta5To8));
                                params.mScanDeltas = sScanDelta5To8.data();
                            } else {
                                params.mScanDeltaSize = static_cast<uint32_t>(std::size(sScanDelta5To5));
                                params.mScanDeltas = sScanDelta5To5.data();
                            }
                        }

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

    int dr = bestResults[1].mBlockColorUnscaled.r - bestResults[0].mBlockColorUnscaled.r;
    int dg = bestResults[1].mBlockColorUnscaled.g - bestResults[0].mBlockColorUnscaled.g;
    int db = bestResults[1].mBlockColorUnscaled.b - bestResults[0].mBlockColorUnscaled.b;
    assert(bestUseColor4 || ((Etc1::minimum(dr, dg, db) >= ColorDeltaMin) && (Etc1::maximum(dr, dg, db) <= ColorDeltaMax)));

    if (bestUseColor4) {
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

    dstBlock.mBytes[3] = static_cast<uint8_t>((bestResults[1].mBlockIntenTable << 2) | (bestResults[0].mBlockIntenTable << 5) | ((~bestUseColor4 & 1) << 1) | bestFlip);

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

    dstBlock.mBytes[4] = static_cast<uint8_t>(selector1 >> 8);
    dstBlock.mBytes[5] = static_cast<uint8_t>(selector1 & 0xFF);
    dstBlock.mBytes[6] = static_cast<uint8_t>(selector0 >> 8);
    dstBlock.mBytes[7] = static_cast<uint8_t>(selector0 & 0xFF);

    return static_cast<uint32_t>(bestError);
}
// NOLINTEND(cert-dcl03-c)
} // namespace Etc1
