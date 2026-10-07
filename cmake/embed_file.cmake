# Turns a text file into a C++ char array: cmake -DIN=<file> -DOUT=<cpp> -P embed_file.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "// Generated from ${IN}. Do not edit.\nnamespace wf {\nextern const char kWingFollowJsfx[] = {${bytes}0x00};\n}\n")
