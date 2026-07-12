RWStructuredBuffer<uint> state : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint ignored;
    [allow_uav_condition]
    while (state[0] != 0xffffffffu)
        InterlockedAdd(state[0], id.x + 1u, ignored);
}
