contract Test {
    bytes x;
    function set(bytes memory _a) public { x = _a; }
}
// ====
// compileViaYul: also
// ----
// set(bytes): 0x20, 3, "abc"
// storageEmpty -> false
// set(bytes): 0x20, 0
// storageEmpty -> true
// set(bytes): 0x20, 31, "1234567890123456789012345678901"
// storageEmpty -> false
// set(bytes): 0x20, 36, "12345678901234567890123456789012", "XXXX"
// storageEmpty -> false
// set(bytes): 0x20, 3, "abc"
// storageEmpty -> false
// set(bytes): 0x20, 0
// storageEmpty -> true
// set(bytes): 0x20, 3, "abc"
// storageEmpty -> false
// set(bytes): 0x20, 36, "12345678901234567890123456789012", "XXXX"
// storageEmpty -> false
// set(bytes): 0x20, 0
// storageEmpty -> true
// set(bytes): 0x20, 66, "12345678901234567890123456789012", "12345678901234567890123456789012", "12"
// storageEmpty -> false
// set(bytes): 0x20, 3, "abc"
// storageEmpty -> false
// set(bytes): 0x20, 0
// storageEmpty -> true
