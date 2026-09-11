param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$sourcePath = Join-Path $ProjectRoot 'native\src\single_biome_radius.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "Source file not found: $sourcePath"
}

$text = [System.IO.File]::ReadAllText($sourcePath)

if ($text.Contains('TUNDRA_P11_FAST_PERMUTATION_RNG')) {
    Write-Host 'Tundra P11 fast exact permutation RNG is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P10_PARALLEL_OCTAVE_INIT')) {
    throw 'P11 requires the P10 parallel-octave initializer first.'
}
if (-not $text.Contains('TUNDRA_P9_BOUNDED_RAIN')) {
    throw 'P11 requires the P9 bounded-rain scout.'
}
if (-not $text.Contains('TUNDRA_P8_LAZY_RAIN')) {
    throw 'P11 requires the P8 lazy-rain base.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P11 requires exact 864x864 square semantics.'
}

$backupPath = $sourcePath + '.p10-before-p11.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# ---------------------------------------------------------------------------
# P11 strategy
# ---------------------------------------------------------------------------
# P10 parallelized octave construction, but both the real Fisher-Yates build
# and every RNG-only replay still call Java Random.nextInt(bound) 256 times per
# octave. For non-power-of-two bounds the generic C++ '%' lowers to an expensive
# variable integer divide/modulo on the GPU.
#
# Here bound is always 1..256. For non-powers of two, precompute
#   m = floor(2^32 / bound)
# and compute
#   q0 = high32(bits * m)
#   r  = bits - q0*bound
#   if (r >= bound) r -= bound
#
# Because m <= 2^32/d and m > 2^32/d - 1, q0 is never above floor(bits/d)
# and can be at most one below it for bits < 2^32. Therefore one correction
# produces bits % bound EXACTLY. Java's signed overflow rejection test is then
# preserved verbatim. Powers of two keep Java's original multiply/shift path.
#
# P11 also replaces discarded nextDouble() sequences with exact affine jumps
# of Java's 48-bit LCG: two steps for the unused c offset and six steps when
# replaying all a/b/c offsets of a preceding octave. These jumps are exact
# compositions of the original LCG, not approximate skip-ahead logic.
# ---------------------------------------------------------------------------

$initMarker = '__device__ __forceinline__ void initSearchPerlin('
if ([regex]::Matches($text, [regex]::Escape($initMarker)).Count -ne 1) {
    throw 'Could not find unique initSearchPerlin insertion point.'
}

