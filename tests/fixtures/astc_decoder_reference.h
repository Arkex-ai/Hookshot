// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Frozen partition and endpoint reference from Hookshot 51da0464 astc_decoder.comp.
// GLSL vector/bitfield adapters live in the C++ test harness. This is host shader
// source, not game-derived data. Do not regenerate it from the optimized shader.
namespace reference {
uvec2 block_dims;
struct EncodingData {uint data;};
constexpr uint JUST_BITS=0, QUINT=1, TRIT=2;
constexpr uint encoding_values[22] = {
    (JUST_BITS), (JUST_BITS | (1u << 8u)), (TRIT), (JUST_BITS | (2u << 8u)),
    (QUINT), (TRIT | (1u << 8u)), (JUST_BITS | (3u << 8u)), (QUINT | (1u << 8u)),
    (TRIT | (2u << 8u)), (JUST_BITS | (4u << 8u)), (QUINT | (2u << 8u)), (TRIT | (3u << 8u)),
    (JUST_BITS | (5u << 8u)), (QUINT | (3u << 8u)), (TRIT | (4u << 8u)), (JUST_BITS | (6u << 8u)),
    (QUINT | (4u << 8u)), (TRIT | (5u << 8u)), (JUST_BITS | (7u << 8u)), (QUINT | (5u << 8u)),
    (TRIT | (6u << 8u)), (JUST_BITS | (8u << 8u))};


uint Hash52(uint p) {
    p ^= p >> 15;
    p -= p << 17;
    p += p << 7;
    p += p << 4;
    p ^= p >> 5;
    p += p << 16;
    p ^= p >> 7;
    p ^= p >> 3;
    p ^= p << 6;
    p ^= p >> 17;
    return p;
}
uint Select2DPartition(uint seed, uint x, uint y, uint partition_count) {
    if ((block_dims.y * block_dims.x) < 32) {
        x <<= 1;
        y <<= 1;
    }

    seed += (partition_count - 1) * 1024;

    const uint rnum = Hash52(uint(seed));
    uint seed1 = uint(rnum & 0xF);
    uint seed2 = uint((rnum >> 4) & 0xF);
    uint seed3 = uint((rnum >> 8) & 0xF);
    uint seed4 = uint((rnum >> 12) & 0xF);
    uint seed5 = uint((rnum >> 16) & 0xF);
    uint seed6 = uint((rnum >> 20) & 0xF);
    uint seed7 = uint((rnum >> 24) & 0xF);
    uint seed8 = uint((rnum >> 28) & 0xF);

    seed1 = (seed1 * seed1);
    seed2 = (seed2 * seed2);
    seed3 = (seed3 * seed3);
    seed4 = (seed4 * seed4);
    seed5 = (seed5 * seed5);
    seed6 = (seed6 * seed6);
    seed7 = (seed7 * seed7);
    seed8 = (seed8 * seed8);

    uint sh1, sh2;
    if ((seed & 1) > 0) {
        sh1 = (seed & 2) > 0 ? 4 : 5;
        sh2 = (partition_count == 3) ? 6 : 5;
    } else {
        sh1 = (partition_count == 3) ? 6 : 5;
        sh2 = (seed & 2) > 0 ? 4 : 5;
    }
    seed1 >>= sh1;
    seed2 >>= sh2;
    seed3 >>= sh1;
    seed4 >>= sh2;
    seed5 >>= sh1;
    seed6 >>= sh2;
    seed7 >>= sh1;
    seed8 >>= sh2;

    uint a = seed1 * x + seed2 * y + (rnum >> 14);
    uint b = seed3 * x + seed4 * y + (rnum >> 10);
    uint c = seed5 * x + seed6 * y + (rnum >> 6);
    uint d = seed7 * x + seed8 * y + (rnum >> 2);

    a &= 0x3F;
    b &= 0x3F;
    c &= 0x3F;
    d &= 0x3F;

    if (partition_count < 4) {
        d = 0;
    }
    if (partition_count < 3) {
        c = 0;
    }

    if (a >= b && a >= c && a >= d) {
        return 0;
    } else if (b >= c && b >= d) {
        return 1;
    } else if (c >= d) {
        return 2;
    } else {
        return 3;
    }
}
uint Encoding(EncodingData val) {
    return bitfieldExtract(val.data, 0, 8);
}
uint NumBits(EncodingData val) {
    return bitfieldExtract(val.data, 8, 8);
}
uint Div3Floor(uint v) {
    return (v * 0x5556) >> 16;
}
uint Div3Ceil(uint v) {
    return Div3Floor(v + 2);
}
uint Div5Floor(uint v) {
    return (v * 0x3334) >> 16;
}
uint Div5Ceil(uint v) {
    return Div5Floor(v + 4);
}
uint GetBitLength(uint n_vals, uint encoding_index) {
    const EncodingData encoding_value = EncodingData(encoding_values[encoding_index]);
    const uint encoding = Encoding(encoding_value);
    uint total_bits = NumBits(encoding_value) * n_vals;
    if (encoding == TRIT) {
        total_bits += Div5Ceil(n_vals * 8);
    } else if (encoding == QUINT) {
        total_bits += Div3Ceil(n_vals * 7);
    }
    return total_bits;
}
uint endpoint(uint num_values,uint color_data_bits){
    int range = 0;
    while (++range < 22) {
        const uint bit_length = GetBitLength(num_values, range);
        if (bit_length > color_data_bits) {
            break;
        }
    }
return uint(range-1);}
}
