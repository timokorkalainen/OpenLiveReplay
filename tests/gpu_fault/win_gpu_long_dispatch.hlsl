RWStructuredBuffer<uint> output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint value = id.x + 1;
    [loop]
    for (uint i = 0; i < 1000000; ++i)
        value = value * 1664525u + 1013904223u;
    if (id.x == 0)
        output[0] = value;
}
