RWStructuredBuffer<uint> state : register(u0);
Texture2D<float> workerSurfaceY : register(t0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint ignored;
    [allow_uav_condition]
    while (state[0] == 0u) {
        const uint2 pixel = uint2(id.x & 15u, (id.x >> 4u) & 15u);
        const uint surfaceByte = (uint)(workerSurfaceY.Load(int3(pixel, 0)) * 255.0f);
        InterlockedAdd(state[1], (id.x + 1u) ^ surfaceByte, ignored);
    }
}