$helper = @'
// TUNDRA_P11_FAST_PERMUTATION_RNG
// floor(2^32 / d), d=0..256. Entries for powers of two are unused because
// Java's exact power-of-two nextInt path is cheaper and is kept separately.
__device__ __constant__ unsigned int P11_RECIP32[257] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x55555555u, 0x00000000u, 0x33333333u, 0x2AAAAAAAu, 0x24924924u,
    0x00000000u, 0x1C71C71Cu, 0x19999999u, 0x1745D174u, 0x15555555u, 0x13B13B13u, 0x12492492u, 0x11111111u,
    0x00000000u, 0x0F0F0F0Fu, 0x0E38E38Eu, 0x0D79435Eu, 0x0CCCCCCCu, 0x0C30C30Cu, 0x0BA2E8BAu, 0x0B21642Cu,
    0x0AAAAAAAu, 0x0A3D70A3u, 0x09D89D89u, 0x097B425Eu, 0x09249249u, 0x08D3DCB0u, 0x08888888u, 0x08421084u,
    0x00000000u, 0x07C1F07Cu, 0x07878787u, 0x07507507u, 0x071C71C7u, 0x06EB3E45u, 0x06BCA1AFu, 0x06906906u,
    0x06666666u, 0x063E7063u, 0x06186186u, 0x05F417D0u, 0x05D1745Du, 0x05B05B05u, 0x0590B216u, 0x0572620Au,
    0x05555555u, 0x05397829u, 0x051EB851u, 0x05050505u, 0x04EC4EC4u, 0x04D4873Eu, 0x04BDA12Fu, 0x04A7904Au,
    0x04924924u, 0x047DC11Fu, 0x0469EE58u, 0x0456C797u, 0x04444444u, 0x04325C53u, 0x04210842u, 0x04104104u,
    0x00000000u, 0x03F03F03u, 0x03E0F83Eu, 0x03D22635u, 0x03C3C3C3u, 0x03B5CC0Eu, 0x03A83A83u, 0x039B0AD1u,
    0x038E38E3u, 0x0381C0E0u, 0x03759F22u, 0x0369D036u, 0x035E50D7u, 0x03531DECu, 0x03483483u, 0x033D91D2u,
    0x03333333u, 0x0329161Fu, 0x031F3831u, 0x03159721u, 0x030C30C3u, 0x03030303u, 0x02FA0BE8u, 0x02F14990u,
    0x02E8BA2Eu, 0x02E05C0Bu, 0x02D82D82u, 0x02D02D02u, 0x02C8590Bu, 0x02C0B02Cu, 0x02B93105u, 0x02B1DA46u,
    0x02AAAAAAu, 0x02A3A0FDu, 0x029CBC14u, 0x0295FAD4u, 0x028F5C28u, 0x0288DF0Cu, 0x02828282u, 0x027C4597u,
    0x02762762u, 0x02702702u, 0x026A439Fu, 0x02647C69u, 0x025ED097u, 0x02593F69u, 0x0253C825u, 0x024E6A17u,
    0x02492492u, 0x0243F6F0u, 0x023EE08Fu, 0x0239E0D5u, 0x0234F72Cu, 0x02302302u, 0x022B63CBu, 0x0226B902u,
    0x02222222u, 0x021D9EADu, 0x02192E29u, 0x0214D021u, 0x02108421u, 0x020C49BAu, 0x02082082u, 0x02040810u,
    0x00000000u, 0x01FC07F0u, 0x01F81F81u, 0x01F44659u, 0x01F07C1Fu, 0x01ECC07Bu, 0x01E9131Au, 0x01E573ACu,
    0x01E1E1E1u, 0x01DE5D6Eu, 0x01DAE607u, 0x01D77B65u, 0x01D41D41u, 0x01D0CB58u, 0x01CD8568u, 0x01CA4B30u,
    0x01C71C71u, 0x01C3F8F0u, 0x01C0E070u, 0x01BDD2B8u, 0x01BACF91u, 0x01B7D6C3u, 0x01B4E81Bu, 0x01B20364u,
    0x01AF286Bu, 0x01AC5701u, 0x01A98EF6u, 0x01A6D01Au, 0x01A41A41u, 0x01A16D3Fu, 0x019EC8E9u, 0x019C2D14u,
    0x01999999u, 0x01970E4Fu, 0x01948B0Fu, 0x01920FB4u, 0x018F9C18u, 0x018D3018u, 0x018ACB90u, 0x01886E5Fu,
    0x01861861u, 0x0183C977u, 0x01818181u, 0x017F405Fu, 0x017D05F4u, 0x017AD220u, 0x0178A4C8u, 0x01767DCEu,
    0x01745D17u, 0x01724287u, 0x01702E05u, 0x016E1F76u, 0x016C16C1u, 0x016A13CDu, 0x01681681u, 0x01661EC6u,
    0x01642C85u, 0x01623FA7u, 0x01605816u, 0x015E75BBu, 0x015C9882u, 0x015AC056u, 0x0158ED23u, 0x01571ED3u,
    0x01555555u, 0x01539094u, 0x0151D07Eu, 0x01501501u, 0x014E5E0Au, 0x014CAB88u, 0x014AFD6Au, 0x0149539Eu,
    0x0147AE14u, 0x01460CBCu, 0x01446F86u, 0x0142D662u, 0x01414141u, 0x013FB013u, 0x013E22CBu, 0x013C995Au,
    0x013B13B1u, 0x013991C2u, 0x01381381u, 0x013698DFu, 0x013521CFu, 0x0133AE45u, 0x01323E34u, 0x0130D190u,
    0x012F684Bu, 0x012E025Cu, 0x012C9FB4u, 0x012B404Au, 0x0129E412u, 0x01288B01u, 0x0127350Bu, 0x0125E227u,
    0x01249249u, 0x01234567u, 0x0121FB78u, 0x0120B470u, 0x011F7047u, 0x011E2EF3u, 0x011CF06Au, 0x011BB4A4u,
    0x011A7B96u, 0x01194538u, 0x01181181u, 0x0116E068u, 0x0115B1E5u, 0x011485F0u, 0x01135C81u, 0x0112358Eu,
    0x01111111u, 0x010FEF01u, 0x010ECF56u, 0x010DB20Au, 0x010C9714u, 0x010B7E6Eu, 0x010A6810u, 0x010953F3u,
    0x01084210u, 0x01073260u, 0x010624DDu, 0x0105197Fu, 0x01041041u, 0x0103091Bu, 0x01020408u, 0x01010101u,
    0x00000000u
};

// Exact Java Random.nextInt(bound) for the only bounds used by the scout
// Fisher-Yates shuffle: 1..256. Non-power-of-two remainder uses a reciprocal
// multiply plus at most one correction instead of variable integer division.
__device__ __forceinline__ int p11NextIntSmallExact(
        p20::JavaRandom& random,
        int bound
) {
    if (bound <= 0 || bound > 256) return random.nextInt(bound);

    if ((bound & -bound) == bound) {
        return static_cast<int>(
                (static_cast<std::int64_t>(bound) * random.nextBits(31)) >> 31);
    }

    for (;;) {
        const unsigned int bits = random.nextBits(31);
        const unsigned int d = static_cast<unsigned int>(bound);
        const unsigned int reciprocal = P11_RECIP32[d];
        const unsigned int q0 = static_cast<unsigned int>(
                (static_cast<unsigned long long>(bits) * reciprocal) >> 32);
        unsigned int val = bits - q0 * d;
        if (val >= d) val -= d; // q0 can be at most one too small.

        // Same 32-bit signed-overflow rejection test as JavaRandom::nextInt().
        const unsigned int wrapped = bits - val + (d - 1u);
        if (static_cast<std::int32_t>(wrapped) >= 0) {
            return static_cast<int>(val);
        }
    }
}

