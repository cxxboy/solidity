contract c {
    uint spacer1;
    uint spacer2;
    uint[20] data;
    function fill() public {
        for (uint i = 0; i < data.length; ++i) data[i] = i+1;
    }
    function clear() public { delete data; }
}
// ====
// compileViaYul: also
// compileToEwasm: also
// ----
// storageEmpty -> true
// fill() ->
// storageEmpty -> false
// clear() ->
// storageEmpty -> true
