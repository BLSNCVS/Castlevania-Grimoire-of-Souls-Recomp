#include "../msgpack.h"
#include <cassert>
#include <cstdio>
using namespace gosr::msgpack;
int main()
{
    Value m = Value::Map();
    m.Set("a", 1);
    m.Set("neg", int64_t(-70000));
    m.Set("big", uint64_t(0xF000000000000000ull));
    m.Set("s", std::string(40, 'x'));
    m.Set("f", 1.5);
    Value arr = Value::Array();
    for (int i = 0; i < 20; i++) arr.Push(i * 1000);
    m.Set("arr", arr);
    m.Set("nil", nullptr);
    auto bytes = Encode(m);
    Value back;
    size_t used = 0;
    assert(Decode(bytes.data(), bytes.size(), back, &used) && used == bytes.size());
    assert(Encode(back) == bytes);
    assert(back["neg"].AsInt() == -70000 && back["arr"].Items()[19].AsInt() == 19000);
    assert(uint64_t(back["big"].AsInt()) == 0xF000000000000000ull);
    printf("ok %zu bytes: %s\n", bytes.size(), back.Dump().substr(0, 80).c_str());
}
