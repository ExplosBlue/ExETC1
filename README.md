# rg-etc1

A fast, high-quality **ETC1 (Ericsson Texture Compression)** encoder and decoder.

This project is a modernized fork of [Rich Geldreich's rg_etc1](https://github.com/richgel999/rg-etc1).

## Additions

* **Compile-time table generation**

  * Lookup tables are generated using `consteval`/`constexpr` instead of a runtime initialization step.
  * No `initEtc1Tables()` call is required.
  * Fully initialized before `main()`.
  * Requires C++23.

* **Automatic SIMD dispatch**

  * SSSE3 and AVX2 optimized code paths are compiled with explicit target attributes.
  * The optimal implementation is selected automatically at runtime based on CPU capabilities.

## Building

### As a subdirectory

```cmake
add_subdirectory(third_party/rg-etc1)
target_link_libraries(yourapp PRIVATE rg_etc1::rg_etc1)
```

### Installed package

```cmake
find_package(rg_etc1 CONFIG REQUIRED)
target_link_libraries(yourapp PRIVATE rg_etc1::rg_etc1)
```

### Standalone

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## License

This project is licensed under the **zlib License**.

It is based on the original **rg_etc1** by Rich Geldreich. See [LICENSE](LICENSE) for the complete license text.
