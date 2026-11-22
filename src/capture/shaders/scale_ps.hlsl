// Downscales one axis with a Lanczos-3 kernel widened by the scale, so every source pixel counts and nothing
// aliases. Works on the gamma-encoded values, as ffmpeg's scalers do. No clamp against the kernel's overshoot: on
// the issue 179 bench clips it scored within 0.1-2.2 VMAF of ffmpeg's Lanczos
// (4.0 on a static menu at 1080p) with no visible halos.
Texture2D<float4> source : register(t0);

cbuffer ScalePass : register(b0) {
    float2 viewOrigin; // Target pixel where the pass's viewport starts.
    float sourceStart; // Source coordinate (along the axis) of the scaled region's first edge.
    float scale;       // Source pixels per output pixel along the axis, at least 1.
    int acrossStart;   // Source pixel (across the axis) of the viewport's first row or column.
    int low;           // First and last source pixel along the axis that may be read.
    int high;
    uint vertical;     // 0 scales x, 1 scales y.
};

static const float lobes = 3;

float lanczos(float x) {
    x = abs(x);
    if (x < 1e-5) return 1;
    if (x >= lobes) return 0;
    const float px = 3.14159265 * x;
    return lobes * sin(px) * sin(px / lobes) / (px * px);
}

float4 main(float4 position : SV_Position) : SV_Target {
    const float2 local = position.xy - viewOrigin;
    const float centre = sourceStart + (vertical ? local.y : local.x) * scale;
    const int across = int(vertical ? local.x : local.y) + acrossStart;
    const float reach = lobes * scale;
    // Taps past the region's edge are left out and the rest renormalised.
    const int first = max(int(floor(centre - reach)), low);
    const int last = min(int(ceil(centre + reach)), high);
    float3 sum = 0;
    float total = 0;
    [loop] for (int i = first; i <= last; ++i) {
        const float weight = lanczos((i + 0.5 - centre) / scale);
        sum += weight * source.Load(int3(vertical ? int2(across, i) : int2(i, across), 0)).rgb;
        total += weight;
    }
    // The first pass keeps its overshoot in a float texture; only the final values are clipped.
    const float3 colour = sum / total;
    return float4(vertical ? saturate(colour) : colour, 1);
}
