# cmake -DINPUT=<file> -DOUTPUT=<file.c> -DSYMBOL=<name> -P embed_binary.cmake : the file's bytes as `const unsigned char SYMBOL[]`
# and `const unsigned long SYMBOL_size`.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}" "const unsigned char ${SYMBOL}[] = {${bytes}0};\nconst unsigned long ${SYMBOL}_size = ${size};\n")