// Exact two-step affine composition of Java's 48-bit LCG. Used to consume the
// legacy c-offset nextDouble() whose numerical value the 2-D scout never uses.
__device__ __forceinline__ void p11AdvanceJava2(p20::JavaRandom& random) {
    random.state =
            (random.state * 0xBB20B4600A69ULL + 0x0040942DE6BAULL) & p20::JAVA_MASK;
}

// Exact six-step composition: consumes the three discarded nextDouble() calls
// (a,b,c offsets) when P10 replays a preceding octave's RNG stream.
__device__ __forceinline__ void p11AdvanceJava6(p20::JavaRandom& random) {
    random.state =
            (random.state * 0x45D73749A7F9ULL + 0x17617168255EULL) & p20::JAVA_MASK;
}

'@
$text = $text.Replace($initMarker, $helper + $initMarker)

# initSearchPerlin: keep a/b bit-identical, jump exactly over unused c, and use
# the exact reciprocal nextInt for every Fisher-Yates draw.
$cPattern = '\(void\)random\.nextDouble\(\); // legacy c offset: consume it even though simplex2 does not use it'
if ([regex]::Matches($text, $cPattern).Count -ne 1) {
    throw 'Could not find unique unused c-offset consumption in initSearchPerlin.'
}
$text = [regex]::Replace(
    $text,
    $cPattern,
    'p11AdvanceJava2(random); // exact consume of unused legacy c nextDouble()',
    1
)

$shufflePattern = 'const int j = random\.nextInt\(256 - i\) \+ i;'
if ([regex]::Matches($text, $shufflePattern).Count -ne 1) {
    throw 'Could not find unique scout Fisher-Yates nextInt call.'
}
$text = [regex]::Replace(
    $text,
    $shufflePattern,
    'const int j = p11NextIntSmallExact(random, 256 - i) + i;',
    1
)

# P10 RNG-only replay: six raw LCG advances become one exact affine jump, and
# the 256 variable-modulo nextInt calls use the same exact reciprocal helper.
$sixDrawPattern = '(?s)    // a, b, c offsets: three Java Random\.nextDouble\(\) calls, two state steps each\.\r?\n    \(void\)random\.nextBits\(26\); \(void\)random\.nextBits\(27\);\r?\n    \(void\)random\.nextBits\(26\); \(void\)random\.nextBits\(27\);\r?\n    \(void\)random\.nextBits\(26\); \(void\)random\.nextBits\(27\);'
if ([regex]::Matches($text, $sixDrawPattern).Count -ne 1) {
    throw 'Could not find P10 six-step discarded-offset replay.'
}
$text = [regex]::Replace(
    $text,
    $sixDrawPattern,
    "    // a/b/c values are discarded here; one exact 6-step LCG composition is sufficient.`r`n    p11AdvanceJava6(random);",
    1
)

$consumeNextIntPattern = '\(void\)random\.nextInt\(256 - i\);'
if ([regex]::Matches($text, $consumeNextIntPattern).Count -ne 1) {
    throw 'Could not find unique P10 RNG-only Fisher-Yates nextInt replay.'
}
$text = [regex]::Replace(
    $text,
    $consumeNextIntPattern,
    '(void)p11NextIntSmallExact(random, 256 - i);',
    1
)

# Upgrade banner only; preserve every prior marker for provenance/rollback.
$text = $text.Replace(
    'P10 scout: TUNDRA-only | parallel exact octave init | lazy bounded rain | fused SIMD | warp votes | GPU compaction | tuned 4x16',
    'P11 scout: TUNDRA-only | fast exact permutation RNG | parallel octave init | lazy bounded rain | warp votes | GPU compaction | tuned 4x16'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P11 fast exact permutation RNG.' -ForegroundColor Green
Write-Host 'Scout Fisher-Yates nextInt(1..256) now uses exact reciprocal remainder instead of variable integer modulo.'
Write-Host 'Java nextInt power-of-two behavior and rejection loops are preserved exactly.'
Write-Host 'Discarded c-offset RNG uses an exact 2-step affine LCG jump.'
Write-Host 'P10 preceding-octave replay uses an exact 6-step affine jump for discarded a/b/c values.'
Write-Host 'Permutation contents, RNG states, biome math, probe set, acceptance set and exact verification are unchanged.'
Write-Host 'P10 parallel init, P9 bounded rain, P8 lazy rain, P7 warp votes, P4 compaction and tuned batch are preserved.'
Write-Host 'True 864x864 all-Tundra jackpot recall is unchanged.'
