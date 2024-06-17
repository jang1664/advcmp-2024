cmake -S project3 -G Ninja -B build
cmake --build build
clang -O0 -S -emit-llvm -Xclang -disable-O0-optnone -Xclang -disable-llvm-passes -fno-discard-value-names test.c -o test.ll
opt -S -passes='mem2reg' test.ll -o input.ll
opt -S -load-pass-plugin build/lib/libSimpleSCCP.so -passes='simple-sccp' ./input.ll -o out.ll
opt -S -passes=sccp ./input.ll -o llvm_out.ll
clang ./out.ll -o test